/**************************************************************************/
/*  ffmpeg_common.cpp                                                     */
/**************************************************************************/

#include "ffmpeg_common.h"

void FFmpegInputContext::clear() {
	format.reset();
	avio.reset();
	file.unref();
}

void FFmpegOutputContext::clear() {
	if (format) {
		format->pb = nullptr;
	}
	format.reset();
	avio.reset();
	file.unref();
}

String FFmpegCommon::error_string(int p_error) {
	char error_buffer[AV_ERROR_MAX_STRING_SIZE];
	av_strerror(p_error, error_buffer, sizeof(error_buffer));
	return String(error_buffer);
}

void FFmpegCommon::print_error(const char *p_message, int p_error) {
	ERR_PRINT(vformat("%s: %s", p_message, error_string(p_error)));
}

void FFmpegCommon::enable_multithreading(AVCodecContext *p_codec_context, const AVCodec *p_codec) {
	ERR_FAIL_NULL(p_codec_context);
	ERR_FAIL_NULL(p_codec);

	p_codec_context->thread_count = MAX(1, OS::get_singleton()->get_processor_count() - 1);

	if (p_codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
		p_codec_context->thread_type = FF_THREAD_FRAME;
	} else if (p_codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
		p_codec_context->thread_type = FF_THREAD_SLICE;
	} else {
		p_codec_context->thread_count = 1;
	}
}

int FFmpegCommon::get_frame(AVFormatContext *p_format_context, AVCodecContext *p_codec_context, int p_stream_id, AVFrame *p_frame, AVPacket *p_packet) {
	ERR_FAIL_NULL_V(p_format_context, AVERROR(EINVAL));
	ERR_FAIL_NULL_V(p_codec_context, AVERROR(EINVAL));
	ERR_FAIL_NULL_V(p_frame, AVERROR(EINVAL));
	ERR_FAIL_NULL_V(p_packet, AVERROR(EINVAL));

	int response = 0;
	bool eof = false;

	av_frame_unref(p_frame);
	while ((response = avcodec_receive_frame(p_codec_context, p_frame)) == AVERROR(EAGAIN) && !eof) {
		do {
			av_packet_unref(p_packet);
			response = av_read_frame(p_format_context, p_packet);
		} while (response >= 0 && p_packet->stream_index != p_stream_id);

		if (response == AVERROR_EOF) {
			eof = true;
			response = avcodec_send_packet(p_codec_context, nullptr);
		} else if (response < 0) {
			return response;
		} else {
			response = avcodec_send_packet(p_codec_context, p_packet);
			if (response < 0 && response != AVERROR_INVALIDDATA) {
				return response;
			}
		}
		av_frame_unref(p_frame);
	}

	return response;
}

Error FFmpegCommon::open_input(FFmpegInputContext &r_input, const String &p_path, const String &p_headers, bool p_icy) {
	r_input.clear();

	AVFormatContext *raw_format_context = nullptr;
	AVDictionary *options = nullptr;

	if (!p_headers.is_empty()) {
		CharString headers_utf8 = p_headers.utf8();
		av_dict_set(&options, "headers", headers_utf8.get_data(), 0);
	}
	if (p_icy) {
		av_dict_set(&options, "icy", "1", 0);
	}
	if (p_path.begins_with("rtsp://")) {
		av_dict_set(&options, "rtsp_transport", "tcp", 0);
	}

	if (p_path.begins_with("res://") || p_path.begins_with("user://")) {
		raw_format_context = avformat_alloc_context();
		if (!raw_format_context) {
			if (options) {
				av_dict_free(&options);
			}
			return ERR_OUT_OF_MEMORY;
		}

		Error err = OK;
		r_input.file = FileAccess::open(p_path, FileAccess::READ, &err);
		if (err != OK || r_input.file.is_null() || r_input.file->get_length() == 0 || r_input.file->get_length() > uint64_t(INT64_MAX)) {
			if (options) {
				av_dict_free(&options);
			}
			avformat_free_context(raw_format_context);
			r_input.clear();
			return err == OK ? ERR_FILE_CANT_READ : err;
		}

		unsigned char *avio_buffer = static_cast<unsigned char *>(av_malloc(AVIO_CONTEXT_BUFFER_SIZE));
		if (!avio_buffer) {
			if (options) {
				av_dict_free(&options);
			}
			avformat_free_context(raw_format_context);
			r_input.clear();
			return ERR_OUT_OF_MEMORY;
		}

		AVIOContext *raw_avio = avio_alloc_context(avio_buffer, AVIO_CONTEXT_BUFFER_SIZE, 0, &r_input.file, &FFmpegCommon::read_file_packet, nullptr, &FFmpegCommon::seek_file);
		if (!raw_avio) {
			if (options) {
				av_dict_free(&options);
			}
			av_free(avio_buffer);
			avformat_free_context(raw_format_context);
			r_input.clear();
			return ERR_OUT_OF_MEMORY;
		}

		r_input.avio = FFmpegAVIOContextPtr(raw_avio);
		raw_format_context->pb = r_input.avio.get();

		int response = avformat_open_input(&raw_format_context, nullptr, nullptr, &options);
		if (options) {
			av_dict_free(&options);
		}
		if (response < 0) {
			print_error("FFmpeg failed to open file input", response);
			avformat_free_context(raw_format_context);
			r_input.clear();
			return ERR_CANT_OPEN;
		}
	} else {
		CharString path_utf8 = p_path.utf8();
		int response = avformat_open_input(&raw_format_context, path_utf8.get_data(), nullptr, &options);
		if (options) {
			av_dict_free(&options);
		}
		if (response < 0) {
			print_error("FFmpeg failed to open input", response);
			return ERR_CANT_OPEN;
		}
	}

	r_input.format = FFmpegFormatContextPtr(raw_format_context);

	int response = avformat_find_stream_info(r_input.format.get(), nullptr);
	if (response < 0) {
		print_error("FFmpeg failed to read stream info", response);
		r_input.clear();
		return ERR_FILE_CORRUPT;
	}

	return OK;
}

