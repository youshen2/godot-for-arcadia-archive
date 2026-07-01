#include "register_types.h"

#include "core/gif_reader.h"
#include "core/gif_texture.h"
#include "core/gif_writer.h"
#include "core/io/resource_loader.h"
#include "core/object/class_db.h"
#include "node/gif_player.h"

static Ref<ResourceFormatLoaderGIFTexture> resource_loader_gif_texture;

#ifdef TOOLS_ENABLED
#include "editor/resource_importer_gif_texture.h"

#include "editor/editor_node.h"

static void _editor_init() {
	Ref<ResourceImporterGIFTexture> gif_importer;
	gif_importer.instantiate();
	ResourceFormatImporter::get_singleton()->add_importer(gif_importer);
}
#endif

void initialize_godot_gif_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		GDREGISTER_CLASS(GIFReader);
		GDREGISTER_CLASS(GIFWriter);
		GDREGISTER_CLASS(GIFTexture);
		GDREGISTER_CLASS(GIFPlayer);

		resource_loader_gif_texture.instantiate();
		ResourceLoader::add_resource_format_loader(resource_loader_gif_texture, true);
	}

#ifdef TOOLS_ENABLED
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(ResourceImporterGIFTexture);
		EditorNode::add_init_callback(_editor_init);
	}
#endif
}

void uninitialize_godot_gif_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_SCENE) {
		ResourceLoader::remove_resource_format_loader(resource_loader_gif_texture);
		resource_loader_gif_texture.unref();
	}
}
