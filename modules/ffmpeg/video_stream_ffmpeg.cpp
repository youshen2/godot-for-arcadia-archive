/**************************************************************************/
/*  video_stream_ffmpeg.cpp                                               */
/**************************************************************************/

#include "video_stream_ffmpeg.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"

static bool _ffmpeg_path_has_video_extension(const String &p_path) {
	static const char *extensions[] = {
		"mp4", "m4v", "mov", "mkv", "webm", "ogv", "avi", "wmv", "flv", "mpeg", "mpg", "m2ts", "ts", "gif", "webp"
	};

	for (const char *extension : extensions) {
		if (p_path.has_extension(extension)) {
			return true;
		}
	}
	return false;
}

VideoStreamPlaybackFFmpeg::VideoStreamPlaybackFFmpeg() {
	texture.instantiate();
	video_packet = FFmpegPacketPtr(av_packet_alloc());
	video_frame = FFmpegFramePtr(av_frame_alloc());
	rgba_frame = FFmpegFramePtr(av_frame_alloc());
	audio_packet = FFmpegPacketPtr(av_packet_alloc());
	audio_frame = FFmpegFramePtr(av_frame_alloc());
	audio_convert_frame = FFmpegFramePtr(av_frame_alloc());
}

VideoStreamPlaybackFFmpeg::~VideoStreamPlaybackFFmpeg() {
	close_audio();
	close_video();
}

Error VideoStreamPlaybackFFmpeg::open_codec_context(AVStream *p_stream, FFmpegCodecContextPtr &r_codec_context) {
	ERR_FAIL_NULL_V(p_stream, ERR_INVALID_PARAMETER);

	const AVCodec *codec = avcodec_find_decoder(p_stream->codecpar->codec_id);
	ERR_FAIL_NULL_V_MSG(codec, ERR_UNAVAILABLE, "FFmpeg decoder not found for stream.");

	AVCodecContext *raw_context = avcodec_alloc_context3(codec);
	ERR_FAIL_NULL_V(raw_context, ERR_OUT_OF_MEMORY);

	r_codec_context = FFmpegCodecContextPtr(raw_context);
	int response = avcodec_parameters_to_context(r_codec_context.get(), p_stream->codecpar);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to copy codec parameters", response);
		r_codec_context.reset();
		return ERR_CANT_CREATE;
	}

	FFmpegCommon::enable_multithreading(r_codec_context.get(), codec);
	if (p_stream->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && key_frame_only) {
		r_codec_context->skip_frame = AVDISCARD_NONKEY;
	}

	response = avcodec_open2(r_codec_context.get(), codec, nullptr);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to open codec", response);
		r_codec_context.reset();
		return ERR_CANT_OPEN;
	}

	return OK;
}

double VideoStreamPlaybackFFmpeg::get_stream_duration(AVFormatContext *p_format_context, AVStream *p_stream) const {
	if (p_stream && p_stream->duration != AV_NOPTS_VALUE) {
		return p_stream->duration * av_q2d(p_stream->time_base);
	}
	if (p_format_context && p_format_context->duration != AV_NOPTS_VALUE) {
		return static_cast<double>(p_format_context->duration) / AV_TIME_BASE;
	}
	return 0.0;
}

int VideoStreamPlaybackFFmpeg::find_video_stream_index() const {
	if (!video_input.format) {
		return -1;
	}

	AVFormatContext *format_context = video_input.format.get();
	int video_ordinal = 0;
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream *stream = format_context->streams[i];
		AVCodecParameters *params = stream->codecpar;
		if (params->codec_type != AVMEDIA_TYPE_VIDEO || (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) || !avcodec_find_decoder(params->codec_id)) {
			continue;
		}
		if (video_ordinal == video_track) {
			return i;
		}
		video_ordinal++;
	}
	return -1;
}

