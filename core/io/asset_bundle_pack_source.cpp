/**************************************************************************/
/*  asset_bundle_pack_source.cpp                                          */
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

#include "asset_bundle_pack_source.h"

#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/io/file_access_pack.h"
#include "core/io/json.h"

static int _asset_bundle_hex_char_to_int(char32_t p_char) {
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

static bool _asset_bundle_hex_to_md5(const String &p_hex, uint8_t r_md5[16]) {
	if (p_hex.length() != 32) {
		return false;
	}

	for (int i = 0; i < 16; i++) {
		int hi = _asset_bundle_hex_char_to_int(p_hex[i * 2]);
		int lo = _asset_bundle_hex_char_to_int(p_hex[i * 2 + 1]);
		if (hi < 0 || lo < 0) {
			return false;
		}
		r_md5[i] = uint8_t((hi << 4) | lo);
	}

	return true;
}

static String _asset_bundle_normalize_portable_path(const String &p_path) {
	return p_path.replace("\\", "/").simplify_path();
}

struct AssetBundlePackEntry {
	String resource_path;
	String chunk_path;
	uint64_t offset = 0;
	uint64_t size = 0;
	bool encrypted = false;
	uint8_t md5[16] = {};
};

static String _asset_bundle_get_dictionary_string(const Dictionary &p_dictionary, const String &p_key, const String &p_default = String()) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::STRING && value.get_type() != Variant::STRING_NAME && value.get_type() != Variant::NODE_PATH) {
		return p_default;
	}

	return String(value);
}

static uint64_t _asset_bundle_get_dictionary_uint64(const Dictionary &p_dictionary, const String &p_key, uint64_t p_default = 0) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT) {
		return p_default;
	}

	return uint64_t(int64_t(value));
}

static bool _asset_bundle_get_dictionary_bool(const Dictionary &p_dictionary, const String &p_key, bool p_default = false) {
	if (!p_dictionary.has(p_key)) {
		return p_default;
	}

	Variant value = p_dictionary[p_key];
	if (value.get_type() != Variant::BOOL) {
		return p_default;
	}

	return bool(value);
}

static Ref<FileAccess> _asset_bundle_open_chunk_file(const String &p_path, Error *r_error = nullptr) {
	Error physical_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ | FileAccess::SKIP_PACK, &physical_error);
	if (file.is_valid()) {
		if (r_error != nullptr) {
			*r_error = OK;
		}
		return file;
	}

	return FileAccess::open(p_path, FileAccess::READ, r_error);
}

static int64_t _asset_bundle_get_chunk_file_size(const String &p_path) {
	Error err = OK;
	Ref<FileAccess> file = _asset_bundle_open_chunk_file(p_path, &err);
	if (err != OK || file.is_null()) {
		return -1;
	}
	return file->get_length();
}

static String _asset_bundle_get_file_as_string(const String &p_path, Error *r_error = nullptr) {
	Error err = OK;
	Ref<FileAccess> file = _asset_bundle_open_chunk_file(p_path, &err);
	if (r_error != nullptr) {
		*r_error = err;
	}
	if (err != OK || file.is_null()) {
		return String();
	}
	return file->get_as_text();
}

static bool _asset_bundle_append_pack_entry(const Dictionary &p_file, const String &p_chunk_path, bool p_default_encrypted, Vector<AssetBundlePackEntry> &r_entries) {
	const String resource_path = _asset_bundle_normalize_portable_path(_asset_bundle_get_dictionary_string(p_file, "path"));
	if (resource_path.is_empty()) {
		return false;
	}

	AssetBundlePackEntry entry;
	entry.resource_path = resource_path;
	entry.chunk_path = p_chunk_path;
	entry.offset = _asset_bundle_get_dictionary_uint64(p_file, "offset", 0);
	const int64_t chunk_file_size = _asset_bundle_get_chunk_file_size(p_chunk_path);
	entry.size = _asset_bundle_get_dictionary_uint64(p_file, "size", chunk_file_size > 0 ? uint64_t(chunk_file_size) : 0);
	entry.encrypted = _asset_bundle_get_dictionary_bool(p_file, "encrypted", p_default_encrypted);
	if (p_file.has("md5")) {
		_asset_bundle_hex_to_md5(String(p_file["md5"]), entry.md5);
	}

	r_entries.push_back(entry);
	return true;
}

