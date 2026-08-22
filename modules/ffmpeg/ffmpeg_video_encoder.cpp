/**************************************************************************/
/*  ffmpeg_video_encoder.cpp                                              */
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

#include "ffmpeg_video_encoder.h"

#include "core/math/math_funcs.h"

struct FFmpegHardwareEncoderCandidate {
	AVCodecID codec_id;
	const char *name;
};

static const FFmpegHardwareEncoderCandidate ffmpeg_hardware_encoder_candidates[] = {
#if defined(MACOS_ENABLED) || defined(IOS_ENABLED) || defined(VISIONOS_ENABLED)
	{ AV_CODEC_ID_H264, "h264_videotoolbox" },
	{ AV_CODEC_ID_HEVC, "hevc_videotoolbox" },
#elif defined(WINDOWS_ENABLED)
	{ AV_CODEC_ID_H264, "h264_mf" },
	{ AV_CODEC_ID_HEVC, "hevc_mf" },
	{ AV_CODEC_ID_AV1, "av1_mf" },
#elif defined(ANDROID_ENABLED)
	{ AV_CODEC_ID_H264, "h264_mediacodec" },
	{ AV_CODEC_ID_HEVC, "hevc_mediacodec" },
	{ AV_CODEC_ID_AV1, "av1_mediacodec" },
	{ AV_CODEC_ID_VP9, "vp9_mediacodec" },
#elif defined(LINUXBSD_ENABLED)
	{ AV_CODEC_ID_H264, "h264_vaapi" },
	{ AV_CODEC_ID_HEVC, "hevc_vaapi" },
	{ AV_CODEC_ID_AV1, "av1_vaapi" },
	{ AV_CODEC_ID_VP9, "vp9_vaapi" },
#endif
	{ AV_CODEC_ID_NONE, nullptr },
};

static String _ffmpeg_get_encoder_backend(const AVCodec *p_codec) {
	if (!p_codec || !p_codec->name) {
		return "software";
	}

	const String name = String::utf8(p_codec->name);
	if (name.contains("videotoolbox")) {
		return "videotoolbox";
	}
	if (name.ends_with("_mf")) {
		return "mediafoundation";
	}
	if (name.contains("mediacodec")) {
		return "mediacodec";
	}
	if (name.contains("vaapi")) {
		return "vaapi";
	}
	if ((p_codec->capabilities & AV_CODEC_CAP_HARDWARE) || (p_codec->capabilities & AV_CODEC_CAP_HYBRID)) {
		return name;
	}
	return "software";
}

static bool _ffmpeg_is_hardware_encoder(const AVCodec *p_codec) {
	return _ffmpeg_get_encoder_backend(p_codec) != "software";
}

static const AVCodec *_ffmpeg_find_hardware_encoder(const AVOutputFormat *p_output_format) {
	ERR_FAIL_NULL_V(p_output_format, nullptr);

	for (int pass = 0; pass < 2; pass++) {
		for (const FFmpegHardwareEncoderCandidate *candidate = ffmpeg_hardware_encoder_candidates; candidate->name; candidate++) {
			if (pass == 0 && candidate->codec_id != p_output_format->video_codec) {
				continue;
			}
			const AVCodec *codec = avcodec_find_encoder_by_name(candidate->name);
			if (codec && avformat_query_codec(p_output_format, codec->id, FF_COMPLIANCE_NORMAL) > 0) {
				return codec;
			}
		}
	}
	return nullptr;
}

static int _ffmpeg_choose_encoder_pixel_format(const AVCodec *p_codec, bool p_allow_hardware_format, AVPixelFormat &r_pixel_format) {
	const void *pixel_format_config = nullptr;
	int pixel_format_count = 0;
	const int result = avcodec_get_supported_config(nullptr, p_codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &pixel_format_config, &pixel_format_count);
	if (result < 0) {
		return result;
	}

	const AVPixelFormat *pixel_formats = static_cast<const AVPixelFormat *>(pixel_format_config);
	if (!pixel_formats || pixel_format_count <= 0) {
		r_pixel_format = AV_PIX_FMT_YUV420P;
		return 0;
	}

	AVPixelFormat software_format = AV_PIX_FMT_NONE;
	AVPixelFormat hardware_format = AV_PIX_FMT_NONE;
	for (int i = 0; i < pixel_format_count; i++) {
		const AVPixFmtDescriptor *descriptor = av_pix_fmt_desc_get(pixel_formats[i]);
		if (descriptor && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
			if (hardware_format == AV_PIX_FMT_NONE) {
				hardware_format = pixel_formats[i];
			}
			continue;
		}
		if (!sws_isSupportedOutput(pixel_formats[i])) {
			continue;
		}
		if (pixel_formats[i] == AV_PIX_FMT_YUV420P) {
			r_pixel_format = pixel_formats[i];
			return 0;
		}
		if (software_format == AV_PIX_FMT_NONE || pixel_formats[i] == AV_PIX_FMT_NV12) {
			software_format = pixel_formats[i];
		}
	}

	if (software_format != AV_PIX_FMT_NONE) {
		r_pixel_format = software_format;
		return 0;
	}
	if (p_allow_hardware_format && hardware_format != AV_PIX_FMT_NONE) {
		r_pixel_format = hardware_format;
		return 0;
	}
	r_pixel_format = AV_PIX_FMT_NONE;
	return 0;
}