Error VideoStreamPlaybackFFmpeg::open_video() {
	close_video();

	Error err = FFmpegCommon::open_input(video_input, file, headers);
	if (err != OK) {
		return err;
	}

	AVFormatContext *format_context = video_input.format.get();
	const int stream_index = find_video_stream_index();
	if (stream_index < 0) {
		close_video();
		return ERR_FILE_UNRECOGNIZED;
	}
	video_stream = format_context->streams[stream_index];

	err = open_codec_context(video_stream, video_codec_context);
	if (err != OK) {
		close_video();
		return err;
	}

	source_width = video_codec_context->width;
	source_height = video_codec_context->height;
	update_display_rotation();
	frame_size = get_rotated_frame_size();
	length = get_stream_duration(format_context, video_stream);

	if (video_stream->avg_frame_rate.num > 0 && video_stream->avg_frame_rate.den > 0) {
		frame_rate = av_q2d(video_stream->avg_frame_rate);
	} else if (video_stream->r_frame_rate.num > 0 && video_stream->r_frame_rate.den > 0) {
		frame_rate = av_q2d(video_stream->r_frame_rate);
	}
	if (frame_rate <= 0.0) {
		frame_rate = 24.0;
	}

	Ref<Image> image = Image::create_empty(MAX(1, frame_size.x), MAX(1, frame_size.y), false, Image::FORMAT_RGBA8);
	texture->set_image(image);

	video_eof = false;
	has_pending_frame = false;
	time = 0.0;
	last_frame_upload_time = -1.0;
	read_next_video_frame();
	if (has_pending_frame) {
		upload_pending_frame();
	}

	return OK;
}

void VideoStreamPlaybackFFmpeg::close_video() {
	sws_context.reset();
	video_codec_context.reset();
	video_input.clear();
	video_stream = nullptr;
	source_width = 0;
	source_height = 0;
	source_pixel_format = AV_PIX_FMT_NONE;
	frame_size = Size2i();
	video_eof = false;
	has_pending_frame = false;
	last_frame_upload_time = -1.0;
	display_rotation = 0;
	frame_data.clear();
	pending_frame_data.clear();
}

Size2i VideoStreamPlaybackFFmpeg::get_rotated_frame_size() const {
	if (apply_rotation_metadata && (display_rotation == 90 || display_rotation == 270)) {
		return Size2i(source_height, source_width);
	}
	return Size2i(source_width, source_height);
}

void VideoStreamPlaybackFFmpeg::update_display_rotation() {
	display_rotation = 0;
	if (!video_stream) {
		return;
	}

	const AVPacketSideData *side_data = av_packet_side_data_get(video_stream->codecpar->coded_side_data, video_stream->codecpar->nb_coded_side_data, AV_PKT_DATA_DISPLAYMATRIX);
	if (!side_data || side_data->size < 9 * sizeof(int32_t)) {
		return;
	}

	const double rotation = -av_display_rotation_get(reinterpret_cast<const int32_t *>(side_data->data));
	int rounded_rotation = static_cast<int>(Math::round(rotation / 90.0)) * 90;
	rounded_rotation %= 360;
	if (rounded_rotation < 0) {
		rounded_rotation += 360;
	}
	if (rounded_rotation == 90 || rounded_rotation == 180 || rounded_rotation == 270) {
		display_rotation = rounded_rotation;
	}
}

void VideoStreamPlaybackFFmpeg::rotate_frame_data(const Vector<uint8_t> &p_source, Vector<uint8_t> &r_target) const {
	if (!apply_rotation_metadata || display_rotation == 0) {
		r_target = p_source;
		return;
	}

	const int output_width = frame_size.x;
	const int output_height = frame_size.y;
	ERR_FAIL_COND(source_width <= 0 || source_height <= 0 || output_width <= 0 || output_height <= 0);

	r_target.resize(output_width * output_height * 4);
	const uint8_t *src = p_source.ptr();
	uint8_t *dst = r_target.ptrw();

	for (int y = 0; y < source_height; y++) {
		for (int x = 0; x < source_width; x++) {
			int dst_x = x;
			int dst_y = y;
			switch (display_rotation) {
				case 90:
					dst_x = source_height - 1 - y;
					dst_y = x;
					break;
				case 180:
					dst_x = source_width - 1 - x;
					dst_y = source_height - 1 - y;
					break;
				case 270:
					dst_x = y;
					dst_y = source_width - 1 - x;
					break;
				default:
					break;
			}

			const int src_offset = (y * source_width + x) * 4;
			const int dst_offset = (dst_y * output_width + dst_x) * 4;
			dst[dst_offset + 0] = src[src_offset + 0];
			dst[dst_offset + 1] = src[src_offset + 1];
			dst[dst_offset + 2] = src[src_offset + 2];
			dst[dst_offset + 3] = src[src_offset + 3];
		}
	}
}

int VideoStreamPlaybackFFmpeg::find_audio_stream_index() const {
	if (!audio_input.format) {
		return -1;
	}

	AVFormatContext *format_context = audio_input.format.get();
	int audio_ordinal = 0;
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream *stream = format_context->streams[i];
		AVCodecParameters *params = stream->codecpar;
		if (params->codec_type != AVMEDIA_TYPE_AUDIO || !avcodec_find_decoder(params->codec_id)) {
			continue;
		}
		if (audio_ordinal == audio_track) {
			return i;
		}
		audio_ordinal++;
	}
	return -1;
}

