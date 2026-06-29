/**************************************************************************/
/*  hash_calculator.cpp                                                   */
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

#include "hash_calculator.h"

#include "core/crypto/crypto_core.h"
#include "core/error/error_macros.h"
#include "core/io/file_access.h"
#include "core/object/class_db.h"
#include "core/os/memory.h"
#include "core/string/string_name.h"
#include "core/string/ustring.h"

namespace {

class HashState {
	void *ctx = nullptr;
	HashingContext::HashType type = HashingContext::HASH_MD5;

public:
	Error start(HashingContext::HashType p_type) {
		clear();
		type = p_type;

		switch (type) {
			case HashingContext::HASH_MD5: {
				CryptoCore::MD5Context *md5 = memnew(CryptoCore::MD5Context);
				ctx = md5;
				return md5->start();
			}
			case HashingContext::HASH_SHA1: {
				CryptoCore::SHA1Context *sha1 = memnew(CryptoCore::SHA1Context);
				ctx = sha1;
				return sha1->start();
			}
			case HashingContext::HASH_SHA256: {
				CryptoCore::SHA256Context *sha256 = memnew(CryptoCore::SHA256Context);
				ctx = sha256;
				return sha256->start();
			}
			default:
				return ERR_INVALID_PARAMETER;
		}
	}

	Error update(const uint8_t *p_data, size_t p_size) {
		ERR_FAIL_NULL_V(ctx, ERR_UNCONFIGURED);

		if (p_size == 0) {
			return OK;
		}

		switch (type) {
			case HashingContext::HASH_MD5:
				return ((CryptoCore::MD5Context *)ctx)->update(p_data, p_size);
			case HashingContext::HASH_SHA1:
				return ((CryptoCore::SHA1Context *)ctx)->update(p_data, p_size);
			case HashingContext::HASH_SHA256:
				return ((CryptoCore::SHA256Context *)ctx)->update(p_data, p_size);
		}

		return ERR_INVALID_PARAMETER;
	}

	Error finish(PackedByteArray &r_hash) {
		ERR_FAIL_NULL_V(ctx, ERR_UNCONFIGURED);

		Error err = FAILED;
		switch (type) {
			case HashingContext::HASH_MD5:
				r_hash.resize(16);
				err = ((CryptoCore::MD5Context *)ctx)->finish(r_hash.ptrw());
				break;
			case HashingContext::HASH_SHA1:
				r_hash.resize(20);
				err = ((CryptoCore::SHA1Context *)ctx)->finish(r_hash.ptrw());
				break;
			case HashingContext::HASH_SHA256:
				r_hash.resize(32);
				err = ((CryptoCore::SHA256Context *)ctx)->finish(r_hash.ptrw());
				break;
		}

		clear();
		return err;
	}

	void clear() {
		if (ctx == nullptr) {
			return;
		}

		switch (type) {
			case HashingContext::HASH_MD5:
				memdelete((CryptoCore::MD5Context *)ctx);
				break;
			case HashingContext::HASH_SHA1:
				memdelete((CryptoCore::SHA1Context *)ctx);
				break;
			case HashingContext::HASH_SHA256:
				memdelete((CryptoCore::SHA256Context *)ctx);
				break;
		}

		ctx = nullptr;
	}

	~HashState() {
		clear();
	}
};

} // namespace

int HashCalculator::_get_hash_size(HashingContext::HashType p_type) {
	switch (p_type) {
		case HashingContext::HASH_MD5:
			return 16;
		case HashingContext::HASH_SHA1:
			return 20;
		case HashingContext::HASH_SHA256:
			return 32;
	}

	return 0;
}

Error HashCalculator::_validate_parameters(HashingContext::HashType p_type, int64_t p_chunk_size) {
	ERR_FAIL_COND_V_MSG(_get_hash_size(p_type) == 0, ERR_INVALID_PARAMETER, "Unsupported hash type.");
	ERR_FAIL_COND_V_MSG(p_chunk_size <= 0, ERR_INVALID_PARAMETER, "Chunk size must be greater than 0.");
	ERR_FAIL_COND_V_MSG(p_chunk_size > INT32_MAX, ERR_INVALID_PARAMETER, "Chunk size is too large.");
	return OK;
}

