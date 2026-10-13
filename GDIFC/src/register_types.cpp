#include "register_types.h"

#include "gd_ifc_manager.h"
#include "gd_ifc_node.h"
#include "gd_ifc_settings.h"
#include "gd_ifc_entity_base.h"
#include "gd_ifc_model.h"

#include <IfcSchema.h>

#include <gdextension_interface.h>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

void initialize_example_module(ModuleInitializationLevel p_level)
{
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE)
    {
        return;
    }

    // Base class must be registered before any generated subclasses
    ClassDB::register_class<GDIFCEntityBase>();
    ClassDB::register_class<IFCModel>();
    ClassDB::register_class<IFCGeoreference>();

    // Register all ~876 generated per-entity classes + populate factory map
    register_all_gd_ifc_entities();

    ClassDB::register_class<GDIFCManager>();
    ClassDB::register_class<IFCNode>();
    ClassDB::register_class<GDIFCLoaderSettings>();

    // IfcParse builds its schemas lazily without a lock, so two IFC files parsed
    // on worker threads at once (e.g. a scene cold-reloading several models)
    // would race. schema_by_name() populates every compiled schema up front.
    try {
        IfcParse::schema_by_name("IFC4");
    } catch (...) {}
}

void uninitialize_example_module(ModuleInitializationLevel p_level)
{
    if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE)
    {
        return;
    }
}

extern "C"
{
    // Initialization.
    GDExtensionBool GDE_EXPORT gdifc_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, const GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization)
    {
        godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

        init_obj.register_initializer(initialize_example_module);
        init_obj.register_terminator(uninitialize_example_module);
        init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

        return init_obj.init();
    }
}