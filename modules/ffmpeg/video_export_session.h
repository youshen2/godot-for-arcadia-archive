/**************************************************************************/
/*  video_export_session.h                                                */
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

#include "ffmpeg_video_encoder.h"

#include "core/object/ref_counted.h"

class Viewport;

class VideoExportSession : public RefCounted {
	GDCLASS(VideoExportSession, RefCounted)

public:
	enum EncoderMode {
		ENCODER_MODE_AUTO,
		ENCODER_MODE_SOFTWARE,
		ENCODER_MODE_HARDWARE,
	};

private:
	FFmpegVideoEncoder encoder;
	ObjectID viewport_id;
	Size2i output_size;
	uint32_t fps = 60;
	uint32_t audio_mix_rate = 0;
	uint64_t frame_limit = 0;
	bool audio_enabled = false;
	EncoderMode encoder_mode = ENCODER_MODE_AUTO;
	String session_error;

	Error _report_error(Error p_error, const String &p_message = String());
	Error _add_frame(const Ref<Image> &p_image, const PackedVector2Array &p_audio_frames);

protected:
	static void _bind_methods();

public:
	void set_viewport(Viewport *p_viewport);
	Viewport *get_viewport() const;
	void set_encoder_mode(EncoderMode p_mode);
	EncoderMode get_encoder_mode() const;

	Error start(const String &p_output_path, const Size2i &p_output_size, int p_fps = 60, int64_t p_video_bitrate = 12000000, const String &p_codec = String(), const String &p_encoding_preset = "veryfast", int p_keyframe_interval = 0, int64_t p_frame_limit = 0, bool p_include_audio = false, int p_audio_mix_rate = 48000, int64_t p_audio_bitrate = 192000);
	Error add_frame(const Ref<Image> &p_image, const PackedVector2Array &p_audio_frames = PackedVector2Array());
	Error capture_frame(const PackedVector2Array &p_audio_frames = PackedVector2Array());
	Error render_frame(const PackedVector2Array &p_audio_frames = PackedVector2Array());
	Error finish();

	bool is_active() const;
	bool is_audio_enabled() const;
	int64_t get_frame_count() const;
	int64_t get_frame_limit() const;
	int get_required_audio_frame_count() const;
	double get_duration() const;
	double get_progress() const;
	Size2i get_output_size() const;
	int get_fps() const;
	int get_audio_mix_rate() const;
	String get_output_path() const;
	String get_codec_name() const;
	String get_audio_codec_name() const;
	bool is_using_hardware_encoder() const;
	String get_encoder_backend() const;
	String get_last_error() const;
};

VARIANT_ENUM_CAST(VideoExportSession::EncoderMode);
