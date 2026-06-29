/**************************************************************************/
/*  hash_calculator.h                                                     */
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

#include "core/crypto/hashing_context.h"
#include "core/object/ref_counted.h"
#include "core/object/worker_thread_pool.h"
#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"

class HashCalculator : public RefCounted {
	GDCLASS(HashCalculator, RefCounted);

public:
	static constexpr int64_t DEFAULT_CHUNK_SIZE = 1024 * 1024;

private:
	struct FileResult {
		String path;
		PackedByteArray hash;
		String hash_hex;
		Error error = ERR_BUSY;
		uint64_t size = 0;
		uint64_t processed_size = 0;
	};

	mutable Mutex results_mutex;
	PackedStringArray paths;
	Vector<FileResult> results;

	HashingContext::HashType hash_type = HashingContext::HASH_MD5;
	int64_t chunk_size = DEFAULT_CHUNK_SIZE;
	WorkerThreadPool::GroupID group_id = WorkerThreadPool::INVALID_TASK_ID;

	SafeNumeric<uint64_t> processed_bytes;
	SafeNumeric<uint32_t> processed_files;
	uint64_t total_bytes = 0;
	uint32_t total_files = 0;
	SafeFlag running;
	SafeFlag finished;

	static int _get_hash_size(HashingContext::HashType p_type);
	static Error _validate_parameters(HashingContext::HashType p_type, int64_t p_chunk_size);
	static Error _hash_buffer(HashingContext::HashType p_type, const uint8_t *p_data, size_t p_size, PackedByteArray &r_hash);
	static FileResult _hash_file(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size, SafeNumeric<uint64_t> *r_processed_bytes);
	static Dictionary _result_to_dictionary(const FileResult &p_result);
	static void _hash_file_task(void *p_userdata, uint32_t p_index);

	void _hash_file_index(uint32_t p_index);
	void _finish_group();

protected:
	static void _bind_methods();

public:
	static PackedByteArray hash_bytes(HashingContext::HashType p_type, const PackedByteArray &p_bytes);
	static String hash_bytes_hex(HashingContext::HashType p_type, const PackedByteArray &p_bytes);
	static PackedByteArray hash_string(HashingContext::HashType p_type, const String &p_string);
	static String hash_string_hex(HashingContext::HashType p_type, const String &p_string);
	static PackedByteArray hash_file(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size = DEFAULT_CHUNK_SIZE);
	static String hash_file_hex(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size = DEFAULT_CHUNK_SIZE);
	static TypedArray<Dictionary> hash_files(HashingContext::HashType p_type, const PackedStringArray &p_paths, int p_thread_count = -1, int64_t p_chunk_size = DEFAULT_CHUNK_SIZE, bool p_high_priority = false);

	Error start_files(HashingContext::HashType p_type, const PackedStringArray &p_paths, int p_thread_count = -1, int64_t p_chunk_size = DEFAULT_CHUNK_SIZE, bool p_high_priority = false);
	bool is_running();
	bool is_finished();
	Error wait_to_finish();
	void clear();

	double get_progress();
	int64_t get_processed_bytes() const;
	int64_t get_total_bytes() const;
	int get_processed_file_count() const;
	int get_total_file_count() const;
	TypedArray<Dictionary> get_results() const;
	Dictionary get_result(int p_index) const;

	~HashCalculator();
};
