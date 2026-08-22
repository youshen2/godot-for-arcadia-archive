/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "audio_stream_ffmpeg.h"
#include "movie_writer_ffmpeg.h"
#include "video_export_session.h"
#include "video_stream_ffmpeg.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"

static Ref<ResourceFormatLoaderFFmpegVideo> resource_loader_ffmpeg_video;
static Ref<ResourceFormatLoaderFFmpegAudio> resource_loader_ffmpeg_audio;
static MovieWriterFFmpeg *writer_ffmpeg = nullptr;

void initialize_ffmpeg_module(ModuleInitializationLevel p_level) {
	switch (p_level) {
		case MODULE_INITIALIZATION_LEVEL_SERVERS: {
			if constexpr (GD_IS_CLASS_ENABLED(MovieWriterFFmpeg)) {
				writer_ffmpeg = memnew(MovieWriterFFmpeg);
				MovieWriter::add_writer(writer_ffmpeg);
			}
		} break;
		case MODULE_INITIALIZATION_LEVEL_SCENE: {
			GDREGISTER_CLASS(VideoStreamFFmpeg);
			GDREGISTER_CLASS(AudioStreamFFmpeg);
			GDREGISTER_CLASS(VideoExportSession);

			resource_loader_ffmpeg_video.instantiate();
			resource_loader_ffmpeg_audio.instantiate();
			ResourceLoader::add_resource_format_loader(resource_loader_ffmpeg_audio, true);
			ResourceLoader::add_resource_format_loader(resource_loader_ffmpeg_video, true);
		} break;
		default:
			break;
	}
}

void uninitialize_ffmpeg_module(ModuleInitializationLevel p_level) {
	switch (p_level) {
		case MODULE_INITIALIZATION_LEVEL_SCENE: {
			ResourceLoader::remove_resource_format_loader(resource_loader_ffmpeg_audio);
			ResourceLoader::remove_resource_format_loader(resource_loader_ffmpeg_video);
			resource_loader_ffmpeg_audio.unref();
			resource_loader_ffmpeg_video.unref();
		} break;
		case MODULE_INITIALIZATION_LEVEL_SERVERS: {
			if constexpr (GD_IS_CLASS_ENABLED(MovieWriterFFmpeg)) {
				memdelete(writer_ffmpeg);
				writer_ffmpeg = nullptr;
			}
		} break;
		default:
			break;
	}
}
