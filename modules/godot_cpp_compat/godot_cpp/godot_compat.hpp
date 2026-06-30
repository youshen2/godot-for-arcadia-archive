#pragma once

#include "core/config/project_settings.h"
#include "core/core_bind.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/io/json.h"
#include "core/io/resource.h"
#include "core/object/class_db.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/os.h"
#include "core/os/time.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/array.h"
#include "core/variant/callable.h"
#include "core/variant/dictionary.h"
#include "core/variant/typed_array.h"
#include "scene/gui/control.h"
#include "scene/resources/audio_stream_wav.h"
#include "scene/resources/image_texture.h"
#include "servers/audio/audio_stream.h"
#include "servers/rendering/rendering_server.h"

namespace godot {

using ::Array;
using ::AudioFrame;
using ::AudioStream;
using ::AudioStreamPlayback;
using ::AudioStreamPlaybackResampled;
using ::AudioStreamWAV;
using ::Callable;
using ::CharString;
using ::ClassDB;
using ::Control;
using ::Dictionary;
using ::DirAccess;
using ::Error;
using ::FileAccess;
using ::Image;
using ::ImageTexture;
using ::CoreBind::Marshalls;
using ::Mutex;
using ::Object;
using ::OS;
using ::PackedByteArray;
using ::PackedInt32Array;
using ::PackedStringArray;
using ::ProjectSettings;
using ::PropertyInfo;
using ::Ref;
using ::RefCounted;
using ::RenderingServer;
using ::Resource;
using ::String;
using ::StringName;
using ::Time;
using ::TypedArray;
using ::Variant;
using ::Vector2i;

class UtilityFunctions {
	static void _append(String &r_string, const String &p_value) {
		r_string += p_value;
	}

	static void _append(String &r_string, const char *p_value) {
		if (p_value) {
			r_string += String::utf8(p_value);
		}
	}

	template <size_t N>
	static void _append(String &r_string, const char (&p_value)[N]) {
		r_string += String::utf8(p_value);
	}

	static void _append(String &r_string, const CharString &p_value) {
		r_string += String::utf8(p_value.get_data());
	}

	template <typename T>
	static void _append(String &r_string, const T &p_value) {
		Variant value = p_value;
		r_string += value.stringify();
	}

public:
	template <typename... VarArgs>
	static void print(const VarArgs &...p_args) {
		String message;
		(_append(message, p_args), ...);
		print_line(message);
	}

	template <typename... VarArgs>
	static void printerr(const VarArgs &...p_args) {
		String message;
		(_append(message, p_args), ...);
		print_error(message);
	}
};

} // namespace godot