Error FFmpegCommon::open_output(FFmpegOutputContext &r_output, const String &p_path) {
	r_output.clear();

	CharString path_utf8 = p_path.utf8();
	AVFormatContext *raw_format_context = nullptr;
	int response = avformat_alloc_output_context2(&raw_format_context, nullptr, nullptr, path_utf8.get_data());
	if (response < 0 || !raw_format_context) {
		print_error("FFmpeg failed to select an output container", response < 0 ? response : AVERROR(EINVAL));
		return ERR_INVALID_PARAMETER;
	}
	r_output.format = FFmpegOutputFormatContextPtr(raw_format_context);

	Error err = OK;
	r_output.file = FileAccess::open(p_path, FileAccess::WRITE, &err);
	if (err != OK || r_output.file.is_null()) {
		r_output.clear();
		return err == OK ? ERR_CANT_OPEN : err;
	}

	unsigned char *avio_buffer = static_cast<unsigned char *>(av_malloc(AVIO_CONTEXT_BUFFER_SIZE));
	if (!avio_buffer) {
		r_output.clear();
		return ERR_OUT_OF_MEMORY;
	}

	AVIOContext *raw_avio = avio_alloc_context(avio_buffer, AVIO_CONTEXT_BUFFER_SIZE, 1, &r_output.file, nullptr, &FFmpegCommon::write_file_packet, &FFmpegCommon::seek_file);
	if (!raw_avio) {
		av_free(avio_buffer);
		r_output.clear();
		return ERR_OUT_OF_MEMORY;
	}

	r_output.avio = FFmpegAVIOContextPtr(raw_avio);
	r_output.format->pb = r_output.avio.get();
	r_output.format->flags |= AVFMT_FLAG_CUSTOM_IO;
	return OK;
}

int FFmpegCommon::read_file_packet(void *p_opaque, uint8_t *p_buffer, int p_buffer_size) {
	Ref<FileAccess> *file = static_cast<Ref<FileAccess> *>(p_opaque);
	if (file == nullptr || file->is_null() || p_buffer == nullptr || p_buffer_size < 0) {
		return AVERROR(EINVAL);
	}
	if (p_buffer_size == 0) {
		return 0;
	}

	const uint64_t bytes_read = (*file)->get_buffer(p_buffer, p_buffer_size);
	if (bytes_read > 0) {
		return static_cast<int>(bytes_read);
	}
	return (*file)->eof_reached() ? AVERROR_EOF : AVERROR(EIO);
}

int FFmpegCommon::write_file_packet(void *p_opaque, const uint8_t *p_buffer, int p_buffer_size) {
	Ref<FileAccess> *file = static_cast<Ref<FileAccess> *>(p_opaque);
	if (file == nullptr || file->is_null() || p_buffer == nullptr || p_buffer_size < 0) {
		return AVERROR(EINVAL);
	}
	if (p_buffer_size == 0) {
		return 0;
	}

	(*file)->store_buffer(p_buffer, p_buffer_size);
	return (*file)->get_error() == OK ? p_buffer_size : AVERROR(EIO);
}

int64_t FFmpegCommon::seek_file(void *p_opaque, int64_t p_offset, int p_whence) {
	Ref<FileAccess> *file = static_cast<Ref<FileAccess> *>(p_opaque);
	if (file == nullptr || file->is_null()) {
		return -1;
	}

	p_whence &= ~AVSEEK_FORCE;
	if (p_whence == AVSEEK_SIZE) {
		return (*file)->get_length();
	}

	int64_t new_offset = 0;
	switch (p_whence) {
		case SEEK_SET:
			new_offset = p_offset;
			break;
		case SEEK_CUR:
			new_offset = static_cast<int64_t>((*file)->get_position()) + p_offset;
			break;
		case SEEK_END:
			new_offset = static_cast<int64_t>((*file)->get_length()) + p_offset;
			break;
		default:
			return -1;
	}

	if (new_offset < 0 || new_offset > static_cast<int64_t>((*file)->get_length())) {
		return -1;
	}

	(*file)->seek(new_offset);
	return (*file)->get_position() == uint64_t(new_offset) ? new_offset : -1;
}
