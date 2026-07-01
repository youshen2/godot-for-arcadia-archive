/**************************************************************************/
/*  asset_bundle.h                                                        */
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

#include "core/io/resource.h"
#include "core/object/ref_counted.h"
#include "core/templates/hash_map.h"
#include "core/templates/hash_set.h"
#include "core/variant/array.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

class AssetBundle : public RefCounted {
	GDCLASS(AssetBundle, RefCounted);

public:
	enum CacheMode {
		CACHE_MODE_IGNORE,
		CACHE_MODE_REUSE,
		CACHE_MODE_REPLACE,
		CACHE_MODE_IGNORE_DEEP,
		CACHE_MODE_REPLACE_DEEP,
	};

private:
	struct ResourceEntry {
		String path;
		String type;
		String chunk;
		String hash;
		String md5;
		int64_t size = 0;
		int64_t offset = 0;
		bool encrypted = false;
		bool file_only = false;
		int64_t packed_size = 0;
		String packed_hash;
	};

	struct BundleInfo {
		String name;
		String path;
		String resolved_path;
		String version;
		String hash;
		int64_t size = 0;
		int offset = 0;
		PackedStringArray dependencies;
		Vector<ResourceEntry> resources;
		Vector<ResourceEntry> hot_replace_resources;
	};

	String manifest_path;
	String manifest_base_dir;
	String manifest_version;
	HashMap<String, BundleInfo> bundles;
	Vector<String> bundle_order;
	HashSet<String> loaded_bundles;

	bool loading = false;
	bool load_hot_replace_cached = true;
	bool load_replace_files = true;
	Vector<String> pending_bundles;
	Vector<ResourceEntry> pending_resources;
	int pending_bundle_index = 0;
	int pending_resource_index = 0;
	int processed_steps = 0;
	int total_steps = 0;
	double load_progress = 0.0;
	String loading_bundle;
	Error last_error_code = OK;
	String last_error;

	static bool _variant_is_string_like(const Variant &p_value);
	static String _variant_to_string(const Variant &p_value);
	static String _get_dictionary_string(const Dictionary &p_dictionary, const String &p_key, const String &p_default = String());
	static int _get_dictionary_int(const Dictionary &p_dictionary, const String &p_key, int p_default = 0);
	static int64_t _get_dictionary_int64(const Dictionary &p_dictionary, const String &p_key, int64_t p_default = 0);
	static bool _get_dictionary_bool(const Dictionary &p_dictionary, const String &p_key, bool p_default = false);
	static PackedStringArray _get_dictionary_string_array(const Dictionary &p_dictionary, const String &p_key);
	static Dictionary _read_manifest_dictionary(const String &p_manifest_path, Error &r_error, String &r_error_message);
	static Variant _canonicalize_manifest_value(const Variant &p_value);
	static Vector<String> _dictionary_string_keys_sorted(const Dictionary &p_dictionary);
	static Dictionary _manifest_bundles_to_map(const Dictionary &p_manifest);
	static Dictionary _bundle_chunks_to_map(const Dictionary &p_bundle);
	static String _normalize_portable_path(const String &p_path);

	Error _set_error(Error p_error, const String &p_message);
	Error _parse_manifest_dictionary(const Dictionary &p_manifest);
	Error _parse_bundle_dictionary(const Dictionary &p_bundle, const String &p_fallback_name, BundleInfo &r_bundle);
	Error _parse_bundle_resources(const Variant &p_resources, BundleInfo &r_bundle);
	Error _parse_bundle_resource_dictionary(const Dictionary &p_resource, const String &p_inherited_chunk, bool p_inherited_encrypted, BundleInfo &r_bundle);
	String _resolve_bundle_path(const String &p_path) const;
	String _normalize_resource_path(const String &p_path) const;
	String _get_bundle_base_dir(const BundleInfo &p_bundle) const;
	String _get_bundle_manifest_path(const BundleInfo &p_bundle) const;
	String _get_chunk_file_path(const BundleInfo &p_bundle, const ResourceEntry &p_entry) const;
	Dictionary _resource_entry_to_dictionary(const BundleInfo &p_bundle, const ResourceEntry &p_entry, bool p_include_file_path) const;
	Dictionary _bundle_to_dictionary(const BundleInfo &p_bundle) const;
	Dictionary _verify_resource_entry(const BundleInfo &p_bundle, const ResourceEntry &p_entry, bool p_verify_hash, bool p_verify_md5) const;

