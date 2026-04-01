#ifndef GD_IFC_MODEL_H
#define GD_IFC_MODEL_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "gd_ifc_entity_base.h"

#include <IfcFile.h>
#include <IfcSpfHeader.h>
#include <Header_section_schema.h>

#include <memory>

namespace godot {

/// IFCModel is the root scene-tree node for a loaded IFC file.
/// It provides header metadata access, entity search/creation, and file saving.
class IFCModel : public Node3D {
    GDCLASS(IFCModel, Node3D)

public:
    std::shared_ptr<IfcParse::IfcFile> ifc_file_;

    // ── Called by GDIFCManager after loading ──────────────────────────────
    void init(std::shared_ptr<IfcParse::IfcFile> file);

    // ── Header metadata ───────────────────────────────────────────────────
    godot::String            get_schema_name() const;
    godot::PackedStringArray get_file_description() const;
    godot::String            get_file_name() const;
    godot::String            get_timestamp() const;
    godot::PackedStringArray get_author() const;
    godot::PackedStringArray get_organization() const;
    godot::String            get_preprocessor() const;
    godot::String            get_originating_system() const;
    godot::String            get_authorization() const;

    // ── Entity lookup ─────────────────────────────────────────────────────

    /// Return entity with the given EXPRESS numeric id.
    Ref<GDIFCEntityBase> get_by_id(int64_t express_id);

    /// Return entity with the given GlobalId (GUID) string.
    Ref<GDIFCEntityBase> get_by_global_id(godot::String global_id);

    /// Return all instances of the given IFC class name (including subtypes).
    godot::Array instances_by_type(godot::String ifc_class);

    /// Return all instances of the given IFC class name (excluding subtypes).
    godot::Array instances_by_type_exact(godot::String ifc_class);

    /// Convenience: return the single IfcProject entity.
    Ref<GDIFCEntityBase> get_project();

    // ── Entity creation ───────────────────────────────────────────────────

    /// Create a new entity of the given IFC class name and add it to the file.
    /// Returns a typed GD wrapper; attributes start unset.
    Ref<GDIFCEntityBase> create(godot::String ifc_class);

    // ── Serialisation ─────────────────────────────────────────────────────

    /// Write the file to disk in STEP/SPFF format.
    godot::Error save(godot::String path);

    /// Return the complete STEP/SPFF serialisation as a string.
    godot::String to_step_string();

protected:
    static void _bind_methods();
};

} // namespace godot

#endif // GD_IFC_MODEL_H
