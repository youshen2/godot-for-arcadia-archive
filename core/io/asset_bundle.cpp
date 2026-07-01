/**************************************************************************/
/*  asset_bundle.cpp                                                      */
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

#include "asset_bundle.h"

#include "core/config/project_settings.h"
#include "core/crypto/hash_calculator.h"
#include "core/crypto/hashing_context.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/json.h"
#include "core/io/resource_loader.h"
#include "core/math/math_funcs.h"
#include "core/object/class_db.h"

static String _asset_bundle_hash_file_access_hex(Ref<FileAccess> p_file, HashingContext::HashType p_hash_type, Error &r_error) {
	r_error = OK;
	ERR_FAIL_COND_V(p_file.is_null() || !p_file->is_open(), String());

	const uint64_t file_size = p_file->get_length();
	if (file_size == 0) {
		PackedByteArray empty;
		return HashCalculator::hash_bytes_hex(p_hash_type, empty);
	}

	Ref<HashingContext> hashing;
	hashing.instantiate();
	ERR_FAIL_COND_V(hashing.is_null(), String());
	r_error = hashing->start(p_hash_type);
	ERR_FAIL_COND_V(r_error != OK, String());

	PackedByteArray buffer;
	uint64_t processed_size = 0;
	while (processed_size < file_size) {
		const uint64_t remaining = file_size - processed_size;
		const int64_t to_read = int64_t(MIN(remaining, uint64_t(HashCalculator::DEFAULT_CHUNK_SIZE)));
		buffer.resize(int(to_read));
		const uint64_t bytes_read = p_file->get_buffer(buffer.ptrw(), to_read);
		if (bytes_read != uint64_t(to_read)) {
			r_error = p_file->get_error();
			if (r_error == OK || r_error == ERR_FILE_EOF) {
				r_error = ERR_FILE_CANT_READ;
			}
			return String();
		}

		r_error = hashing->update(buffer);
		if (r_error != OK) {
			return String();
		}
		processed_size += bytes_read;
	}

	PackedByteArray hash = hashing->finish();
	ERR_FAIL_COND_V(hash.is_empty(), String());
	return String::hex_encode_buffer(hash.ptr(), hash.size());
}

bool AssetBundle::_variant_is_string_like(const Variant &p_value) {
	return p_value.get_type() == Variant::STRING || p_value.get_type() == Variant::STRING_NAME || p_value.get_type() == Variant::NODE_PATH;
}

String AssetBundle::_variant_to_string(const Variant &p_value) {
	return _variant_is_string_like(p_value) ? String(p_value) : String();
}

String AssetBundle::_get_dictionary_string(const Dictionary &p_dictionary, const String &p_key, const String &p_default) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (!_variant_is_string_like(value)) {
		return p_default;
	}

	return String(value);
}

int AssetBundle::_get_dictionary_int(const Dictionary &p_dictionary, const String &p_key, int p_default) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) {
		return p_default;
	}

	return int(value);
}

int64_t AssetBundle::_get_dictionary_int64(const Dictionary &p_dictionary, const String &p_key, int64_t p_default) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) {
		return p_default;
	}

	return int64_t(value);
}

bool AssetBundle::_get_dictionary_bool(const Dictionary &p_dictionary, const String &p_key, bool p_default) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::BOOL) {
		return p_default;
	}

	return bool(value);
}

PackedStringArray AssetBundle::_get_dictionary_string_array(const Dictionary &p_dictionary, const String &p_key) {
	PackedStringArray result;
	if (!p_dictionary.has(p_key)) {
		return result;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() == Variant::PACKED_STRING_ARRAY) {
		return value;
	}

	if (value.get_type() != Variant::ARRAY) {
		return result;
	}

	Array array = value;
	for (int i = 0; i < array.size(); i++) {
		if (_variant_is_string_like(array[i])) {
			result.push_back(String(array[i]));
		}
	}

	return result;
}

Dictionary AssetBundle::_read_manifest_dictionary(const String &p_manifest_path, Error &r_error, String &r_error_message) {
	r_error = OK;
	r_error_message.clear();

	Error file_error = OK;
	String manifest_text = FileAccess::get_file_as_string(p_manifest_path, &file_error);
	if (file_error != OK) {
		r_error = file_error;
		r_error_message = vformat("AssetBundle failed to read manifest '%s'.", p_manifest_path);
		return Dictionary();
	}

	Variant parsed = JSON::parse_string(manifest_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		r_error = ERR_PARSE_ERROR;
		r_error_message = vformat("AssetBundle manifest '%s' is not a JSON object.", p_manifest_path);
		return Dictionary();
	}

	return parsed;
}

Variant AssetBundle::_canonicalize_manifest_value(const Variant &p_value) {
	if (p_value.get_type() == Variant::DICTIONARY) {
		Dictionary source = p_value;
		Dictionary result;
		LocalVector<Variant> keys = source.get_key_list();
		for (const Variant &key : keys) {
			result[key] = _canonicalize_manifest_value(source[key]);
		}
		return result;
	}

	if (p_value.get_type() == Variant::ARRAY) {
		Array source = p_value;
		Array result;
		for (int i = 0; i < source.size(); i++) {
			result.push_back(_canonicalize_manifest_value(source[i]));
		}
		return result;
	}

	if (p_value.get_type() == Variant::FLOAT) {
		const double value = double(p_value);
		const double rounded = Math::round(value);
		if (value == rounded && rounded >= double(INT64_MIN) && rounded <= double(INT64_MAX)) {
			return int64_t(rounded);
		}
	}

	return p_value;
}

String AssetBundle::_normalize_portable_path(const String &p_path) {
	return p_path.replace("\\", "/").simplify_path();
}

Vector<String> AssetBundle::_dictionary_string_keys_sorted(const Dictionary &p_dictionary) {
	Vector<String> result;
	LocalVector<Variant> keys = p_dictionary.get_key_list();
	for (const Variant &key : keys) {
		const String key_string = _variant_to_string(key);
		if (!key_string.is_empty()) {
			result.push_back(key_string);
		}
	}
	result.sort();
	return result;
}

Dictionary AssetBundle::_bundle_chunks_to_map(const Dictionary &p_bundle) {
	Dictionary result;
	Variant chunks_value;
	if (p_bundle.has("chunks")) {
		chunks_value = p_bundle["chunks"];
	} else if (p_bundle.has("resources")) {
		chunks_value = p_bundle["resources"];
	} else if (p_bundle.has("assets")) {
		chunks_value = p_bundle["assets"];
	}

	if (chunks_value.get_type() != Variant::ARRAY) {
		return result;
	}

	Array chunks = chunks_value;
	for (int i = 0; i < chunks.size(); i++) {
		if (chunks[i].get_type() != Variant::DICTIONARY) {
			continue;
		}

		Dictionary chunk = chunks[i];
		const String hash = _get_dictionary_string(chunk, "hash");
		const String packed_hash = _get_dictionary_string(chunk, "packed_hash");
		const String path = _normalize_portable_path(_get_dictionary_string(chunk, "path"));
		const String chunk_path = _normalize_portable_path(_get_dictionary_string(chunk, "chunk"));
		if (!path.is_empty() || !chunk_path.is_empty() || !hash.is_empty()) {
			const bool physical_chunk = !packed_hash.is_empty() || _get_dictionary_int64(chunk, "packed_size", 0) > 0 || (chunk.has("files") && chunk["files"].get_type() == Variant::ARRAY);
			const String chunk_key = physical_chunk ? (chunk_path + "::" + (!packed_hash.is_empty() ? packed_hash : hash)) : (path + "::" + chunk_path + "::" + hash);

			Dictionary normalized_chunk = chunk.duplicate(true);
			normalized_chunk["chunk_key"] = chunk_key;
			if (normalized_chunk.has("files") && normalized_chunk["files"].get_type() == Variant::ARRAY) {
				Array files = normalized_chunk["files"];
				normalized_chunk["file_count"] = files.size();
			}

			if (physical_chunk && result.has(chunk_key) && !(normalized_chunk.has("files") && normalized_chunk["files"].get_type() == Variant::ARRAY)) {
				Dictionary existing = result[chunk_key];
				Array files;
				if (existing.has("files") && existing["files"].get_type() == Variant::ARRAY) {
					files = existing["files"];
				} else {
					Dictionary existing_file = existing.duplicate(true);
					existing_file.erase("chunk_key");
					existing_file.erase("file_count");
					files.push_back(existing_file);
				}

				Dictionary file = normalized_chunk.duplicate(true);
				file.erase("chunk_key");
				file.erase("file_count");
				files.push_back(file);

				existing["files"] = files;
				existing["file_count"] = files.size();
				existing["chunk_key"] = chunk_key;
				if (_get_dictionary_int64(existing, "packed_size", 0) == 0 && _get_dictionary_int64(normalized_chunk, "packed_size", 0) > 0) {
					existing["packed_size"] = normalized_chunk["packed_size"];
				}
				if (_get_dictionary_string(existing, "packed_hash").is_empty() && !packed_hash.is_empty()) {
					existing["packed_hash"] = packed_hash;
				}
				result[chunk_key] = existing;
			} else {
				result[chunk_key] = normalized_chunk;
			}
		}
	}

	return result;
}