static bool _asset_bundle_validate_pack_entry(const AssetBundlePackEntry &p_entry, const Vector<uint8_t> &p_decryption_key) {
	Error open_error = OK;
	Ref<FileAccess> chunk_file = _asset_bundle_open_chunk_file(p_entry.chunk_path, &open_error);
	if (open_error != OK || chunk_file.is_null()) {
		ERR_PRINT(vformat("AssetBundle chunk file '%s' does not exist.", p_entry.chunk_path));
		return false;
	}

	if (!p_entry.encrypted) {
		const int64_t chunk_size = chunk_file->get_length();
		if (chunk_size < 0 || p_entry.offset + p_entry.size > uint64_t(chunk_size)) {
			ERR_PRINT(vformat("AssetBundle chunk '%s' is too small for resource '%s' (offset: %d, size: %d, chunk size: %d).", p_entry.chunk_path, p_entry.resource_path, int64_t(p_entry.offset), int64_t(p_entry.size), chunk_size));
			return false;
		}
		return true;
	}

	PackedData::PackedFile packed_file;
	packed_file.pack = p_entry.chunk_path;
	packed_file.offset = p_entry.offset;
	packed_file.size = p_entry.size;
	for (int i = 0; i < 16; i++) {
		packed_file.md5[i] = p_entry.md5[i];
	}
	packed_file.src = nullptr;
	packed_file.encrypted = true;
	packed_file.bundle = false;
	packed_file.delta = false;
	packed_file.skip_pack = true;

	Ref<FileAccess> file(memnew(FileAccessPack(p_entry.resource_path, packed_file, p_decryption_key)));
	if (file.is_null() || !file->is_open()) {
		ERR_PRINT(vformat("AssetBundle failed to decrypt chunk '%s' for resource '%s'.", p_entry.chunk_path, p_entry.resource_path));
		return false;
	}

	if (file->get_length() != p_entry.size) {
		ERR_PRINT(vformat("AssetBundle decrypted size mismatch for resource '%s'.", p_entry.resource_path));
		return false;
	}
	return true;
}

bool PackedSourceAssetBundle::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	ERR_FAIL_COND_V_MSG(p_offset != 0, false, "AssetBundle directories do not support non-zero offsets.");

	String manifest_path = _asset_bundle_normalize_portable_path(p_path);
	if (!manifest_path.ends_with(BUNDLE_MANIFEST_FILE)) {
		manifest_path = manifest_path.path_join(BUNDLE_MANIFEST_FILE);
	}

	Error err = OK;
	String manifest_text = _asset_bundle_get_file_as_string(manifest_path, &err);
	if (err != OK) {
		return false;
	}

	Variant parsed = JSON::parse_string(manifest_text);
	if (parsed.get_type() != Variant::DICTIONARY) {
		return false;
	}

	Dictionary manifest = parsed;
	if (!manifest.has("chunks")) {
		return false;
	}

	Variant chunks_value = manifest["chunks"];
	if (chunks_value.get_type() != Variant::ARRAY) {
		return false;
	}

	const String base_dir = manifest_path.get_base_dir();
	Array chunks = chunks_value;
	Vector<AssetBundlePackEntry> entries;
	for (int i = 0; i < chunks.size(); i++) {
		if (chunks[i].get_type() != Variant::DICTIONARY) {
			continue;
		}

		Dictionary chunk = chunks[i];
		if (!chunk.has("chunk")) {
			continue;
		}

		const String chunk_path = base_dir.path_join(_asset_bundle_normalize_portable_path(String(chunk["chunk"])));
		const bool chunk_encrypted = _asset_bundle_get_dictionary_bool(chunk, "encrypted", false);
		if (chunk.has("files") && chunk["files"].get_type() == Variant::ARRAY) {
			Array files = chunk["files"];
			for (int j = 0; j < files.size(); j++) {
				if (files[j].get_type() != Variant::DICTIONARY) {
					continue;
				}
				_asset_bundle_append_pack_entry(files[j], chunk_path, chunk_encrypted, entries);
			}
		} else if (chunk.has("path")) {
			_asset_bundle_append_pack_entry(chunk, chunk_path, chunk_encrypted, entries);
		}
	}

	for (const AssetBundlePackEntry &entry : entries) {
		if (!_asset_bundle_validate_pack_entry(entry, p_decryption_key)) {
			return false;
		}
	}

	for (const AssetBundlePackEntry &entry : entries) {
		PackedData::get_singleton()->add_path(entry.chunk_path, entry.resource_path, entry.offset, entry.size, entry.md5, this, p_replace_files, entry.encrypted, false, false, String(), true);
	}

	return true;
}

Ref<FileAccess> PackedSourceAssetBundle::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key) {
	Ref<FileAccess> file(memnew(FileAccessPack(p_path, *p_file, p_decryption_key)));
	if (file.is_null() || !file->is_open()) {
		return Ref<FileAccess>();
	}
	return file;
}
