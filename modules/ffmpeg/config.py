def can_build(env, platform):
    import os

    def rooted(path):
        if not path:
            return ""
        if os.path.isabs(path):
            return path
        if path.startswith("#"):
            return env.Dir(path).abspath
        return os.path.join(env.Dir("#").abspath, path)

    def has_library(lib_dir, name):
        if not os.path.isdir(lib_dir):
            return False
        prefixes = ("lib" + name + ".", "lib" + name + "-")
        exact = {"lib" + name + suffix for suffix in (".a", ".so", ".dylib", ".lib")}
        exact.add(name + ".lib")
        return any(candidate in exact or candidate.startswith(prefixes) for candidate in os.listdir(lib_dir))

    def has_install(prefix):
        include_dir = os.path.join(prefix, "include")
        lib_dir = os.path.join(prefix, "lib")
        if not os.path.exists(os.path.join(include_dir, "libavformat", "avformat.h")):
            return False
        required_libs = ["avformat", "avcodec", "swscale", "swresample", "avutil"]
        return all(has_library(lib_dir, lib) for lib in required_libs)

    prefix = env.get("ffmpeg_prefix", "")
    if prefix:
        prefix = rooted(prefix)
    else:
        build_root = rooted(env.get("ffmpeg_build_root", "thirdparty/FFmpeg/build"))
        prefix = build_root if has_install(build_root) else os.path.join(build_root, "%s-%s" % (platform, env["arch"]))

    include_dir = rooted(env.get("ffmpeg_include_path", "")) or os.path.join(prefix, "include")
    lib_dir = rooted(env.get("ffmpeg_lib_path", "")) or os.path.join(prefix, "lib")
    if os.path.exists(os.path.join(include_dir, "libavformat", "avformat.h")):
        required_libs = ["avformat", "avcodec", "swscale", "swresample", "avutil"]
        if all(has_library(lib_dir, lib) for lib in required_libs):
            return True

    ffmpeg_source = os.path.join(env.Dir("#").abspath, "thirdparty", "FFmpeg", "configure")
    return os.path.exists(ffmpeg_source)


def get_opts(platform):
    return [
        ("ffmpeg_prefix", "Path to a prebuilt FFmpeg install prefix. Overrides ffmpeg_build_root/<platform>-<arch>.", ""),
        ("ffmpeg_build_root", "Root directory for FFmpeg builds created by the module.", "thirdparty/FFmpeg/build"),
        ("ffmpeg_include_path", "Path to FFmpeg headers. Overrides the selected FFmpeg prefix include directory.", ""),
        ("ffmpeg_lib_path", "Path to FFmpeg libraries. Overrides the selected FFmpeg prefix lib directory.", ""),
        ("ffmpeg_configure_flags", "Extra flags appended to FFmpeg configure.", ""),
        ("ffmpeg_make", "Make executable used to build FFmpeg.", "make"),
        ("ffmpeg_make_jobs", "Number of jobs used to build FFmpeg. Defaults to SCons num_jobs or host CPU count.", ""),
        ("ffmpeg_extra_libs", "Comma-separated extra libraries needed by the local FFmpeg build.", ""),
    ]


def configure(env):
    pass


def get_doc_classes():
    return [
        "AudioStreamFFmpeg",
        "VideoStreamFFmpeg",
    ]


def get_doc_path():
    return "doc_classes"