static AVPixelFormat _ffmpeg_choose_hardware_transfer_format(AVBufferRef *p_device_context) {
	AVPixelFormat result = AV_PIX_FMT_NV12;
	AVHWFramesConstraints *constraints = av_hwdevice_get_hwframe_constraints(p_device_context, nullptr);
	if (!constraints || !constraints->valid_sw_formats) {
		av_hwframe_constraints_free(&constraints);
		return result;
	}

	result = AV_PIX_FMT_NONE;
	for (const AVPixelFormat *format = constraints->valid_sw_formats; *format != AV_PIX_FMT_NONE; format++) {
		if (!sws_isSupportedOutput(*format)) {
			continue;
		}
		if (*format == AV_PIX_FMT_NV12) {
			result = *format;
			break;
		}
		if (result == AV_PIX_FMT_NONE || *format == AV_PIX_FMT_YUV420P) {
			result = *format;
		}
	}
	av_hwframe_constraints_free(&constraints);
	return result;
}

Error FFmpegVideoEncoder::_set_error(Error p_error, const String &p_message, int p_ffmpeg_error) {
	last_error_message = p_message;
	if (p_ffmpeg_error < 0) {
		last_error_message += ": " + FFmpegCommon::error_string(p_ffmpeg_error);
	}
	ERR_PRINT(last_error_message);
	return p_error;
}

void FFmpegVideoEncoder::_clear() {
	if (audio_fifo) {
		av_audio_fifo_free(audio_fifo);
		audio_fifo = nullptr;
	}
	swr_context.reset();
	sws_context.reset();
	hardware_video_frame.reset();
	video_frame.reset();
	audio_codec_context.reset();
	video_codec_context.reset();
	hardware_frames_context.reset();
	hardware_device_context.reset();
	video_stream = nullptr;
	audio_stream = nullptr;
	output.clear();
	active = false;
	audio_enabled = false;
}