Dictionary AssetBundle::_manifest_bundles_to_map(const Dictionary &p_manifest) {
	Dictionary result;
	Variant bundles_value;
	if (p_manifest.has("bundles")) {
		bundles_value = p_manifest["bundles"];
	} else if (p_manifest.has("packages")) {
		bundles_value = p_manifest["packages"];
	}

	if (bundles_value.get_type() == Variant::ARRAY) {
		Array bundle_array = bundles_value;
		for (int i = 0; i < bundle_array.size(); i++) {
			if (bundle_array[i].get_type() != Variant::DICTIONARY) {
				continue;
			}
			Dictionary bundle = bundle_array[i];
			const String name = _get_dictionary_string(bundle, "name");
			if (!name.is_empty()) {
				result[name] = bundle;
			}
		}
	} else if (bundles_value.get_type() == Variant::DICTIONARY) {
		Dictionary bundle_dictionary = bundles_value;
		LocalVector<Variant> keys = bundle_dictionary.get_key_list();
		for (const Variant &key : keys) {
			String name = _variant_to_string(key);
			if (name.is_empty()) {
				continue;
			}

			Variant value = bundle_dictionary[key];
			Dictionary bundle;
			if (value.get_type() == Variant::DICTIONARY) {
				bundle = value;
				if (!bundle.has("name")) {
					bundle["name"] = name;
				}
			} else if (_variant_is_string_like(value)) {
				bundle["name"] = name;
				bundle["path"] = String(value);
			}
			if (!bundle.is_empty()) {
				result[name] = bundle;
			}
		}
	}

	return result;
}

Error AssetBundle::_set_error(Error p_error, const String &p_message) {
	last_error_code = p_error;
	last_error = p_message;
	if (!p_message.is_empty()) {
		ERR_PRINT(p_message);
	}
	return p_error;
}

Error AssetBundle::_parse_manifest_dictionary(const Dictionary &p_manifest) {
	manifest_version = _get_dictionary_string(p_manifest, "version");
	if (manifest_version.is_empty()) {
		manifest_version = _get_dictionary_string(p_manifest, "manifest_version");
	}

	Variant bundles_value;
	if (p_manifest.has("bundles")) {
		bundles_value = p_manifest["bundles"];
	} else if (p_manifest.has("packages")) {
		bundles_value = p_manifest["packages"];
	} else {
		return _set_error(ERR_INVALID_DATA, "AssetBundle manifest must contain a 'bundles' or 'packages' entry.");
	}

	if (bundles_value.get_type() == Variant::ARRAY) {
		Array bundle_array = bundles_value;
		for (int i = 0; i < bundle_array.size(); i++) {
			if (bundle_array[i].get_type() != Variant::DICTIONARY) {
				return _set_error(ERR_INVALID_DATA, "AssetBundle manifest bundle array entries must be dictionaries.");
			}

			BundleInfo bundle;
			Error err = _parse_bundle_dictionary(bundle_array[i], String(), bundle);
			if (err != OK) {
				return err;
			}

			if (bundles.has(bundle.name)) {
				return _set_error(ERR_ALREADY_EXISTS, vformat("AssetBundle manifest declares bundle '%s' more than once.", bundle.name));
			}

			bundle_order.push_back(bundle.name);
			bundles.insert(bundle.name, bundle);
		}
	} else if (bundles_value.get_type() == Variant::DICTIONARY) {
		Dictionary bundle_dictionary = bundles_value;
		LocalVector<Variant> keys = bundle_dictionary.get_key_list();
		for (const Variant &key : keys) {
			String bundle_name = _variant_to_string(key);
			if (bundle_name.is_empty()) {
				return _set_error(ERR_INVALID_DATA, "AssetBundle manifest bundle dictionary keys must be strings.");
			}

			BundleInfo bundle;
			Variant value = bundle_dictionary[key];
			if (value.get_type() == Variant::DICTIONARY) {
				Error err = _parse_bundle_dictionary(value, bundle_name, bundle);
				if (err != OK) {
					return err;
				}
			} else if (_variant_is_string_like(value)) {
				bundle.name = bundle_name;
				bundle.path = _normalize_portable_path(String(value));
				bundle.resolved_path = _resolve_bundle_path(bundle.path);
			} else {
				return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' must be a dictionary or path string.", bundle_name));
			}

			if (bundles.has(bundle.name)) {
				return _set_error(ERR_ALREADY_EXISTS, vformat("AssetBundle manifest declares bundle '%s' more than once.", bundle.name));
			}

			bundle_order.push_back(bundle.name);
			bundles.insert(bundle.name, bundle);
		}
	} else {
		return _set_error(ERR_INVALID_DATA, "AssetBundle manifest 'bundles' entry must be an array or dictionary.");
	}

	return OK;
}

Error AssetBundle::_parse_bundle_dictionary(const Dictionary &p_bundle, const String &p_fallback_name, BundleInfo &r_bundle) {
	r_bundle.name = _get_dictionary_string(p_bundle, "name", p_fallback_name);
	r_bundle.path = _normalize_portable_path(_get_dictionary_string(p_bundle, "path"));
	if (r_bundle.path.is_empty()) {
		r_bundle.path = _normalize_portable_path(_get_dictionary_string(p_bundle, "pack"));
	}

	if (r_bundle.name.is_empty()) {
		return _set_error(ERR_INVALID_DATA, "AssetBundle manifest bundle is missing a 'name' field.");
	}
	if (r_bundle.path.is_empty()) {
		return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' is missing a 'path' or 'pack' field.", r_bundle.name));
	}

	r_bundle.version = _get_dictionary_string(p_bundle, "version");
	r_bundle.hash = _get_dictionary_string(p_bundle, "hash");
	r_bundle.size = _get_dictionary_int64(p_bundle, "size", 0);
	r_bundle.offset = _get_dictionary_int(p_bundle, "offset", 0);
	r_bundle.dependencies = _get_dictionary_string_array(p_bundle, "dependencies");
	if (r_bundle.dependencies.is_empty()) {
		r_bundle.dependencies = _get_dictionary_string_array(p_bundle, "depends");
	}

	if (p_bundle.has("resources")) {
		Error err = _parse_bundle_resources(p_bundle["resources"], r_bundle);
		if (err != OK) {
			return err;
		}
	} else if (p_bundle.has("assets")) {
		Error err = _parse_bundle_resources(p_bundle["assets"], r_bundle);
		if (err != OK) {
			return err;
		}
	} else if (p_bundle.has("chunks")) {
		Error err = _parse_bundle_resources(p_bundle["chunks"], r_bundle);
		if (err != OK) {
			return err;
		}
	}

	r_bundle.resolved_path = _resolve_bundle_path(r_bundle.path);
	return OK;
}

