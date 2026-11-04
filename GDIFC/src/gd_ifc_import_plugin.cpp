#include "gd_ifc_import_plugin.h"

#include "gd_ifc_manager.h"
#include "gd_ifc_settings.h"

#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

using namespace godot;

static Dictionary make_option(const String &name, const Variant &default_value,
                              PropertyHint hint = PROPERTY_HINT_NONE, const String &hint_string = String()) {
    Dictionary option;
    option["name"] = name;
    option["default_value"] = default_value;
    option["property_hint"] = hint;
    option["hint_string"] = hint_string;
    return option;
}

String GDIFCImportPlugin::_get_importer_name() const { return "gdifc.ifc_scene"; }
String GDIFCImportPlugin::_get_visible_name() const { return "IFC Scene"; }
int32_t GDIFCImportPlugin::_get_preset_count() const { return 1; }
String GDIFCImportPlugin::_get_preset_name(int32_t p_preset_index) const { return "Default"; }
PackedStringArray GDIFCImportPlugin::_get_recognized_extensions() const { return PackedStringArray({ "ifc" }); }
String GDIFCImportPlugin::_get_save_extension() const { return "scn"; }
String GDIFCImportPlugin::_get_resource_type() const { return "PackedScene"; }
float GDIFCImportPlugin::_get_priority() const { return 1.0f; }
// Same order as Godot's scene importer, so textures and materials are imported first
int32_t GDIFCImportPlugin::_get_import_order() const { return 100; }
// Bump to reimport every .ifc file when the generated scene changes
int32_t GDIFCImportPlugin::_get_format_version() const { return 1; }
// web-ifc keeps process-wide state and loads one model at a time anyway
bool GDIFCImportPlugin::_can_import_threaded() const { return false; }

TypedArray<Dictionary> GDIFCImportPlugin::_get_import_options(const String &p_path, int32_t p_preset_index) const {
    TypedArray<Dictionary> options;
    options.push_back(make_option("geometry/coordinate_to_origin", false));
    options.push_back(make_option("geometry/circle_segments", 12, PROPERTY_HINT_RANGE, "3,128,1"));
    options.push_back(make_option("lods/generate", true));
    options.push_back(make_option("lods/normal_merge_angle", 60.0, PROPERTY_HINT_RANGE, "0,180,0.1,degrees"));
    options.push_back(make_option("materials/ifc_materials", false));
    options.push_back(make_option("materials/textures", false));
    options.push_back(make_option("collision/create", false));
    // Exact IFC class names, e.g. IfcWall, IfcSlab. Empty creates collision for every element.
    options.push_back(make_option("collision/classes", PackedStringArray()));
    return options;
}

bool GDIFCImportPlugin::_get_option_visibility(const String &p_path, const StringName &p_option_name, const Dictionary &p_options) const {
    if (p_option_name == StringName("lods/normal_merge_angle")) {
        return p_options.get("lods/generate", true);
    }
    if (p_option_name == StringName("materials/textures")) {
        return p_options.get("materials/ifc_materials", false);
    }
    if (p_option_name == StringName("collision/classes")) {
        return p_options.get("collision/create", false);
    }
    return true;
}

Error GDIFCImportPlugin::_import(const String &p_source_file, const String &p_save_path, const Dictionary &p_options,
                                 const TypedArray<String> &p_platform_variants, const TypedArray<String> &p_gen_files) const {
    Ref<GDIFCLoaderSettings> settings;
    settings.instantiate();
    settings->setCoordinateToOrigin(p_options.get("geometry/coordinate_to_origin", false));
    settings->setCircleSegments((uint16_t)(int)p_options.get("geometry/circle_segments", 12));
    settings->setGenerateLods(p_options.get("lods/generate", true));
    settings->setLodNormalMergeAngle((float)(double)p_options.get("lods/normal_merge_angle", 60.0));
    settings->setIfcMaterials(p_options.get("materials/ifc_materials", false));
    settings->setIfcTextures(p_options.get("materials/textures", false));

    const bool create_collision = p_options.get("collision/create", false);
    const PackedStringArray class_names = p_options.get("collision/classes", PackedStringArray());
    Array collision_classes;
    for (const String &class_name : class_names) {
        if (!class_name.strip_edges().is_empty()) {
            collision_classes.push_back(class_name.strip_edges());
        }
    }

    GDIFCManager *root = memnew(GDIFCManager);
    root->set_name(p_source_file.get_file().get_basename().validate_node_name());
    root->set_gdifc_settings(settings);

    Error err = root->read_ifc_blocking(p_source_file, create_collision, collision_classes);
    if (err != OK) {
        memdelete(root);
        ERR_FAIL_V_MSG(err, String("GDIFC: failed to import ") + p_source_file);
    }

    Ref<PackedScene> scene;
    scene.instantiate();
    err = scene->pack(root);
    memdelete(root);
    ERR_FAIL_COND_V_MSG(err != OK, err, String("GDIFC: failed to pack the scene of ") + p_source_file);

    return ResourceSaver::get_singleton()->save(scene, p_save_path + String(".") + _get_save_extension());
}

void GDIFCEditorPlugin::_enter_tree() {
    importer_.instantiate();
    add_import_plugin(importer_);
}

void GDIFCEditorPlugin::_exit_tree() {
    if (importer_.is_valid()) {
        remove_import_plugin(importer_);
        importer_.unref();
    }
}