Error HashCalculator::_hash_buffer(HashingContext::HashType p_type, const uint8_t *p_data, size_t p_size, PackedByteArray &r_hash) {
	int hash_size = _get_hash_size(p_type);
	ERR_FAIL_COND_V(hash_size == 0, ERR_INVALID_PARAMETER);

	r_hash.resize(hash_size);
	static const uint8_t empty_buffer = 0;
	const uint8_t *data = p_size > 0 ? p_data : &empty_buffer;

	switch (p_type) {
		case HashingContext::HASH_MD5:
			return CryptoCore::md5(data, p_size, r_hash.ptrw());
		case HashingContext::HASH_SHA1:
			return CryptoCore::sha1(data, p_size, r_hash.ptrw());
		case HashingContext::HASH_SHA256:
			return CryptoCore::sha256(data, p_size, r_hash.ptrw());
	}

	return ERR_INVALID_PARAMETER;
}

HashCalculator::FileResult HashCalculator::_hash_file(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size, SafeNumeric<uint64_t> *r_processed_bytes) {
	FileResult result;
	result.path = p_path;

	Error err = _validate_parameters(p_type, p_chunk_size);
	if (err != OK) {
		result.error = err;
		return result;
	}

	Error open_error = OK;
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::READ, &open_error);
	if (file.is_null()) {
		result.error = open_error != OK ? open_error : ERR_FILE_CANT_OPEN;
		return result;
	}

	result.size = file->get_length();

	HashState hash_state;
	err = hash_state.start(p_type);
	if (err != OK) {
		result.error = err;
		return result;
	}

	Vector<uint8_t> buffer;
	buffer.resize((int)p_chunk_size);

	while (result.processed_size < result.size) {
		const uint64_t remaining = result.size - result.processed_size;
		const uint64_t to_read = MIN(remaining, (uint64_t)p_chunk_size);
		const uint64_t bytes_read = file->get_buffer(buffer.ptrw(), to_read);

		if (bytes_read == 0 && to_read > 0) {
			err = file->get_error();
			result.error = err != OK && err != ERR_FILE_EOF ? err : ERR_FILE_CANT_READ;
			return result;
		}

		err = hash_state.update(buffer.ptr(), bytes_read);
		if (err != OK) {
			result.error = err;
			return result;
		}

		result.processed_size += bytes_read;
		if (r_processed_bytes != nullptr) {
			r_processed_bytes->add(bytes_read);
		}

		if (bytes_read < to_read) {
			err = file->get_error();
			result.error = err != OK && err != ERR_FILE_EOF ? err : ERR_FILE_CANT_READ;
			return result;
		}
	}

	err = hash_state.finish(result.hash);
	if (err != OK) {
		result.error = err;
		return result;
	}

	result.hash_hex = String::hex_encode_buffer(result.hash.ptr(), result.hash.size());
	result.error = OK;
	return result;
}

Dictionary HashCalculator::_result_to_dictionary(const FileResult &p_result) {
	Dictionary result;
	result["path"] = p_result.path;
	result["hash"] = p_result.hash;
	result["hash_hex"] = p_result.hash_hex;
	result["error"] = p_result.error;
	result["size"] = (int64_t)p_result.size;
	result["processed_size"] = (int64_t)p_result.processed_size;
	return result;
}

void HashCalculator::_hash_file_task(void *p_userdata, uint32_t p_index) {
	HashCalculator *calculator = static_cast<HashCalculator *>(p_userdata);
	calculator->_hash_file_index(p_index);
}

void HashCalculator::_hash_file_index(uint32_t p_index) {
	const FileResult result = _hash_file(hash_type, paths[p_index], chunk_size, &processed_bytes);

	{
		MutexLock lock(results_mutex);
		results.write[p_index] = result;
	}

	processed_files.increment();
}

void HashCalculator::_finish_group() {
	if (group_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *worker_pool = WorkerThreadPool::get_singleton();
		if (worker_pool != nullptr) {
			worker_pool->wait_for_group_task_completion(group_id);
		}
		group_id = WorkerThreadPool::INVALID_TASK_ID;
	}

	running.clear();
	finished.set();
}

PackedByteArray HashCalculator::hash_bytes(HashingContext::HashType p_type, const PackedByteArray &p_bytes) {
	PackedByteArray hash;
	Error err = _hash_buffer(p_type, p_bytes.ptr(), p_bytes.size(), hash);
	ERR_FAIL_COND_V(err != OK, PackedByteArray());
	return hash;
}

String HashCalculator::hash_bytes_hex(HashingContext::HashType p_type, const PackedByteArray &p_bytes) {
	PackedByteArray hash = hash_bytes(p_type, p_bytes);
	ERR_FAIL_COND_V(hash.is_empty(), String());
	return String::hex_encode_buffer(hash.ptr(), hash.size());
}

