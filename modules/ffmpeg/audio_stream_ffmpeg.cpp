/**************************************************************************/
/*  audio_stream_ffmpeg.cpp                                               */
/**************************************************************************/

#include "audio_stream_ffmpeg.h"

#include "core/object/class_db.h"

static bool _ffmpeg_path_has_audio_extension(const String &p_path) {
	static const char *extensions[] = {
		"mp3", "ogg", "oga", "opus", "wav", "flac", "aac", "m4a", "wma", "aiff", "aif", "alac", "webm", "mp4", "mkv"
	};

	for (const char *extension : extensions) {
		if (p_path.has_extension(extension)) {
			return true;
		}
	}
	return false;
}

AudioStreamPlaybackFFmpeg::AudioStreamPlaybackFFmpeg() {
	packet = FFmpegPacketPtr(av_packet_alloc());
	frame = FFmpegFramePtr(av_frame_alloc());
	convert_frame = FFmpegFramePtr(av_frame_alloc());
}

AudioStreamPlaybackFFmpeg::~AudioStreamPlaybackFFmpeg() {
	close_decoder();
}

void AudioStreamPlaybackFFmpeg::set_stream(const Ref<AudioStreamFFmpeg> &p_stream) {
	stream = p_stream;
}

Error AudioStreamPlaybackFFmpeg::open_decoder() {
	close_decoder();
	ERR_FAIL_COND_V(stream.is_null(), ERR_INVALID_PARAMETER);
	ERR_FAIL_COND_V(stream->file.is_empty(), ERR_INVALID_PARAMETER);

	Error err = FFmpegCommon::open_input(input, stream->file, stream->headers, stream->use_icy);
	if (err != OK) {
		return err;
	}

	int selected_stream = -1;
	if (stream->stream_index >= 0 && stream->stream_index < static_cast<int>(input.format->nb_streams)) {
		AVCodecParameters *params = input.format->streams[stream->stream_index]->codecpar;
		if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
			selected_stream = stream->stream_index;
		}
	}
	if (selected_stream < 0) {
		for (unsigned int i = 0; i < input.format->nb_streams; i++) {
			AVCodecParameters *params = input.format->streams[i]->codecpar;
			if (params->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(params->codec_id)) {
				selected_stream = i;
				break;
			}
		}
	}
	if (selected_stream < 0) {
		close_decoder();
		return ERR_DOES_NOT_EXIST;
	}

	audio_stream = input.format->streams[selected_stream];
	const AVCodec *codec = avcodec_find_decoder(audio_stream->codecpar->codec_id);
	if (!codec) {
		close_decoder();
		return ERR_UNAVAILABLE;
	}

	AVCodecContext *raw_context = avcodec_alloc_context3(codec);
	if (!raw_context) {
		close_decoder();
		return ERR_OUT_OF_MEMORY;
	}
	codec_context = FFmpegCodecContextPtr(raw_context);

	int response = avcodec_parameters_to_context(codec_context.get(), audio_stream->codecpar);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to copy audio codec parameters", response);
		close_decoder();
		return ERR_CANT_CREATE;
	}
	FFmpegCommon::enable_multithreading(codec_context.get(), codec);
	response = avcodec_open2(codec_context.get(), codec, nullptr);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to open audio codec", response);
		close_decoder();
		return ERR_CANT_OPEN;
	}

	AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
	SwrContext *raw_swr = nullptr;
	response = swr_alloc_set_opts2(&raw_swr, &output_layout, AV_SAMPLE_FMT_FLT, codec_context->sample_rate, &codec_context->ch_layout, codec_context->sample_fmt, codec_context->sample_rate, 0, nullptr);
	if (response < 0 || !raw_swr) {
		FFmpegCommon::print_error("FFmpeg failed to allocate audio resampler", response);
		close_decoder();
		return ERR_CANT_CREATE;
	}
	swr_context = FFmpegSwrContextPtr(raw_swr);
	response = swr_init(swr_context.get());
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to initialize audio resampler", response);
		close_decoder();
		return ERR_CANT_CREATE;
	}

	mix_rate = codec_context->sample_rate;
	eof = false;
	frames_mixed = 0;
	loop_count = 0;
	return OK;
}