Error FFmpegVideoEncoder::_create_video_stream(const String &p_requested_codec, int64_t p_video_bitrate, const String &p_preset, uint32_t p_keyframe_interval, EncoderMode p_encoder_mode) {
	const AVCodec *codec = nullptr;
	if (!p_requested_codec.is_empty()) {
		CharString requested_codec_utf8 = p_requested_codec.utf8();
		codec = avcodec_find_encoder_by_name(requested_codec_utf8.get_data());
		if (!codec) {
			return _set_error(ERR_UNAVAILABLE, vformat("FFmpeg video encoder '%s' is not available in this build", p_requested_codec));
		}
	} else if (p_encoder_mode != ENCODER_MODE_SOFTWARE) {
		codec = _ffmpeg_find_hardware_encoder(output.format->oformat);
		if (!codec && p_encoder_mode == ENCODER_MODE_HARDWARE) {
			return _set_error(ERR_UNAVAILABLE, "FFmpeg could not find a platform hardware video encoder supported by the selected container");
		}
	}
	if (!codec && output.format->oformat->video_codec != AV_CODEC_ID_NONE) {
		codec = avcodec_find_encoder(output.format->oformat->video_codec);
	}

	if (!p_requested_codec.is_empty() && avformat_query_codec(output.format->oformat, codec->id, FF_COMPLIANCE_NORMAL) <= 0) {
		return _set_error(ERR_INVALID_PARAMETER, vformat("FFmpeg video encoder '%s' is not supported by the selected container", p_requested_codec));
	}
	if (codec && p_encoder_mode == ENCODER_MODE_SOFTWARE && _ffmpeg_is_hardware_encoder(codec)) {
		if (!p_requested_codec.is_empty()) {
			return _set_error(ERR_INVALID_PARAMETER, vformat("FFmpeg encoder '%s' is a hardware encoder, but software encoding was requested", p_requested_codec));
		}
		codec = nullptr;
	}
	if (!codec || avformat_query_codec(output.format->oformat, codec->id, FF_COMPLIANCE_NORMAL) <= 0) {
		codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
	}
	if (!codec || avformat_query_codec(output.format->oformat, codec->id, FF_COMPLIANCE_NORMAL) <= 0) {
		return _set_error(ERR_UNAVAILABLE, "FFmpeg could not find a software video encoder supported by the selected container");
	}

	const bool selected_hardware_encoder = _ffmpeg_is_hardware_encoder(codec);
	if (p_encoder_mode == ENCODER_MODE_HARDWARE && !selected_hardware_encoder) {
		return _set_error(ERR_INVALID_PARAMETER, vformat("FFmpeg encoder '%s' does not provide hardware encoding", String(codec->name)));
	}

	AVPixelFormat pixel_format = AV_PIX_FMT_NONE;
	int response = _ffmpeg_choose_encoder_pixel_format(codec, selected_hardware_encoder, pixel_format);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to query the video encoder pixel formats", response);
	}
	const AVPixFmtDescriptor *pixel_descriptor = av_pix_fmt_desc_get(pixel_format);
	if (!selected_hardware_encoder && (!pixel_descriptor || (pixel_descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL))) {
		codec = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
		if (!codec || avformat_query_codec(output.format->oformat, codec->id, FF_COMPLIANCE_NORMAL) <= 0) {
			return _set_error(ERR_UNAVAILABLE, "FFmpeg could not find a CPU video encoder supported by the selected container");
		}
		response = _ffmpeg_choose_encoder_pixel_format(codec, false, pixel_format);
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to query the fallback video encoder pixel formats", response);
		}
		pixel_descriptor = av_pix_fmt_desc_get(pixel_format);
	}
	if (!pixel_descriptor) {
		return _set_error(ERR_UNAVAILABLE, vformat("FFmpeg encoder '%s' does not expose a usable pixel format", String(codec->name)));
	}
	const bool requires_hardware_frames = pixel_descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL;
	AVPixelFormat input_pixel_format = pixel_format;

	AVCodecContext *raw_codec_context = avcodec_alloc_context3(codec);
	if (!raw_codec_context) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the video encoder context");
	}
	video_codec_context = FFmpegCodecContextPtr(raw_codec_context);

	video_codec_context->codec_id = codec->id;
	video_codec_context->width = output_size.width;
	video_codec_context->height = output_size.height;
	video_codec_context->time_base = AVRational{ 1, int(fps) };
	video_codec_context->framerate = AVRational{ int(fps), 1 };
	video_codec_context->pix_fmt = pixel_format;
	video_codec_context->bit_rate = p_video_bitrate;
	video_codec_context->gop_size = p_keyframe_interval > 0 ? p_keyframe_interval : fps * 2;
	video_codec_context->max_b_frames = selected_hardware_encoder || codec->id == AV_CODEC_ID_MPEG4 ? 0 : 2;
	video_codec_context->color_range = AVCOL_RANGE_MPEG;
	video_codec_context->color_primaries = AVCOL_PRI_BT709;
	video_codec_context->color_trc = AVCOL_TRC_BT709;
	video_codec_context->colorspace = AVCOL_SPC_BT709;
	if (output.format->oformat->flags & AVFMT_GLOBALHEADER) {
		video_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	FFmpegCommon::enable_multithreading(video_codec_context.get(), codec);

	if (requires_hardware_frames) {
		const AVCodecHWConfig *hardware_config = nullptr;
		for (int config_index = 0;; config_index++) {
			const AVCodecHWConfig *candidate_config = avcodec_get_hw_config(codec, config_index);
			if (!candidate_config) {
				break;
			}
			if (candidate_config->pix_fmt == pixel_format && (candidate_config->methods & (AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX | AV_CODEC_HW_CONFIG_METHOD_HW_FRAMES_CTX))) {
				hardware_config = candidate_config;
				break;
			}
		}
		if (!hardware_config) {
			return _set_error(ERR_UNAVAILABLE, vformat("FFmpeg encoder '%s' does not expose a usable hardware frame configuration", String(codec->name)));
		}

		AVBufferRef *raw_device_context = nullptr;
		response = av_hwdevice_ctx_create(&raw_device_context, hardware_config->device_type, nullptr, nullptr, 0);
		if (response < 0 || !raw_device_context) {
			const char *device_name = av_hwdevice_get_type_name(hardware_config->device_type);
			return _set_error(ERR_CANT_CREATE, vformat("FFmpeg failed to create the '%s' hardware encoding device", device_name ? String::utf8(device_name) : String("unknown")), response);
		}
		hardware_device_context = FFmpegAVBufferRefPtr(raw_device_context);
		input_pixel_format = _ffmpeg_choose_hardware_transfer_format(hardware_device_context.get());
		if (input_pixel_format == AV_PIX_FMT_NONE) {
			return _set_error(ERR_UNAVAILABLE, vformat("FFmpeg hardware encoder '%s' does not accept a CPU-transferable pixel format", String(codec->name)));
		}

		AVBufferRef *raw_frames_context = av_hwframe_ctx_alloc(hardware_device_context.get());
		if (!raw_frames_context) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the hardware encoding frame pool");
		}
		hardware_frames_context = FFmpegAVBufferRefPtr(raw_frames_context);
		AVHWFramesContext *frames_context = reinterpret_cast<AVHWFramesContext *>(hardware_frames_context->data);
		frames_context->format = pixel_format;
		frames_context->sw_format = input_pixel_format;
		frames_context->width = output_size.width;
		frames_context->height = output_size.height;
		frames_context->initial_pool_size = 32;
		response = av_hwframe_ctx_init(hardware_frames_context.get());
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to initialize the hardware encoding frame pool", response);
		}
		video_codec_context->hw_device_ctx = av_buffer_ref(hardware_device_context.get());
		video_codec_context->hw_frames_ctx = av_buffer_ref(hardware_frames_context.get());
		if (!video_codec_context->hw_device_ctx || !video_codec_context->hw_frames_ctx) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to reference the hardware encoding contexts");
		}
	}

	AVDictionary *codec_options = nullptr;
	if (!p_preset.is_empty()) {
		CharString preset_utf8 = p_preset.utf8();
		av_dict_set(&codec_options, "preset", preset_utf8.get_data(), 0);
	}
	const String selected_backend = _ffmpeg_get_encoder_backend(codec);
	if (selected_backend == "mediafoundation") {
		av_dict_set(&codec_options, "hw_encoding", "1", 0);
	} else if (selected_backend == "videotoolbox") {
		av_dict_set(&codec_options, "allow_sw", "0", 0);
	}
	response = avcodec_open2(video_codec_context.get(), codec, &codec_options);
	av_dict_free(&codec_options);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, vformat("FFmpeg failed to open video encoder '%s'", String(codec->name)), response);
	}

	AVFrame *raw_video_frame = av_frame_alloc();
	if (!raw_video_frame) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the video frame");
	}
	video_frame = FFmpegFramePtr(raw_video_frame);
	video_frame->format = input_pixel_format;
	video_frame->width = output_size.width;
	video_frame->height = output_size.height;
	response = av_frame_get_buffer(video_frame.get(), 32);
	if (response < 0) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the video frame buffer", response);
	}

	if (requires_hardware_frames) {
		AVFrame *raw_hardware_frame = av_frame_alloc();
		if (!raw_hardware_frame) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the hardware video frame");
		}
		hardware_video_frame = FFmpegFramePtr(raw_hardware_frame);
	}

	SwsContext *raw_sws_context = sws_getContext(output_size.width, output_size.height, AV_PIX_FMT_RGBA, output_size.width, output_size.height, input_pixel_format, SWS_BICUBIC, nullptr, nullptr, nullptr);
	if (!raw_sws_context) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to create the image conversion context");
	}
	sws_context = FFmpegSwsContextPtr(raw_sws_context);

	AVStream *stream = avformat_new_stream(output.format.get(), nullptr);
	if (!stream) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the video stream");
	}

	stream->time_base = video_codec_context->time_base;
	stream->avg_frame_rate = video_codec_context->framerate;
	response = avcodec_parameters_from_context(stream->codecpar, video_codec_context.get());
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to copy the video stream parameters", response);
	}
	stream->codecpar->codec_tag = 0;
	video_stream = stream;
	codec_name = String(codec->name);
	hardware_encoding = selected_hardware_encoder;
	encoder_backend = selected_backend;
	return OK;
}

