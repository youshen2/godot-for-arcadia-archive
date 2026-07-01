/**************************************************************************/
/*  asset_bundle_manager.cpp                                              */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "asset_bundle_manager.h"

#include "core/config/project_settings.h"
#include "core/crypto/hash_calculator.h"
#include "core/io/config_file.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/file_access_encrypted.h"
#include "core/io/json.h"
#include "core/io/resource_importer.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "editor/editor_node.h"
#include "editor/editor_string_names.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/gui/editor_file_dialog.h"
#include "editor/themes/editor_scale.h"
#include "scene/gui/box_container.h"
#include "scene/gui/button.h"
#include "scene/gui/check_box.h"
#include "scene/gui/item_list.h"
#include "scene/gui/label.h"
#include "scene/gui/line_edit.h"
#include "scene/gui/split_container.h"
#include "scene/gui/tree.h"

String AssetBundleManagerDialog::_sanitize_name(const String &p_name) {
	String result;
	for (int i = 0; i < p_name.length(); i++) {
		const char32_t c = p_name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
			result += String::chr(c);
		} else if (c == ' ' || c == '.') {
			result += "_";
		}
	}
	return result.is_empty() ? "asset_bundle" : result;
}

static String _asset_bundle_manager_normalize_portable_path(const String &p_path) {
	return p_path.replace("\\", "/").simplify_path();
}

static bool _asset_bundle_manager_is_file_only_resource(const String &p_path) {
	const String extension = p_path.get_extension().to_lower();
	return extension == "txt" || extension == "json" || p_path.ends_with(".import");
}

static String _asset_bundle_manager_get_resource_type(const String &p_path, const StringName &p_editor_type = StringName()) {
	String type = ResourceLoader::get_resource_type(p_path);
	if (!type.is_empty()) {
		return type;
	}

	const String extension = p_path.get_extension().to_lower();
	if (extension == "txt") {
		return "TextFile";
	}
	if (p_path.ends_with(".import")) {
		return "TextFile";
	}
	if (extension == "json") {
		return "JSON";
	}

	if (!p_editor_type.is_empty() && p_editor_type != SNAME("OtherFile")) {
		return String(p_editor_type);
	}
	return String();
}

static bool _asset_bundle_manager_can_bundle_resource(const String &p_path, const StringName &p_editor_type = StringName()) {
	if (EditorFileSystem::_should_skip_directory(p_path.get_base_dir())) {
		return false;
	}
	if (_asset_bundle_manager_is_file_only_resource(p_path)) {
		return true;
	}
	if (p_editor_type == SNAME("TextFile") || p_editor_type == SNAME("OtherFile")) {
		return false;
	}
	return !ResourceLoader::get_resource_type(p_path).is_empty() || FileAccess::exists(p_path + ".import");
}

struct AssetBundleManagerExportFile {
	String resource_path;
	String source_path;
	String type;
	String hash;
	String md5;
	Vector<uint8_t> data;
	int64_t size = 0;
	uint64_t offset = 0;
	bool encrypted = false;
	bool file_only = false;
};

struct AssetBundleManagerExportChunk {
	String seed_path;
	Vector<AssetBundleManagerExportFile> files;
	bool grouped = false;
};

struct AssetBundleManagerExportChunkSort {
	bool operator()(const AssetBundleManagerExportChunk &p_left, const AssetBundleManagerExportChunk &p_right) const {
		return p_left.seed_path < p_right.seed_path;
	}
};

static int _asset_bundle_manager_hex_char_to_int(char32_t p_char) {
	if (p_char >= '0' && p_char <= '9') {
		return int(p_char - '0');
	}
	if (p_char >= 'a' && p_char <= 'f') {
		return int(p_char - 'a') + 10;
	}
	if (p_char >= 'A' && p_char <= 'F') {
		return int(p_char - 'A') + 10;
	}
	return -1;
}

static bool _asset_bundle_manager_hex_to_bytes(const String &p_hex, Vector<uint8_t> &r_bytes) {
	if ((p_hex.length() % 2) != 0) {
		return false;
	}

	r_bytes.resize(p_hex.length() / 2);
	for (int i = 0; i < r_bytes.size(); i++) {
		const int hi = _asset_bundle_manager_hex_char_to_int(p_hex[i * 2]);
		const int lo = _asset_bundle_manager_hex_char_to_int(p_hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			r_bytes.clear();
			return false;
		}
		r_bytes.write[i] = uint8_t((hi << 4) | lo);
	}

	return true;
}

static bool _asset_bundle_manager_is_valid_encryption_key(const String &p_key) {
	Vector<uint8_t> key;
	return p_key.length() == 64 && _asset_bundle_manager_hex_to_bytes(p_key, key) && key.size() == 32;
}

static Vector<uint8_t> _asset_bundle_manager_make_iv(const String &p_seed) {
	Vector<uint8_t> iv;
	Vector<uint8_t> seed_bytes;
	if (!_asset_bundle_manager_hex_to_bytes(p_seed.sha256_text().substr(0, 32), seed_bytes) || seed_bytes.size() != 16) {
		return iv;
	}

	iv = seed_bytes;
	return iv;
}

