/**************************************************************************/
/*  ffmpeg_video_encoder.h                                                */
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

#include "ffmpeg_common.h"

#include "core/io/image.h"

class FFmpegVideoEncoder {
	FFmpegOutputContext output;
	FFmpegCodecContextPtr video_codec_context;
	FFmpegCodecContextPtr audio_codec_context;
	FFmpegFramePtr video_frame;
	FFmpegSwsContextPtr sws_context;
	FFmpegSwrContextPtr swr_context;
	AVAudioFifo *audio_fifo = nullptr;
	AVStream *video_stream = nullptr;
	AVStream *audio_stream = nullptr;

	Size2i output_size;
	uint32_t fps = 0;
	uint32_t audio_input_rate = 0;
	uint64_t frame_count = 0;
	int64_t audio_pts = 0;
	bool active = false;
	bool audio_enabled = false;

	String output_path;
	String codec_name;
	String audio_codec_name;
	String last_error_message;

	Error _set_error(Error p_error, const String &p_message, int p_ffmpeg_error = 0);
	void _clear();
	Error _create_video_stream(const String &p_requested_codec, int64_t p_video_bitrate, const String &p_preset, uint32_t p_keyframe_interval);
	Error _create_audio_stream(uint32_t p_mix_rate, int64_t p_audio_bitrate);
	Error _encode_frame(AVCodecContext *p_codec_context, AVStream *p_stream, AVFrame *p_frame);
	Error _append_audio(const int32_t *p_audio_data, uint32_t p_sample_count);
	Error _drain_audio_fifo(bool p_final);
	Error _write_converted_audio(uint8_t **p_data, int p_sample_count);
	Ref<Image> _prepare_image(const Ref<Image> &p_image) const;

public:
	Error begin(const String &p_output_path, const Size2i &p_output_size, uint32_t p_fps, int64_t p_video_bitrate, const String &p_codec, const String &p_preset, uint32_t p_keyframe_interval, bool p_include_audio = false, uint32_t p_mix_rate = 48000, int64_t p_audio_bitrate = 192000);
	Error add_frame(const Ref<Image> &p_image, const int32_t *p_audio_data = nullptr, uint32_t p_audio_sample_count = 0);
	Error finish();

	bool is_active() const { return active; }
	bool has_audio() const { return audio_enabled; }
	uint64_t get_frame_count() const { return frame_count; }
	double get_duration() const { return fps > 0 ? double(frame_count) / fps : 0.0; }
	String get_output_path() const { return output_path; }
	String get_codec_name() const { return codec_name; }
	String get_audio_codec_name() const { return audio_codec_name; }
	String get_last_error_message() const { return last_error_message; }

	~FFmpegVideoEncoder();
};
