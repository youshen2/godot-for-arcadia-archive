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
	for (const ManifestInfo &manifest : manifests) {
		const String section = "manifest/" + _sanitize_name(manifest.name);
		config->set_value(section, "name", manifest.name);
		config->set_value(section, "version", manifest.version);
		config->set_value(section, "output_path", manifest.output_path);
		config->set_value(section, "resources", manifest.resources);
	}

	Error err = config->save(CONFIG_PATH);
	if (err == OK && EditorFileSystem::get_singleton() != nullptr) {
		EditorFileSystem::get_singleton()->update_file(CONFIG_PATH);
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
	info_label->set_text(vformat(TTR("%d resources selected."), manifest.resources.size()));
	_fill_resource_tree();
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
		TreeItem *file_item = resource_tree->create_item(dir_item);
		file_item->set_cell_mode(0, TreeItem::CELL_MODE_CHECK);
		file_item->set_text(0, p_dir->get_file(i));
		file_item->set_editable(0, true);
		file_item->set_metadata(0, path);
		file_item->set_checked(0, p_selected.has(path));

		const String type = p_dir->get_file_type(i);
		Ref<Texture2D> icon;
		if (!type.is_empty()) {
			icon = EditorNode::get_singleton()->get_class_icon(type);
		}
		if (icon.is_valid()) {
			file_item->set_icon(0, icon);
		}
		used = true;
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
	if (err == OK) {
		info_label->set_text(TTR("AssetBundle exported."));
	}
}

Error AssetBundleManagerDialog::_export_manifest(const ManifestInfo &p_manifest) {
	HashSet<String> export_set;
	ResourceFormatImporter *importer = ResourceFormatImporter::get_singleton();
	for (int i = 0; i < p_manifest.resources.size(); i++) {
		const String resource_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->localize_path(p_manifest.resources[i]));
		if (FileAccess::exists(resource_path)) {
			export_set.insert(resource_path);
		}

		const String import_sidecar_path = resource_path + ".import";
		if (FileAccess::exists(import_sidecar_path)) {
			export_set.insert(import_sidecar_path);
		}

		if (importer != nullptr) {
			List<String> imported_paths;
			importer->get_internal_resource_path_list(resource_path, &imported_paths);
			for (const String &imported_path : imported_paths) {
				const String normalized_imported_path = _asset_bundle_manager_normalize_portable_path(ProjectSettings::get_singleton()->localize_path(imported_path));
				if (FileAccess::exists(normalized_imported_path)) {
					export_set.insert(normalized_imported_path);
				}
			}
		}
	}

	Vector<String> resources;
	for (const String &resource_path : export_set) {
		resources.push_back(resource_path);
	}
	resources.sort();

	const String safe_name = _sanitize_name(p_manifest.name);
	const String output_root = ProjectSettings::get_singleton()->globalize_path(p_manifest.output_path.path_join(safe_name));
	const String bundle_id = "ab_" + safe_name.sha256_text().substr(0, 16);
	const String bundle_dir = output_root.path_join(bundle_id);

	Error err = _remove_recursive(bundle_dir);
	ERR_FAIL_COND_V(err != OK, err);
	err = DirAccess::make_dir_recursive_absolute(bundle_dir);
	ERR_FAIL_COND_V(err != OK, err);

	EditorProgress progress("asset_bundle_export", TTR("Export AssetBundle"), resources.size() + 3);
	Array chunks;
	int64_t total_size = 0;

	for (int i = 0; i < resources.size(); i++) {
		const String resource_path = resources[i];
		progress.step(resource_path, i);

		const String source_path = ProjectSettings::get_singleton()->globalize_path(resource_path);
		const String sha256 = HashCalculator::hash_file_hex(HashingContext::HASH_SHA256, source_path);
		const String md5 = HashCalculator::hash_file_hex(HashingContext::HASH_MD5, source_path);
		const int64_t size = FileAccess::get_size(source_path);
		ERR_FAIL_COND_V_MSG(sha256.is_empty(), ERR_FILE_CANT_READ, vformat("Cannot hash resource '%s'.", resource_path));

		const String chunk_hash = (safe_name + "\n" + resource_path + "\n" + sha256).sha256_text();
		const String chunk_rel_path = _asset_bundle_manager_normalize_portable_path(chunk_hash.substr(0, 2).path_join(chunk_hash.substr(2, 2)).path_join(chunk_hash.substr(4, 28) + ".ab"));
		const String chunk_abs_path = bundle_dir.path_join(chunk_rel_path);
		err = DirAccess::make_dir_recursive_absolute(chunk_abs_path.get_base_dir());
		ERR_FAIL_COND_V(err != OK, err);
		err = DirAccess::copy_absolute(source_path, chunk_abs_path);
		ERR_FAIL_COND_V(err != OK, err);

		Dictionary chunk;
		chunk["path"] = resource_path;
		chunk["type"] = ResourceLoader::get_resource_type(resource_path);
		chunk["chunk"] = chunk_rel_path;
		chunk["hash"] = sha256;
		chunk["md5"] = md5;
		chunk["size"] = size;
		chunks.push_back(chunk);
		total_size += size;
	}

	Dictionary bundle_manifest;
	bundle_manifest["format"] = "GodotAssetBundle";
	bundle_manifest["format_version"] = 1;
	bundle_manifest["name"] = p_manifest.name;
	bundle_manifest["version"] = p_manifest.version;
	bundle_manifest["chunks"] = chunks;

	const String bundle_json = JSON::stringify(bundle_manifest, "\t", true);
	const String bundle_hash = bundle_json.sha256_text();
	bundle_manifest["hash"] = bundle_hash;
	bundle_manifest["size"] = total_size;

	progress.step(TTR("Writing bundle manifest"), resources.size());
	err = _write_text_file(bundle_dir.path_join("bundle.json"), JSON::stringify(bundle_manifest, "\t", true));
	ERR_FAIL_COND_V(err != OK, err);

	Dictionary bundle_entry;
	bundle_entry["name"] = p_manifest.name;
	bundle_entry["path"] = bundle_id;
	bundle_entry["version"] = p_manifest.version;
	bundle_entry["hash"] = bundle_hash;
	bundle_entry["size"] = total_size;
	bundle_entry["chunks"] = chunks;

	Array bundles;
	bundles.push_back(bundle_entry);

	Dictionary root_manifest;
	root_manifest["format"] = "GodotAssetBundleManifest";
	root_manifest["format_version"] = 1;
	root_manifest["version"] = p_manifest.version;
	root_manifest["bundles"] = bundles;

	progress.step(TTR("Writing manifest"), resources.size() + 1);
	err = _write_text_file(output_root.path_join("manifest.json"), JSON::stringify(root_manifest, "\t", true));
	ERR_FAIL_COND_V(err != OK, err);

	progress.step(TTR("Done"), resources.size() + 2);
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