static Error _asset_bundle_manager_prepare_export_file(AssetBundleManagerExportFile &r_file) {
	Error read_error = OK;
	r_file.data = FileAccess::get_file_as_bytes(r_file.source_path, &read_error);
	ERR_FAIL_COND_V_MSG(read_error != OK, read_error, vformat("Cannot read resource '%s'.", r_file.resource_path));

	r_file.hash = HashCalculator::hash_bytes_hex(HashingContext::HASH_SHA256, r_file.data);
	r_file.md5 = HashCalculator::hash_bytes_hex(HashingContext::HASH_MD5, r_file.data);
	r_file.size = r_file.data.size();
	ERR_FAIL_COND_V_MSG(r_file.hash.is_empty(), ERR_FILE_CANT_READ, vformat("Cannot hash resource '%s'.", r_file.resource_path));
	ERR_FAIL_COND_V_MSG(r_file.md5.is_empty(), ERR_FILE_CANT_READ, vformat("Cannot hash resource '%s'.", r_file.resource_path));
	return OK;
}

static Error _asset_bundle_manager_store_export_file(Ref<FileAccess> p_chunk_file, AssetBundleManagerExportFile &r_file, const Vector<uint8_t> &p_encryption_key) {
	r_file.offset = p_chunk_file->get_position();
	r_file.encrypted = !p_encryption_key.is_empty();

	Ref<FileAccess> target_file = p_chunk_file;
	Ref<FileAccessEncrypted> encrypted_file;
	if (r_file.encrypted) {
		encrypted_file.instantiate();
		ERR_FAIL_COND_V(encrypted_file.is_null(), ERR_CANT_CREATE);

		const Vector<uint8_t> iv = _asset_bundle_manager_make_iv(r_file.resource_path + "\n" + r_file.hash);
		Error err = encrypted_file->open_and_parse(p_chunk_file, p_encryption_key, FileAccessEncrypted::MODE_WRITE_AES256, false, iv);
		ERR_FAIL_COND_V(err != OK, err);
		target_file = encrypted_file;
	}

	const bool stored = r_file.data.is_empty() || target_file->store_buffer(r_file.data.ptr(), r_file.data.size());

	if (encrypted_file.is_valid()) {
		target_file.unref();
		encrypted_file.unref();
	}

	ERR_FAIL_COND_V_MSG(!stored, ERR_FILE_CANT_WRITE, vformat("Cannot write resource '%s' to AssetBundle chunk.", r_file.resource_path));
	if (!r_file.encrypted) {
		const uint64_t written_size = p_chunk_file->get_position() - r_file.offset;
		ERR_FAIL_COND_V_MSG(written_size != uint64_t(r_file.size), ERR_FILE_CANT_WRITE, vformat("AssetBundle wrote %d bytes for resource '%s', expected %d bytes.", int64_t(written_size), r_file.resource_path, r_file.size));
	}

	r_file.data.clear();
	return OK;
}

static Dictionary _asset_bundle_manager_export_file_to_dictionary(const AssetBundleManagerExportFile &p_file) {
	Dictionary file;
	file["path"] = p_file.resource_path;
	file["type"] = p_file.type;
	file["hash"] = p_file.hash;
	file["md5"] = p_file.md5;
	file["size"] = p_file.size;
	file["offset"] = int64_t(p_file.offset);
	file["encrypted"] = p_file.encrypted;
	file["file_only"] = p_file.file_only;
	return file;
}

Error AssetBundleManagerDialog::_write_text_file(const String &p_path, const String &p_text) {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	ERR_FAIL_COND_V(err != OK, err);
	file->store_string(p_text);
	return OK;
}

Error AssetBundleManagerDialog::_remove_recursive(const String &p_path) {
	Ref<DirAccess> dir = DirAccess::open(p_path);
	if (dir.is_null()) {
		return OK;
	}

	for (const String &file_name : dir->get_files()) {
		Error err = DirAccess::remove_absolute(p_path.path_join(file_name));
		if (err != OK) {
			return err;
		}
	}

	for (const String &dir_name : dir->get_directories()) {
		Error err = _remove_recursive(p_path.path_join(dir_name));
		if (err != OK) {
			return err;
		}
	}

	return DirAccess::remove_absolute(p_path);
}

void AssetBundleManagerDialog::_load_config() {
	manifests.clear();

	Ref<ConfigFile> config;
	config.instantiate();
	if (config->load(CONFIG_PATH) != OK) {
		ManifestInfo manifest;
		manifest.name = "main";
		manifests.push_back(manifest);
		current_manifest = 0;
		return;
	}

	Ref<ConfigFile> credentials;
	credentials.instantiate();
	credentials->load(CREDENTIALS_PATH);

	Vector<String> sections = config->get_sections();
	sections.sort();
	for (const String &section : sections) {
		if (!section.begins_with("manifest/")) {
			continue;
		}

		ManifestInfo manifest;
		manifest.name = config->get_value(section, "name", section.trim_prefix("manifest/"));
		manifest.version = config->get_value(section, "version", "1.0.0");
		manifest.output_path = config->get_value(section, "output_path", "res://asset_bundles");
		manifest.encryption_enabled = config->get_value(section, "encryption_enabled", false);
		manifest.encryption_key = credentials->get_value(section, "encryption_key", config->get_value(section, "encryption_key", String()));
		manifest.resources = config->get_value(section, "resources", PackedStringArray());
		manifests.push_back(manifest);
	}

	if (manifests.is_empty()) {
		ManifestInfo manifest;
		manifest.name = "main";
		manifests.push_back(manifest);
	}
	current_manifest = 0;
}

