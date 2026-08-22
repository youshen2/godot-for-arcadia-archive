/**************************************************************************/
/*  video_export_session.cpp                                              */
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

#include "video_export_session.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "core/object/object.h"
#include "scene/main/viewport.h"
#include "servers/rendering/rendering_server.h"

Error VideoExportSession::_report_error(Error p_error, const String &p_message) {
	if (p_error != OK) {
		session_error = p_message.is_empty() ? encoder.get_last_error_message() : p_message;
		emit_signal(SNAME("failed"), p_error, session_error);
	}
	return p_error;
}

void VideoExportSession::set_viewport(Viewport *p_viewport) {
	viewport_id = p_viewport ? p_viewport->get_instance_id() : ObjectID();
}

Viewport *VideoExportSession::get_viewport() const {
	return Object::cast_to<Viewport>(ObjectDB::get_instance(viewport_id));
}

void VideoExportSession::set_encoder_mode(EncoderMode p_mode) {
	ERR_FAIL_INDEX(p_mode, ENCODER_MODE_HARDWARE + 1);
	ERR_FAIL_COND_MSG(encoder.is_active(), "The encoder mode cannot be changed while a video export session is active.");
	encoder_mode = p_mode;
}

VideoExportSession::EncoderMode VideoExportSession::get_encoder_mode() const {
	return encoder_mode;
}

Error VideoExportSession::start(const String &p_output_path, const Size2i &p_output_size, int p_fps, int64_t p_video_bitrate, const String &p_codec, const String &p_encoding_preset, int p_keyframe_interval, int64_t p_frame_limit, bool p_include_audio, int p_audio_mix_rate, int64_t p_audio_bitrate) {
	if (p_fps <= 0) {
		return _report_error(ERR_INVALID_PARAMETER, "Video export FPS must be greater than zero.");
	}
	if (p_keyframe_interval < 0) {
		return _report_error(ERR_INVALID_PARAMETER, "Video export keyframe interval must not be negative.");
	}
	if (p_frame_limit < 0) {
		return _report_error(ERR_INVALID_PARAMETER, "Video export frame limit must not be negative.");
	}
	if (p_include_audio && (p_audio_mix_rate < 8000 || p_audio_mix_rate > 192000)) {
		return _report_error(ERR_INVALID_PARAMETER, "Video export audio mix rate must be between 8000 and 192000 Hz.");
	}
	if (p_include_audio && p_audio_bitrate <= 0) {
		return _report_error(ERR_INVALID_PARAMETER, "Video export audio bitrate must be greater than zero.");
	}
	const String extension = p_output_path.get_extension().to_lower();
	if (extension != "mp4" && extension != "mov" && extension != "mkv") {
		return _report_error(ERR_INVALID_PARAMETER, "Video export requires an MP4, MOV, or MKV output path.");
	}
	Error err = encoder.begin(p_output_path, p_output_size, p_fps, p_video_bitrate, p_codec, p_encoding_preset, p_keyframe_interval, p_include_audio, uint32_t(p_audio_mix_rate), p_audio_bitrate, static_cast<FFmpegVideoEncoder::EncoderMode>(encoder_mode));
	if (err != OK) {
		return _report_error(err);
	}
	output_size = p_output_size;
	fps = p_fps;
	audio_enabled = p_include_audio;
	audio_mix_rate = p_include_audio ? uint32_t(p_audio_mix_rate) : 0;
	frame_limit = p_frame_limit;
	session_error = String();
	emit_signal(SNAME("started"), p_output_path);
	return OK;
}

Error VideoExportSession::_add_frame(const Ref<Image> &p_image, const PackedVector2Array &p_audio_frames) {
	if (frame_limit > 0 && encoder.get_frame_count() >= frame_limit) {
		return _report_error(ERR_ALREADY_EXISTS, "The configured video export frame limit has already been reached.");
	}

	if (!audio_enabled) {
		if (!p_audio_frames.is_empty()) {
			return _report_error(ERR_INVALID_PARAMETER, "Audio frames were provided, but audio was not enabled when starting the video export.");
		}
		const Error err = encoder.add_frame(p_image);
		return err == OK ? OK : _report_error(err);
	}

	const int required_audio_frames = get_required_audio_frame_count();
	if (p_audio_frames.size() != required_audio_frames) {
		return _report_error(ERR_INVALID_PARAMETER, vformat("The next video frame requires exactly %d stereo audio frames, but %d were provided.", required_audio_frames, p_audio_frames.size()));
	}

	Vector<int32_t> interleaved_audio;
	interleaved_audio.resize(required_audio_frames * 2);
	int32_t *audio_write = interleaved_audio.ptrw();
	const Vector2 *audio_read = p_audio_frames.ptr();
	for (int i = 0; i < required_audio_frames; i++) {
		if (!audio_read[i].is_finite()) {
			return _report_error(ERR_INVALID_DATA, vformat("Audio frame %d contains a non-finite sample.", i));
		}

		const float left = CLAMP(float(audio_read[i].x), -1.0f, 1.0f);
		const int32_t left_sample = left * ((1 << 20) - 1);
		audio_write[i * 2] = (left_sample < 0 ? -1 : 1) * (Math::abs(left_sample) << 11);

		const float right = CLAMP(float(audio_read[i].y), -1.0f, 1.0f);
		const int32_t right_sample = right * ((1 << 20) - 1);
		audio_write[i * 2 + 1] = (right_sample < 0 ? -1 : 1) * (Math::abs(right_sample) << 11);
	}

	const Error err = encoder.add_frame(p_image, interleaved_audio.ptr(), required_audio_frames);
	return err == OK ? OK : _report_error(err);
}

