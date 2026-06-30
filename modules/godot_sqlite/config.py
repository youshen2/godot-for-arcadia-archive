def can_build(env, platform):
    return platform in ["macos", "android", "windows", "ios", "linuxbsd"]


def get_opts(platform):
    from SCons.Variables import BoolVariable

    return [
        BoolVariable("godot_sqlite_enable_fts5", "Enable SQLite FTS5 support in the godot_sqlite module", False),
        BoolVariable(
            "godot_sqlite_enable_math_functions",
            "Enable SQLite built-in math SQL functions in the godot_sqlite module",
            False,
        ),
        BoolVariable("godot_sqlite_enable_rtree", "Enable SQLite R*Tree support in the godot_sqlite module", False),
    ]


def configure(env):
    pass


def get_doc_classes():
    return [
        "SQLite",
    ]


def get_doc_path():
    return "doc_classes"