Error FFmpegVideoEncoder::_create_audio_stream(uint32_t p_mix_rate, int64_t p_audio_bitrate) {
	if (output.format->oformat->audio_codec == AV_CODEC_ID_NONE) {
		return _set_error(ERR_UNAVAILABLE, "The selected FFmpeg container does not define an audio codec");
	}
	const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
	if (!codec || avformat_query_codec(output.format->oformat, codec->id, FF_COMPLIANCE_NORMAL) <= 0) {
		codec = avcodec_find_encoder(output.format->oformat->audio_codec);
	}
	if (!codec) {
		return _set_error(ERR_UNAVAILABLE, "The FFmpeg audio encoder required by the selected container is not available");
	}

	AVStream *stream = avformat_new_stream(output.format.get(), nullptr);
	if (!stream) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the audio stream");
	}
	AVCodecContext *raw_codec_context = avcodec_alloc_context3(codec);
	if (!raw_codec_context) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the audio encoder context");
	}
	audio_codec_context = FFmpegCodecContextPtr(raw_codec_context);
	audio_codec_context->codec_id = codec->id;
	audio_codec_context->bit_rate = p_audio_bitrate;

	const void *sample_rate_config = nullptr;
	int sample_rate_count = 0;
	int response = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_RATE, 0, &sample_rate_config, &sample_rate_count);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to query the audio encoder sample rates", response);
	}
	audio_codec_context->sample_rate = p_mix_rate;
	const int *sample_rates = static_cast<const int *>(sample_rate_config);
	if (sample_rates && sample_rate_count > 0) {
		audio_codec_context->sample_rate = sample_rates[0];
		for (int i = 1; i < sample_rate_count; i++) {
			if (Math::abs(sample_rates[i] - int(p_mix_rate)) < Math::abs(audio_codec_context->sample_rate - int(p_mix_rate))) {
				audio_codec_context->sample_rate = sample_rates[i];
			}
		}
	}

	const void *sample_format_config = nullptr;
	int sample_format_count = 0;
	response = avcodec_get_supported_config(nullptr, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &sample_format_config, &sample_format_count);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to query the audio encoder sample formats", response);
	}
	const AVSampleFormat *sample_formats = static_cast<const AVSampleFormat *>(sample_format_config);
	audio_codec_context->sample_fmt = sample_formats && sample_format_count > 0 ? sample_formats[0] : AV_SAMPLE_FMT_FLTP;
	av_channel_layout_default(&audio_codec_context->ch_layout, 2);
	audio_codec_context->time_base = AVRational{ 1, audio_codec_context->sample_rate };
	if (output.format->oformat->flags & AVFMT_GLOBALHEADER) {
		audio_codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	}
	FFmpegCommon::enable_multithreading(audio_codec_context.get(), codec);

	response = avcodec_open2(audio_codec_context.get(), codec, nullptr);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, vformat("FFmpeg failed to open audio encoder '%s'", String(codec->name)), response);
	}
	audio_codec_name = String(codec->name);
	stream->time_base = audio_codec_context->time_base;
	response = avcodec_parameters_from_context(stream->codecpar, audio_codec_context.get());
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to copy the audio stream parameters", response);
	}
	stream->codecpar->codec_tag = 0;
	audio_stream = stream;

	AVChannelLayout input_layout = AV_CHANNEL_LAYOUT_STEREO;
	SwrContext *raw_swr_context = nullptr;
	response = swr_alloc_set_opts2(&raw_swr_context, &audio_codec_context->ch_layout, audio_codec_context->sample_fmt, audio_codec_context->sample_rate, &input_layout, AV_SAMPLE_FMT_S32, p_mix_rate, 0, nullptr);
	if (response < 0 || !raw_swr_context) {
		if (raw_swr_context) {
			swr_free(&raw_swr_context);
		}
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to create the audio conversion context", response);
	}
	swr_context = FFmpegSwrContextPtr(raw_swr_context);
	response = swr_init(swr_context.get());
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to initialize the audio conversion context", response);
	}

	audio_fifo = av_audio_fifo_alloc(audio_codec_context->sample_fmt, audio_codec_context->ch_layout.nb_channels, MAX(1, audio_codec_context->frame_size));
	if (!audio_fifo) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the audio queue");
	}
	audio_input_rate = p_mix_rate;
	audio_enabled = true;
	return OK;
}

