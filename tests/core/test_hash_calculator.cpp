/**************************************************************************/
/*  test_hash_calculator.cpp                                              */
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

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_hash_calculator)

#include "core/crypto/hash_calculator.h"
#include "core/io/file_access.h"
#include "tests/test_utils.h"

namespace TestHashCalculator {

static void write_test_file(const String &p_path, const PackedByteArray &p_data) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	CHECK(file->store_buffer(p_data));
	file->close();
}

TEST_CASE("[HashCalculator] Hash strings and byte arrays") {
	const String text = "xyz";
	const PackedByteArray bytes = text.to_utf8_buffer();

	CHECK(HashCalculator::hash_string_hex(HashingContext::HASH_MD5, text) == "d16fb36f0911f878998c136191af705e");
	CHECK(HashCalculator::hash_string_hex(HashingContext::HASH_SHA1, text) == "66b27417d37e024c46526c2f6d358a754fc552f3");
	CHECK(HashCalculator::hash_string_hex(HashingContext::HASH_SHA256, text) == "3608bca1e44ea6c4d268eb6db02260269892c0b42b86bbf1e77a6fa16c3c9282");

	CHECK(HashCalculator::hash_bytes_hex(HashingContext::HASH_MD5, bytes) == "d16fb36f0911f878998c136191af705e");
	CHECK(HashCalculator::hash_bytes_hex(HashingContext::HASH_SHA1, bytes) == "66b27417d37e024c46526c2f6d358a754fc552f3");
	CHECK(HashCalculator::hash_bytes_hex(HashingContext::HASH_SHA256, bytes) == "3608bca1e44ea6c4d268eb6db02260269892c0b42b86bbf1e77a6fa16c3c9282");
}

TEST_CASE("[HashCalculator] Hash empty inputs") {
	CHECK(HashCalculator::hash_string_hex(HashingContext::HASH_MD5, String()) == "d41d8cd98f00b204e9800998ecf8427e");
	CHECK(HashCalculator::hash_bytes_hex(HashingContext::HASH_SHA1, PackedByteArray()) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
	CHECK(HashCalculator::hash_string_hex(HashingContext::HASH_SHA256, String()) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST_CASE("[HashCalculator] Hash file") {
	const String file_path = TestUtils::get_temp_path("hash_calculator_file.bin");
	write_test_file(file_path, String("xyz").to_utf8_buffer());

	CHECK(HashCalculator::hash_file_hex(HashingContext::HASH_MD5, file_path, 2) == "d16fb36f0911f878998c136191af705e");
	CHECK(HashCalculator::hash_file_hex(HashingContext::HASH_SHA256, file_path, 2) == "3608bca1e44ea6c4d268eb6db02260269892c0b42b86bbf1e77a6fa16c3c9282");
}

TEST_CASE("[HashCalculator] Hash multiple files") {
	const String file_path_a = TestUtils::get_temp_path("hash_calculator_a.bin");
	const String file_path_b = TestUtils::get_temp_path("hash_calculator_b.bin");
	write_test_file(file_path_a, String("abc").to_utf8_buffer());
	write_test_file(file_path_b, String("xyz").to_utf8_buffer());

	PackedStringArray paths;
	paths.push_back(file_path_a);
	paths.push_back(file_path_b);

	HashCalculator calculator;
	CHECK(calculator.start_files(HashingContext::HASH_MD5, paths, 2, 2) == OK);
	CHECK(calculator.wait_to_finish() == OK);
	CHECK(calculator.is_finished());
	CHECK(calculator.get_progress() == doctest::Approx(1.0));
	CHECK(calculator.get_processed_bytes() == 6);
	CHECK(calculator.get_total_bytes() == 6);
	CHECK(calculator.get_processed_file_count() == 2);
	CHECK(calculator.get_total_file_count() == 2);

	TypedArray<Dictionary> results = calculator.get_results();
	REQUIRE(results.size() == 2);

	Dictionary result_a = results[0];
	CHECK((int)result_a["error"] == OK);
	CHECK(String(result_a["path"]) == file_path_a);
	CHECK(String(result_a["hash_hex"]) == "900150983cd24fb0d6963f7d28e17f72");
	CHECK((int64_t)result_a["size"] == 3);
	CHECK((int64_t)result_a["processed_size"] == 3);

	Dictionary result_b = results[1];
	CHECK((int)result_b["error"] == OK);
	CHECK(String(result_b["path"]) == file_path_b);
	CHECK(String(result_b["hash_hex"]) == "d16fb36f0911f878998c136191af705e");
	CHECK((int64_t)result_b["size"] == 3);
	CHECK((int64_t)result_b["processed_size"] == 3);
}

TEST_CASE("[HashCalculator] Hash multiple files with a missing path") {
	const String file_path = TestUtils::get_temp_path("hash_calculator_existing.bin");
	const String missing_path = TestUtils::get_temp_path("hash_calculator_missing.bin");
	write_test_file(file_path, String("abc").to_utf8_buffer());

	PackedStringArray paths;
	paths.push_back(file_path);
	paths.push_back(missing_path);

	TypedArray<Dictionary> results = HashCalculator::hash_files(HashingContext::HASH_SHA256, paths, 2, 2);
	REQUIRE(results.size() == 2);

	Dictionary result_ok = results[0];
	CHECK((int)result_ok["error"] == OK);
	CHECK(String(result_ok["hash_hex"]) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

	Dictionary result_missing = results[1];
	CHECK((int)result_missing["error"] != OK);
	CHECK(String(result_missing["hash_hex"]).is_empty());
}

} // namespace TestHashCalculator
