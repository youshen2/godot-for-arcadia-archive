/**************************************************************************/
/*  movie_writer_ffmpeg.cpp                                               */
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

#include "movie_writer_ffmpeg.h"

#include "core/config/project_settings.h"

uint32_t MovieWriterFFmpeg::get_audio_mix_rate() const {
	return mix_rate;
}

AudioServer::SpeakerMode MovieWriterFFmpeg::get_audio_speaker_mode() const {
	// FFmpeg movie export currently uses a stable stereo layout. This avoids
	// ambiguities between Godot's internal surround channel pairs and container layouts.
	return AudioServer::SPEAKER_MODE_STEREO;
}

bool MovieWriterFFmpeg::handles_file(const String &p_path) const {
	const String extension = p_path.get_extension().to_lower();
	return extension == "mp4" || extension == "mov" || extension == "mkv";
}

void MovieWriterFFmpeg::get_supported_extensions(List<String> *r_extensions) const {
	r_extensions->push_back("mp4");
	r_extensions->push_back("mov");
	r_extensions->push_back("mkv");
}

Error MovieWriterFFmpeg::write_begin(const Size2i &p_movie_size, uint32_t p_fps, const String &p_base_path) {
	fps = p_fps;
	String output_path = p_base_path;
	if (output_path.is_relative_path()) {
		output_path = "res://" + output_path;
	}
	const FFmpegVideoEncoder::EncoderMode encoder_mode = codec.is_empty() ? FFmpegVideoEncoder::ENCODER_MODE_SOFTWARE : FFmpegVideoEncoder::ENCODER_MODE_AUTO;
	Error err = encoder.begin(output_path, p_movie_size, fps, video_bitrate, codec, preset, keyframe_interval, true, mix_rate, audio_bitrate, encoder_mode);
	write_failed = err != OK;
	return err;
}

Error MovieWriterFFmpeg::write_frame(const Ref<Image> &p_image, const int32_t *p_audio_data) {
	if (write_failed) {
		return ERR_CANT_CREATE;
	}
	Error err = encoder.add_frame(p_image, p_audio_data, mix_rate / fps);
	write_failed = err != OK;
	return err;
}

void MovieWriterFFmpeg::write_end() {
	if (encoder.is_active()) {
		encoder.finish();
	}
	write_failed = false;
}

MovieWriterFFmpeg::MovieWriterFFmpeg() {
	mix_rate = GLOBAL_GET("editor/movie_writer/mix_rate");
	video_bitrate = GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ffmpeg/video_bitrate", PROPERTY_HINT_RANGE, "100000,200000000,100000,suffix:bps"), 12000000);
	audio_bitrate = GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ffmpeg/audio_bitrate", PROPERTY_HINT_RANGE, "32000,512000,1000,suffix:bps"), 192000);
	codec = GLOBAL_DEF(PropertyInfo(Variant::STRING, "editor/movie_writer/ffmpeg/video_codec", PROPERTY_HINT_PLACEHOLDER_TEXT, "自动选择"), "");
	preset = GLOBAL_DEF(PropertyInfo(Variant::STRING, "editor/movie_writer/ffmpeg/encoding_preset", PROPERTY_HINT_ENUM, "ultrafast,superfast,veryfast,faster,fast,medium,slow,slower,veryslow"), "veryfast");
	keyframe_interval = GLOBAL_DEF(PropertyInfo(Variant::INT, "editor/movie_writer/ffmpeg/keyframe_interval", PROPERTY_HINT_RANGE, "0,3600,1"), 0);
}