Error FFmpegVideoEncoder::begin(const String &p_output_path, const Size2i &p_output_size, uint32_t p_fps, int64_t p_video_bitrate, const String &p_codec, const String &p_preset, uint32_t p_keyframe_interval, bool p_include_audio, uint32_t p_mix_rate, int64_t p_audio_bitrate, EncoderMode p_encoder_mode) {
	if (active) {
		return _set_error(ERR_ALREADY_IN_USE, "This FFmpeg video encoder is already active.");
	}
	if (p_output_path.is_empty()) {
		return _set_error(ERR_INVALID_PARAMETER, "The video output path must not be empty.");
	}
	if (p_output_size.width <= 0 || p_output_size.height <= 0) {
		return _set_error(ERR_INVALID_PARAMETER, "The video output size must be positive.");
	}
	if ((p_output_size.width & 1) != 0 || (p_output_size.height & 1) != 0) {
		return _set_error(ERR_INVALID_PARAMETER, "The video output width and height must both be even.");
	}
	if (p_fps == 0 || p_fps > 1000) {
		return _set_error(ERR_INVALID_PARAMETER, "The video FPS must be between 1 and 1000.");
	}
	if (p_video_bitrate <= 0) {
		return _set_error(ERR_INVALID_PARAMETER, "The video bitrate must be greater than zero.");
	}
	if (p_include_audio && (p_mix_rate == 0 || p_audio_bitrate <= 0)) {
		return _set_error(ERR_INVALID_PARAMETER, "The audio mix rate and bitrate must be greater than zero.");
	}
	if (p_encoder_mode < ENCODER_MODE_AUTO || p_encoder_mode > ENCODER_MODE_HARDWARE) {
		return _set_error(ERR_INVALID_PARAMETER, "The video encoder mode is invalid.");
	}

	_clear();
	last_error_message = String();
	codec_name = String();
	audio_codec_name = String();
	hardware_encoding = false;
	encoder_backend = "software";
	output_path = p_output_path;
	output_size = p_output_size;
	fps = p_fps;
	frame_count = 0;
	audio_pts = 0;

	Error err = FFmpegCommon::open_output(output, p_output_path);
	if (err != OK) {
		_clear();
		return _set_error(err, vformat("Could not open video output '%s'", p_output_path));
	}
	const bool automatic_hardware_attempt = p_encoder_mode == ENCODER_MODE_AUTO && p_codec.is_empty() && _ffmpeg_find_hardware_encoder(output.format->oformat);
	err = _create_video_stream(p_codec, p_video_bitrate, p_preset, p_keyframe_interval, p_encoder_mode);
	if (err != OK && automatic_hardware_attempt && output.format->nb_streams == 0) {
		sws_context.reset();
		hardware_video_frame.reset();
		video_frame.reset();
		video_codec_context.reset();
		hardware_frames_context.reset();
		hardware_device_context.reset();
		video_stream = nullptr;
		codec_name = String();
		hardware_encoding = false;
		encoder_backend = "software";
		last_error_message = String();
		err = _create_video_stream(p_codec, p_video_bitrate, p_preset, p_keyframe_interval, ENCODER_MODE_SOFTWARE);
	}
	if (err != OK) {
		_clear();
		return err;
	}
	if (p_include_audio) {
		err = _create_audio_stream(p_mix_rate, p_audio_bitrate);
		if (err != OK) {
			_clear();
			return err;
		}
	}

	int response = avformat_write_header(output.format.get(), nullptr);
	if (response < 0) {
		_clear();
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to write the video container header", response);
	}
	active = true;
	return OK;
}

