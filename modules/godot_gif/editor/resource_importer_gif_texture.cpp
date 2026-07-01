#include "resource_importer_gif_texture.h"

#include "../core/gif_reader.h"
#include "../core/gif_texture.h"

#include "core/io/file_access.h"
#include "core/io/resource_saver.h"

String ResourceImporterGIFTexture::get_importer_name() const {
	return "texture_gif";
}

String ResourceImporterGIFTexture::get_visible_name() const {
	return "GIF Texture";
}

void ResourceImporterGIFTexture::get_recognized_extensions(List<String> *p_extensions) const {
	p_extensions->push_back("gif");
}

String ResourceImporterGIFTexture::get_save_extension() const {
	return "res";
}

String ResourceImporterGIFTexture::get_resource_type() const {
	return "GIFTexture";
}

int ResourceImporterGIFTexture::get_preset_count() const {
	return 0;
}

String ResourceImporterGIFTexture::get_preset_name(int p_preset) const {
	return String();
}

void ResourceImporterGIFTexture::get_import_options(const String &p_path, List<ImportOption> *r_options, int p_preset) const {
	r_options->push_back(ImportOption(PropertyInfo(Variant::BOOL, "storage/compress"), true));
}

bool ResourceImporterGIFTexture::get_option_visibility(const String &p_path, const String &p_option, const HashMap<StringName, Variant> &p_options) const {
	return true;
}

int ResourceImporterGIFTexture::get_format_version() const {
	return 1;
}

Error ResourceImporterGIFTexture::import(ResourceUID::ID p_source_id, const String &p_source_file, const String &p_save_path, const HashMap<StringName, Variant> &p_options, List<String> *r_platform_variants, List<String> *r_gen_files, Variant *r_metadata) {
	Ref<FileAccess> file = FileAccess::open(p_source_file, FileAccess::READ);
	ERR_FAIL_COND_V_MSG(file.is_null(), ERR_CANT_OPEN, vformat("Cannot open GIF file \"%s\".", p_source_file));

	PackedByteArray gif_data = file->get_buffer(file->get_length());
	ERR_FAIL_COND_V_MSG(gif_data.is_empty(), ERR_FILE_CORRUPT, vformat("GIF file \"%s\" is empty.", p_source_file));

	Ref<GIFReader> reader;
	reader.instantiate();
	GIFReader::GIFError gif_error = reader->open_from_buffer(gif_data);
	ERR_FAIL_COND_V_MSG(gif_error != GIFReader::SUCCEEDED, ERR_FILE_CORRUPT, vformat("Cannot parse GIF file \"%s\". Error code: %d.", p_source_file, gif_error));

	const int frame_count = reader->get_image_count();
	ERR_FAIL_COND_V_MSG(frame_count <= 0, ERR_FILE_CORRUPT, vformat("GIF file \"%s\" does not contain any frame.", p_source_file));

	PackedFloat32Array frame_delays;
	frame_delays.resize(frame_count);
	float *frame_delays_ptr = frame_delays.ptrw();
	for (int frame_idx = 0; frame_idx < frame_count; frame_idx++) {
		frame_delays_ptr[frame_idx] = reader->get_frame_delay(frame_idx) / 1000.0f;
	}

	Ref<GIFTexture> gif_texture;
	gif_texture.instantiate();
	gif_texture->set_data(gif_data);
	gif_texture->set_metadata(reader->get_size(), frame_count, frame_delays, reader->get_loop_count());

	uint32_t save_flags = ResourceSaver::FLAG_NONE;
	if (p_options.has("storage/compress") && bool(p_options["storage/compress"])) {
		save_flags |= ResourceSaver::FLAG_COMPRESS;
	}

	return ResourceSaver::save(gif_texture, p_save_path + ".res", save_flags);
}
