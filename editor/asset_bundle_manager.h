/**************************************************************************/
/*  asset_bundle_manager.h                                                */
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

#pragma once

#include "scene/gui/dialogs.h"

#include "core/templates/hash_set.h"

class Button;
class CheckBox;
class EditorFileSystemDirectory;
class EditorFileDialog;
class ItemList;
class Label;
class LineEdit;
class Tree;
class TreeItem;

class AssetBundleManagerDialog : public ConfirmationDialog {
	GDCLASS(AssetBundleManagerDialog, ConfirmationDialog);

	struct ManifestInfo {
		String name;
		String version = "1.0.0";
		String output_path = "res://asset_bundles";
		bool encryption_enabled = false;
		String encryption_key;
		PackedStringArray resources;
	};

	static constexpr const char *CONFIG_PATH = "res://asset_bundle_manifests.cfg";
	static constexpr const char *CREDENTIALS_PATH = "res://.godot/asset_bundle_credentials.cfg";

	ItemList *manifest_list = nullptr;
	Button *add_button = nullptr;
	Button *delete_button = nullptr;
	LineEdit *name_edit = nullptr;
	LineEdit *version_edit = nullptr;
	LineEdit *output_path_edit = nullptr;
	Button *output_path_browse_button = nullptr;
	EditorFileDialog *output_path_dialog = nullptr;
	CheckBox *encryption_enabled_check = nullptr;
	LineEdit *encryption_key_edit = nullptr;
	Button *encryption_key_visibility_button = nullptr;
	Label *info_label = nullptr;
	Tree *resource_tree = nullptr;
	Button *export_button = nullptr;

	Vector<ManifestInfo> manifests;
	int current_manifest = -1;
	bool updating = false;

	static String _sanitize_name(const String &p_name);
	static Error _write_text_file(const String &p_path, const String &p_text);
	static Error _remove_recursive(const String &p_path);

	void _load_config();
	void _save_config();
	void _update_manifest_list();
	void _edit_manifest(int p_index);
	void _store_current_manifest();
	void _fill_resource_tree();
	bool _fill_resource_tree_dir(EditorFileSystemDirectory *p_dir, TreeItem *p_parent, const HashSet<String> &p_selected);
	void _collect_checked_resources(TreeItem *p_item, PackedStringArray &r_resources) const;
	void _add_manifest();
	void _delete_manifest();
	void _manifest_selected(int p_index);
	void _manifest_name_changed(const String &p_text);
	void _manifest_version_changed(const String &p_text);
	void _manifest_output_changed(const String &p_text);
	void _manifest_encryption_enabled_toggled(bool p_pressed);
	void _manifest_encryption_key_changed(const String &p_text);
	void _manifest_encryption_key_visibility_toggled(bool p_pressed);
	void _browse_output_path();
	void _output_path_selected(const String &p_path);
	void _resource_tree_edited();
	void _export_current_manifest();
	Error _export_manifest(const ManifestInfo &p_manifest);

protected:
	static void _bind_methods();
	void _notification(int p_what);

public:
	void popup_manager();

	AssetBundleManagerDialog();
};