	Error _append_bundle_with_dependencies(const String &p_bundle_name, HashSet<String> &r_visiting, HashSet<String> &r_visited, Vector<String> &r_order);
	Error _setup_load_request(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached, bool p_replace_files);
	void _update_progress(const String &p_bundle_name);
	Error _finish_load_request();
	Error _fail_load_request(Error p_error, const String &p_bundle_name, const String &p_message);
	Error _poll_mount_bundle();
	Error _poll_replace_resource();

protected:
	static void _bind_methods();

public:
	Error load_manifest(const String &p_manifest_path);
	void clear();

	bool has_manifest() const;
	String get_manifest_path() const;
	String get_manifest_version() const;
	PackedStringArray get_bundles() const;
	int get_bundle_count() const;
	bool has_bundle(const String &p_bundle_name) const;
	Dictionary get_bundle_info(const String &p_bundle_name) const;
	String get_bundle_version(const String &p_bundle_name) const;
	String get_bundle_path(const String &p_bundle_name) const;
	PackedStringArray get_bundle_dependencies(const String &p_bundle_name) const;
	PackedStringArray get_bundle_resources(const String &p_bundle_name) const;
	String get_bundle_hash(const String &p_bundle_name) const;
	int64_t get_bundle_size(const String &p_bundle_name) const;
	String get_bundle_manifest_path(const String &p_bundle_name) const;
	Array get_bundle_chunks(const String &p_bundle_name, bool p_include_file_paths = false) const;
	PackedStringArray get_bundle_chunk_file_paths(const String &p_bundle_name) const;
	Dictionary get_chunk_info(const String &p_bundle_name, const String &p_resource_path, bool p_include_file_path = false) const;
	String get_chunk_file_path(const String &p_bundle_name, const String &p_resource_path) const;
	bool has_bundle_resource(const String &p_bundle_name, const String &p_resource_path) const;
	PackedStringArray get_resource_bundles(const String &p_resource_path) const;
	PackedStringArray get_missing_chunks(const String &p_bundle_name) const;
	bool is_bundle_available(const String &p_bundle_name) const;
	Dictionary get_manifest_info() const;
	Dictionary get_manifest_diff(const String &p_other_manifest_path) const;
	Dictionary verify_bundle(const String &p_bundle_name, bool p_verify_hash = true, bool p_verify_md5 = false) const;
	Dictionary verify_all_bundles(bool p_verify_hash = true, bool p_verify_md5 = false) const;

	Error start_load_bundle(const String &p_bundle_name, bool p_hot_replace_cached = true, bool p_replace_files = true);
	Error start_load_bundles(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached = true, bool p_replace_files = true);
	Error start_load_all_bundles(bool p_hot_replace_cached = true, bool p_replace_files = true);
	Error poll_load();
	Error load_bundle(const String &p_bundle_name, bool p_hot_replace_cached = true, bool p_replace_files = true);
	Error load_bundles(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached = true, bool p_replace_files = true);
	Error load_all_bundles(bool p_hot_replace_cached = true, bool p_replace_files = true);

	bool is_loading() const;
	double get_load_progress() const;
	String get_loading_bundle() const;
	PackedStringArray get_loaded_bundles() const;
	bool is_bundle_loaded(const String &p_bundle_name) const;
	Error get_last_error_code() const;
	String get_last_error() const;

	Ref<Resource> load_resource(const String &p_path, const String &p_type_hint = "", CacheMode p_cache_mode = CACHE_MODE_REUSE);
};

VARIANT_ENUM_CAST(AssetBundle::CacheMode);
