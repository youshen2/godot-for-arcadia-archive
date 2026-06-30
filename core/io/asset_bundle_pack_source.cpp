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

bool PackedSourceAssetBundle::try_open_pack(const String &p_path, bool p_replace_files, uint64_t p_offset, const Vector<uint8_t> &p_decryption_key) {
	ERR_FAIL_COND_V_MSG(p_offset != 0, false, "AssetBundle directories do not support non-zero offsets.");

	String manifest_path = _asset_bundle_normalize_portable_path(p_path);
	if (!manifest_path.ends_with(BUNDLE_MANIFEST_FILE)) {
		manifest_path = manifest_path.path_join(BUNDLE_MANIFEST_FILE);
	}

	Error err = OK;
	String manifest_text = FileAccess::get_file_as_string(manifest_path, &err);
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
	for (int i = 0; i < chunks.size(); i++) {
		if (chunks[i].get_type() != Variant::DICTIONARY) {
			continue;
		}

		Dictionary chunk = chunks[i];
		if (!chunk.has("path") || !chunk.has("chunk")) {
			continue;
		}

		const String resource_path = _asset_bundle_normalize_portable_path(String(chunk["path"]));
		const String chunk_path = base_dir.path_join(_asset_bundle_normalize_portable_path(String(chunk["chunk"])));
		const uint64_t size = chunk.has("size") ? uint64_t(int64_t(chunk["size"])) : uint64_t(FileAccess::get_size(chunk_path));
		uint8_t md5[16] = {};
		if (chunk.has("md5")) {
			_asset_bundle_hex_to_md5(String(chunk["md5"]), md5);
		}

		PackedData::get_singleton()->add_path(chunk_path, resource_path, 0, size, md5, this, p_replace_files, false, false, false);
	}

	return true;
}

Ref<FileAccess> PackedSourceAssetBundle::get_file(const String &p_path, PackedData::PackedFile *p_file, const Vector<uint8_t> &p_decryption_key) {
	Error err = OK;
	Ref<FileAccess> file = FileAccess::open(p_file->pack, FileAccess::READ, &err);
	if (err != OK) {
		return Ref<FileAccess>();
	}
	return file;
}
