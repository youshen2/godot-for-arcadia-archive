/**************************************************************************/
/*  ffmpeg_common.h                                                       */
/**************************************************************************/

#pragma once

#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/string/print_string.h"
#include "core/templates/vector.h"

#include <string.h>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/display.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

struct FFmpegAVFormatInputDeleter {
	void operator()(AVFormatContext *p_context) const {
		if (p_context) {
			avformat_close_input(&p_context);
		}
	}
};

struct FFmpegAVCodecContextDeleter {
	void operator()(AVCodecContext *p_context) const {
		if (p_context) {
			avcodec_free_context(&p_context);
		}
	}
};

struct FFmpegAVFrameDeleter {
	void operator()(AVFrame *p_frame) const {
		if (p_frame) {
			av_frame_free(&p_frame);
		}
	}
};

struct FFmpegAVPacketDeleter {
	void operator()(AVPacket *p_packet) const {
		if (p_packet) {
			av_packet_free(&p_packet);
		}
	}
};

struct FFmpegAVIOContextDeleter {
	void operator()(AVIOContext *p_context) const {
		if (!p_context) {
			return;
		}
		if (p_context->buffer) {
			av_free(p_context->buffer);
		}
		avio_context_free(&p_context);
	}
};

struct FFmpegSwrContextDeleter {
	void operator()(SwrContext *p_context) const {
		if (p_context) {
			swr_free(&p_context);
		}
	}
};

struct FFmpegSwsContextDeleter {
	void operator()(SwsContext *p_context) const {
		if (p_context) {
			sws_freeContext(p_context);
		}
	}
};

using FFmpegFormatContextPtr = std::unique_ptr<AVFormatContext, FFmpegAVFormatInputDeleter>;
using FFmpegCodecContextPtr = std::unique_ptr<AVCodecContext, FFmpegAVCodecContextDeleter>;
using FFmpegFramePtr = std::unique_ptr<AVFrame, FFmpegAVFrameDeleter>;
using FFmpegPacketPtr = std::unique_ptr<AVPacket, FFmpegAVPacketDeleter>;
using FFmpegAVIOContextPtr = std::unique_ptr<AVIOContext, FFmpegAVIOContextDeleter>;
using FFmpegSwrContextPtr = std::unique_ptr<SwrContext, FFmpegSwrContextDeleter>;
using FFmpegSwsContextPtr = std::unique_ptr<SwsContext, FFmpegSwsContextDeleter>;

struct FFmpegBufferData {
	uint8_t *ptr = nullptr;
	size_t size = 0;
	size_t offset = 0;
};

struct FFmpegInputContext {
	FFmpegFormatContextPtr format;
	FFmpegAVIOContextPtr avio;
	Vector<uint8_t> file_buffer;
	FFmpegBufferData buffer_data;

	void clear();
};

class FFmpegCommon {
public:
	static constexpr int AVIO_CONTEXT_BUFFER_SIZE = 4 * 1024 * 1024;

	static String error_string(int p_error);
	static void print_error(const char *p_message, int p_error);
	static void enable_multithreading(AVCodecContext *p_codec_context, const AVCodec *p_codec);
	static int get_frame(AVFormatContext *p_format_context, AVCodecContext *p_codec_context, int p_stream_id, AVFrame *p_frame, AVPacket *p_packet);
	static Error open_input(FFmpegInputContext &r_input, const String &p_path, const String &p_headers = String(), bool p_icy = false);
	static int read_buffer_packet(void *p_opaque, uint8_t *p_buffer, int p_buffer_size);
	static int64_t seek_buffer(void *p_opaque, int64_t p_offset, int p_whence);
};