PackedByteArray HashCalculator::hash_string(HashingContext::HashType p_type, const String &p_string) {
	CharString utf8 = p_string.utf8();
	PackedByteArray hash;
	Error err = _hash_buffer(p_type, (const uint8_t *)utf8.ptr(), utf8.length(), hash);
	ERR_FAIL_COND_V(err != OK, PackedByteArray());
	return hash;
}

String HashCalculator::hash_string_hex(HashingContext::HashType p_type, const String &p_string) {
	PackedByteArray hash = hash_string(p_type, p_string);
	ERR_FAIL_COND_V(hash.is_empty(), String());
	return String::hex_encode_buffer(hash.ptr(), hash.size());
}

PackedByteArray HashCalculator::hash_file(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size) {
	FileResult result = _hash_file(p_type, p_path, p_chunk_size, nullptr);
	ERR_FAIL_COND_V_MSG(result.error != OK, PackedByteArray(), vformat("Could not hash file '%s'. Error: %d.", p_path, result.error));
	return result.hash;
}

String HashCalculator::hash_file_hex(HashingContext::HashType p_type, const String &p_path, int64_t p_chunk_size) {
	PackedByteArray hash = hash_file(p_type, p_path, p_chunk_size);
	ERR_FAIL_COND_V(hash.is_empty(), String());
	return String::hex_encode_buffer(hash.ptr(), hash.size());
}

TypedArray<Dictionary> HashCalculator::hash_files(HashingContext::HashType p_type, const PackedStringArray &p_paths, int p_thread_count, int64_t p_chunk_size, bool p_high_priority) {
	Ref<HashCalculator> calculator;
	calculator.instantiate();

	Error err = calculator->start_files(p_type, p_paths, p_thread_count, p_chunk_size, p_high_priority);
	ERR_FAIL_COND_V(err != OK, TypedArray<Dictionary>());

	err = calculator->wait_to_finish();
	ERR_FAIL_COND_V(err != OK, TypedArray<Dictionary>());

	return calculator->get_results();
}

Error HashCalculator::start_files(HashingContext::HashType p_type, const PackedStringArray &p_paths, int p_thread_count, int64_t p_chunk_size, bool p_high_priority) {
	if (is_running()) {
		return ERR_ALREADY_IN_USE;
	}

	Error err = _validate_parameters(p_type, p_chunk_size);
	ERR_FAIL_COND_V(err != OK, err);

	clear();

	hash_type = p_type;
	chunk_size = p_chunk_size;
	paths = p_paths;
	total_files = paths.size();
	total_bytes = 0;
	processed_bytes.set(0);
	processed_files.set(0);
	finished.clear();

	{
		MutexLock lock(results_mutex);
		results.resize(total_files);

		for (uint32_t i = 0; i < total_files; i++) {
			FileResult result;
			result.path = paths[i];
			result.error = ERR_BUSY;

			int64_t size = FileAccess::get_size(paths[i]);
			if (size > 0) {
				total_bytes += (uint64_t)size;
				result.size = (uint64_t)size;
			}

			results.write[i] = result;
		}
	}

	if (total_files == 0) {
		finished.set();
		return OK;
	}

	WorkerThreadPool *worker_pool = WorkerThreadPool::get_singleton();
	if (worker_pool == nullptr) {
		finished.set();
		return ERR_UNAVAILABLE;
	}

	running.set();

	if (worker_pool->get_thread_count() <= 0) {
		for (uint32_t i = 0; i < total_files; i++) {
			_hash_file_index(i);
		}
		running.clear();
		finished.set();
		return OK;
	}

	const int task_count = p_thread_count <= 0 ? -1 : MIN(p_thread_count, (int)total_files);
	group_id = worker_pool->add_native_group_task(&HashCalculator::_hash_file_task, this, total_files, task_count, p_high_priority, SNAME("HashCalculator"));
	if (group_id == WorkerThreadPool::INVALID_TASK_ID) {
		running.clear();
		finished.set();
		return ERR_CANT_CREATE;
	}

	return OK;
}

bool HashCalculator::is_running() {
	if (group_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *worker_pool = WorkerThreadPool::get_singleton();
		if (worker_pool != nullptr && worker_pool->is_group_task_completed(group_id)) {
			_finish_group();
		}
	}

	return running.is_set();
}

bool HashCalculator::is_finished() {
	if (group_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *worker_pool = WorkerThreadPool::get_singleton();
		if (worker_pool != nullptr && worker_pool->is_group_task_completed(group_id)) {
			_finish_group();
		}
	}

	return finished.is_set();
}