void AudioStreamPlaybackFFmpeg::close_decoder() {
	swr_context.reset();
	codec_context.reset();
	input.clear();
	audio_stream = nullptr;
	buffer.clear();
	buffer_offset = 0;
	loop_count = 0;
	eof = false;
}

bool AudioStreamPlaybackFFmpeg::decode_next_frame() {
	if (!input.format || !codec_context || !audio_stream || !swr_context || eof) {
		return false;
	}

	int response = FFmpegCommon::get_frame(input.format.get(), codec_context.get(), audio_stream->index, frame.get(), packet.get());
	if (response == AVERROR_EOF) {
		eof = true;
		return false;
	}
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to decode audio frame", response);
		eof = true;
		return false;
	}

	av_frame_unref(convert_frame.get());
	convert_frame->format = AV_SAMPLE_FMT_FLT;
	AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
	convert_frame->ch_layout = output_layout;
	convert_frame->sample_rate = frame->sample_rate;
	convert_frame->nb_samples = swr_get_out_samples(swr_context.get(), frame->nb_samples);

	response = av_frame_get_buffer(convert_frame.get(), 0);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to allocate converted audio frame", response);
		av_frame_unref(frame.get());
		return false;
	}

	response = swr_convert_frame(swr_context.get(), convert_frame.get(), frame.get());
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to convert audio frame", response);
		av_frame_unref(frame.get());
		av_frame_unref(convert_frame.get());
		return false;
	}

	const int old_size = buffer.size();
	buffer.resize(old_size + convert_frame->nb_samples);
	const float *samples = reinterpret_cast<const float *>(convert_frame->extended_data[0]);
	AudioFrame *write = buffer.ptrw() + old_size;
	for (int i = 0; i < convert_frame->nb_samples; i++) {
		float left = samples[i * 2 + 0];
		float right = samples[i * 2 + 1];
		if (downmix_to_mono) {
			left = (left + right) * 0.5f;
			right = left;
		}
		write[i] = AudioFrame(left, right);
	}

	av_frame_unref(frame.get());
	av_frame_unref(convert_frame.get());
	av_packet_unref(packet.get());
	return true;
}

bool AudioStreamPlaybackFFmpeg::fill_buffer(int p_frames) {
	while (!eof && buffer.size() - buffer_offset < p_frames) {
		if (!decode_next_frame()) {
			break;
		}
	}
	return buffer.size() - buffer_offset > 0;
}

int AudioStreamPlaybackFFmpeg::_mix_internal(AudioFrame *p_buffer, int p_frames) {
	const bool has_loop_range = loop_enabled && loop_end > loop_start;
	const double active_end_time = has_loop_range ? loop_end : end_time;
	int total_mixed = 0;

	while (total_mixed < p_frames) {
		int frames_to_mix = p_frames - total_mixed;
		if (active_end_time > 0.0) {
			const int frames_until_end = static_cast<int>(MAX(0.0, active_end_time * mix_rate - frames_mixed));
			if (frames_until_end <= 0) {
				if (has_loop_range) {
					seek(loop_start);
					loop_count++;
					continue;
				}
				active = false;
				break;
			}
			frames_to_mix = MIN(frames_to_mix, frames_until_end);
		}

		if (!active || !fill_buffer(frames_to_mix)) {
			break;
		}

		const int available = buffer.size() - buffer_offset;
		const int mixed = MIN(available, frames_to_mix);
		if (mixed <= 0) {
			break;
		}

		const AudioFrame *read = buffer.ptr() + buffer_offset;
		for (int i = 0; i < mixed; i++) {
			p_buffer[total_mixed + i] = read[i];
		}

		buffer_offset += mixed;
		frames_mixed += mixed;
		total_mixed += mixed;
		if (buffer_offset >= buffer.size()) {
			buffer.clear();
			buffer_offset = 0;
		}

		if (has_loop_range && get_playback_position() >= loop_end) {
			seek(loop_start);
			loop_count++;
		} else if (!has_loop_range && end_time > 0.0 && get_playback_position() >= end_time) {
			active = false;
			break;
		} else if (mixed < frames_to_mix && eof) {
			if (has_loop_range) {
				seek(loop_start);
				loop_count++;
				continue;
			}
			active = false;
			break;
		}
	}

	return total_mixed;
}