void AssetBundleManagerDialog::_save_config() {
	_store_current_manifest();

	Ref<ConfigFile> config;
	config.instantiate();
	Ref<ConfigFile> credentials;
	credentials.instantiate();
	for (const ManifestInfo &manifest : manifests) {
		const String section = "manifest/" + _sanitize_name(manifest.name);
		config->set_value(section, "name", manifest.name);
		config->set_value(section, "version", manifest.version);
		config->set_value(section, "output_path", manifest.output_path);
		config->set_value(section, "encryption_enabled", manifest.encryption_enabled);
		config->set_value(section, "resources", manifest.resources);
		credentials->set_value(section, "encryption_key", manifest.encryption_key);
	}

	Error err = config->save(CONFIG_PATH);
	if (err == OK && EditorFileSystem::get_singleton() != nullptr) {
		EditorFileSystem::get_singleton()->update_file(CONFIG_PATH);
	}

	err = DirAccess::make_dir_recursive_absolute(ProjectSettings::get_singleton()->globalize_path("res://.godot"));
	if (err == OK) {
		credentials->save(CREDENTIALS_PATH);
	}
}

void AssetBundleManagerDialog::_update_manifest_list() {
	updating = true;
	manifest_list->clear();
	for (const ManifestInfo &manifest : manifests) {
		manifest_list->add_item(manifest.name);
	}
	if (current_manifest >= 0 && current_manifest < manifests.size()) {
		manifest_list->select(current_manifest);
	}
	updating = false;
}

void AssetBundleManagerDialog::_edit_manifest(int p_index) {
	if (p_index < 0 || p_index >= manifests.size()) {
		current_manifest = -1;
		return;
	}

	current_manifest = p_index;
	const ManifestInfo &manifest = manifests[current_manifest];

	updating = true;
	name_edit->set_text(manifest.name);
	version_edit->set_text(manifest.version);
	output_path_edit->set_text(manifest.output_path);
	encryption_enabled_check->set_pressed(manifest.encryption_enabled);
	encryption_key_edit->set_text(manifest.encryption_key);
	encryption_key_edit->set_editable(manifest.encryption_enabled);
	encryption_key_visibility_button->set_disabled(!manifest.encryption_enabled);
	if (!manifest.encryption_enabled) {
		encryption_key_visibility_button->set_pressed(false);
		encryption_key_edit->set_secret(true);
	}
	_fill_resource_tree();
	PackedStringArray visible_resources;
	_collect_checked_resources(resource_tree->get_root(), visible_resources);
	info_label->set_text(vformat(TTR("%d resources selected."), visible_resources.size()));
	updating = false;
}

void AssetBundleManagerDialog::_store_current_manifest() {
	if (updating || current_manifest < 0 || current_manifest >= manifests.size()) {
		return;
	}

	ManifestInfo &manifest = manifests.write[current_manifest];
	manifest.name = name_edit->get_text().strip_edges();
	if (manifest.name.is_empty()) {
		manifest.name = "main";
	}
	manifest.version = version_edit->get_text().strip_edges();
	if (manifest.version.is_empty()) {
		manifest.version = "1.0.0";
	}
	manifest.output_path = output_path_edit->get_text().strip_edges();
	if (manifest.output_path.is_empty()) {
		manifest.output_path = "res://asset_bundles";
	}
	manifest.encryption_enabled = encryption_enabled_check->is_pressed();
	manifest.encryption_key = encryption_key_edit->get_text().strip_edges();
}

void AssetBundleManagerDialog::_fill_resource_tree() {
	resource_tree->clear();
	TreeItem *root = resource_tree->create_item();

	HashSet<String> selected;
	if (current_manifest >= 0 && current_manifest < manifests.size()) {
		for (int i = 0; i < manifests[current_manifest].resources.size(); i++) {
			selected.insert(manifests[current_manifest].resources[i]);
		}
	}

	EditorFileSystemDirectory *filesystem = EditorFileSystem::get_singleton()->get_filesystem();
	if (filesystem != nullptr) {
		_fill_resource_tree_dir(filesystem, root, selected);
	}
}