Error AssetBundle::_parse_bundle_resources(const Variant &p_resources, BundleInfo &r_bundle) {
	if (p_resources.get_type() == Variant::PACKED_STRING_ARRAY) {
		PackedStringArray resources = p_resources;
		for (int i = 0; i < resources.size(); i++) {
			ResourceEntry entry;
			entry.path = _normalize_portable_path(resources[i]);
			r_bundle.resources.push_back(entry);
		}
		return OK;
	}

	if (p_resources.get_type() != Variant::ARRAY) {
		return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' resources must be an array.", r_bundle.name));
	}

	Array resources = p_resources;
	for (int i = 0; i < resources.size(); i++) {
		ResourceEntry entry;
		Variant value = resources[i];
		if (_variant_is_string_like(value)) {
			entry.path = _normalize_portable_path(String(value));
		} else if (value.get_type() == Variant::DICTIONARY) {
			Dictionary resource_dictionary = value;
			if (resource_dictionary.has("files") && resource_dictionary["files"].get_type() == Variant::ARRAY) {
				ResourceEntry hot_replace_entry;
				hot_replace_entry.path = _normalize_portable_path(_get_dictionary_string(resource_dictionary, "path"));
				if (!hot_replace_entry.path.is_empty()) {
					hot_replace_entry.type = _get_dictionary_string(resource_dictionary, "type");
					r_bundle.hot_replace_resources.push_back(hot_replace_entry);
				}

				const String inherited_chunk = _normalize_portable_path(_get_dictionary_string(resource_dictionary, "chunk"));
				const bool inherited_encrypted = _get_dictionary_bool(resource_dictionary, "encrypted", false);
				Array files = resource_dictionary["files"];
				for (int j = 0; j < files.size(); j++) {
					if (files[j].get_type() != Variant::DICTIONARY) {
						return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' grouped chunk files must be dictionaries.", r_bundle.name));
					}
					Error err = _parse_bundle_resource_dictionary(files[j], inherited_chunk, inherited_encrypted, r_bundle);
					if (err != OK) {
						return err;
					}
				}
				continue;
			} else {
				Error err = _parse_bundle_resource_dictionary(resource_dictionary, String(), false, r_bundle);
				if (err != OK) {
					return err;
				}
				continue;
			}
		} else {
			return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' has a resource entry that is not a path string or dictionary.", r_bundle.name));
		}

		if (entry.path.is_empty()) {
			return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' has a resource entry without a path.", r_bundle.name));
		}

		r_bundle.resources.push_back(entry);
	}

	return OK;
}

Error AssetBundle::_parse_bundle_resource_dictionary(const Dictionary &p_resource, const String &p_inherited_chunk, bool p_inherited_encrypted, BundleInfo &r_bundle) {
	ResourceEntry entry;
	entry.path = _normalize_portable_path(_get_dictionary_string(p_resource, "path"));
	if (entry.path.is_empty()) {
		entry.path = _normalize_portable_path(_get_dictionary_string(p_resource, "resource"));
	}
	entry.type = _get_dictionary_string(p_resource, "type");
	if (entry.type.is_empty()) {
		entry.type = _get_dictionary_string(p_resource, "type_hint");
	}
	entry.chunk = _normalize_portable_path(_get_dictionary_string(p_resource, "chunk", p_inherited_chunk));
	entry.hash = _get_dictionary_string(p_resource, "hash");
	entry.md5 = _get_dictionary_string(p_resource, "md5");
	entry.size = _get_dictionary_int64(p_resource, "size", 0);
	entry.offset = _get_dictionary_int64(p_resource, "offset", 0);
	entry.encrypted = _get_dictionary_bool(p_resource, "encrypted", p_inherited_encrypted);
	entry.file_only = _get_dictionary_bool(p_resource, "file_only", false);
	entry.packed_size = _get_dictionary_int64(p_resource, "packed_size", 0);
	entry.packed_hash = _get_dictionary_string(p_resource, "packed_hash");

	if (entry.path.is_empty()) {
		return _set_error(ERR_INVALID_DATA, vformat("AssetBundle manifest bundle '%s' has a resource entry without a path.", r_bundle.name));
	}

	r_bundle.resources.push_back(entry);
	return OK;
}

String AssetBundle::_resolve_bundle_path(const String &p_path) const {
	const String path = _normalize_portable_path(p_path);
	if (path.contains("://") || path.is_absolute_path() || manifest_base_dir.is_empty()) {
		return path;
	}

	return manifest_base_dir.path_join(path);
}

String AssetBundle::_normalize_resource_path(const String &p_path) const {
	return _normalize_portable_path(ProjectSettings::get_singleton()->localize_path(p_path));
}

String AssetBundle::_get_bundle_base_dir(const BundleInfo &p_bundle) const {
	if (p_bundle.resolved_path.ends_with("bundle.json")) {
		return p_bundle.resolved_path.get_base_dir();
	}
	return p_bundle.resolved_path;
}

String AssetBundle::_get_bundle_manifest_path(const BundleInfo &p_bundle) const {
	if (p_bundle.resolved_path.ends_with("bundle.json")) {
		return p_bundle.resolved_path;
	}
	return p_bundle.resolved_path.path_join("bundle.json");
}

String AssetBundle::_get_chunk_file_path(const BundleInfo &p_bundle, const ResourceEntry &p_entry) const {
	if (p_entry.chunk.is_empty()) {
		return String();
	}

	const String chunk_path = _normalize_portable_path(p_entry.chunk);
	if (chunk_path.contains("://") || chunk_path.is_absolute_path()) {
		return chunk_path;
	}

	return _get_bundle_base_dir(p_bundle).path_join(chunk_path);
}

Dictionary AssetBundle::_resource_entry_to_dictionary(const BundleInfo &p_bundle, const ResourceEntry &p_entry, bool p_include_file_path) const {
	Dictionary resource;
	resource["path"] = p_entry.path;
	resource["type"] = p_entry.type;
	resource["chunk"] = p_entry.chunk;
	resource["hash"] = p_entry.hash;
	resource["md5"] = p_entry.md5;
	resource["size"] = p_entry.size;
	resource["offset"] = p_entry.offset;
	resource["encrypted"] = p_entry.encrypted;
	resource["file_only"] = p_entry.file_only;
	resource["packed_size"] = p_entry.packed_size;
	resource["packed_hash"] = p_entry.packed_hash;
	if (p_include_file_path) {
		resource["file_path"] = _get_chunk_file_path(p_bundle, p_entry);
	}
	return resource;
}

Dictionary AssetBundle::_bundle_to_dictionary(const BundleInfo &p_bundle) const {
	Dictionary result;
	result["name"] = p_bundle.name;
	result["path"] = p_bundle.path;
	result["resolved_path"] = p_bundle.resolved_path;
	result["version"] = p_bundle.version;
	result["hash"] = p_bundle.hash;
	result["size"] = p_bundle.size;
	result["offset"] = p_bundle.offset;
	result["dependencies"] = p_bundle.dependencies;
	result["loaded"] = loaded_bundles.has(p_bundle.name);

	Array resources;
	for (const ResourceEntry &entry : p_bundle.resources) {
		resources.push_back(_resource_entry_to_dictionary(p_bundle, entry, false));
	}
	result["resources"] = resources;
	result["chunks"] = resources;

	PackedStringArray hot_replace_resources;
	for (const ResourceEntry &entry : p_bundle.hot_replace_resources) {
		hot_replace_resources.push_back(entry.path);
	}
	result["hot_replace_resources"] = hot_replace_resources;

	return result;
}

