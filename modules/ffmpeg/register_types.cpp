/**************************************************************************/
/*  register_types.cpp                                                    */
/**************************************************************************/

#include "register_types.h"

#include "audio_stream_ffmpeg.h"
#include "video_stream_ffmpeg.h"

#include "core/io/resource_loader.h"
#include "core/object/class_db.h"

static Ref<ResourceFormatLoaderFFmpegVideo> resource_loader_ffmpeg_video;
static Ref<ResourceFormatLoaderFFmpegAudio> resource_loader_ffmpeg_audio;

void initialize_ffmpeg_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(VideoStreamFFmpeg);
	GDREGISTER_CLASS(AudioStreamFFmpeg);

	resource_loader_ffmpeg_video.instantiate();
	resource_loader_ffmpeg_audio.instantiate();
	ResourceLoader::add_resource_format_loader(resource_loader_ffmpeg_audio, true);
	ResourceLoader::add_resource_format_loader(resource_loader_ffmpeg_video, true);
}

void uninitialize_ffmpeg_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ResourceLoader::remove_resource_format_loader(resource_loader_ffmpeg_audio);
	ResourceLoader::remove_resource_format_loader(resource_loader_ffmpeg_video);
	resource_loader_ffmpeg_audio.unref();
	resource_loader_ffmpeg_video.unref();
}