float AudioStreamPlaybackFFmpeg::get_stream_sampling_rate() {
	return mix_rate;
}

void AudioStreamPlaybackFFmpeg::start(double p_from_pos) {
	if (!input.format && open_decoder() != OK) {
		return;
	}
	active = true;
	seek(p_from_pos);
	begin_resample();
}

void AudioStreamPlaybackFFmpeg::stop() {
	active = false;
}

bool AudioStreamPlaybackFFmpeg::is_playing() const {
	return active;
}

int AudioStreamPlaybackFFmpeg::get_loop_count() const {
	return loop_count;
}

double AudioStreamPlaybackFFmpeg::get_playback_position() const {
	return static_cast<double>(frames_mixed) / MAX(1, mix_rate);
}

void AudioStreamPlaybackFFmpeg::seek(double p_time) {
	if (!input.format || !codec_context || !audio_stream) {
		return;
	}

	const int64_t timestamp = av_rescale_q(static_cast<int64_t>(MAX(0.0, p_time) * AV_TIME_BASE), AV_TIME_BASE_Q, audio_stream->time_base);
	avcodec_flush_buffers(codec_context.get());
	const int response = av_seek_frame(input.format.get(), audio_stream->index, timestamp, AVSEEK_FLAG_BACKWARD);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to seek audio", response);
	}

	buffer.clear();
	buffer_offset = 0;
	eof = false;
	frames_mixed = p_time * mix_rate;
}

void AudioStreamPlaybackFFmpeg::tag_used_streams() {
	if (stream.is_valid()) {
		stream->tag_used(get_playback_position());
	}
}

void AudioStreamPlaybackFFmpeg::set_parameter(const StringName &p_name, const Variant &p_value) {
	if (p_name == SNAME("loop")) {
		loop_enabled = p_value;
	} else if (p_name == SNAME("loop_start")) {
		loop_start = MAX(0.0, double(p_value));
	} else if (p_name == SNAME("loop_end")) {
		loop_end = MAX(0.0, double(p_value));
	} else if (p_name == SNAME("end_time")) {
		end_time = MAX(0.0, double(p_value));
	} else if (p_name == SNAME("downmix_to_mono")) {
		downmix_to_mono = p_value;
	}
}

Variant AudioStreamPlaybackFFmpeg::get_parameter(const StringName &p_name) const {
	if (p_name == SNAME("loop")) {
		return loop_enabled;
	}
	if (p_name == SNAME("loop_start")) {
		return loop_start;
	}
	if (p_name == SNAME("loop_end")) {
		return loop_end;
	}
	if (p_name == SNAME("end_time")) {
		return end_time;
	}
	if (p_name == SNAME("downmix_to_mono")) {
		return downmix_to_mono;
	}
	return Variant();
}

AudioStreamFFmpeg::AudioStreamFFmpeg() {}

AudioStreamFFmpeg::~AudioStreamFFmpeg() {
	close();
}

void AudioStreamFFmpeg::clear_probe() const {
	probe_input.clear();
	probe_loaded = false;
	length = 0.0;
	monophonic = false;
	tags.clear();
}

