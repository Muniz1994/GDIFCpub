#ifndef GD_IFC_IMPORT_PLUGIN_H
#define GD_IFC_IMPORT_PLUGIN_H

#include <godot_cpp/classes/editor_import_plugin.hpp>
#include <godot_cpp/classes/editor_plugin.hpp>

namespace godot {

// Imports .ifc files as PackedScenes, the way glTF files are imported. The scene is built once
// per import and saved in binary under .godot/imported, so a scene that instances it only stores
// a reference to the .ifc file instead of every mesh, shape and property set of the model.
//
// The imported scene's root is a GDIFCManager pointing at the source file, so instances re-link
// their typed IFC objects (ifc_object, IFCModel queries) when they enter the tree.
class GDIFCImportPlugin : public EditorImportPlugin {
    GDCLASS(GDIFCImportPlugin, EditorImportPlugin)

protected:
    static void _bind_methods() {}

public:
    String _get_importer_name() const override;
    String _get_visible_name() const override;
    int32_t _get_preset_count() const override;
    String _get_preset_name(int32_t p_preset_index) const override;
    PackedStringArray _get_recognized_extensions() const override;
    TypedArray<Dictionary> _get_import_options(const String &p_path, int32_t p_preset_index) const override;
    String _get_save_extension() const override;
    String _get_resource_type() const override;
    float _get_priority() const override;
    int32_t _get_import_order() const override;
    int32_t _get_format_version() const override;
    bool _get_option_visibility(const String &p_path, const StringName &p_option_name, const Dictionary &p_options) const override;
    bool _can_import_threaded() const override;
    Error _import(const String &p_source_file, const String &p_save_path, const Dictionary &p_options,
                  const TypedArray<String> &p_platform_variants, const TypedArray<String> &p_gen_files) const override;
};

// Registers the importer whenever the extension is loaded in the editor, independently of the
// GDIFC dock plugin being enabled.
class GDIFCEditorPlugin : public EditorPlugin {
    GDCLASS(GDIFCEditorPlugin, EditorPlugin)

    Ref<GDIFCImportPlugin> importer_;

protected:
    static void _bind_methods() {}

public:
    String _get_plugin_name() const override { return "GDIFC Importer"; }
    void _enter_tree() override;
    void _exit_tree() override;
};

} // namespace godot

#endif // GD_IFC_IMPORT_PLUGIN_H
