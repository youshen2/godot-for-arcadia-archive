/**************************************************************************/
/*  test_asset_bundle.cpp                                                 */
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
/* "Software"), to deal in the Software without restriction, including   */
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
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,    */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE       */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_asset_bundle)

#include "core/crypto/hash_calculator.h"
#include "core/io/asset_bundle.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "tests/test_utils.h"

namespace TestAssetBundle {

static void write_text_file(const String &p_path, const String &p_contents) {
	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	REQUIRE(file.is_valid());
	CHECK(file->store_string(p_contents));
}

static Dictionary make_resource_entry(const String &p_path, const String &p_chunk, const String &p_contents, int64_t p_offset = 0) {
	Dictionary entry;
	entry["path"] = p_path;
	entry["chunk"] = p_chunk;
	entry["size"] = p_contents.to_utf8_buffer().size();
	entry["offset"] = p_offset;
	entry["hash"] = HashCalculator::hash_string_hex(HashingContext::HASH_SHA256, p_contents);
	entry["md5"] = HashCalculator::hash_string_hex(HashingContext::HASH_MD5, p_contents);
	return entry;
}

TEST_CASE("[AssetBundle] Manifest can be loaded from a JSON string") {
	const String root_path = TestUtils::get_temp_path("asset_bundle_manifest_string");

	Dictionary bundle;
	bundle["name"] = "string_bundle";
	bundle["path"] = "string_bundle.json";

	Array bundles;
	bundles.push_back(bundle);

	Dictionary manifest;
	manifest["version"] = "string-version";
	manifest["bundles"] = bundles;

	Ref<AssetBundle> asset_bundle;
	asset_bundle.instantiate();
	REQUIRE(asset_bundle->load_manifest_from_string(JSON::stringify(manifest), root_path) == OK);
	CHECK(asset_bundle->has_manifest());
	CHECK(asset_bundle->get_manifest_path().is_empty());
	CHECK(asset_bundle->get_manifest_version() == "string-version");
	CHECK(asset_bundle->get_bundle_count() == 1);
	CHECK(asset_bundle->get_bundle_path("string_bundle") == root_path.path_join("string_bundle.json"));

	Dictionary manifest_info = asset_bundle->get_manifest_info();
	CHECK(String(manifest_info["base_dir"]) == root_path);

	Dictionary empty_manifest;
	empty_manifest["bundles"] = Array();
	REQUIRE(asset_bundle->load_manifest_from_string(JSON::stringify(empty_manifest)) == OK);
	CHECK(asset_bundle->has_manifest());
	CHECK(asset_bundle->get_bundle_count() == 0);

	asset_bundle->clear();
	CHECK_FALSE(asset_bundle->has_manifest());
}

TEST_CASE("[AssetBundle] Full verification matches individual bundle results") {
	const String root_path = TestUtils::get_temp_path("asset_bundle_verify");
	const String bundle_a_path = root_path.path_join("bundle_a");
	const String bundle_b_path = root_path.path_join("bundle_b");
	REQUIRE(DirAccess::make_dir_recursive_absolute(bundle_a_path) == OK);
	REQUIRE(DirAccess::make_dir_recursive_absolute(bundle_b_path) == OK);

	write_text_file(bundle_a_path.path_join("bundle.json"), "{}");
	write_text_file(bundle_b_path.path_join("bundle.json"), "{}");
	write_text_file(bundle_a_path.path_join("shared.ab"), "abcxyz");
	write_text_file(bundle_b_path.path_join("present.ab"), "present");

	Array bundle_a_resources;
	bundle_a_resources.push_back(make_resource_entry("res://a.txt", "shared.ab", "abc"));
	bundle_a_resources.push_back(make_resource_entry("res://b.txt", "shared.ab", "xyz", 3));

	Array bundle_b_resources;
	bundle_b_resources.push_back(make_resource_entry("res://present.txt", "present.ab", "present"));
	bundle_b_resources.push_back(make_resource_entry("res://missing.txt", "missing.ab", "missing"));

	Dictionary bundle_a;
	bundle_a["name"] = "bundle_a";
	bundle_a["path"] = "bundle_a";
	bundle_a["hash"] = "intentionally-invalid-manifest-hash";
	bundle_a["resources"] = bundle_a_resources;

	Dictionary bundle_b;
	bundle_b["name"] = "bundle_b";
	bundle_b["path"] = "bundle_b";
	bundle_b["resources"] = bundle_b_resources;

	Array bundles;
	bundles.push_back(bundle_a);
	bundles.push_back(bundle_b);
	Dictionary manifest;
	manifest["bundles"] = bundles;
	const String manifest_path = root_path.path_join("manifest.json");
	write_text_file(manifest_path, JSON::stringify(manifest, "\t", true));

	Ref<AssetBundle> asset_bundle;
	asset_bundle.instantiate();
	REQUIRE(asset_bundle->load_manifest(manifest_path) == OK);

	Dictionary all_results = asset_bundle->verify_all_bundles(true, true);
	Array all_bundles = all_results["bundles"];
	REQUIRE(all_bundles.size() == 2);
	CHECK(JSON::stringify(all_bundles[0]) == JSON::stringify(asset_bundle->verify_bundle("bundle_a", true, true)));
	CHECK(JSON::stringify(all_bundles[1]) == JSON::stringify(asset_bundle->verify_bundle("bundle_b", true, true)));
	CHECK(int(all_results["invalid_bundles"]) == 2);
	CHECK(int(all_results["missing_chunks"]) == 1);

	Dictionary size_only_results = asset_bundle->verify_all_bundles(false, false);
	Array size_only_bundles = size_only_results["bundles"];
	REQUIRE(size_only_bundles.size() == 2);
	CHECK(bool(Dictionary(size_only_bundles[0])["valid"]));
	CHECK(!bool(Dictionary(size_only_bundles[1])["valid"]));
}

TEST_CASE("[AssetBundle] Batch loading hot replaces cached resources") {
	const String root_path = TestUtils::get_temp_path("asset_bundle_batch_load");
	REQUIRE(DirAccess::make_dir_recursive_absolute(root_path) == OK);

	Array bundles;
	Vector<Ref<Resource>> cached_resources;
	Vector<String> expected_names;
	for (int bundle_index = 0; bundle_index < 2; bundle_index++) {
		const String bundle_name = "load_bundle_" + itos(bundle_index);
		const String bundle_path = root_path.path_join(bundle_name);
		REQUIRE(DirAccess::make_dir_recursive_absolute(bundle_path) == OK);

		Array chunks;
		for (int resource_index = 0; resource_index < 2; resource_index++) {
			const String resource_path = vformat("res://asset_bundle_batch_load/resource_%d_%d.tres", bundle_index, resource_index);
			const String resource_name = vformat("loaded_%d_%d", bundle_index, resource_index);
			const String chunk_name = vformat("resource_%d.tres.ab", resource_index);
			const String resource_text = vformat("[gd_resource type=\"Resource\" format=3]\n\n[resource]\nresource_name = \"%s\"\n", resource_name);
			write_text_file(bundle_path.path_join(chunk_name), resource_text);
			chunks.push_back(make_resource_entry(resource_path, chunk_name, resource_text));

			Ref<Resource> cached_resource;
			cached_resource.instantiate();
			cached_resource->set_name("old");
			cached_resource->set_path(resource_path);
			cached_resources.push_back(cached_resource);
			expected_names.push_back(resource_name);
		}

		Dictionary bundle_manifest;
		bundle_manifest["chunks"] = chunks;
		write_text_file(bundle_path.path_join("bundle.json"), JSON::stringify(bundle_manifest, "\t", true));

		Dictionary bundle;
		bundle["name"] = bundle_name;
		bundle["path"] = bundle_name;
		bundle["chunks"] = chunks;
		bundles.push_back(bundle);
	}

	Dictionary manifest;
	manifest["bundles"] = bundles;
	const String manifest_path = root_path.path_join("manifest.json");
	write_text_file(manifest_path, JSON::stringify(manifest, "\t", true));

	Ref<AssetBundle> asset_bundle;
	asset_bundle.instantiate();
	REQUIRE(asset_bundle->load_manifest(manifest_path) == OK);

	PackedStringArray first_bundle;
	first_bundle.push_back("load_bundle_0");
	REQUIRE(asset_bundle->load_bundles(first_bundle) == OK);
	CHECK(cached_resources[0]->get_name() == expected_names[0]);
	CHECK(cached_resources[1]->get_name() == expected_names[1]);
	CHECK(cached_resources[2]->get_name() == "old");
	CHECK(cached_resources[3]->get_name() == "old");

	REQUIRE(asset_bundle->load_all_bundles() == OK);
	for (int i = 0; i < cached_resources.size(); i++) {
		CHECK(cached_resources[i]->get_name() == expected_names[i]);
	}
}

TEST_CASE("[AssetBundle] Flat bundles load and unload with previous mappings restored") {
	const String root_path = TestUtils::get_temp_path("asset_bundle_flat_unload");
	REQUIRE(DirAccess::make_dir_recursive_absolute(root_path) == OK);

	const String resource_path = "res://asset_bundle_flat_unload/value.txt";
	const String base_chunk_name = String("1").repeat(64) + ".ab";
	const String override_chunk_name = String("2").repeat(64) + ".ab";
	write_text_file(root_path.path_join(base_chunk_name), "base");
	write_text_file(root_path.path_join(override_chunk_name), "override");

	Array base_chunks;
	base_chunks.push_back(make_resource_entry(resource_path, base_chunk_name, "base"));
	Dictionary base_manifest;
	base_manifest["flat_layout"] = true;
	base_manifest["chunks"] = base_chunks;
	write_text_file(root_path.path_join("base.json"), JSON::stringify(base_manifest, "\t", true));

	Array override_chunks;
	override_chunks.push_back(make_resource_entry(resource_path, override_chunk_name, "override"));
	Dictionary override_manifest;
	override_manifest["flat_layout"] = true;
	override_manifest["chunks"] = override_chunks;
	write_text_file(root_path.path_join("override.json"), JSON::stringify(override_manifest, "\t", true));

	Dictionary base_bundle;
	base_bundle["name"] = "base";
	base_bundle["path"] = "base.json";
	base_bundle["chunks"] = base_chunks;
	Dictionary override_bundle;
	override_bundle["name"] = "override";
	override_bundle["path"] = "override.json";
	override_bundle["chunks"] = override_chunks;

	Array bundles;
	bundles.push_back(base_bundle);
	bundles.push_back(override_bundle);
	Dictionary manifest;
	manifest["flat_layout"] = true;
	manifest["bundles"] = bundles;
	const String manifest_path = root_path.path_join("manifest.json");
	write_text_file(manifest_path, JSON::stringify(manifest, "\t", true));

	Ref<AssetBundle> asset_bundle;
	asset_bundle.instantiate();
	REQUIRE(asset_bundle->load_manifest(manifest_path) == OK);
	CHECK(asset_bundle->get_bundle_manifest_path("base") == root_path.path_join("base.json"));
	CHECK(asset_bundle->get_chunk_file_path("base", resource_path) == root_path.path_join(base_chunk_name));

	REQUIRE(asset_bundle->load_bundle("base", false) == OK);
	CHECK(FileAccess::get_file_as_string(resource_path) == "base");
	REQUIRE(asset_bundle->load_bundle("override", false) == OK);
	CHECK(FileAccess::get_file_as_string(resource_path) == "override");
	REQUIRE(asset_bundle->unload_bundle("base", false) == OK);
	CHECK(FileAccess::get_file_as_string(resource_path) == "override");
	REQUIRE(asset_bundle->unload_bundle("override", false) == OK);
	CHECK_FALSE(FileAccess::exists(resource_path));

	REQUIRE(asset_bundle->load_bundle("base", false) == OK);
	REQUIRE(asset_bundle->load_bundle("override", false) == OK);
	REQUIRE(asset_bundle->unload_bundle("override", false) == OK);
	CHECK(FileAccess::get_file_as_string(resource_path) == "base");
	REQUIRE(asset_bundle->unload_bundle("base", false) == OK);
	CHECK_FALSE(FileAccess::exists(resource_path));

	REQUIRE(asset_bundle->load_bundle("base", false) == OK);
	REQUIRE(asset_bundle->load_bundle("override", false) == OK);
	REQUIRE(asset_bundle->unload_all_bundles(false) == OK);
	CHECK_FALSE(FileAccess::exists(resource_path));
}

TEST_CASE("[AssetBundle] Physical deletion preserves files shared by other bundles") {
	const String root_path = TestUtils::get_temp_path("asset_bundle_delete");
	REQUIRE(DirAccess::make_dir_recursive_absolute(root_path) == OK);

	write_text_file(root_path.path_join("target.ab"), "target");
	write_text_file(root_path.path_join("shared.ab"), "shared");
	write_text_file(root_path.path_join("other.ab"), "other");

	Array target_chunks;
	target_chunks.push_back(make_resource_entry("res://asset_bundle_delete/target.txt", "target.ab", "target"));
	target_chunks.push_back(make_resource_entry("res://asset_bundle_delete/target_shared.txt", "shared.ab", "shared"));
	Dictionary target_manifest;
	target_manifest["chunks"] = target_chunks;
	write_text_file(root_path.path_join("target.json"), JSON::stringify(target_manifest, "\t", true));

	Array other_chunks;
	other_chunks.push_back(make_resource_entry("res://asset_bundle_delete/other.txt", "other.ab", "other"));
	other_chunks.push_back(make_resource_entry("res://asset_bundle_delete/other_shared.txt", "shared.ab", "shared"));
	Dictionary other_manifest;
	other_manifest["chunks"] = other_chunks;
	write_text_file(root_path.path_join("other.json"), JSON::stringify(other_manifest, "\t", true));

	Dictionary target_bundle;
	target_bundle["name"] = "target";
	target_bundle["path"] = "target.json";
	target_bundle["chunks"] = target_chunks;
	Dictionary other_bundle;
	other_bundle["name"] = "other";
	other_bundle["path"] = "other.json";
	other_bundle["chunks"] = other_chunks;
	Array bundles;
	bundles.push_back(target_bundle);
	bundles.push_back(other_bundle);
	Dictionary manifest;
	manifest["bundles"] = bundles;
	const String manifest_path = root_path.path_join("manifest.json");
	write_text_file(manifest_path, JSON::stringify(manifest, "\t", true));

	Ref<AssetBundle> asset_bundle;
	asset_bundle.instantiate();
	REQUIRE(asset_bundle->load_manifest(manifest_path) == OK);
	REQUIRE(asset_bundle->delete_bundle("target") == OK);
	CHECK_FALSE(FileAccess::exists(root_path.path_join("target.json")));
	CHECK_FALSE(FileAccess::exists(root_path.path_join("target.ab")));
	CHECK(FileAccess::exists(root_path.path_join("shared.ab")));
	CHECK(FileAccess::exists(root_path.path_join("other.json")));
	CHECK(FileAccess::exists(root_path.path_join("other.ab")));
	CHECK(FileAccess::exists(manifest_path));
	CHECK(asset_bundle->delete_bundle("target") == OK);

	REQUIRE(asset_bundle->delete_all_bundles() == OK);
	CHECK_FALSE(FileAccess::exists(root_path.path_join("shared.ab")));
	CHECK_FALSE(FileAccess::exists(root_path.path_join("other.json")));
	CHECK_FALSE(FileAccess::exists(root_path.path_join("other.ab")));
	CHECK(FileAccess::exists(manifest_path));
}

} // namespace TestAssetBundle