Error AudioStreamFFmpeg::ensure_probe() const {
	if (probe_loaded) {
		return OK;
	}
	if (file.is_empty()) {
		return ERR_INVALID_PARAMETER;
	}

	Error err = FFmpegCommon::open_input(probe_input, file, headers, use_icy);
	if (err != OK) {
		return err;
	}

	AVStream *selected_stream = nullptr;
	if (stream_index >= 0 && stream_index < static_cast<int>(probe_input.format->nb_streams)) {
		AVCodecParameters *params = probe_input.format->streams[stream_index]->codecpar;
		if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
			selected_stream = probe_input.format->streams[stream_index];
		}
	}
	if (!selected_stream) {
		for (unsigned int i = 0; i < probe_input.format->nb_streams; i++) {
			AVCodecParameters *params = probe_input.format->streams[i]->codecpar;
			if (params->codec_type == AVMEDIA_TYPE_AUDIO && avcodec_find_decoder(params->codec_id)) {
				selected_stream = probe_input.format->streams[i];
				break;
			}
		}
	}

	if (!selected_stream) {
		clear_probe();
		return ERR_DOES_NOT_EXIST;
	}

	if (selected_stream->duration != AV_NOPTS_VALUE) {
		length = selected_stream->duration * av_q2d(selected_stream->time_base);
	} else if (probe_input.format->duration != AV_NOPTS_VALUE) {
		length = static_cast<double>(probe_input.format->duration) / AV_TIME_BASE;
	}
	monophonic = selected_stream->codecpar->ch_layout.nb_channels == 1;

	AVDictionaryEntry *entry = nullptr;
	while ((entry = av_dict_get(probe_input.format->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		tags[String(entry->key).replace("icy-", "stream_")] = String(entry->value);
	}
	while ((entry = av_dict_get(selected_stream->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		tags[String(entry->key).replace("icy-", "stream_")] = String(entry->value);
	}

	probe_loaded = true;
	return OK;
}

Error AudioStreamFFmpeg::open(const String &p_path, int p_stream_index) {
	file = p_path;
	stream_index = p_stream_index;
	clear_probe();
	return ensure_probe();
}

void AudioStreamFFmpeg::close() {
	clear_probe();
}

bool AudioStreamFFmpeg::is_open() const {
	return ensure_probe() == OK;
}

void AudioStreamFFmpeg::set_file(const String &p_file) {
	if (file == p_file) {
		return;
	}
	file = p_file;
	clear_probe();
	emit_changed();
}

String AudioStreamFFmpeg::get_file() const {
	return file;
}

void AudioStreamFFmpeg::set_stream_index(int p_stream_index) {
	if (stream_index == p_stream_index) {
		return;
	}
	stream_index = p_stream_index;
	clear_probe();
	emit_changed();
}

int AudioStreamFFmpeg::get_stream_index() const {
	return stream_index;
}

void AudioStreamFFmpeg::set_use_icy(bool p_enabled) {
	if (use_icy == p_enabled) {
		return;
	}
	use_icy = p_enabled;
	clear_probe();
	emit_changed();
}

bool AudioStreamFFmpeg::get_use_icy() const {
	return use_icy;
}

Dictionary AudioStreamFFmpeg::get_icy_headers() const {
	Dictionary result;
	if (!use_icy || ensure_probe() != OK || !probe_input.format) {
		return result;
	}

	char *metadata = nullptr;
	av_opt_get(probe_input.format.get(), "icy_metadata_headers", AV_OPT_SEARCH_CHILDREN, reinterpret_cast<uint8_t **>(&metadata));
	if (!metadata) {
		return result;
	}

	PackedStringArray headers_array = String(metadata).split("\n");
	for (int i = 0; i < headers_array.size(); i++) {
		PackedStringArray key_value = headers_array[i].split(": ");
		if (key_value.size() == 2) {
			result[key_value[0].replace("icy-", "stream_")] = key_value[1];
		}
	}
	av_freep(&metadata);
	return result;
}

String AudioStreamFFmpeg::get_stream_title() const {
	if (!use_icy || ensure_probe() != OK || !probe_input.format) {
		return String();
	}

	char *metadata = nullptr;
	av_opt_get(probe_input.format.get(), "icy_metadata_packet", AV_OPT_SEARCH_CHILDREN, reinterpret_cast<uint8_t **>(&metadata));
	if (!metadata) {
		return String();
	}

	String title;
	PackedStringArray parts = String(metadata).split(";");
	for (int i = 0; i < parts.size(); i++) {
		PackedStringArray key_value = parts[i].split("=");
		if (key_value.size() == 2 && key_value[0] == "StreamTitle") {
			title = String(key_value[1]).lstrip("'").rstrip("'");
			break;
		}
	}
	av_freep(&metadata);
	return title;
}

void AudioStreamFFmpeg::set_headers(const String &p_headers) {
	if (headers == p_headers) {
		return;
	}
	headers = p_headers;
	clear_probe();
	emit_changed();
}

String AudioStreamFFmpeg::get_headers() const {
	return headers;
}

Ref<AudioStreamPlayback> AudioStreamFFmpeg::instantiate_playback() {
	Ref<AudioStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	playback->set_stream(Ref<AudioStreamFFmpeg>(this));
	if (playback->open_decoder() != OK) {
		return Ref<AudioStreamPlayback>();
	}
	return playback;
}

String AudioStreamFFmpeg::get_stream_name() const {
	if (!file.is_empty()) {
		return file.get_file();
	}
	return "FFmpeg Audio";
}

double AudioStreamFFmpeg::get_length() const {
	ensure_probe();
	return length;
}

bool AudioStreamFFmpeg::is_monophonic() const {
	ensure_probe();
	return monophonic;
}

Dictionary AudioStreamFFmpeg::get_tags() const {
	ensure_probe();
	return tags;
}

void AudioStreamFFmpeg::get_parameter_list(List<Parameter> *r_parameters) {
	r_parameters->push_back(Parameter(PropertyInfo(Variant::BOOL, "loop", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_CHECKABLE), false));
	r_parameters->push_back(Parameter(PropertyInfo(Variant::FLOAT, "loop_start", PROPERTY_HINT_RANGE, "0,1280000,0.001,or_greater,suffix:s"), 0.0));
	r_parameters->push_back(Parameter(PropertyInfo(Variant::FLOAT, "loop_end", PROPERTY_HINT_RANGE, "0,1280000,0.001,or_greater,suffix:s"), 0.0));
	r_parameters->push_back(Parameter(PropertyInfo(Variant::FLOAT, "end_time", PROPERTY_HINT_RANGE, "0,1280000,0.001,or_greater,suffix:s"), 0.0));
	r_parameters->push_back(Parameter(PropertyInfo(Variant::BOOL, "downmix_to_mono"), false));
}

void AudioStreamFFmpeg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open", "path", "stream_index"), &AudioStreamFFmpeg::open, DEFVAL(-1));
	ClassDB::bind_method(D_METHOD("close"), &AudioStreamFFmpeg::close);
	ClassDB::bind_method(D_METHOD("is_open"), &AudioStreamFFmpeg::is_open);

	ClassDB::bind_method(D_METHOD("set_file", "file"), &AudioStreamFFmpeg::set_file);
	ClassDB::bind_method(D_METHOD("get_file"), &AudioStreamFFmpeg::get_file);

	ClassDB::bind_method(D_METHOD("set_stream_index", "stream_index"), &AudioStreamFFmpeg::set_stream_index);
	ClassDB::bind_method(D_METHOD("get_stream_index"), &AudioStreamFFmpeg::get_stream_index);

	ClassDB::bind_method(D_METHOD("set_use_icy", "enabled"), &AudioStreamFFmpeg::set_use_icy);
	ClassDB::bind_method(D_METHOD("get_use_icy"), &AudioStreamFFmpeg::get_use_icy);
	ClassDB::bind_method(D_METHOD("get_icy_headers"), &AudioStreamFFmpeg::get_icy_headers);
	ClassDB::bind_method(D_METHOD("get_stream_title"), &AudioStreamFFmpeg::get_stream_title);

	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &AudioStreamFFmpeg::set_headers);
	ClassDB::bind_method(D_METHOD("get_headers"), &AudioStreamFFmpeg::get_headers);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stream_index", PROPERTY_HINT_RANGE, "-1,128,1"), "set_stream_index", "get_stream_index");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_icy"), "set_use_icy", "get_use_icy");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "headers"), "set_headers", "get_headers");
}

Ref<Resource> ResourceFormatLoaderFFmpegAudio::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<AudioStreamFFmpeg> stream;
	stream.instantiate();
	stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}
	return stream;
}

void ResourceFormatLoaderFFmpegAudio::get_recognized_extensions(List<String> *p_extensions) const {
	static const char *extensions[] = {
		"mp3", "ogg", "oga", "opus", "wav", "flac", "aac", "m4a", "wma", "aiff", "aif", "alac", "webm", "mp4", "mkv"
	};

	for (const char *extension : extensions) {
		p_extensions->push_back(extension);
	}
}

bool ResourceFormatLoaderFFmpegAudio::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "AudioStream");
}

String ResourceFormatLoaderFFmpegAudio::get_resource_type(const String &p_path) const {
	if (_ffmpeg_path_has_audio_extension(p_path)) {
		return "AudioStreamFFmpeg";
	}
	return "";
}