Ref<Image> FFmpegVideoEncoder::_prepare_image(const Ref<Image> &p_image) const {
	ERR_FAIL_COND_V(p_image.is_null() || p_image->is_empty(), Ref<Image>());
	Ref<Image> image = p_image->duplicate();
	if (image->get_format() != Image::FORMAT_RGBA8) {
		image->convert(Image::FORMAT_RGBA8);
	}
	if (image->get_size() == output_size) {
		return image;
	}

	const float source_aspect = image->get_size().aspect();
	const float output_aspect = output_size.aspect();
	int crop_width = image->get_width();
	int crop_height = image->get_height();
	int crop_x = 0;
	int crop_y = 0;
	if (source_aspect > output_aspect) {
		crop_width = int(image->get_height() * output_aspect);
		crop_x = (image->get_width() - crop_width) / 2;
	} else if (source_aspect < output_aspect) {
		crop_height = int(image->get_width() / output_aspect);
		crop_y = (image->get_height() - crop_height) / 2;
	}
	image->crop_from_point(crop_x, crop_y, crop_width, crop_height);
	image->resize(output_size.width, output_size.height, Image::INTERPOLATE_BILINEAR);
	return image;
}

Error FFmpegVideoEncoder::_encode_frame(AVCodecContext *p_codec_context, AVStream *p_stream, AVFrame *p_frame) {
	int response = avcodec_send_frame(p_codec_context, p_frame);
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to submit a frame to the encoder", response);
	}

	FFmpegPacketPtr packet(av_packet_alloc());
	if (!packet) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate an encoded packet");
	}
	while (true) {
		response = avcodec_receive_packet(p_codec_context, packet.get());
		if (response == AVERROR(EAGAIN) || response == AVERROR_EOF) {
			return OK;
		}
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to receive an encoded packet", response);
		}
		av_packet_rescale_ts(packet.get(), p_codec_context->time_base, p_stream->time_base);
		packet->stream_index = p_stream->index;
		response = av_interleaved_write_frame(output.format.get(), packet.get());
		av_packet_unref(packet.get());
		if (response < 0) {
			return _set_error(ERR_FILE_CANT_WRITE, "FFmpeg failed to write an encoded packet", response);
		}
	}
}