bool AssetBundleManagerDialog::_fill_resource_tree_dir(EditorFileSystemDirectory *p_dir, TreeItem *p_parent, const HashSet<String> &p_selected) {
	if (EditorFileSystem::_should_skip_directory(p_dir->get_path())) {
		return false;
	}

	TreeItem *dir_item = resource_tree->create_item(p_parent);
	dir_item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
	dir_item->set_text(0, p_dir->get_name() + "/");
	dir_item->set_icon(0, get_theme_icon(SNAME("folder"), SNAME("FileDialog")));
	dir_item->set_editable(0, true);
	dir_item->set_metadata(0, p_dir->get_path());

	bool used = false;
	for (int i = 0; i < p_dir->get_subdir_count(); i++) {
		if (_fill_resource_tree_dir(p_dir->get_subdir(i), dir_item, p_selected)) {
			used = true;
		}
	}

	for (int i = 0; i < p_dir->get_file_count(); i++) {
		const String path = p_dir->get_file_path(i);
		const StringName type = p_dir->get_file_type(i);
		if (!_asset_bundle_manager_can_bundle_resource(path, type)) {
			continue;
		}

		TreeItem *file_item = resource_tree->create_item(dir_item);
		file_item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
		file_item->set_text(0, p_dir->get_file(i));
		file_item->set_editable(0, true);
		file_item->set_metadata(0, path);
		file_item->set_checked(0, p_selected.has(path));

		Ref<Texture2D> icon = EditorNode::get_singleton()->get_class_icon(type);
		if (icon.is_valid()) {
			file_item->set_icon(0, icon);
		}
		used = true;
	}

	if (!used) {
		memdelete(dir_item);
	}

	return used;
}

void AssetBundleManagerDialog::_collect_checked_resources(TreeItem *p_item, PackedStringArray &r_resources) const {
	if (p_item == nullptr) {
		return;
	}

	Variant metadata = p_item->get_metadata(0);
	if (metadata.get_type() == Variant::STRING) {
		const String path = metadata;
		if (!path.ends_with("/") && p_item->is_checked(0) && FileAccess::exists(path)) {
			r_resources.push_back(path);
		}
	}

	TreeItem *child = p_item->get_first_child();
	while (child != nullptr) {
		_collect_checked_resources(child, r_resources);
		child = child->get_next();
	}
}

void AssetBundleManagerDialog::_add_manifest() {
	_store_current_manifest();

	ManifestInfo manifest;
	int index = manifests.size() + 1;
	while (true) {
		manifest.name = "manifest_" + itos(index);
		bool exists = false;
		for (const ManifestInfo &existing : manifests) {
			if (existing.name == manifest.name) {
				exists = true;
				break;
			}
		}
		if (!exists) {
			break;
		}
		index++;
	}

	manifests.push_back(manifest);
	current_manifest = manifests.size() - 1;
	_update_manifest_list();
	_edit_manifest(current_manifest);
	_save_config();
}

void AssetBundleManagerDialog::_delete_manifest() {
	if (current_manifest < 0 || current_manifest >= manifests.size()) {
		return;
	}

	manifests.remove_at(current_manifest);
	if (manifests.is_empty()) {
		ManifestInfo manifest;
		manifest.name = "main";
		manifests.push_back(manifest);
	}
	current_manifest = MIN(current_manifest, manifests.size() - 1);
	_update_manifest_list();
	_edit_manifest(current_manifest);
	_save_config();
}

void AssetBundleManagerDialog::_manifest_selected(int p_index) {
	if (updating) {
		return;
	}

	_store_current_manifest();
	_edit_manifest(p_index);
	_save_config();
}

void AssetBundleManagerDialog::_manifest_name_changed(const String &p_text) {
	if (updating) {
		return;
	}
	_store_current_manifest();
	_update_manifest_list();
	_save_config();
}

void AssetBundleManagerDialog::_manifest_version_changed(const String &p_text) {
	if (updating) {
		return;
	}
	_store_current_manifest();
	_save_config();
}

void AssetBundleManagerDialog::_manifest_output_changed(const String &p_text) {
	if (updating) {
		return;
	}
	_store_current_manifest();
	_save_config();
}

void AssetBundleManagerDialog::_manifest_encryption_enabled_toggled(bool p_pressed) {
	if (updating) {
		return;
	}

	encryption_key_edit->set_editable(p_pressed);
	encryption_key_visibility_button->set_disabled(!p_pressed);
	if (!p_pressed) {
		encryption_key_visibility_button->set_pressed(false);
		encryption_key_edit->set_secret(true);
	}
	_store_current_manifest();
	_save_config();
}

void AssetBundleManagerDialog::_manifest_encryption_key_changed(const String &p_text) {
	if (updating) {
		return;
	}
	_store_current_manifest();
	_save_config();
}

void AssetBundleManagerDialog::_manifest_encryption_key_visibility_toggled(bool p_pressed) {
	encryption_key_edit->set_secret(!p_pressed);
	encryption_key_visibility_button->set_button_icon(get_theme_icon(p_pressed ? SNAME("GuiVisibilityVisible") : SNAME("GuiVisibilityHidden"), EditorStringName(EditorIcons)));
	encryption_key_visibility_button->set_tooltip_text(p_pressed ? TTRC("Hide encryption key") : TTRC("Show encryption key"));
}