Dictionary AssetBundle::_verify_resource_entry(const BundleInfo &p_bundle, const ResourceEntry &p_entry, bool p_verify_hash, bool p_verify_md5) const {
	Dictionary result = _resource_entry_to_dictionary(p_bundle, p_entry, true);
	const String file_path = result["file_path"];
	result["valid"] = false;
	result["error"] = OK;
	result["error_message"] = String();
	result["actual_size"] = int64_t(0);
	result["actual_hash"] = String();
	result["actual_md5"] = String();

	if (file_path.is_empty()) {
		result["error"] = ERR_INVALID_DATA;
		result["error_message"] = vformat("Resource '%s' does not declare a chunk path.", p_entry.path);
		return result;
	}

	if (!FileAccess::exists(file_path)) {
		result["error"] = ERR_FILE_NOT_FOUND;
		result["error_message"] = vformat("Chunk file '%s' does not exist.", file_path);
		return result;
	}

	PackedData::PackedFile packed_file;
	packed_file.pack = file_path;
	packed_file.offset = p_entry.offset;
	packed_file.size = p_entry.size;
	for (int i = 0; i < 16; i++) {
		packed_file.md5[i] = 0;
	}
	packed_file.src = nullptr;
	packed_file.encrypted = p_entry.encrypted;
	packed_file.bundle = false;
	packed_file.delta = false;
	packed_file.skip_pack = true;

	Ref<FileAccess> file(memnew(FileAccessPack(p_entry.path, packed_file)));
	if (file.is_null() || !file->is_open()) {
		result["error"] = p_entry.encrypted ? ERR_FILE_CORRUPT : ERR_FILE_CANT_READ;
		result["error_message"] = vformat("Could not open chunk '%s' for resource '%s'.", file_path, p_entry.path);
		return result;
	}

	const int64_t actual_size = file->get_length();
	result["actual_size"] = actual_size;
	if (p_entry.size > 0 && actual_size != p_entry.size) {
		result["error"] = ERR_FILE_CORRUPT;
		result["error_message"] = vformat("Chunk '%s' size mismatch. Expected %d, got %d.", p_entry.chunk, p_entry.size, actual_size);
		return result;
	}

	if (p_verify_hash && !p_entry.hash.is_empty()) {
		Error hash_error = OK;
		file->seek(0);
		const String actual_hash = _asset_bundle_hash_file_access_hex(file, HashingContext::HASH_SHA256, hash_error);
		result["actual_hash"] = actual_hash;
		if (hash_error != OK || actual_hash.is_empty()) {
			result["error"] = hash_error != OK ? hash_error : ERR_FILE_CANT_READ;
			result["error_message"] = vformat("Could not calculate SHA-256 for chunk '%s'.", file_path);
			return result;
		}
		if (actual_hash.to_lower() != p_entry.hash.to_lower()) {
			result["error"] = ERR_FILE_CORRUPT;
			result["error_message"] = vformat("Chunk '%s' SHA-256 mismatch.", p_entry.chunk);
			return result;
		}
	}

	if (p_verify_md5 && !p_entry.md5.is_empty()) {
		Error hash_error = OK;
		file->seek(0);
		const String actual_md5 = _asset_bundle_hash_file_access_hex(file, HashingContext::HASH_MD5, hash_error);
		result["actual_md5"] = actual_md5;
		if (hash_error != OK || actual_md5.is_empty()) {
			result["error"] = hash_error != OK ? hash_error : ERR_FILE_CANT_READ;
			result["error_message"] = vformat("Could not calculate MD5 for chunk '%s'.", file_path);
			return result;
		}
		if (actual_md5.to_lower() != p_entry.md5.to_lower()) {
			result["error"] = ERR_FILE_CORRUPT;
			result["error_message"] = vformat("Chunk '%s' MD5 mismatch.", p_entry.chunk);
			return result;
		}
	}

	result["valid"] = true;
	return result;
}

Error AssetBundle::_append_bundle_with_dependencies(const String &p_bundle_name, HashSet<String> &r_visiting, HashSet<String> &r_visited, Vector<String> &r_order) {
	if (!bundles.has(p_bundle_name)) {
		return _set_error(ERR_DOES_NOT_EXIST, vformat("AssetBundle manifest does not contain bundle '%s'.", p_bundle_name));
	}
	if (r_visited.has(p_bundle_name)) {
		return OK;
	}
	if (r_visiting.has(p_bundle_name)) {
		return _set_error(ERR_CYCLIC_LINK, vformat("AssetBundle manifest has a cyclic dependency involving bundle '%s'.", p_bundle_name));
	}

	r_visiting.insert(p_bundle_name);
	const BundleInfo &bundle = bundles[p_bundle_name];
	for (int i = 0; i < bundle.dependencies.size(); i++) {
		Error err = _append_bundle_with_dependencies(bundle.dependencies[i], r_visiting, r_visited, r_order);
		if (err != OK) {
			return err;
		}
	}
	r_visiting.erase(p_bundle_name);

	r_visited.insert(p_bundle_name);
	r_order.push_back(p_bundle_name);
	return OK;
}

Error AssetBundle::_setup_load_request(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached, bool p_replace_files) {
	if (loading) {
		return _set_error(ERR_BUSY, "AssetBundle is already loading bundles.");
	}
	if (bundles.is_empty()) {
		return _set_error(ERR_UNCONFIGURED, "AssetBundle has no loaded manifest.");
	}
	if (p_bundle_names.is_empty()) {
		return _set_error(ERR_INVALID_PARAMETER, "AssetBundle load request must contain at least one bundle.");
	}

	HashSet<String> visiting;
	HashSet<String> visited;
	Vector<String> load_order;
	for (int i = 0; i < p_bundle_names.size(); i++) {
		Error err = _append_bundle_with_dependencies(p_bundle_names[i], visiting, visited, load_order);
		if (err != OK) {
			return err;
		}
	}

	pending_bundles = load_order;
	pending_resources.clear();
	for (const String &bundle_name : pending_bundles) {
		const BundleInfo &bundle = bundles[bundle_name];
		for (const ResourceEntry &entry : bundle.resources) {
			if (!entry.file_only) {
				pending_resources.push_back(entry);
			}
		}
		for (const ResourceEntry &entry : bundle.hot_replace_resources) {
			pending_resources.push_back(entry);
		}
	}

	load_hot_replace_cached = p_hot_replace_cached;
	load_replace_files = p_replace_files;
	pending_bundle_index = 0;
	pending_resource_index = 0;
	processed_steps = 0;
	total_steps = pending_bundles.size();
	if (load_hot_replace_cached) {
		total_steps += pending_resources.size();
	}
	load_progress = 0.0;
	loading_bundle = String();
	last_error_code = OK;
	last_error.clear();
	loading = true;
	_update_progress(String());

	return OK;
}

void AssetBundle::_update_progress(const String &p_bundle_name) {
	if (total_steps <= 0) {
		load_progress = loading ? 0.0 : 1.0;
	} else {
		load_progress = CLAMP(double(processed_steps) / double(total_steps), 0.0, 1.0);
	}

	emit_signal("load_progressed", p_bundle_name, load_progress);
}

Error AssetBundle::_finish_load_request() {
	loading = false;
	loading_bundle.clear();
	load_progress = 1.0;
	last_error_code = OK;
	last_error.clear();
	emit_signal("load_progressed", String(), load_progress);
	emit_signal("load_finished");
	return OK;
}

