/**************************************************************************/
/*  video_stream_ffmpeg.h                                                 */
/**************************************************************************/

#pragma once

#include "ffmpeg_common.h"

#include "core/io/resource_loader.h"
#include "core/os/mutex.h"
#include "core/variant/dictionary.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/video_stream.h"

class VideoStreamPlaybackFFmpeg : public VideoStreamPlayback {
	GDCLASS(VideoStreamPlaybackFFmpeg, VideoStreamPlayback);

	FFmpegInputContext video_input;
	FFmpegCodecContextPtr video_codec_context;
	AVStream *video_stream = nullptr;
	FFmpegPacketPtr video_packet;
	FFmpegFramePtr video_frame;
	FFmpegFramePtr rgba_frame;
	FFmpegSwsContextPtr sws_context;

	FFmpegInputContext audio_input;
	FFmpegCodecContextPtr audio_codec_context;
	AVStream *audio_stream = nullptr;
	FFmpegPacketPtr audio_packet;
	FFmpegFramePtr audio_frame;
	FFmpegFramePtr audio_convert_frame;
	FFmpegSwrContextPtr swr_context;

	Ref<ImageTexture> texture;
	Vector<uint8_t> frame_data;
	Vector<uint8_t> pending_frame_data;
	Vector<float> pending_audio;

	String file;
	String headers;

	Size2i frame_size;
	int source_width = 0;
	int source_height = 0;
	AVPixelFormat source_pixel_format = AV_PIX_FMT_NONE;

	bool playing = false;
	bool paused = false;
	bool video_eof = false;
	bool audio_eof = false;
	bool has_pending_frame = false;
	bool audio_enabled = true;
	bool audio_speed_to_sync = false;
	bool pitch_adjust = true;
	bool debug = false;
	bool accurate_seek = true;
	bool apply_rotation_metadata = true;

	double time = 0.0;
	double length = 0.0;
	double pending_frame_time = 0.0;
	double audio_decoded_time = 0.0;
	double frame_rate = 24.0;
	double max_video_fps = 0.0;
	double last_frame_upload_time = -1.0;

	int audio_track = 0;
	int video_track = 0;
	int audio_channels = 0;
	int audio_mix_rate = 0;
	int audio_buffering_msec = 500;
	int max_video_frames_per_update = 32;
	int pending_audio_frames = 0;
	int pending_audio_offset = 0;
	int color_profile = 0;
	bool frame_dropping = false;
	bool key_frame_only = false;
	int display_rotation = 0;

	Error open_video();
	void close_video();
	Error open_audio();
	void close_audio();
	Error open_codec_context(AVStream *p_stream, FFmpegCodecContextPtr &r_codec_context);
	int find_video_stream_index() const;
	bool read_next_video_frame();
	void upload_pending_frame();
	Size2i get_rotated_frame_size() const;
	void update_display_rotation();
	void rotate_frame_data(const Vector<uint8_t> &p_source, Vector<uint8_t> &r_target) const;
	bool send_pending_audio();
	bool decode_next_audio_frame();
	void decode_audio_until(double p_time);
	void recreate_sws_context(const AVFrame *p_frame);
	void apply_sws_color_profile(const AVFrame *p_frame);
	double get_frame_time(const AVFrame *p_frame) const;
	double get_stream_duration(AVFormatContext *p_format_context, AVStream *p_stream) const;
	int find_audio_stream_index() const;

protected:
	static void _bind_methods() {}

public:
	virtual void play() override;
	virtual void stop() override;
	virtual bool is_playing() const override;

	virtual void set_paused(bool p_paused) override;
	virtual bool is_paused() const override;

	virtual double get_length() const override;
	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;

	virtual void set_video_track(int p_idx) override;
	virtual void set_audio_track(int p_idx) override;
	virtual void set_audio_enabled(bool p_enabled) override;
	virtual void set_audio_speed_to_sync(bool p_enabled) override;
	virtual void set_audio_buffering_msec(int p_msec) override;
	virtual void set_pitch_adjust_enabled(bool p_enabled) override;
	virtual void set_color_profile(int p_profile) override;
	virtual void set_debug_enabled(bool p_enabled) override;
	virtual void set_max_video_fps(double p_fps) override;
	virtual void set_max_video_frames_per_update(int p_frames) override;
	virtual void set_frame_dropping_enabled(bool p_enabled) override;
	virtual void set_key_frame_only_enabled(bool p_enabled) override;
	virtual void set_accurate_seek_enabled(bool p_enabled) override;
	virtual void set_apply_rotation_metadata_enabled(bool p_enabled) override;

	void set_file(const String &p_file);
	void set_headers(const String &p_headers);

	virtual Ref<Texture2D> get_texture() const override;
	virtual void update(double p_delta) override;

	virtual int get_channels() const override;
	virtual int get_mix_rate() const override;

	VideoStreamPlaybackFFmpeg();
	~VideoStreamPlaybackFFmpeg();
};

class VideoStreamFFmpeg : public VideoStream {
	GDCLASS(VideoStreamFFmpeg, VideoStream);

	String headers;
	mutable FFmpegInputContext probe_input;
	mutable bool probe_loaded = false;
	mutable PackedInt32Array video_streams;
	mutable PackedInt32Array audio_streams;
	mutable PackedInt32Array subtitle_streams;

	Error ensure_probe() const;
	void clear_probe() const;

protected:
	static void _bind_methods();

public:
	virtual Ref<VideoStreamPlayback> instantiate_playback() override;

	virtual void set_file(const String &p_file) override;

	void set_headers(const String &p_headers);
	String get_headers() const;

	PackedInt32Array get_video_streams() const;
	PackedInt32Array get_audio_streams() const;
	PackedInt32Array get_subtitle_streams() const;
	Dictionary get_stream_metadata(int p_stream_index) const;
	int get_chapter_count() const;
	double get_chapter_start(int p_chapter_index) const;
	double get_chapter_end(int p_chapter_index) const;
	Dictionary get_chapter_metadata(int p_chapter_index) const;
	double get_duration() const;

	VideoStreamFFmpeg();
	~VideoStreamFFmpeg();
};

class ResourceFormatLoaderFFmpegVideo : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderFFmpegVideo, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
