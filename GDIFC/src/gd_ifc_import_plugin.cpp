#include "gd_ifc_import_plugin.h"

#include "gd_ifc_manager.h"
#include "gd_ifc_settings.h"
#include "gd_ifc_collision.h"

#include <godot_cpp/classes/config_file.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/resource_uid.hpp>

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

// Also written by the dock (gdifcdock.gd IFC_IMPORTER) into the .import files
static const char *IMPORTER_NAME = "gdifc.ifc_scene";

String GDIFCImportPlugin::_get_importer_name() const { return IMPORTER_NAME; }
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
    // Off: instances don't re-parse the .ifc file, and have no ifc_object or IFCModel queries
    options.push_back(make_option("data/ifc_objects", true));
    options.push_back(make_option("collision/create", false));
    // A GDIFCCollisionMatrix choosing the shape and layers per class. Replaces the class list when set.
    options.push_back(make_option("collision/matrix", String(), PROPERTY_HINT_FILE, "*.tres,*.res"));
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
    if (p_option_name == StringName("collision/matrix")) {
        return p_options.get("collision/create", false);
    }
    if (p_option_name == StringName("collision/classes")) {
        return p_options.get("collision/create", false) && String(p_options.get("collision/matrix", String())).is_empty();
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
    settings->setIfcObjects(p_options.get("data/ifc_objects", true));

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

    const String matrix_path = p_options.get("collision/matrix", String());
    if (create_collision && !matrix_path.is_empty()) {
        Ref<GDIFCCollisionMatrix> matrix = ResourceLoader::get_singleton()->load(matrix_path, "GDIFCCollisionMatrix");
        if (matrix.is_valid()) {
            root->set_collision_matrix(matrix);
        } else {
            WARN_PRINT(String("GDIFC: collision matrix '") + matrix_path + "' of " + p_source_file
                + " could not be loaded. Using the collision class list instead.");
        }
    }

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
    connect("resource_saved", callable_mp(this, &GDIFCEditorPlugin::_on_resource_saved));
}

// Import options may hold a uid:// path or a res:// one
static String resolved_path(const String &path) {
    return path.begins_with("uid://") ? ResourceUID::get_singleton()->ensure_path(path) : path;
}

static void find_files_with_matrix(EditorFileSystemDirectory *dir, const String &matrix_path, PackedStringArray &r_files) {
    for (int i = 0; i < dir->get_file_count(); i++) {
        const String path = dir->get_file_path(i);
        if (path.get_extension().to_lower() != "ifc") continue;
        Ref<ConfigFile> import_config;
        import_config.instantiate();
        if (import_config->load(path + String(".import")) != OK) continue;
        if (String(import_config->get_value("remap", "importer", String())) != IMPORTER_NAME) continue;
        if (!bool(import_config->get_value("params", "collision/create", false))) continue;
        if (resolved_path(import_config->get_value("params", "collision/matrix", String())) == matrix_path) {
            r_files.push_back(path);
        }
    }
    for (int i = 0; i < dir->get_subdir_count(); i++) {
        find_files_with_matrix(dir->get_subdir(i), matrix_path, r_files);
    }
}

// The importer doesn't know which resources its options point to, so editing a collision matrix
// wouldn't reimport the models built with it
void GDIFCEditorPlugin::_on_resource_saved(const Ref<Resource> &resource) {
    Ref<GDIFCCollisionMatrix> matrix = resource;
    if (matrix.is_null() || matrix->get_path().is_empty()) return;

    EditorFileSystem *resource_filesystem = EditorInterface::get_singleton()->get_resource_filesystem();
    PackedStringArray files;
    find_files_with_matrix(resource_filesystem->get_filesystem(), matrix->get_path(), files);
    if (!files.is_empty()) {
        // Not from inside the save
        resource_filesystem->call_deferred("reimport_files", files);
    }
}

void GDIFCEditorPlugin::_exit_tree() {
    const Callable on_resource_saved = callable_mp(this, &GDIFCEditorPlugin::_on_resource_saved);
    if (is_connected("resource_saved", on_resource_saved)) {
        disconnect("resource_saved", on_resource_saved);
    }
    if (importer_.is_valid()) {
        remove_import_plugin(importer_);
        importer_.unref();
    }
}