Error AssetBundle::_fail_load_request(Error p_error, const String &p_bundle_name, const String &p_message) {
	loading = false;
	loading_bundle = p_bundle_name;
	_set_error(p_error, p_message);
	emit_signal("load_failed", p_bundle_name, p_error, p_message);
	return p_error;
}

Error AssetBundle::_poll_mount_bundle() {
	const String bundle_name = pending_bundles[pending_bundle_index];
	loading_bundle = bundle_name;
	const BundleInfo &bundle = bundles[bundle_name];

	if (!ProjectSettings::get_singleton()->load_resource_pack(bundle.resolved_path, load_replace_files, bundle.offset)) {
		return _fail_load_request(ERR_CANT_OPEN, bundle_name, vformat("AssetBundle failed to load bundle '%s' for bundle '%s'.", bundle.resolved_path, bundle_name));
	}

	loaded_bundles.insert(bundle_name);
	processed_steps++;
	pending_bundle_index++;
	emit_signal("bundle_loaded", bundle_name);
	_update_progress(bundle_name);
	return OK;
}

Error AssetBundle::_poll_replace_resource() {
	const ResourceEntry &entry = pending_resources[pending_resource_index];
	String resource_path = _normalize_resource_path(entry.path);
	loading_bundle.clear();

	if (ResourceCache::has(resource_path)) {
		Error load_error = OK;
		Ref<Resource> resource = ResourceLoader::load(resource_path, entry.type, ResourceLoader::CACHE_MODE_REPLACE, &load_error);
		if (load_error != OK || resource.is_null()) {
			return _fail_load_request(load_error != OK ? load_error : ERR_CANT_OPEN, String(), vformat("AssetBundle failed to hot replace cached resource '%s'.", resource_path));
		}
		emit_signal("resource_hot_replaced", resource_path);
	}

	processed_steps++;
	pending_resource_index++;
	_update_progress(String());
	return OK;
}

Error AssetBundle::load_manifest(const String &p_manifest_path) {
	if (loading) {
		return _set_error(ERR_BUSY, "AssetBundle cannot load a manifest while a bundle load request is running.");
	}

	Error file_error = OK;
	String manifest_text = FileAccess::get_file_as_string(p_manifest_path, &file_error);
	if (file_error != OK) {
		return _set_error(file_error, vformat("AssetBundle failed to read manifest '%s'.", p_manifest_path));
	}

	Variant parsed = JSON::parse_string(manifest_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return _set_error(ERR_PARSE_ERROR, vformat("AssetBundle manifest '%s' is not a JSON object.", p_manifest_path));
	}

	clear();
	manifest_path = p_manifest_path;
	manifest_base_dir = p_manifest_path.get_base_dir();

	Error err = _parse_manifest_dictionary(parsed);
	if (err != OK) {
		const Error error_code = last_error_code;
		const String error_message = last_error;
		clear();
		last_error_code = error_code;
		last_error = error_message;
		return err;
	}

	last_error_code = OK;
	last_error.clear();
	return OK;
}

void AssetBundle::clear() {
	manifest_path.clear();
	manifest_base_dir.clear();
	manifest_version.clear();
	bundles.clear();
	bundle_order.clear();
	loaded_bundles.clear();
	loading = false;
	pending_bundles.clear();
	pending_resources.clear();
	pending_bundle_index = 0;
	pending_resource_index = 0;
	processed_steps = 0;
	total_steps = 0;
	load_progress = 0.0;
	loading_bundle.clear();
	last_error_code = OK;
	last_error.clear();
}

bool AssetBundle::has_manifest() const {
	return !manifest_path.is_empty();
}

String AssetBundle::get_manifest_path() const {
	return manifest_path;
}

String AssetBundle::get_manifest_version() const {
	return manifest_version;
}

PackedStringArray AssetBundle::get_bundles() const {
	PackedStringArray result;
	for (const String &bundle_name : bundle_order) {
		result.push_back(bundle_name);
	}
	return result;
}

int AssetBundle::get_bundle_count() const {
	return bundle_order.size();
}

bool AssetBundle::has_bundle(const String &p_bundle_name) const {
	return bundles.has(p_bundle_name);
}

Dictionary AssetBundle::get_bundle_info(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return Dictionary();
	}

	return _bundle_to_dictionary(*bundle);
}

String AssetBundle::get_bundle_version(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? bundle->version : String();
}

String AssetBundle::get_bundle_path(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? bundle->resolved_path : String();
}

PackedStringArray AssetBundle::get_bundle_dependencies(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? bundle->dependencies : PackedStringArray();
}

PackedStringArray AssetBundle::get_bundle_resources(const String &p_bundle_name) const {
	PackedStringArray result;
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return result;
	}

	for (const ResourceEntry &entry : bundle->resources) {
		result.push_back(entry.path);
	}
	for (const ResourceEntry &entry : bundle->hot_replace_resources) {
		result.push_back(entry.path);
	}
	return result;
}

String AssetBundle::get_bundle_hash(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? bundle->hash : String();
}

int64_t AssetBundle::get_bundle_size(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? bundle->size : 0;
}

String AssetBundle::get_bundle_manifest_path(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	return bundle != nullptr ? _get_bundle_manifest_path(*bundle) : String();
}

Array AssetBundle::get_bundle_chunks(const String &p_bundle_name, bool p_include_file_paths) const {
	Array result;
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return result;
	}

	for (const ResourceEntry &entry : bundle->resources) {
		result.push_back(_resource_entry_to_dictionary(*bundle, entry, p_include_file_paths));
	}
	return result;
}

PackedStringArray AssetBundle::get_bundle_chunk_file_paths(const String &p_bundle_name) const {
	PackedStringArray result;
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return result;
	}

	HashSet<String> seen;
	for (const ResourceEntry &entry : bundle->resources) {
		const String file_path = _get_chunk_file_path(*bundle, entry);
		if (!file_path.is_empty() && !seen.has(file_path)) {
			result.push_back(file_path);
			seen.insert(file_path);
		}
	}
	return result;
}

Dictionary AssetBundle::get_chunk_info(const String &p_bundle_name, const String &p_resource_path, bool p_include_file_path) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return Dictionary();
	}

	const String resource_path = _normalize_resource_path(p_resource_path);
	for (const ResourceEntry &entry : bundle->resources) {
		if (_normalize_resource_path(entry.path) == resource_path) {
			return _resource_entry_to_dictionary(*bundle, entry, p_include_file_path);
		}
	}
	return Dictionary();
}

String AssetBundle::get_chunk_file_path(const String &p_bundle_name, const String &p_resource_path) const {
	Dictionary chunk = get_chunk_info(p_bundle_name, p_resource_path, true);
	return chunk.has("file_path") ? String(chunk["file_path"]) : String();
}

bool AssetBundle::has_bundle_resource(const String &p_bundle_name, const String &p_resource_path) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return false;
	}

	const String resource_path = _normalize_resource_path(p_resource_path);
	for (const ResourceEntry &entry : bundle->resources) {
		if (_normalize_resource_path(entry.path) == resource_path) {
			return true;
		}
	}
	for (const ResourceEntry &entry : bundle->hot_replace_resources) {
		if (_normalize_resource_path(entry.path) == resource_path) {
			return true;
		}
	}
	return false;
}

PackedStringArray AssetBundle::get_resource_bundles(const String &p_resource_path) const {
	PackedStringArray result;
	const String resource_path = _normalize_resource_path(p_resource_path);
	for (const String &bundle_name : bundle_order) {
		const BundleInfo *bundle = bundles.getptr(bundle_name);
		if (bundle == nullptr) {
			continue;
		}

		bool found = false;
		for (const ResourceEntry &entry : bundle->resources) {
			if (_normalize_resource_path(entry.path) == resource_path) {
				found = true;
				break;
			}
		}
		if (!found) {
			for (const ResourceEntry &entry : bundle->hot_replace_resources) {
				if (_normalize_resource_path(entry.path) == resource_path) {
					found = true;
					break;
				}
			}
		}
		if (found) {
			result.push_back(bundle_name);
		}
	}
	return result;
}