void AssetBundleManagerDialog::_browse_output_path() {
	if (output_path_dialog == nullptr) {
		return;
	}

	_store_current_manifest();

	String current_path = output_path_edit->get_text().strip_edges();
	if (current_path.is_empty()) {
		current_path = "res://asset_bundles";
	}
	if (current_path.is_relative_path()) {
		current_path = ProjectSettings::get_singleton()->globalize_path("res://");
	} else {
		current_path = ProjectSettings::get_singleton()->globalize_path(current_path);
	}
	while (!DirAccess::dir_exists_absolute(current_path)) {
		String parent_path = current_path.get_base_dir();
		if (parent_path.is_empty() || parent_path == current_path) {
			current_path = ProjectSettings::get_singleton()->globalize_path("res://");
			break;
		}
		current_path = parent_path;
	}
	output_path_dialog->set_current_dir(current_path);
	output_path_dialog->popup_file_dialog();
}

void AssetBundleManagerDialog::_output_path_selected(const String &p_path) {
	if (updating) {
		return;
	}

	updating = true;
	output_path_edit->set_text(ProjectSettings::get_singleton()->localize_path(p_path));
	updating = false;
	_store_current_manifest();
	_save_config();
}

void AssetBundleManagerDialog::_resource_tree_edited() {
	if (updating || current_manifest < 0 || current_manifest >= manifests.size()) {
		return;
	}

	TreeItem *edited = resource_tree->get_edited();
	if (edited != nullptr && edited->get_first_child() != nullptr) {
		edited->propagate_check(0);
	}

	PackedStringArray resources;
	_collect_checked_resources(resource_tree->get_root(), resources);
	manifests.write[current_manifest].resources = resources;
	info_label->set_text(vformat(TTR("%d resources selected."), resources.size()));
	_save_config();
}

void AssetBundleManagerDialog::_export_current_manifest() {
	if (current_manifest < 0 || current_manifest >= manifests.size()) {
		return;
	}

	_store_current_manifest();
	Error err = _export_manifest(manifests[current_manifest]);
	if (err != OK && !info_label->get_text().begins_with(TTR("Invalid encryption key"))) {
		info_label->set_text(TTR("AssetBundle export failed."));
	}
}

