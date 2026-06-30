/**************************************************************************/
/*  video_stream_player.h                                                 */
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

#include "scene/gui/control.h"
#include "scene/resources/video_stream.h"
#include "servers/audio/audio_rb_resampler.h"

class VideoStreamPlayer : public Control {
	GDCLASS(VideoStreamPlayer, Control);

public:
	enum ColorProfile {
		COLOR_PROFILE_AUTO,
		COLOR_PROFILE_BT470,
		COLOR_PROFILE_BT601,
		COLOR_PROFILE_BT709,
		COLOR_PROFILE_BT2020,
		COLOR_PROFILE_BT2100,
	};

private:
	Ref<VideoStreamPlayback> playback;
	Ref<VideoStream> stream;

	int sp_get_channel_count() const;
	bool mix(AudioFrame *p_buffer, int p_frames);

	Ref<Texture2D> texture;
	Size2 texture_size;
	void texture_changed(const Ref<Texture2D> &p_texture);

	AudioRBResampler resampler;
	Vector<AudioFrame> mix_buffer;
	int wait_resampler = 0;
	int wait_resampler_limit = 2;

	bool paused = false;
	bool paused_from_tree = false;
	bool autoplay = false;
	float volume = 1.0;
	float speed_scale = 1.0;
	Vector2 playback_speed_override;
	double max_video_fps = 0.0;
	double loop_start = 0.0;
	double loop_end = 0.0;
	bool expand = false;
	bool loop = false;
	bool first_frame = false;
	bool audio_enabled = true;
	bool audio_speed_to_sync = false;
	bool pitch_adjust = true;
	bool frame_dropping = false;
	bool key_frame_only = false;
	bool accurate_seek = true;
	bool apply_rotation_metadata = true;
	bool debug = false;
	int buffering_ms = 500;
	int max_video_frames_per_update = 32;
	int video_track = 0;
	int audio_track = 0;
	int bus_index = 0;
	ColorProfile color_profile = COLOR_PROFILE_AUTO;

	StringName bus;

	void _apply_playback_settings();
	void _mix_audio();
	static int _audio_mix_callback(void *p_udata, const float *p_data, int p_frames);
	static void _mix_audios(void *p_self);

protected:
	static void _bind_methods();
	void _notification(int p_notification);
	void _validate_property(PropertyInfo &p_property) const;

public:
	Size2 get_minimum_size() const override;
	void set_expand(bool p_expand);
	bool has_expand() const;

	Ref<Texture2D> get_video_texture() const;

	void set_stream(const Ref<VideoStream> &p_stream);
	Ref<VideoStream> get_stream() const;

	void play();
	void stop();
	bool is_playing() const;

	void set_loop(bool p_loop);
	bool has_loop() const;

	void set_paused(bool p_paused);
	bool is_paused() const;

	void set_volume(float p_vol);
	float get_volume() const;

	void set_volume_db(float p_db);
	float get_volume_db() const;

	void set_speed_scale(float p_speed_scale);
	float get_speed_scale() const;

	void set_playback_speed_override(const Vector2 &p_override);
	Vector2 get_playback_speed_override() const;

	void set_max_video_fps(double p_fps);
	double get_max_video_fps() const;

	void set_max_video_frames_per_update(int p_frames);
	int get_max_video_frames_per_update() const;

	void set_frame_dropping_enabled(bool p_enabled);
	bool is_frame_dropping_enabled() const;

	void set_key_frame_only_enabled(bool p_enabled);
	bool is_key_frame_only_enabled() const;

	void set_accurate_seek_enabled(bool p_enabled);
	bool is_accurate_seek_enabled() const;

	void set_apply_rotation_metadata_enabled(bool p_enabled);
	bool is_apply_rotation_metadata_enabled() const;

	void set_loop_start(double p_time);
	double get_loop_start() const;

	void set_loop_end(double p_time);
	double get_loop_end() const;

	void set_audio_enabled(bool p_enabled);
	bool is_audio_enabled() const;

	void set_audio_speed_to_sync(bool p_enabled);
	bool is_audio_speed_to_sync_enabled() const;

	void set_pitch_adjust_enabled(bool p_enabled);
	bool is_pitch_adjust_enabled() const;

	void set_color_profile(ColorProfile p_profile);
	ColorProfile get_color_profile() const;

	void set_debug_enabled(bool p_enabled);
	bool is_debug_enabled() const;

	String get_stream_name() const;
	double get_stream_length() const;
	double get_stream_position() const;
	void set_stream_position(double p_position);

	void set_autoplay(bool p_enable);
	bool has_autoplay() const;

	void set_audio_track(int p_track);
	int get_audio_track() const;

	void set_video_track(int p_track);
	int get_video_track() const;

	void set_buffering_msec(int p_msec);
	int get_buffering_msec() const;

	void set_bus(const StringName &p_bus);
	StringName get_bus() const;

	~VideoStreamPlayer();
};

VARIANT_ENUM_CAST(VideoStreamPlayer::ColorProfile);