PackedStringArray AssetBundle::get_missing_chunks(const String &p_bundle_name) const {
	PackedStringArray result;
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return result;
	}

	HashSet<String> seen;
	for (const ResourceEntry &entry : bundle->resources) {
		const String file_path = _get_chunk_file_path(*bundle, entry);
		if (!file_path.is_empty() && !seen.has(file_path) && !FileAccess::exists(file_path)) {
			result.push_back(file_path);
		}
		seen.insert(file_path);
	}
	return result;
}

bool AssetBundle::is_bundle_available(const String &p_bundle_name) const {
	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		return false;
	}

	if (!FileAccess::exists(_get_bundle_manifest_path(*bundle))) {
		return false;
	}

	for (const ResourceEntry &entry : bundle->resources) {
		const String file_path = _get_chunk_file_path(*bundle, entry);
		if (!file_path.is_empty() && !FileAccess::exists(file_path)) {
			return false;
		}
	}
	return true;
}

Dictionary AssetBundle::get_manifest_info() const {
	Dictionary result;
	result["path"] = manifest_path;
	result["base_dir"] = manifest_base_dir;
	result["version"] = manifest_version;
	result["bundle_count"] = bundle_order.size();
	result["loaded_bundle_count"] = loaded_bundles.size();
	result["bundles"] = get_bundles();

	int64_t total_size = 0;
	int resource_count = 0;
	for (const String &bundle_name : bundle_order) {
		const BundleInfo *bundle = bundles.getptr(bundle_name);
		if (bundle == nullptr) {
			continue;
		}
		total_size += bundle->size;
		resource_count += bundle->resources.size();
	}
	result["resource_count"] = resource_count;
	result["total_size"] = total_size;
	return result;
}

Dictionary AssetBundle::get_manifest_diff(const String &p_other_manifest_path) const {
	Dictionary result;
	PackedStringArray added_bundles;
	PackedStringArray updated_bundles;
	PackedStringArray removed_bundles;
	PackedStringArray unchanged_bundles;
	Array download_bundles;
	Array download_chunks;
	Array removed_chunks;
	int64_t total_download_size = 0;

	Error err = OK;
	String error_message;
	Dictionary other_manifest = _read_manifest_dictionary(p_other_manifest_path, err, error_message);
	if (err != OK) {
		result["error"] = err;
		result["error_message"] = error_message;
		return result;
	}

	Dictionary local_bundles;
	for (const String &bundle_name : bundle_order) {
		const BundleInfo *bundle = bundles.getptr(bundle_name);
		if (bundle != nullptr) {
			local_bundles[bundle_name] = _bundle_to_dictionary(*bundle);
		}
	}

	Dictionary remote_bundles = _manifest_bundles_to_map(other_manifest);
	Vector<String> remote_names = _dictionary_string_keys_sorted(remote_bundles);
	HashSet<String> seen_remote_bundles;

	for (const String &remote_name : remote_names) {
		seen_remote_bundles.insert(remote_name);
		Dictionary remote_bundle = remote_bundles[remote_name];
		Dictionary remote_chunks = _bundle_chunks_to_map(remote_bundle);

		bool bundle_changed = false;
		if (!local_bundles.has(remote_name)) {
			added_bundles.push_back(remote_name);
			download_bundles.push_back(remote_bundle);
			bundle_changed = true;
		} else {
			Dictionary local_bundle = local_bundles[remote_name];
			const String local_hash = _get_dictionary_string(local_bundle, "hash");
			const String remote_hash = _get_dictionary_string(remote_bundle, "hash");
			if (!local_hash.is_empty() && !remote_hash.is_empty()) {
				bundle_changed = local_hash != remote_hash;
			} else {
				Dictionary local_chunks = _bundle_chunks_to_map(local_bundle);
				bundle_changed = local_chunks.size() != remote_chunks.size();
				if (!bundle_changed) {
					Vector<String> remote_chunk_keys = _dictionary_string_keys_sorted(remote_chunks);
					for (const String &remote_chunk_key : remote_chunk_keys) {
						if (!local_chunks.has(remote_chunk_key)) {
							bundle_changed = true;
							break;
						}
					}
				}
			}

			if (bundle_changed) {
				updated_bundles.push_back(remote_name);
				download_bundles.push_back(remote_bundle);
			} else {
				unchanged_bundles.push_back(remote_name);
			}
		}

		Dictionary local_chunks;
		if (local_bundles.has(remote_name)) {
			Dictionary local_bundle = local_bundles[remote_name];
			local_chunks = _bundle_chunks_to_map(local_bundle);
		}

		Vector<String> remote_chunk_keys = _dictionary_string_keys_sorted(remote_chunks);
		for (const String &remote_chunk_key : remote_chunk_keys) {
			if (local_chunks.has(remote_chunk_key)) {
				continue;
			}

			Dictionary chunk = remote_chunks[remote_chunk_key];
			chunk["bundle"] = remote_name;
			download_chunks.push_back(chunk);
			total_download_size += _get_dictionary_int64(chunk, "packed_size", _get_dictionary_int64(chunk, "size", 0));
		}

		Vector<String> local_chunk_keys = _dictionary_string_keys_sorted(local_chunks);
		for (const String &local_chunk_key : local_chunk_keys) {
			if (remote_chunks.has(local_chunk_key)) {
				continue;
			}

			Dictionary chunk = local_chunks[local_chunk_key];
			chunk["bundle"] = remote_name;
			removed_chunks.push_back(chunk);
		}
	}

	Vector<String> local_names = _dictionary_string_keys_sorted(local_bundles);
	for (const String &local_name : local_names) {
		if (seen_remote_bundles.has(local_name)) {
			continue;
		}

		removed_bundles.push_back(local_name);
		Dictionary local_bundle = local_bundles[local_name];
		Dictionary local_chunks = _bundle_chunks_to_map(local_bundle);
		Vector<String> local_chunk_keys = _dictionary_string_keys_sorted(local_chunks);
		for (const String &local_chunk_key : local_chunk_keys) {
			Dictionary chunk = local_chunks[local_chunk_key];
			chunk["bundle"] = local_name;
			removed_chunks.push_back(chunk);
		}
	}

	result["error"] = OK;
	result["error_message"] = String();
	result["current_manifest_version"] = manifest_version;
	result["other_manifest_version"] = _get_dictionary_string(other_manifest, "version", _get_dictionary_string(other_manifest, "manifest_version"));
	result["added_bundles"] = added_bundles;
	result["updated_bundles"] = updated_bundles;
	result["removed_bundles"] = removed_bundles;
	result["unchanged_bundles"] = unchanged_bundles;
	result["download_bundles"] = download_bundles;
	result["download_chunks"] = download_chunks;
	result["removed_chunks"] = removed_chunks;
	result["total_download_size"] = total_download_size;
	return result;
}