Error AssetBundleManagerDialog::_export_manifest(const ManifestInfo &p_manifest) {
	Vector<uint8_t> encryption_key;
	if (p_manifest.encryption_enabled) {
		const String key_text = p_manifest.encryption_key.strip_edges().to_lower();
		if (!_asset_bundle_manager_is_valid_encryption_key(key_text) || !_asset_bundle_manager_hex_to_bytes(key_text, encryption_key)) {
			info_label->set_text(TTR("Invalid encryption key. It must be 64 hexadecimal characters."));
			return ERR_INVALID_PARAMETER;
		}
	}

	Vector<AssetBundleManagerExportChunk> export_chunks;
	HashSet<String> exported_paths;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	const String imported_files_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->get_imported_files_path());
	const String imported_files_prefix = imported_files_path.ends_with("/") ? imported_files_path : imported_files_path + "/";

	auto add_file_to_chunk = [&](AssetBundleManagerExportChunk &r_chunk, const String &p_resource_path, const StringName &p_editor_type) -> Error {
		const String resource_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->localize_path(p_resource_path));
		if (exported_paths.has(resource_path)) {
			return OK;
		}
		if (!FileAccess::exists(resource_path)) {
			return OK;
		}

		AssetBundleManagerExportFile file;
		file.resource_path = resource_path;
		file.source_path = ProjectSettings::get_singleton()->globalize_path(resource_path);
		file.type = _asset_bundle_manager_get_resource_type(resource_path, p_editor_type);
		file.file_only = _asset_bundle_manager_is_file_only_resource(resource_path);
		r_chunk.files.push_back(file);
		exported_paths.insert(resource_path);
		return OK;
	};

	for (int i = 0; i < p_manifest.resources.size(); i++) {
		const String resource_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->localize_path(p_manifest.resources[i]));
		const String import_sidecar_path = resource_path + ".import";
		const bool has_import_sidecar = FileAccess::exists(import_sidecar_path);
		StringName editor_type;
		if (EditorFileSystem::get_singleton() != nullptr) {
			editor_type = EditorFileSystem::get_singleton()->get_file_type(resource_path);
		}
		if (!_asset_bundle_manager_can_bundle_resource(resource_path, editor_type)) {
			continue;
		}

		Vector<String> imported_paths;
		bool has_imported_files_output = false;
		if (importer != nullptr) {
			List<String> importer_paths;
			importer->get_internal_resource_path_list(resource_path, &importer_paths);
			for (const String &imported_path : importer_paths) {
				const String normalized_imported_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->localize_path(imported_path));
				if (FileAccess::exists(normalized_imported_path)) {
					imported_paths.push_back(normalized_imported_path);
					if (normalized_imported_path.begins_with(imported_files_prefix)) {
						has_imported_files_output = true;
					}
				}
			}
		}
		imported_paths.sort();

		if (has_import_sidecar && has_imported_files_output) {
			AssetBundleManagerExportChunk chunk;
			chunk.seed_path = resource_path;
			chunk.grouped = true;
			Error err = add_file_to_chunk(chunk, import_sidecar_path, StringName());
			ERR_FAIL_COND_V(err != OK, err);
			for (const String &imported_path : imported_paths) {
				err = add_file_to_chunk(chunk, imported_path, StringName());
				ERR_FAIL_COND_V(err != OK, err);
			}
			if (!chunk.files.is_empty()) {
				export_chunks.push_back(chunk);
			}
		} else {
			AssetBundleManagerExportChunk resource_chunk;
			resource_chunk.seed_path = resource_path;
			Error err = add_file_to_chunk(resource_chunk, resource_path, editor_type);
			ERR_FAIL_COND_V(err != OK, err);
			if (!resource_chunk.files.is_empty()) {
				export_chunks.push_back(resource_chunk);
			}

			AssetBundleManagerExportChunk import_chunk;
			import_chunk.seed_path = import_sidecar_path;
			err = add_file_to_chunk(import_chunk, import_sidecar_path, StringName());
			ERR_FAIL_COND_V(err != OK, err);
			if (!import_chunk.files.is_empty()) {
				export_chunks.push_back(import_chunk);
			}

			for (const String &imported_path : imported_paths) {
				AssetBundleManagerExportChunk imported_chunk;
				imported_chunk.seed_path = imported_path;
				err = add_file_to_chunk(imported_chunk, imported_path, StringName());
				ERR_FAIL_COND_V(err != OK, err);
				if (!imported_chunk.files.is_empty()) {
					export_chunks.push_back(imported_chunk);
				}
			}
		}
	}
	export_chunks.sort_custom<AssetBundleManagerExportChunkSort>();

	int exported_file_count = 0;
	for (AssetBundleManagerExportChunk &chunk : export_chunks) {
		for (AssetBundleManagerExportFile &file : chunk.files) {
			Error err = _asset_bundle_manager_prepare_export_file(file);
			ERR_FAIL_COND_V(err != OK, err);
			exported_file_count++;
		}
	}

	const String safe_name = _sanitize_name(p_manifest.name);
	const String output_root = ProjectSettings::get_singleton()->globalize_path(p_manifest.output_path.path_join(safe_name));
	const String bundle_id = "ab_" + safe_name.sha256_text().substr(0, 16);
	const String bundle_dir = output_root.path_join(bundle_id);

	Error err = _remove_recursive(bundle_dir);
	ERR_FAIL_COND_V(err != OK, err);
	err = DirAccess::make_dir_recursive_absolute(bundle_dir);
	ERR_FAIL_COND_V(err != OK, err);

	EditorProgress progress("asset_bundle_export", TTR("Export AssetBundle"), export_chunks.size() + 3);
	Array chunks;
	int64_t total_size = 0;

	for (int i = 0; i < export_chunks.size(); i++) {
		AssetBundleManagerExportChunk &export_chunk = export_chunks.write[i];
		progress.step(export_chunk.seed_path, i);

		String chunk_hash_source = safe_name + "\n" + export_chunk.seed_path + "\n" + (p_manifest.encryption_enabled ? "encrypted" : "plain");
		for (const AssetBundleManagerExportFile &file : export_chunk.files) {
			chunk_hash_source += "\n" + file.resource_path + "\n" + file.hash;
		}
		const String chunk_hash = chunk_hash_source.sha256_text();
		const String chunk_rel_path = _asset_bundle_manager_normalize_portable_path(chunk_hash.substr(0, 2).path_join(chunk_hash.substr(2, 2)).path_join(chunk_hash.substr(4, 28) + ".ab"));
		const String chunk_abs_path = bundle_dir.path_join(chunk_rel_path);
		err = DirAccess::make_dir_recursive_absolute(chunk_abs_path.get_base_dir());
		ERR_FAIL_COND_V(err != OK, err);

		Ref<FileAccess> chunk_file = FileAccess::open(chunk_abs_path, FileAccess::WRITE, &err);
		ERR_FAIL_COND_V(err != OK, err);
		for (AssetBundleManagerExportFile &file : export_chunk.files) {
			err = _asset_bundle_manager_store_export_file(chunk_file, file, encryption_key);
			ERR_FAIL_COND_V(err != OK, err);
		}
		chunk_file.unref();

		const String packed_hash = HashCalculator::hash_file_hex(HashingContext::HASH_SHA256, chunk_abs_path);
		const int64_t packed_size = FileAccess::get_size(chunk_abs_path);
		ERR_FAIL_COND_V_MSG(packed_hash.is_empty(), ERR_FILE_CANT_READ, vformat("Cannot hash chunk '%s'.", chunk_rel_path));
		ERR_FAIL_COND_V_MSG(packed_size < 0, ERR_FILE_CANT_READ, vformat("Cannot stat chunk '%s'.", chunk_rel_path));

		if (export_chunk.grouped) {
			Array files;
			int64_t chunk_plain_size = 0;
			String group_hash_source = export_chunk.seed_path;
			for (const AssetBundleManagerExportFile &file : export_chunk.files) {
				Dictionary file_entry = _asset_bundle_manager_export_file_to_dictionary(file);
				file_entry["packed_hash"] = packed_hash;
				file_entry["packed_size"] = packed_size;
				files.push_back(file_entry);
				chunk_plain_size += file.size;
				group_hash_source += "\n" + file.resource_path + "\n" + file.hash;
			}

			Dictionary chunk;
			chunk["path"] = export_chunk.seed_path;
			chunk["type"] = _asset_bundle_manager_get_resource_type(export_chunk.seed_path);
			chunk["chunk"] = chunk_rel_path;
			chunk["hash"] = group_hash_source.sha256_text();
			chunk["size"] = packed_size;
			chunk["plain_size"] = chunk_plain_size;
			chunk["packed_hash"] = packed_hash;
			chunk["packed_size"] = packed_size;
			chunk["encrypted"] = p_manifest.encryption_enabled;
			chunk["files"] = files;
			chunks.push_back(chunk);
		} else {
			ERR_FAIL_COND_V(export_chunk.files.size() != 1, ERR_BUG);
			const AssetBundleManagerExportFile &file = export_chunk.files[0];
			Dictionary chunk = _asset_bundle_manager_export_file_to_dictionary(file);
			chunk["chunk"] = chunk_rel_path;
			chunk["packed_hash"] = packed_hash;
			chunk["packed_size"] = packed_size;
			chunks.push_back(chunk);
		}
		total_size += packed_size;
	}

	Dictionary bundle_manifest;
	bundle_manifest["format"] = "GodotAssetBundle";
	bundle_manifest["format_version"] = 1;
	bundle_manifest["name"] = p_manifest.name;
	bundle_manifest["version"] = p_manifest.version;
	bundle_manifest["encrypted"] = p_manifest.encryption_enabled;
	bundle_manifest["chunks"] = chunks;

	const String bundle_json = JSON::stringify(bundle_manifest, "\t", true);
	const String bundle_hash = bundle_json.sha256_text();
	bundle_manifest["hash"] = bundle_hash;
	bundle_manifest["size"] = total_size;

	progress.step(TTR("Writing bundle manifest"), export_chunks.size());
	err = _write_text_file(bundle_dir.path_join("bundle.json"), JSON::stringify(bundle_manifest, "\t", true));
	ERR_FAIL_COND_V(err != OK, err);

	Dictionary bundle_entry;
	bundle_entry["name"] = p_manifest.name;
	bundle_entry["path"] = bundle_id;
	bundle_entry["version"] = p_manifest.version;
	bundle_entry["hash"] = bundle_hash;
	bundle_entry["size"] = total_size;
	bundle_entry["encrypted"] = p_manifest.encryption_enabled;
	bundle_entry["chunks"] = chunks;

	Array bundles;
	bundles.push_back(bundle_entry);

	Dictionary root_manifest;
	root_manifest["format"] = "GodotAssetBundleManifest";
	root_manifest["format_version"] = 1;
	root_manifest["version"] = p_manifest.version;
	root_manifest["bundles"] = bundles;

	progress.step(TTR("Writing manifest"), export_chunks.size() + 1);
	err = _write_text_file(output_root.path_join("manifest.json"), JSON::stringify(root_manifest, "\t", true));
	ERR_FAIL_COND_V(err != OK, err);

	progress.step(TTR("Done"), export_chunks.size() + 2);
	info_label->set_text(vformat(TTR("AssetBundle exported. %d files in %d chunks."), exported_file_count, export_chunks.size()));
	return OK;
}