Error VideoExportSession::add_frame(const Ref<Image> &p_image, const PackedVector2Array &p_audio_frames) {
	Error err = _add_frame(p_image, p_audio_frames);
	if (err != OK) {
		return err;
	}
	emit_signal(SNAME("frame_encoded"), int64_t(encoder.get_frame_count()), encoder.get_duration(), get_progress());
	return OK;
}

Error VideoExportSession::capture_frame(const PackedVector2Array &p_audio_frames) {
	Viewport *viewport = get_viewport();
	if (!viewport) {
		return _report_error(ERR_UNCONFIGURED, "No valid viewport is configured for video export.");
	}
	Ref<ViewportTexture> texture = viewport->get_texture();
	if (texture.is_null()) {
		return _report_error(ERR_UNAVAILABLE, "The video export viewport does not have a readable texture.");
	}
	Ref<Image> image = texture->get_image();
	if (image.is_null() || image->is_empty()) {
		return _report_error(ERR_UNAVAILABLE, "The video export viewport returned an empty image.");
	}
	if (viewport->is_using_hdr_2d()) {
		image->convert(Image::FORMAT_RGBA8);
		image->linear_to_srgb();
	}
	return add_frame(image, p_audio_frames);
}

Error VideoExportSession::render_frame(const PackedVector2Array &p_audio_frames) {
	if (!encoder.is_active()) {
		return _report_error(ERR_UNCONFIGURED, "The video export session has not been started.");
	}
	RenderingServer::get_singleton()->sync();
	RenderingServer::get_singleton()->draw(false, 1.0 / fps);
	return capture_frame(p_audio_frames);
}

Error VideoExportSession::finish() {
	const String completed_path = encoder.get_output_path();
	const int64_t completed_frames = encoder.get_frame_count();
	const double completed_duration = encoder.get_duration();
	Error err = encoder.finish();
	if (err != OK) {
		return _report_error(err);
	}
	emit_signal(SNAME("finished"), completed_path, completed_frames, completed_duration);
	return OK;
}

bool VideoExportSession::is_active() const {
	return encoder.is_active();
}

bool VideoExportSession::is_audio_enabled() const {
	return audio_enabled;
}

int64_t VideoExportSession::get_frame_count() const {
	return encoder.get_frame_count();
}

int64_t VideoExportSession::get_frame_limit() const {
	return frame_limit;
}

int VideoExportSession::get_required_audio_frame_count() const {
	if (!encoder.is_active() || !audio_enabled || audio_mix_rate == 0 || fps == 0) {
		return 0;
	}
	const uint64_t current_frame = encoder.get_frame_count();
	const uint64_t previous_sample_count = current_frame * audio_mix_rate / fps;
	const uint64_t next_sample_count = (current_frame + 1) * audio_mix_rate / fps;
	return int(next_sample_count - previous_sample_count);
}

double VideoExportSession::get_duration() const {
	return encoder.get_duration();
}

double VideoExportSession::get_progress() const {
	return frame_limit > 0 ? MIN(1.0, double(encoder.get_frame_count()) / frame_limit) : 0.0;
}

Size2i VideoExportSession::get_output_size() const {
	return output_size;
}

int VideoExportSession::get_fps() const {
	return fps;
}

int VideoExportSession::get_audio_mix_rate() const {
	return audio_mix_rate;
}

String VideoExportSession::get_output_path() const {
	return encoder.get_output_path();
}

String VideoExportSession::get_codec_name() const {
	return encoder.get_codec_name();
}

String VideoExportSession::get_audio_codec_name() const {
	return encoder.get_audio_codec_name();
}

bool VideoExportSession::is_using_hardware_encoder() const {
	return encoder.is_using_hardware_encoder();
}

String VideoExportSession::get_encoder_backend() const {
	return encoder.get_encoder_backend();
}

String VideoExportSession::get_last_error() const {
	return session_error.is_empty() ? encoder.get_last_error_message() : session_error;
}

void VideoExportSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_viewport", "viewport"), &VideoExportSession::set_viewport);
	ClassDB::bind_method(D_METHOD("get_viewport"), &VideoExportSession::get_viewport);
	ClassDB::bind_method(D_METHOD("set_encoder_mode", "mode"), &VideoExportSession::set_encoder_mode);
	ClassDB::bind_method(D_METHOD("get_encoder_mode"), &VideoExportSession::get_encoder_mode);
	ClassDB::bind_method(D_METHOD("start", "output_path", "output_size", "fps", "video_bitrate", "codec", "encoding_preset", "keyframe_interval", "frame_limit", "include_audio", "audio_mix_rate", "audio_bitrate"), &VideoExportSession::start, DEFVAL(60), DEFVAL(12000000), DEFVAL(String()), DEFVAL("veryfast"), DEFVAL(0), DEFVAL(0), DEFVAL(false), DEFVAL(48000), DEFVAL(192000));
	ClassDB::bind_method(D_METHOD("add_frame", "image", "audio_frames"), &VideoExportSession::add_frame, DEFVAL(PackedVector2Array()));
	ClassDB::bind_method(D_METHOD("capture_frame", "audio_frames"), &VideoExportSession::capture_frame, DEFVAL(PackedVector2Array()));
	ClassDB::bind_method(D_METHOD("render_frame", "audio_frames"), &VideoExportSession::render_frame, DEFVAL(PackedVector2Array()));
	ClassDB::bind_method(D_METHOD("finish"), &VideoExportSession::finish);
	ClassDB::bind_method(D_METHOD("is_active"), &VideoExportSession::is_active);
	ClassDB::bind_method(D_METHOD("is_audio_enabled"), &VideoExportSession::is_audio_enabled);
	ClassDB::bind_method(D_METHOD("get_frame_count"), &VideoExportSession::get_frame_count);
	ClassDB::bind_method(D_METHOD("get_frame_limit"), &VideoExportSession::get_frame_limit);
	ClassDB::bind_method(D_METHOD("get_required_audio_frame_count"), &VideoExportSession::get_required_audio_frame_count);
	ClassDB::bind_method(D_METHOD("get_duration"), &VideoExportSession::get_duration);
	ClassDB::bind_method(D_METHOD("get_progress"), &VideoExportSession::get_progress);
	ClassDB::bind_method(D_METHOD("get_output_size"), &VideoExportSession::get_output_size);
	ClassDB::bind_method(D_METHOD("get_fps"), &VideoExportSession::get_fps);
	ClassDB::bind_method(D_METHOD("get_audio_mix_rate"), &VideoExportSession::get_audio_mix_rate);
	ClassDB::bind_method(D_METHOD("get_output_path"), &VideoExportSession::get_output_path);
	ClassDB::bind_method(D_METHOD("get_codec_name"), &VideoExportSession::get_codec_name);
	ClassDB::bind_method(D_METHOD("get_audio_codec_name"), &VideoExportSession::get_audio_codec_name);
	ClassDB::bind_method(D_METHOD("is_using_hardware_encoder"), &VideoExportSession::is_using_hardware_encoder);
	ClassDB::bind_method(D_METHOD("get_encoder_backend"), &VideoExportSession::get_encoder_backend);
	ClassDB::bind_method(D_METHOD("get_last_error"), &VideoExportSession::get_last_error);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "viewport", PROPERTY_HINT_NODE_TYPE, "Viewport"), "set_viewport", "get_viewport");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "encoder_mode", PROPERTY_HINT_ENUM, "Auto,Software,Hardware"), "set_encoder_mode", "get_encoder_mode");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "active", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "is_active");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "audio_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "is_audio_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_frame_count");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_limit", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_frame_limit");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "required_audio_frame_count", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_required_audio_frame_count");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "duration", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_duration");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "progress", PROPERTY_HINT_RANGE, "0,1,0.001", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_progress");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "output_size", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_output_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "fps", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_fps");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "audio_mix_rate", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_audio_mix_rate");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "output_path", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_output_path");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "codec_name", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_codec_name");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "audio_codec_name", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_audio_codec_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "using_hardware_encoder", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "is_using_hardware_encoder");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "encoder_backend", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_encoder_backend");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "last_error", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_READ_ONLY), "", "get_last_error");

	BIND_ENUM_CONSTANT(ENCODER_MODE_AUTO);
	BIND_ENUM_CONSTANT(ENCODER_MODE_SOFTWARE);
	BIND_ENUM_CONSTANT(ENCODER_MODE_HARDWARE);

	ADD_SIGNAL(MethodInfo("started", PropertyInfo(Variant::STRING, "output_path")));
	ADD_SIGNAL(MethodInfo("frame_encoded", PropertyInfo(Variant::INT, "frame_count"), PropertyInfo(Variant::FLOAT, "duration"), PropertyInfo(Variant::FLOAT, "progress")));
	ADD_SIGNAL(MethodInfo("finished", PropertyInfo(Variant::STRING, "output_path"), PropertyInfo(Variant::INT, "frame_count"), PropertyInfo(Variant::FLOAT, "duration")));
	ADD_SIGNAL(MethodInfo("failed", PropertyInfo(Variant::INT, "error"), PropertyInfo(Variant::STRING, "message")));
}