Error VideoStreamPlaybackFFmpeg::open_audio() {
	close_audio();
	if (!audio_enabled || file.is_empty()) {
		return OK;
	}

	Error err = FFmpegCommon::open_input(audio_input, file, headers);
	if (err != OK) {
		return err;
	}

	const int stream_index = find_audio_stream_index();
	if (stream_index < 0) {
		close_audio();
		return ERR_DOES_NOT_EXIST;
	}

	audio_stream = audio_input.format->streams[stream_index];
	err = open_codec_context(audio_stream, audio_codec_context);
	if (err != OK) {
		close_audio();
		return err;
	}

	AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
	SwrContext *raw_swr = nullptr;
	int response = swr_alloc_set_opts2(&raw_swr, &output_layout, AV_SAMPLE_FMT_FLT, audio_codec_context->sample_rate, &audio_codec_context->ch_layout, audio_codec_context->sample_fmt, audio_codec_context->sample_rate, 0, nullptr);
	if (response < 0 || !raw_swr) {
		FFmpegCommon::print_error("FFmpeg failed to allocate audio resampler", response);
		close_audio();
		return ERR_CANT_CREATE;
	}

	swr_context = FFmpegSwrContextPtr(raw_swr);
	response = swr_init(swr_context.get());
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to initialize audio resampler", response);
		close_audio();
		return ERR_CANT_CREATE;
	}

	audio_channels = 2;
	audio_mix_rate = audio_codec_context->sample_rate;
	audio_decoded_time = time;
	audio_eof = false;

	if (time > 0.0) {
		seek(time);
	}

	return OK;
}

void VideoStreamPlaybackFFmpeg::close_audio() {
	swr_context.reset();
	audio_codec_context.reset();
	audio_input.clear();
	audio_stream = nullptr;
	audio_channels = 0;
	audio_mix_rate = 0;
	audio_eof = false;
	audio_decoded_time = time;
	pending_audio.clear();
	pending_audio_frames = 0;
	pending_audio_offset = 0;
}

double VideoStreamPlaybackFFmpeg::get_frame_time(const AVFrame *p_frame) const {
	if (!p_frame || !video_stream) {
		return time;
	}

	int64_t timestamp = p_frame->best_effort_timestamp;
	if (timestamp == AV_NOPTS_VALUE) {
		timestamp = p_frame->pts;
	}
	if (timestamp == AV_NOPTS_VALUE) {
		return time;
	}
	return timestamp * av_q2d(video_stream->time_base);
}

void VideoStreamPlaybackFFmpeg::recreate_sws_context(const AVFrame *p_frame) {
	ERR_FAIL_NULL(p_frame);

	const AVPixelFormat frame_format = static_cast<AVPixelFormat>(p_frame->format);
	if (sws_context && source_width == p_frame->width && source_height == p_frame->height && source_pixel_format == frame_format) {
		return;
	}

	source_width = p_frame->width;
	source_height = p_frame->height;
	source_pixel_format = frame_format;
	frame_size = get_rotated_frame_size();

	sws_context = FFmpegSwsContextPtr(sws_getContext(source_width, source_height, frame_format, source_width, source_height, AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr));
	ERR_FAIL_COND(!sws_context);
	apply_sws_color_profile(p_frame);
}

void VideoStreamPlaybackFFmpeg::apply_sws_color_profile(const AVFrame *p_frame) {
	if (!sws_context) {
		return;
	}

	int colorspace = SWS_CS_DEFAULT;
	switch (color_profile) {
		case 1:
			colorspace = SWS_CS_FCC;
			break;
		case 2:
			colorspace = SWS_CS_ITU601;
			break;
		case 3:
			colorspace = SWS_CS_ITU709;
			break;
		case 4:
		case 5:
			colorspace = SWS_CS_BT2020;
			break;
		default:
			if (p_frame) {
				if (p_frame->colorspace == AVCOL_SPC_BT709) {
					colorspace = SWS_CS_ITU709;
				} else if (p_frame->colorspace == AVCOL_SPC_BT2020_NCL || p_frame->colorspace == AVCOL_SPC_BT2020_CL) {
					colorspace = SWS_CS_BT2020;
				} else if (p_frame->colorspace == AVCOL_SPC_FCC) {
					colorspace = SWS_CS_FCC;
				}
			}
			break;
	}

	const int *coefficients = sws_getCoefficients(colorspace);
	const int source_range = p_frame && p_frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
	sws_setColorspaceDetails(sws_context.get(), coefficients, source_range, coefficients, 1, 0, 1 << 16, 1 << 16);
}