Error HashCalculator::wait_to_finish() {
	if (group_id != WorkerThreadPool::INVALID_TASK_ID) {
		WorkerThreadPool *worker_pool = WorkerThreadPool::get_singleton();
		ERR_FAIL_NULL_V(worker_pool, ERR_UNAVAILABLE);
		worker_pool->wait_for_group_task_completion(group_id);
		group_id = WorkerThreadPool::INVALID_TASK_ID;
	}

	running.clear();
	finished.set();
	return OK;
}

void HashCalculator::clear() {
	wait_to_finish();

	{
		MutexLock lock(results_mutex);
		paths.clear();
		results.clear();
	}

	hash_type = HashingContext::HASH_MD5;
	chunk_size = DEFAULT_CHUNK_SIZE;
	processed_bytes.set(0);
	processed_files.set(0);
	total_bytes = 0;
	total_files = 0;
	running.clear();
	finished.clear();
}

double HashCalculator::get_progress() {
	if (is_finished()) {
		return 1.0;
	}

	if (total_bytes > 0) {
		return MIN((double)processed_bytes.get() / (double)total_bytes, 1.0);
	}

	if (total_files > 0) {
		return MIN((double)processed_files.get() / (double)total_files, 1.0);
	}

	return 0.0;
}

int64_t HashCalculator::get_processed_bytes() const {
	return (int64_t)processed_bytes.get();
}

int64_t HashCalculator::get_total_bytes() const {
	return (int64_t)total_bytes;
}

int HashCalculator::get_processed_file_count() const {
	return (int)processed_files.get();
}

int HashCalculator::get_total_file_count() const {
	return (int)total_files;
}

TypedArray<Dictionary> HashCalculator::get_results() const {
	TypedArray<Dictionary> output;

	MutexLock lock(results_mutex);
	for (const FileResult &result : results) {
		output.push_back(_result_to_dictionary(result));
	}

	return output;
}

Dictionary HashCalculator::get_result(int p_index) const {
	MutexLock lock(results_mutex);
	ERR_FAIL_INDEX_V(p_index, results.size(), Dictionary());
	return _result_to_dictionary(results[p_index]);
}

void HashCalculator::_bind_methods() {
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_bytes", "type", "bytes"), &HashCalculator::hash_bytes);
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_bytes_hex", "type", "bytes"), &HashCalculator::hash_bytes_hex);
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_string", "type", "text"), &HashCalculator::hash_string);
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_string_hex", "type", "text"), &HashCalculator::hash_string_hex);
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_file", "type", "path", "chunk_size"), &HashCalculator::hash_file, DEFVAL(DEFAULT_CHUNK_SIZE));
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_file_hex", "type", "path", "chunk_size"), &HashCalculator::hash_file_hex, DEFVAL(DEFAULT_CHUNK_SIZE));
	ClassDB::bind_static_method("HashCalculator", D_METHOD("hash_files", "type", "paths", "thread_count", "chunk_size", "high_priority"), &HashCalculator::hash_files, DEFVAL(-1), DEFVAL(DEFAULT_CHUNK_SIZE), DEFVAL(false));

	ClassDB::bind_method(D_METHOD("start_files", "type", "paths", "thread_count", "chunk_size", "high_priority"), &HashCalculator::start_files, DEFVAL(-1), DEFVAL(DEFAULT_CHUNK_SIZE), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_running"), &HashCalculator::is_running);
	ClassDB::bind_method(D_METHOD("is_finished"), &HashCalculator::is_finished);
	ClassDB::bind_method(D_METHOD("wait_to_finish"), &HashCalculator::wait_to_finish);
	ClassDB::bind_method(D_METHOD("clear"), &HashCalculator::clear);
	ClassDB::bind_method(D_METHOD("get_progress"), &HashCalculator::get_progress);
	ClassDB::bind_method(D_METHOD("get_processed_bytes"), &HashCalculator::get_processed_bytes);
	ClassDB::bind_method(D_METHOD("get_total_bytes"), &HashCalculator::get_total_bytes);
	ClassDB::bind_method(D_METHOD("get_processed_file_count"), &HashCalculator::get_processed_file_count);
	ClassDB::bind_method(D_METHOD("get_total_file_count"), &HashCalculator::get_total_file_count);
	ClassDB::bind_method(D_METHOD("get_results"), &HashCalculator::get_results);
	ClassDB::bind_method(D_METHOD("get_result", "index"), &HashCalculator::get_result);
}

HashCalculator::~HashCalculator() {
	wait_to_finish();
}