Dictionary AssetBundle::verify_bundle(const String &p_bundle_name, bool p_verify_hash, bool p_verify_md5) const {
	Dictionary result;
	result["bundle"] = p_bundle_name;
	result["valid"] = false;
	result["error"] = OK;
	result["error_message"] = String();
	result["checked_chunks"] = 0;
	result["valid_chunks"] = 0;
	result["invalid_chunks"] = 0;
	result["missing_chunks"] = 0;
	result["total_size"] = int64_t(0);
	result["chunks"] = Array();

	const BundleInfo *bundle = bundles.getptr(p_bundle_name);
	if (bundle == nullptr) {
		result["error"] = ERR_DOES_NOT_EXIST;
		result["error_message"] = vformat("AssetBundle manifest does not contain bundle '%s'.", p_bundle_name);
		return result;
	}

	const String bundle_manifest_path = _get_bundle_manifest_path(*bundle);
	result["bundle_manifest_path"] = bundle_manifest_path;
	result["expected_bundle_hash"] = bundle->hash;
	result["actual_bundle_hash"] = String();
	result["bundle_hash_valid"] = bundle->hash.is_empty();
	if (!FileAccess::exists(bundle_manifest_path)) {
		result["error"] = ERR_FILE_NOT_FOUND;
		result["error_message"] = vformat("Bundle manifest '%s' does not exist.", bundle_manifest_path);
		return result;
	}

	bool bundle_manifest_valid = true;
	if (p_verify_hash && !bundle->hash.is_empty()) {
		Error file_error = OK;
		String manifest_text = FileAccess::get_file_as_string(bundle_manifest_path, &file_error);
		if (file_error != OK) {
			result["error"] = file_error;
			result["error_message"] = vformat("Could not read bundle manifest '%s'.", bundle_manifest_path);
			return result;
		}

		Variant parsed = JSON::parse_string(manifest_text);
		if (parsed.get_type() != Variant::DICTIONARY) {
			result["error"] = ERR_PARSE_ERROR;
			result["error_message"] = vformat("Bundle manifest '%s' is not a JSON object.", bundle_manifest_path);
			return result;
		}

		Dictionary bundle_manifest = parsed;
		bundle_manifest.erase("hash");
		bundle_manifest.erase("size");
		const String actual_bundle_hash = HashCalculator::hash_string_hex(HashingContext::HASH_SHA256, JSON::stringify(_canonicalize_manifest_value(bundle_manifest), "\t", true));
		result["actual_bundle_hash"] = actual_bundle_hash;
		bundle_manifest_valid = actual_bundle_hash.to_lower() == bundle->hash.to_lower();
		result["bundle_hash_valid"] = bundle_manifest_valid;
	}

	Array chunks;
	int valid_chunks = 0;
	int invalid_chunks = 0;
	int missing_chunks = 0;
	int64_t total_size = 0;

	for (const ResourceEntry &entry : bundle->resources) {
		Dictionary chunk_result = _verify_resource_entry(*bundle, entry, p_verify_hash, p_verify_md5);
		chunks.push_back(chunk_result);
		total_size += int64_t(chunk_result["actual_size"]);

		const bool valid = bool(chunk_result["valid"]);
		const Error error = Error(int(chunk_result["error"]));
		if (valid) {
			valid_chunks++;
		} else {
			invalid_chunks++;
			if (error == ERR_FILE_NOT_FOUND) {
				missing_chunks++;
			}
		}
	}

	const bool valid = bundle_manifest_valid && invalid_chunks == 0;
	result["valid"] = valid;
	result["checked_chunks"] = chunks.size();
	result["valid_chunks"] = valid_chunks;
	result["invalid_chunks"] = invalid_chunks;
	result["missing_chunks"] = missing_chunks;
	result["total_size"] = total_size;
	result["chunks"] = chunks;
	if (!valid) {
		result["error"] = missing_chunks > 0 ? ERR_FILE_NOT_FOUND : ERR_FILE_CORRUPT;
		result["error_message"] = !bundle_manifest_valid ? vformat("Bundle '%s' manifest hash mismatch.", p_bundle_name) : vformat("Bundle '%s' has %d invalid chunk(s).", p_bundle_name, invalid_chunks);
	}
	return result;
}

Dictionary AssetBundle::verify_all_bundles(bool p_verify_hash, bool p_verify_md5) const {
	Dictionary result;
	result["valid"] = false;
	result["error"] = OK;
	result["error_message"] = String();
	result["checked_bundles"] = bundle_order.size();
	result["valid_bundles"] = 0;
	result["invalid_bundles"] = 0;
	result["missing_chunks"] = 0;
	result["invalid_chunks"] = 0;
	result["total_size"] = int64_t(0);

	Array bundle_results;
	int valid_bundles = 0;
	int invalid_bundles = 0;
	int missing_chunks = 0;
	int invalid_chunks = 0;
	int64_t total_size = 0;

	for (const String &bundle_name : bundle_order) {
		Dictionary bundle_result = verify_bundle(bundle_name, p_verify_hash, p_verify_md5);
		bundle_results.push_back(bundle_result);
		if (bool(bundle_result["valid"])) {
			valid_bundles++;
		} else {
			invalid_bundles++;
		}
		missing_chunks += int(bundle_result["missing_chunks"]);
		invalid_chunks += int(bundle_result["invalid_chunks"]);
		total_size += int64_t(bundle_result["total_size"]);
	}

	const bool valid = invalid_bundles == 0;
	result["valid"] = valid;
	result["valid_bundles"] = valid_bundles;
	result["invalid_bundles"] = invalid_bundles;
	result["missing_chunks"] = missing_chunks;
	result["invalid_chunks"] = invalid_chunks;
	result["total_size"] = total_size;
	result["bundles"] = bundle_results;
	if (!valid) {
		result["error"] = missing_chunks > 0 ? ERR_FILE_NOT_FOUND : ERR_FILE_CORRUPT;
		result["error_message"] = vformat("AssetBundle manifest has %d invalid bundle(s).", invalid_bundles);
	}
	return result;
}

Error AssetBundle::start_load_bundle(const String &p_bundle_name, bool p_hot_replace_cached, bool p_replace_files) {
	PackedStringArray bundle_names;
	bundle_names.push_back(p_bundle_name);
	return start_load_bundles(bundle_names, p_hot_replace_cached, p_replace_files);
}

Error AssetBundle::start_load_bundles(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached, bool p_replace_files) {
	return _setup_load_request(p_bundle_names, p_hot_replace_cached, p_replace_files);
}

Error AssetBundle::start_load_all_bundles(bool p_hot_replace_cached, bool p_replace_files) {
	PackedStringArray bundle_names = get_bundles();
	return start_load_bundles(bundle_names, p_hot_replace_cached, p_replace_files);
}

Error AssetBundle::poll_load() {
	if (!loading) {
		return last_error_code;
	}

	if (pending_bundle_index < pending_bundles.size()) {
		return _poll_mount_bundle();
	}

	if (load_hot_replace_cached && pending_resource_index < pending_resources.size()) {
		return _poll_replace_resource();
	}

	return _finish_load_request();
}

Error AssetBundle::load_bundle(const String &p_bundle_name, bool p_hot_replace_cached, bool p_replace_files) {
	Error err = start_load_bundle(p_bundle_name, p_hot_replace_cached, p_replace_files);
	if (err != OK) {
		return err;
	}

	while (is_loading()) {
		err = poll_load();
		if (err != OK) {
			return err;
		}
	}

	return OK;
}

Error AssetBundle::load_bundles(const PackedStringArray &p_bundle_names, bool p_hot_replace_cached, bool p_replace_files) {
	Error err = start_load_bundles(p_bundle_names, p_hot_replace_cached, p_replace_files);
	if (err != OK) {
		return err;
	}

	while (is_loading()) {
		err = poll_load();
		if (err != OK) {
			return err;
		}
	}

	return OK;
}

Error AssetBundle::load_all_bundles(bool p_hot_replace_cached, bool p_replace_files) {
	Error err = start_load_all_bundles(p_hot_replace_cached, p_replace_files);
	if (err != OK) {
		return err;
	}

	while (is_loading()) {
		err = poll_load();
		if (err != OK) {
			return err;
		}
	}

	return OK;
}

bool AssetBundle::is_loading() const {
	return loading;
}