bool VideoStreamPlaybackFFmpeg::read_next_video_frame() {
	if (!video_input.format || !video_codec_context || !video_stream || video_eof) {
		return false;
	}

	int response = FFmpegCommon::get_frame(video_input.format.get(), video_codec_context.get(), video_stream->index, video_frame.get(), video_packet.get());
	if (response == AVERROR_EOF) {
		video_eof = true;
		return false;
	}
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to decode video frame", response);
		video_eof = true;
		return false;
	}

	recreate_sws_context(video_frame.get());
	if (!sws_context) {
		video_eof = true;
		return false;
	}

	const int required_size = av_image_get_buffer_size(AV_PIX_FMT_RGBA, source_width, source_height, 1);
	if (required_size <= 0) {
		video_eof = true;
		return false;
	}

	Vector<uint8_t> converted_frame_data;
	converted_frame_data.resize(required_size);
	av_image_fill_arrays(rgba_frame->data, rgba_frame->linesize, converted_frame_data.ptrw(), AV_PIX_FMT_RGBA, source_width, source_height, 1);
	sws_scale(sws_context.get(), video_frame->data, video_frame->linesize, 0, source_height, rgba_frame->data, rgba_frame->linesize);
	rotate_frame_data(converted_frame_data, pending_frame_data);

	pending_frame_time = get_frame_time(video_frame.get());
	has_pending_frame = true;
	av_frame_unref(video_frame.get());
	av_packet_unref(video_packet.get());
	return true;
}

void VideoStreamPlaybackFFmpeg::upload_pending_frame() {
	if (!has_pending_frame || pending_frame_data.is_empty()) {
		return;
	}

	if (frame_size.x <= 0 || frame_size.y <= 0) {
		return;
	}

	Ref<Image> image = Image::create_from_data(frame_size.x, frame_size.y, false, Image::FORMAT_RGBA8, pending_frame_data);
	if (texture->get_width() != frame_size.x || texture->get_height() != frame_size.y) {
		texture->set_image(image);
	} else {
		texture->update(image);
	}

	has_pending_frame = false;
	last_frame_upload_time = time;
}

bool VideoStreamPlaybackFFmpeg::send_pending_audio() {
	if (!mix_callback || pending_audio_frames <= 0) {
		return pending_audio_frames <= 0;
	}

	const int frames_left = pending_audio_frames - pending_audio_offset;
	if (frames_left <= 0) {
		pending_audio.clear();
		pending_audio_frames = 0;
		pending_audio_offset = 0;
		return true;
	}

	const float *data = pending_audio.ptr() + pending_audio_offset * audio_channels;
	const int mixed = mix_callback(mix_udata, data, frames_left);
	if (mixed <= 0) {
		return false;
	}

	pending_audio_offset += mixed;
	audio_decoded_time += static_cast<double>(mixed) / MAX(1, audio_mix_rate);

	if (pending_audio_offset >= pending_audio_frames) {
		pending_audio.clear();
		pending_audio_frames = 0;
		pending_audio_offset = 0;
		return true;
	}
	return false;
}

bool VideoStreamPlaybackFFmpeg::decode_next_audio_frame() {
	if (!audio_input.format || !audio_codec_context || !audio_stream || !swr_context || audio_eof) {
		return false;
	}
	if (!send_pending_audio()) {
		return true;
	}

	int response = FFmpegCommon::get_frame(audio_input.format.get(), audio_codec_context.get(), audio_stream->index, audio_frame.get(), audio_packet.get());
	if (response == AVERROR_EOF) {
		audio_eof = true;
		return false;
	}
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to decode audio frame", response);
		audio_eof = true;
		return false;
	}

	av_frame_unref(audio_convert_frame.get());
	audio_convert_frame->format = AV_SAMPLE_FMT_FLT;
	AVChannelLayout output_layout = AV_CHANNEL_LAYOUT_STEREO;
	audio_convert_frame->ch_layout = output_layout;
	audio_convert_frame->sample_rate = audio_frame->sample_rate;
	audio_convert_frame->nb_samples = swr_get_out_samples(swr_context.get(), audio_frame->nb_samples);

	response = av_frame_get_buffer(audio_convert_frame.get(), 0);
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to allocate converted audio frame", response);
		av_frame_unref(audio_frame.get());
		return false;
	}

	response = swr_convert_frame(swr_context.get(), audio_convert_frame.get(), audio_frame.get());
	if (response < 0) {
		FFmpegCommon::print_error("FFmpeg failed to convert audio frame", response);
		av_frame_unref(audio_frame.get());
		av_frame_unref(audio_convert_frame.get());
		return false;
	}

	pending_audio_frames = audio_convert_frame->nb_samples;
	pending_audio_offset = 0;
	pending_audio.resize(pending_audio_frames * audio_channels);
	memcpy(pending_audio.ptrw(), audio_convert_frame->extended_data[0], pending_audio.size() * sizeof(float));

	av_frame_unref(audio_frame.get());
	av_frame_unref(audio_convert_frame.get());
	av_packet_unref(audio_packet.get());
	return send_pending_audio();
}