Error FFmpegVideoEncoder::_write_converted_audio(uint8_t **p_data, int p_sample_count) {
	if (p_sample_count <= 0) {
		return OK;
	}
	if (av_audio_fifo_realloc(audio_fifo, av_audio_fifo_size(audio_fifo) + p_sample_count) < 0) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to grow the audio queue");
	}
	if (av_audio_fifo_write(audio_fifo, reinterpret_cast<void **>(p_data), p_sample_count) < p_sample_count) {
		return _set_error(ERR_FILE_CANT_WRITE, "FFmpeg failed to queue converted audio samples");
	}
	return OK;
}

Error FFmpegVideoEncoder::_append_audio(const int32_t *p_audio_data, uint32_t p_sample_count) {
	ERR_FAIL_NULL_V(p_audio_data, ERR_INVALID_PARAMETER);
	const int output_capacity = av_rescale_rnd(swr_get_delay(swr_context.get(), audio_input_rate) + p_sample_count, audio_codec_context->sample_rate, audio_input_rate, AV_ROUND_UP);
	uint8_t **converted_data = nullptr;
	int converted_linesize = 0;
	int response = av_samples_alloc_array_and_samples(&converted_data, &converted_linesize, audio_codec_context->ch_layout.nb_channels, output_capacity, audio_codec_context->sample_fmt, 0);
	if (response < 0) {
		return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate converted audio samples", response);
	}

	const uint8_t *input_data[] = { reinterpret_cast<const uint8_t *>(p_audio_data) };
	const int converted_count = swr_convert(swr_context.get(), converted_data, output_capacity, input_data, p_sample_count);
	Error err = OK;
	if (converted_count < 0) {
		err = _set_error(ERR_CANT_CREATE, "FFmpeg failed to convert audio samples", converted_count);
	} else {
		err = _write_converted_audio(converted_data, converted_count);
	}
	av_freep(&converted_data[0]);
	av_freep(&converted_data);
	if (err != OK) {
		return err;
	}
	return _drain_audio_fifo(false);
}

Error FFmpegVideoEncoder::_drain_audio_fifo(bool p_final) {
	const int encoder_frame_size = audio_codec_context->frame_size;
	while ((encoder_frame_size > 0 && av_audio_fifo_size(audio_fifo) >= encoder_frame_size) || (encoder_frame_size == 0 && av_audio_fifo_size(audio_fifo) > 0) || (p_final && av_audio_fifo_size(audio_fifo) > 0)) {
		const int available_samples = av_audio_fifo_size(audio_fifo);
		int frame_samples = encoder_frame_size > 0 ? encoder_frame_size : available_samples;
		const bool supports_small_last_frame = (audio_codec_context->codec->capabilities & (AV_CODEC_CAP_SMALL_LAST_FRAME | AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) != 0;
		const int samples_to_read = MIN(available_samples, frame_samples);
		if (p_final && samples_to_read < frame_samples && supports_small_last_frame) {
			frame_samples = samples_to_read;
		}

		FFmpegFramePtr frame(av_frame_alloc());
		if (!frame) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate an audio frame");
		}
		frame->nb_samples = frame_samples;
		frame->format = audio_codec_context->sample_fmt;
		frame->sample_rate = audio_codec_context->sample_rate;
		if (av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout) < 0) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to copy the audio channel layout");
		}
		int response = av_frame_get_buffer(frame.get(), 0);
		if (response < 0) {
			return _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the audio frame buffer", response);
		}
		if (av_audio_fifo_read(audio_fifo, reinterpret_cast<void **>(frame->data), samples_to_read) < samples_to_read) {
			return _set_error(ERR_FILE_CANT_READ, "FFmpeg failed to read queued audio samples");
		}
		if (samples_to_read < frame_samples) {
			av_samples_set_silence(frame->data, samples_to_read, frame_samples - samples_to_read, audio_codec_context->ch_layout.nb_channels, audio_codec_context->sample_fmt);
		}
		frame->pts = audio_pts;
		audio_pts += frame_samples;
		Error err = _encode_frame(audio_codec_context.get(), audio_stream, frame.get());
		if (err != OK) {
			return err;
		}
	}
	return OK;
}

