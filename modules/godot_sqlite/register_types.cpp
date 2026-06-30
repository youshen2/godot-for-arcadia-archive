#include "register_types.h"

#include "gdsqlite.hpp"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

void initialize_godot_sqlite_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(SQLite);
}

void uninitialize_godot_sqlite_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
}