void VideoStreamPlaybackFFmpeg::decode_audio_until(double p_time) {
	if (!audio_enabled || audio_channels <= 0 || !mix_callback) {
		return;
	}

	const int max_iterations = 64;
	int iterations = 0;
	while (audio_decoded_time < p_time && iterations < max_iterations) {
		if (!decode_next_audio_frame()) {
			break;
		}
		iterations++;
	}
}

void VideoStreamPlaybackFFmpeg::set_file(const String &p_file) {
	file = p_file;
	open_video();
}

void VideoStreamPlaybackFFmpeg::set_headers(const String &p_headers) {
	headers = p_headers;
	if (!file.is_empty()) {
		open_video();
		if (audio_enabled) {
			open_audio();
		}
	}
}

void VideoStreamPlaybackFFmpeg::play() {
	if (video_input.format == nullptr && !file.is_empty()) {
		open_video();
	}
	if (audio_enabled && audio_input.format == nullptr) {
		open_audio();
	}
	playing = video_input.format != nullptr;
	paused = false;
}

void VideoStreamPlaybackFFmpeg::stop() {
	playing = false;
	seek(0.0);
}

bool VideoStreamPlaybackFFmpeg::is_playing() const {
	return playing;
}

void VideoStreamPlaybackFFmpeg::set_paused(bool p_paused) {
	paused = p_paused;
}

bool VideoStreamPlaybackFFmpeg::is_paused() const {
	return paused;
}

double VideoStreamPlaybackFFmpeg::get_length() const {
	return length;
}

double VideoStreamPlaybackFFmpeg::get_playback_position() const {
	return time;
}

void VideoStreamPlaybackFFmpeg::seek(double p_time) {
	time = MAX(0.0, p_time);
	video_eof = false;
	audio_eof = false;
	has_pending_frame = false;
	last_frame_upload_time = -1.0;

	if (video_input.format && video_codec_context && video_stream) {
		const int64_t timestamp = av_rescale_q(static_cast<int64_t>(time * AV_TIME_BASE), AV_TIME_BASE_Q, video_stream->time_base);
		avcodec_flush_buffers(video_codec_context.get());
		int response = av_seek_frame(video_input.format.get(), video_stream->index, timestamp, AVSEEK_FLAG_BACKWARD);
		if (response < 0) {
			FFmpegCommon::print_error("FFmpeg failed to seek video", response);
		}
		read_next_video_frame();
		if (!accurate_seek && has_pending_frame) {
			upload_pending_frame();
		}
		while (accurate_seek && has_pending_frame && pending_frame_time <= time) {
			upload_pending_frame();
			if (!read_next_video_frame()) {
				break;
			}
		}
		if (has_pending_frame && texture->get_width() == 0) {
			upload_pending_frame();
		}
	}

	if (audio_enabled && audio_input.format && audio_codec_context && audio_stream) {
		const int64_t timestamp = av_rescale_q(static_cast<int64_t>(time * AV_TIME_BASE), AV_TIME_BASE_Q, audio_stream->time_base);
		avcodec_flush_buffers(audio_codec_context.get());
		int response = av_seek_frame(audio_input.format.get(), audio_stream->index, timestamp, AVSEEK_FLAG_BACKWARD);
		if (response < 0) {
			FFmpegCommon::print_error("FFmpeg failed to seek audio", response);
		}
		audio_decoded_time = time;
		pending_audio.clear();
		pending_audio_frames = 0;
		pending_audio_offset = 0;
	}
}

void VideoStreamPlaybackFFmpeg::set_video_track(int p_idx) {
	const int new_track = MAX(0, p_idx);
	if (video_track == new_track) {
		return;
	}

	const bool reopen_video = video_input.format != nullptr;
	const double position = time;
	video_track = new_track;
	if (reopen_video && !file.is_empty()) {
		if (open_video() == OK) {
			seek(position);
		}
	}
}

