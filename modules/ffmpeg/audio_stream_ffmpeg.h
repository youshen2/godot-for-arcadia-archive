/**************************************************************************/
/*  audio_stream_ffmpeg.h                                                 */
/**************************************************************************/

#pragma once

#include "ffmpeg_common.h"

#include "core/io/resource_loader.h"
#include "core/os/mutex.h"
#include "core/variant/dictionary.h"
#include "servers/audio/audio_stream.h"

class AudioStreamFFmpeg;

class AudioStreamPlaybackFFmpeg : public AudioStreamPlaybackResampled {
	GDCLASS(AudioStreamPlaybackFFmpeg, AudioStreamPlaybackResampled);

	Ref<AudioStreamFFmpeg> stream;

	FFmpegInputContext input;
	FFmpegCodecContextPtr codec_context;
	AVStream *audio_stream = nullptr;
	FFmpegPacketPtr packet;
	FFmpegFramePtr frame;
	FFmpegFramePtr convert_frame;
	FFmpegSwrContextPtr swr_context;

	Vector<AudioFrame> buffer;
	int buffer_offset = 0;
	int frames_mixed = 0;
	int loop_count = 0;
	bool active = false;
	bool eof = false;
	bool loop_enabled = false;
	bool downmix_to_mono = false;
	int mix_rate = 44100;
	double loop_start = 0.0;
	double loop_end = 0.0;
	double end_time = 0.0;

	Error open_decoder();
	void close_decoder();
	bool decode_next_frame();
	bool fill_buffer(int p_frames);
	friend class AudioStreamFFmpeg;

protected:
	virtual int _mix_internal(AudioFrame *p_buffer, int p_frames) override;
	virtual float get_stream_sampling_rate() override;
	static void _bind_methods() {}

public:
	virtual void start(double p_from_pos = 0.0) override;
	virtual void stop() override;
	virtual bool is_playing() const override;
	virtual int get_loop_count() const override;
	virtual double get_playback_position() const override;
	virtual void seek(double p_time) override;
	virtual void tag_used_streams() override;
	virtual void set_parameter(const StringName &p_name, const Variant &p_value) override;
	virtual Variant get_parameter(const StringName &p_name) const override;

	void set_stream(const Ref<AudioStreamFFmpeg> &p_stream);

	AudioStreamPlaybackFFmpeg();
	~AudioStreamPlaybackFFmpeg();
};

class AudioStreamFFmpeg : public AudioStream {
	GDCLASS(AudioStreamFFmpeg, AudioStream);
	OBJ_SAVE_TYPE(AudioStream);
	RES_BASE_EXTENSION("ffmpegstr");

	String file;
	String headers;
	bool use_icy = false;
	int stream_index = -1;

	mutable FFmpegInputContext probe_input;
	mutable bool probe_loaded = false;
	mutable double length = 0.0;
	mutable bool monophonic = false;
	mutable Dictionary tags;

	Error ensure_probe() const;
	void clear_probe() const;

protected:
	static void _bind_methods();

public:
	Error open(const String &p_path, int p_stream_index = -1);
	void close();
	bool is_open() const;

	void set_file(const String &p_file);
	String get_file() const;

	void set_stream_index(int p_stream_index);
	int get_stream_index() const;

	void set_use_icy(bool p_enabled);
	bool get_use_icy() const;
	Dictionary get_icy_headers() const;
	String get_stream_title() const;

	void set_headers(const String &p_headers);
	String get_headers() const;

	virtual Ref<AudioStreamPlayback> instantiate_playback() override;
	virtual String get_stream_name() const override;
	virtual double get_length() const override;
	virtual bool is_monophonic() const override;
	virtual Dictionary get_tags() const override;
	virtual void get_parameter_list(List<Parameter> *r_parameters) override;

	AudioStreamFFmpeg();
	~AudioStreamFFmpeg();

	friend class AudioStreamPlaybackFFmpeg;
};

class ResourceFormatLoaderFFmpegAudio : public ResourceFormatLoader {
	GDSOFTCLASS(ResourceFormatLoaderFFmpegAudio, ResourceFormatLoader);

public:
	virtual Ref<Resource> load(const String &p_path, const String &p_original_path = "", Error *r_error = nullptr, bool p_use_sub_threads = false, float *r_progress = nullptr, CacheMode p_cache_mode = CACHE_MODE_REUSE) override;
	virtual void get_recognized_extensions(List<String> *p_extensions) const override;
	virtual bool handles_type(const String &p_type) const override;
	virtual String get_resource_type(const String &p_path) const override;
};