void AssetBundleManagerDialog::_bind_methods() {
}

void AssetBundleManagerDialog::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_THEME_CHANGED: {
			add_button->set_button_icon(get_theme_icon(SNAME("Add"), EditorStringName(EditorIcons)));
			delete_button->set_button_icon(get_theme_icon(SNAME("Remove"), EditorStringName(EditorIcons)));
			output_path_browse_button->set_button_icon(get_theme_icon(SNAME("Folder"), EditorStringName(EditorIcons)));
			encryption_key_visibility_button->set_button_icon(get_theme_icon(encryption_key_visibility_button->is_pressed() ? SNAME("GuiVisibilityVisible") : SNAME("GuiVisibilityHidden"), EditorStringName(EditorIcons)));
		} break;
	}
}

void AssetBundleManagerDialog::popup_manager() {
	_load_config();
	_update_manifest_list();
	_edit_manifest(current_manifest);
	_save_config();
	popup_centered_clamped(Size2(950, 560) * EDSCALE, 0.8);
}

AssetBundleManagerDialog::AssetBundleManagerDialog() {
	set_title(TTRC("AssetBundle Manager"));
	set_ok_button_text(TTRC("Close"));

	HSplitContainer *split = memnew(HSplitContainer);
	split->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
	add_child(split);

	VBoxContainer *left = memnew(VBoxContainer);
	left->set_custom_minimum_size(Size2(220, 0) * EDSCALE);
	split->add_child(left);

	HBoxContainer *manifest_buttons = memnew(HBoxContainer);
	left->add_child(manifest_buttons);

	add_button = memnew(Button);
	add_button->set_tooltip_text(TTRC("Create Manifest"));
	manifest_buttons->add_child(add_button);
	add_button->connect("pressed", callable_mp(this, &AssetBundleManagerDialog::_add_manifest));

	delete_button = memnew(Button);
	delete_button->set_tooltip_text(TTRC("Delete Manifest"));
	manifest_buttons->add_child(delete_button);
	delete_button->connect("pressed", callable_mp(this, &AssetBundleManagerDialog::_delete_manifest));

	manifest_list = memnew(ItemList);
	manifest_list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	left->add_child(manifest_list);
	manifest_list->connect("item_selected", callable_mp(this, &AssetBundleManagerDialog::_manifest_selected));

	VBoxContainer *right = memnew(VBoxContainer);
	right->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	split->add_child(right);

	Label *name_label = memnew(Label(TTRC("Manifest Name")));
	right->add_child(name_label);
	name_edit = memnew(LineEdit);
	right->add_child(name_edit);
	name_edit->connect("text_changed", callable_mp(this, &AssetBundleManagerDialog::_manifest_name_changed));

	Label *version_label = memnew(Label(TTRC("Version")));
	right->add_child(version_label);
	version_edit = memnew(LineEdit);
	right->add_child(version_edit);
	version_edit->connect("text_changed", callable_mp(this, &AssetBundleManagerDialog::_manifest_version_changed));

	Label *output_label = memnew(Label(TTRC("Output Path")));
	right->add_child(output_label);
	HBoxContainer *output_path_container = memnew(HBoxContainer);
	right->add_child(output_path_container);
	output_path_edit = memnew(LineEdit);
	output_path_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	output_path_container->add_child(output_path_edit);
	output_path_edit->connect("text_changed", callable_mp(this, &AssetBundleManagerDialog::_manifest_output_changed));

	output_path_browse_button = memnew(Button);
	output_path_browse_button->set_tooltip_text(TTRC("Browse Output Folder"));
	output_path_container->add_child(output_path_browse_button);
	output_path_browse_button->connect("pressed", callable_mp(this, &AssetBundleManagerDialog::_browse_output_path));

	encryption_enabled_check = memnew(CheckBox);
	encryption_enabled_check->set_text(TTRC("Encrypt AssetBundle Chunks"));
	right->add_child(encryption_enabled_check);
	encryption_enabled_check->connect("toggled", callable_mp(this, &AssetBundleManagerDialog::_manifest_encryption_enabled_toggled));

	Label *encryption_key_label = memnew(Label(TTRC("Encryption Key (256-bits as hexadecimal):")));
	right->add_child(encryption_key_label);
	HBoxContainer *encryption_key_container = memnew(HBoxContainer);
	right->add_child(encryption_key_container);
	encryption_key_edit = memnew(LineEdit);
	encryption_key_edit->set_h_size_flags(Control::SIZE_EXPAND_FILL);
	encryption_key_edit->set_secret(true);
	encryption_key_edit->set_editable(false);
	encryption_key_edit->connect("text_changed", callable_mp(this, &AssetBundleManagerDialog::_manifest_encryption_key_changed));
	encryption_key_container->add_child(encryption_key_edit);

	encryption_key_visibility_button = memnew(Button);
	encryption_key_visibility_button->set_toggle_mode(true);
	encryption_key_visibility_button->set_disabled(true);
	encryption_key_visibility_button->set_tooltip_text(TTRC("Show encryption key"));
	encryption_key_visibility_button->connect("toggled", callable_mp(this, &AssetBundleManagerDialog::_manifest_encryption_key_visibility_toggled));
	encryption_key_container->add_child(encryption_key_visibility_button);

	info_label = memnew(Label);
	right->add_child(info_label);

	resource_tree = memnew(Tree);
	resource_tree->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	resource_tree->set_hide_root(true);
	resource_tree->set_auto_translate_mode(AUTO_TRANSLATE_MODE_DISABLED);
	resource_tree->set_theme_type_variation("TreeSecondary");
	right->add_child(resource_tree);
	resource_tree->connect("item_edited", callable_mp(this, &AssetBundleManagerDialog::_resource_tree_edited));

	export_button = memnew(Button);
	export_button->set_text(TTRC("Export AssetBundle"));
	right->add_child(export_button);
	export_button->connect("pressed", callable_mp(this, &AssetBundleManagerDialog::_export_current_manifest));

	output_path_dialog = memnew(EditorFileDialog);
	output_path_dialog->set_access(EditorFileDialog::ACCESS_FILESYSTEM);
	output_path_dialog->set_file_mode(EditorFileDialog::FILE_MODE_OPEN_DIR);
	output_path_dialog->set_title(TTRC("Select AssetBundle Output Folder"));
	add_child(output_path_dialog);
	output_path_dialog->connect("dir_selected", callable_mp(this, &AssetBundleManagerDialog::_output_path_selected));
}