void VideoStreamPlaybackFFmpeg::set_audio_track(int p_idx) {
	const bool reopen_audio = audio_enabled && audio_input.format != nullptr;
	audio_track = MAX(0, p_idx);
	if (reopen_audio && !file.is_empty()) {
		open_audio();
	}
}

void VideoStreamPlaybackFFmpeg::set_audio_enabled(bool p_enabled) {
	if (audio_enabled == p_enabled && (!audio_enabled || audio_input.format != nullptr)) {
		return;
	}
	audio_enabled = p_enabled;
	if (audio_enabled) {
		open_audio();
	} else {
		close_audio();
	}
}

void VideoStreamPlaybackFFmpeg::set_audio_speed_to_sync(bool p_enabled) {
	audio_speed_to_sync = p_enabled;
}

void VideoStreamPlaybackFFmpeg::set_audio_buffering_msec(int p_msec) {
	audio_buffering_msec = MAX(10, p_msec);
}

void VideoStreamPlaybackFFmpeg::set_pitch_adjust_enabled(bool p_enabled) {
	pitch_adjust = p_enabled;
}

void VideoStreamPlaybackFFmpeg::set_color_profile(int p_profile) {
	color_profile = p_profile;
	if (video_frame) {
		apply_sws_color_profile(video_frame.get());
	}
}

void VideoStreamPlaybackFFmpeg::set_debug_enabled(bool p_enabled) {
	debug = p_enabled;
	av_log_set_level(debug ? AV_LOG_VERBOSE : AV_LOG_INFO);
}

void VideoStreamPlaybackFFmpeg::set_max_video_fps(double p_fps) {
	max_video_fps = MAX(0.0, p_fps);
}

void VideoStreamPlaybackFFmpeg::set_max_video_frames_per_update(int p_frames) {
	max_video_frames_per_update = MAX(1, p_frames);
}

void VideoStreamPlaybackFFmpeg::set_frame_dropping_enabled(bool p_enabled) {
	frame_dropping = p_enabled;
}

void VideoStreamPlaybackFFmpeg::set_key_frame_only_enabled(bool p_enabled) {
	if (key_frame_only == p_enabled) {
		return;
	}

	key_frame_only = p_enabled;
	if (video_codec_context) {
		video_codec_context->skip_frame = key_frame_only ? AVDISCARD_NONKEY : AVDISCARD_DEFAULT;
		avcodec_flush_buffers(video_codec_context.get());
		has_pending_frame = false;
		pending_frame_data.clear();
	}
}

void VideoStreamPlaybackFFmpeg::set_accurate_seek_enabled(bool p_enabled) {
	accurate_seek = p_enabled;
}

void VideoStreamPlaybackFFmpeg::set_apply_rotation_metadata_enabled(bool p_enabled) {
	if (apply_rotation_metadata == p_enabled) {
		return;
	}

	apply_rotation_metadata = p_enabled;
	frame_size = get_rotated_frame_size();
	if (video_input.format && video_codec_context && video_stream) {
		seek(time);
	}
}

Ref<Texture2D> VideoStreamPlaybackFFmpeg::get_texture() const {
	return texture;
}

void VideoStreamPlaybackFFmpeg::update(double p_delta) {
	if (!playing || paused || !video_input.format) {
		return;
	}

	time += p_delta;

	const double audio_target = time + static_cast<double>(audio_buffering_msec) / 1000.0;
	decode_audio_until(audio_target);

	const int max_video_frames = MAX(1, max_video_frames_per_update);
	const double source_frame_interval = frame_rate > 0.0 ? 1.0 / frame_rate : 1.0 / 24.0;
	const double max_fps_interval = max_video_fps > 0.0 ? 1.0 / max_video_fps : 0.0;
	const double stale_threshold = max_video_fps > 0.0 ? max_fps_interval : source_frame_interval;

	if (frame_dropping || max_video_fps > 0.0) {
		int frames_dropped = 0;
		while (has_pending_frame && pending_frame_time + stale_threshold < time && frames_dropped < max_video_frames) {
			has_pending_frame = false;
			pending_frame_data.clear();
			frames_dropped++;
			if (!read_next_video_frame()) {
				break;
			}
		}
	}

	const bool can_upload_frame = max_video_fps <= 0.0 || last_frame_upload_time < 0.0 || time - last_frame_upload_time >= max_fps_interval;
	int frames_uploaded = 0;
	while (can_upload_frame && has_pending_frame && pending_frame_time <= time && frames_uploaded < max_video_frames) {
		upload_pending_frame();
		frames_uploaded++;
		if (!read_next_video_frame()) {
			break;
		}
		if (max_video_fps > 0.0) {
			break;
		}
	}

	if (!has_pending_frame && !video_eof) {
		read_next_video_frame();
	}

	if (video_eof && (!audio_enabled || audio_eof || audio_channels == 0)) {
		playing = false;
	}
	if (length > 0.0 && time >= length && video_eof) {
		playing = false;
	}
}