double AssetBundle::get_load_progress() const {
	return load_progress;
}

String AssetBundle::get_loading_bundle() const {
	return loading_bundle;
}

PackedStringArray AssetBundle::get_loaded_bundles() const {
	PackedStringArray result;
	for (const String &bundle_name : bundle_order) {
		if (loaded_bundles.has(bundle_name)) {
			result.push_back(bundle_name);
		}
	}
	return result;
}

bool AssetBundle::is_bundle_loaded(const String &p_bundle_name) const {
	return loaded_bundles.has(p_bundle_name);
}

Error AssetBundle::get_last_error_code() const {
	return last_error_code;
}

String AssetBundle::get_last_error() const {
	return last_error;
}

Ref<Resource> AssetBundle::load_resource(const String &p_path, const String &p_type_hint, CacheMode p_cache_mode) {
	Error err = OK;
	Ref<Resource> resource = ResourceLoader::load(p_path, p_type_hint, ResourceLoader::CacheMode(p_cache_mode), &err);
	if (err != OK) {
		_set_error(err, vformat("AssetBundle failed to load resource '%s'.", p_path));
	}
	return resource;
}

void AssetBundle::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_manifest", "manifest_path"), &AssetBundle::load_manifest);
	ClassDB::bind_method(D_METHOD("clear"), &AssetBundle::clear);

	ClassDB::bind_method(D_METHOD("has_manifest"), &AssetBundle::has_manifest);
	ClassDB::bind_method(D_METHOD("get_manifest_path"), &AssetBundle::get_manifest_path);
	ClassDB::bind_method(D_METHOD("get_manifest_version"), &AssetBundle::get_manifest_version);
	ClassDB::bind_method(D_METHOD("get_bundles"), &AssetBundle::get_bundles);
	ClassDB::bind_method(D_METHOD("get_bundle_count"), &AssetBundle::get_bundle_count);
	ClassDB::bind_method(D_METHOD("has_bundle", "bundle_name"), &AssetBundle::has_bundle);
	ClassDB::bind_method(D_METHOD("get_bundle_info", "bundle_name"), &AssetBundle::get_bundle_info);
	ClassDB::bind_method(D_METHOD("get_bundle_version", "bundle_name"), &AssetBundle::get_bundle_version);
	ClassDB::bind_method(D_METHOD("get_bundle_path", "bundle_name"), &AssetBundle::get_bundle_path);
	ClassDB::bind_method(D_METHOD("get_bundle_dependencies", "bundle_name"), &AssetBundle::get_bundle_dependencies);
	ClassDB::bind_method(D_METHOD("get_bundle_resources", "bundle_name"), &AssetBundle::get_bundle_resources);
	ClassDB::bind_method(D_METHOD("get_bundle_hash", "bundle_name"), &AssetBundle::get_bundle_hash);
	ClassDB::bind_method(D_METHOD("get_bundle_size", "bundle_name"), &AssetBundle::get_bundle_size);
	ClassDB::bind_method(D_METHOD("get_bundle_manifest_path", "bundle_name"), &AssetBundle::get_bundle_manifest_path);
	ClassDB::bind_method(D_METHOD("get_bundle_chunks", "bundle_name", "include_file_paths"), &AssetBundle::get_bundle_chunks, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_bundle_chunk_file_paths", "bundle_name"), &AssetBundle::get_bundle_chunk_file_paths);
	ClassDB::bind_method(D_METHOD("get_chunk_info", "bundle_name", "resource_path", "include_file_path"), &AssetBundle::get_chunk_info, DEFVAL(false));
	ClassDB::bind_method(D_METHOD("get_chunk_file_path", "bundle_name", "resource_path"), &AssetBundle::get_chunk_file_path);
	ClassDB::bind_method(D_METHOD("has_bundle_resource", "bundle_name", "resource_path"), &AssetBundle::has_bundle_resource);
	ClassDB::bind_method(D_METHOD("get_resource_bundles", "resource_path"), &AssetBundle::get_resource_bundles);
	ClassDB::bind_method(D_METHOD("get_missing_chunks", "bundle_name"), &AssetBundle::get_missing_chunks);
	ClassDB::bind_method(D_METHOD("is_bundle_available", "bundle_name"), &AssetBundle::is_bundle_available);
	ClassDB::bind_method(D_METHOD("get_manifest_info"), &AssetBundle::get_manifest_info);
	ClassDB::bind_method(D_METHOD("get_manifest_diff", "other_manifest_path"), &AssetBundle::get_manifest_diff);
	ClassDB::bind_method(D_METHOD("verify_bundle", "bundle_name", "verify_hash", "verify_md5"), &AssetBundle::verify_bundle, DEFVAL(true), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("verify_all_bundles", "verify_hash", "verify_md5"), &AssetBundle::verify_all_bundles, DEFVAL(true), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("start_load_bundle", "bundle_name", "hot_replace_cached", "replace_files"), &AssetBundle::start_load_bundle, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("start_load_bundles", "bundle_names", "hot_replace_cached", "replace_files"), &AssetBundle::start_load_bundles, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("start_load_all_bundles", "hot_replace_cached", "replace_files"), &AssetBundle::start_load_all_bundles, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("poll_load"), &AssetBundle::poll_load);
	ClassDB::bind_method(D_METHOD("load_bundle", "bundle_name", "hot_replace_cached", "replace_files"), &AssetBundle::load_bundle, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("load_bundles", "bundle_names", "hot_replace_cached", "replace_files"), &AssetBundle::load_bundles, DEFVAL(true), DEFVAL(true));
	ClassDB::bind_method(D_METHOD("load_all_bundles", "hot_replace_cached", "replace_files"), &AssetBundle::load_all_bundles, DEFVAL(true), DEFVAL(true));

	ClassDB::bind_method(D_METHOD("is_loading"), &AssetBundle::is_loading);
	ClassDB::bind_method(D_METHOD("get_load_progress"), &AssetBundle::get_load_progress);
	ClassDB::bind_method(D_METHOD("get_loading_bundle"), &AssetBundle::get_loading_bundle);
	ClassDB::bind_method(D_METHOD("get_loaded_bundles"), &AssetBundle::get_loaded_bundles);
	ClassDB::bind_method(D_METHOD("is_bundle_loaded", "bundle_name"), &AssetBundle::is_bundle_loaded);
	ClassDB::bind_method(D_METHOD("get_last_error_code"), &AssetBundle::get_last_error_code);
	ClassDB::bind_method(D_METHOD("get_last_error"), &AssetBundle::get_last_error);

	ClassDB::bind_method(D_METHOD("load_resource", "path", "type_hint", "cache_mode"), &AssetBundle::load_resource, DEFVAL(""), DEFVAL(CACHE_MODE_REUSE));

	ADD_SIGNAL(MethodInfo("load_progressed", PropertyInfo(Variant::STRING, "bundle_name"), PropertyInfo(Variant::FLOAT, "progress")));
	ADD_SIGNAL(MethodInfo("bundle_loaded", PropertyInfo(Variant::STRING, "bundle_name")));
	ADD_SIGNAL(MethodInfo("resource_hot_replaced", PropertyInfo(Variant::STRING, "path")));
	ADD_SIGNAL(MethodInfo("load_finished"));
	ADD_SIGNAL(MethodInfo("load_failed", PropertyInfo(Variant::STRING, "bundle_name"), PropertyInfo(Variant::INT, "error"), PropertyInfo(Variant::STRING, "message")));

	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REUSE);
	BIND_ENUM_CONSTANT(CACHE_MODE_REPLACE);
	BIND_ENUM_CONSTANT(CACHE_MODE_IGNORE_DEEP);
	BIND_ENUM_CONSTANT(CACHE_MODE_REPLACE_DEEP);
}