Error FFmpegVideoEncoder::add_frame(const Ref<Image> &p_image, const int32_t *p_audio_data, uint32_t p_audio_sample_count) {
	if (!active) {
		return _set_error(ERR_UNCONFIGURED, "The FFmpeg video encoder has not been started.");
	}
	if (p_image.is_null() || p_image->is_empty()) {
		return _set_error(ERR_INVALID_PARAMETER, "The video frame image must not be empty.");
	}
	if (audio_enabled && (!p_audio_data || p_audio_sample_count == 0)) {
		return _set_error(ERR_INVALID_PARAMETER, "This video export requires one audio sample block for every video frame.");
	}

	Ref<Image> image = _prepare_image(p_image);
	if (image.is_null()) {
		return _set_error(ERR_INVALID_DATA, "Could not prepare the video frame image");
	}
	Vector<uint8_t> image_data = image->get_data();
	const uint8_t *source_data[] = { image_data.ptr(), nullptr, nullptr, nullptr };
	const int source_linesize[] = { output_size.width * 4, 0, 0, 0 };
	int response = av_frame_make_writable(video_frame.get());
	if (response < 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg could not make the video frame writable", response);
	}
	response = sws_scale(sws_context.get(), source_data, source_linesize, 0, output_size.height, video_frame->data, video_frame->linesize);
	if (response <= 0) {
		return _set_error(ERR_CANT_CREATE, "FFmpeg failed to convert the video frame", response);
	}
	video_frame->pts = frame_count;
	video_frame->duration = 1;
	AVFrame *frame_to_encode = video_frame.get();
	if (hardware_video_frame) {
		av_frame_unref(hardware_video_frame.get());
		response = av_hwframe_get_buffer(hardware_frames_context.get(), hardware_video_frame.get(), 0);
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to acquire a hardware video frame", response);
		}
		response = av_hwframe_transfer_data(hardware_video_frame.get(), video_frame.get(), 0);
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to upload the video frame to the hardware encoder", response);
		}
		response = av_frame_copy_props(hardware_video_frame.get(), video_frame.get());
		if (response < 0) {
			return _set_error(ERR_CANT_CREATE, "FFmpeg failed to copy the hardware video frame properties", response);
		}
		frame_to_encode = hardware_video_frame.get();
	}
	Error err = _encode_frame(video_codec_context.get(), video_stream, frame_to_encode);
	if (err != OK) {
		return err;
	}
	if (audio_enabled) {
		err = _append_audio(p_audio_data, p_audio_sample_count);
		if (err != OK) {
			return err;
		}
	}
	frame_count++;
	return OK;
}

Error FFmpegVideoEncoder::finish() {
	if (!active) {
		return _set_error(ERR_UNCONFIGURED, "The FFmpeg video encoder has not been started.");
	}
	Error result = OK;
	if (audio_enabled) {
		const int delayed_samples = swr_get_delay(swr_context.get(), audio_input_rate);
		if (delayed_samples > 0) {
			const int output_capacity = av_rescale_rnd(delayed_samples, audio_codec_context->sample_rate, audio_input_rate, AV_ROUND_UP);
			uint8_t **converted_data = nullptr;
			int converted_linesize = 0;
			int response = av_samples_alloc_array_and_samples(&converted_data, &converted_linesize, audio_codec_context->ch_layout.nb_channels, output_capacity, audio_codec_context->sample_fmt, 0);
			if (response >= 0) {
				const int converted_count = swr_convert(swr_context.get(), converted_data, output_capacity, nullptr, 0);
				if (converted_count >= 0) {
					result = _write_converted_audio(converted_data, converted_count);
				} else {
					result = _set_error(ERR_CANT_CREATE, "FFmpeg failed to flush converted audio samples", converted_count);
				}
				av_freep(&converted_data[0]);
				av_freep(&converted_data);
			} else {
				result = _set_error(ERR_OUT_OF_MEMORY, "FFmpeg failed to allocate the final converted audio samples", response);
			}
		}
		if (result == OK) {
			result = _drain_audio_fifo(true);
		}
		if (result == OK) {
			result = _encode_frame(audio_codec_context.get(), audio_stream, nullptr);
		}
	}
	if (result == OK) {
		result = _encode_frame(video_codec_context.get(), video_stream, nullptr);
	}
	if (result == OK) {
		const int response = av_write_trailer(output.format.get());
		if (response < 0) {
			result = _set_error(ERR_FILE_CANT_WRITE, "FFmpeg failed to finalize the video container", response);
		}
	}
	if (output.avio) {
		avio_flush(output.avio.get());
	}
	_clear();
	return result;
}

FFmpegVideoEncoder::~FFmpegVideoEncoder() {
	if (active) {
		finish();
	} else {
		_clear();
	}
}