int VideoStreamPlaybackFFmpeg::get_channels() const {
	return audio_enabled ? audio_channels : 0;
}

int VideoStreamPlaybackFFmpeg::get_mix_rate() const {
	return audio_mix_rate;
}

VideoStreamFFmpeg::VideoStreamFFmpeg() {}

VideoStreamFFmpeg::~VideoStreamFFmpeg() {
	clear_probe();
}

void VideoStreamFFmpeg::clear_probe() const {
	probe_input.clear();
	probe_loaded = false;
	video_streams.clear();
	audio_streams.clear();
	subtitle_streams.clear();
}

Error VideoStreamFFmpeg::ensure_probe() const {
	if (probe_loaded) {
		return OK;
	}

	Error err = FFmpegCommon::open_input(probe_input, file, headers);
	if (err != OK) {
		return err;
	}

	AVFormatContext *format_context = probe_input.format.get();
	for (unsigned int i = 0; i < format_context->nb_streams; i++) {
		AVStream *stream = format_context->streams[i];
		AVCodecParameters *params = stream->codecpar;
		if (!avcodec_find_decoder(params->codec_id)) {
			continue;
		}
		if (params->codec_type == AVMEDIA_TYPE_VIDEO && !(stream->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
			video_streams.append(i);
		} else if (params->codec_type == AVMEDIA_TYPE_AUDIO) {
			audio_streams.append(i);
		} else if (params->codec_type == AVMEDIA_TYPE_SUBTITLE) {
			subtitle_streams.append(i);
		}
	}

	probe_loaded = true;
	return OK;
}

Ref<VideoStreamPlayback> VideoStreamFFmpeg::instantiate_playback() {
	Ref<VideoStreamPlaybackFFmpeg> playback;
	playback.instantiate();
	playback->set_headers(headers);
	playback->set_file(file);
	return playback;
}

void VideoStreamFFmpeg::set_file(const String &p_file) {
	if (file == p_file) {
		return;
	}
	file = p_file;
	clear_probe();
	emit_changed();
}

String VideoStreamFFmpeg::get_file() const {
	return file;
}

void VideoStreamFFmpeg::set_headers(const String &p_headers) {
	if (headers == p_headers) {
		return;
	}
	headers = p_headers;
	clear_probe();
	emit_changed();
}

String VideoStreamFFmpeg::get_headers() const {
	return headers;
}

PackedInt32Array VideoStreamFFmpeg::get_video_streams() const {
	ensure_probe();
	return video_streams;
}

PackedInt32Array VideoStreamFFmpeg::get_audio_streams() const {
	ensure_probe();
	return audio_streams;
}

PackedInt32Array VideoStreamFFmpeg::get_subtitle_streams() const {
	ensure_probe();
	return subtitle_streams;
}

Dictionary VideoStreamFFmpeg::get_stream_metadata(int p_stream_index) const {
	Dictionary metadata;
	if (ensure_probe() != OK || !probe_input.format) {
		return metadata;
	}
	ERR_FAIL_INDEX_V(p_stream_index, static_cast<int>(probe_input.format->nb_streams), metadata);

	AVDictionaryEntry *entry = nullptr;
	while ((entry = av_dict_get(probe_input.format->streams[p_stream_index]->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		metadata[String(entry->key)] = String(entry->value);
	}
	if (!metadata.has("title")) {
		metadata["title"] = "";
	}
	if (!metadata.has("language")) {
		metadata["language"] = "";
	}
	return metadata;
}

int VideoStreamFFmpeg::get_chapter_count() const {
	if (ensure_probe() != OK || !probe_input.format) {
		return 0;
	}
	return probe_input.format->nb_chapters;
}

double VideoStreamFFmpeg::get_chapter_start(int p_chapter_index) const {
	if (ensure_probe() != OK || !probe_input.format) {
		return 0.0;
	}
	ERR_FAIL_INDEX_V(p_chapter_index, static_cast<int>(probe_input.format->nb_chapters), 0.0);
	AVChapter *chapter = probe_input.format->chapters[p_chapter_index];
	return chapter->start * av_q2d(chapter->time_base);
}

double VideoStreamFFmpeg::get_chapter_end(int p_chapter_index) const {
	if (ensure_probe() != OK || !probe_input.format) {
		return 0.0;
	}
	ERR_FAIL_INDEX_V(p_chapter_index, static_cast<int>(probe_input.format->nb_chapters), 0.0);
	AVChapter *chapter = probe_input.format->chapters[p_chapter_index];
	return chapter->end * av_q2d(chapter->time_base);
}

Dictionary VideoStreamFFmpeg::get_chapter_metadata(int p_chapter_index) const {
	Dictionary metadata;
	if (ensure_probe() != OK || !probe_input.format) {
		return metadata;
	}
	ERR_FAIL_INDEX_V(p_chapter_index, static_cast<int>(probe_input.format->nb_chapters), metadata);

	AVDictionaryEntry *entry = nullptr;
	while ((entry = av_dict_get(probe_input.format->chapters[p_chapter_index]->metadata, "", entry, AV_DICT_IGNORE_SUFFIX))) {
		metadata[String(entry->key)] = String(entry->value);
	}
	return metadata;
}

double VideoStreamFFmpeg::get_duration() const {
	if (ensure_probe() != OK || !probe_input.format) {
		return 0.0;
	}
	if (probe_input.format->duration == AV_NOPTS_VALUE) {
		return 0.0;
	}
	return static_cast<double>(probe_input.format->duration) / AV_TIME_BASE;
}

void VideoStreamFFmpeg::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_file", "file"), &VideoStreamFFmpeg::set_file);
	ClassDB::bind_method(D_METHOD("get_file"), &VideoStreamFFmpeg::get_file);
	ClassDB::bind_method(D_METHOD("set_headers", "headers"), &VideoStreamFFmpeg::set_headers);
	ClassDB::bind_method(D_METHOD("get_headers"), &VideoStreamFFmpeg::get_headers);
	ClassDB::bind_method(D_METHOD("get_video_streams"), &VideoStreamFFmpeg::get_video_streams);
	ClassDB::bind_method(D_METHOD("get_audio_streams"), &VideoStreamFFmpeg::get_audio_streams);
	ClassDB::bind_method(D_METHOD("get_subtitle_streams"), &VideoStreamFFmpeg::get_subtitle_streams);
	ClassDB::bind_method(D_METHOD("get_stream_metadata", "stream_index"), &VideoStreamFFmpeg::get_stream_metadata);
	ClassDB::bind_method(D_METHOD("get_chapter_count"), &VideoStreamFFmpeg::get_chapter_count);
	ClassDB::bind_method(D_METHOD("get_chapter_start", "chapter_index"), &VideoStreamFFmpeg::get_chapter_start);
	ClassDB::bind_method(D_METHOD("get_chapter_end", "chapter_index"), &VideoStreamFFmpeg::get_chapter_end);
	ClassDB::bind_method(D_METHOD("get_chapter_metadata", "chapter_index"), &VideoStreamFFmpeg::get_chapter_metadata);
	ClassDB::bind_method(D_METHOD("get_duration"), &VideoStreamFFmpeg::get_duration);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "file"), "set_file", "get_file");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "headers"), "set_headers", "get_headers");
}

Ref<Resource> ResourceFormatLoaderFFmpegVideo::load(const String &p_path, const String &p_original_path, Error *r_error, bool p_use_sub_threads, float *r_progress, CacheMode p_cache_mode) {
	Ref<VideoStreamFFmpeg> stream;
	stream.instantiate();
	stream->set_file(p_path);

	if (r_error) {
		*r_error = OK;
	}
	return stream;
}

void ResourceFormatLoaderFFmpegVideo::get_recognized_extensions(List<String> *p_extensions) const {
	static const char *extensions[] = {
		"mp4", "m4v", "mov", "mkv", "webm", "ogv", "avi", "wmv", "flv", "mpeg", "mpg", "m2ts", "ts", "gif", "webp"
	};

	for (const char *extension : extensions) {
		p_extensions->push_back(extension);
	}
}

bool ResourceFormatLoaderFFmpegVideo::handles_type(const String &p_type) const {
	return ClassDB::is_parent_class(p_type, "VideoStream");
}

String ResourceFormatLoaderFFmpegVideo::get_resource_type(const String &p_path) const {
	if (_ffmpeg_path_has_video_extension(p_path)) {
		return "VideoStreamFFmpeg";
	}
	return "";
}
