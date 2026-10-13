#ifndef GD_IFC_GEOREFERENCE_H
#define GD_IFC_GEOREFERENCE_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <glm/glm.hpp>
#include <cstdint>

namespace godot {

// ── Georeferencing data structs (moved from gd_ifc_manager.h) ────────────

struct MapConversion {
    double Eastings         = 0.0;
    double Northings        = 0.0;
    double OrthogonalHeight = 0.0;
    double XAxisAbscissa    = 1.0;
    double XAxisOrdinate    = 0.0;
    double Scale            = 1.0;
};

struct ProjectedCRS {
    String Name;
    String Description;
    String GeodeticDatum;
    String VerticalDatum;
};

struct GeorreferenceData {
    bool          valid = false;
    MapConversion map_conversion;
    ProjectedCRS  projected_crs;
    // Translation web-ifc applied to every vertex when coordinate_to_origin is on
    // (Godot Y-up axes). Zero otherwise. Kept even when valid == false.
    glm::dvec3    coordination_offset{0.0};

    GeorreferenceData()
        : valid(false),
          map_conversion{},
          projected_crs{"NotDefined", "NotDefined", "NotDefined", "NotDefined"} {}

    GeorreferenceData(MapConversion mc, ProjectedCRS crs)
        : valid(true), map_conversion{mc}, projected_crs{crs} {}
};

// ── IFCGeoreference node ──────────────────────────────────────────────────
// Created under IFCModel when a loaded IFC4+ file contains
// IfcProjectedCRS and IfcMapConversion georeferencing data.

class IFCGeoreference : public Node3D {
    GDCLASS(IFCGeoreference, Node3D)

public:
    /// Bulk-populate all properties from a loaded GeorreferenceData struct.
    void init(const GeorreferenceData& data);

    // ── MapConversion properties ─────────────────────────────────────────
    double get_eastings() const                    { return eastings_; }
    void   set_eastings(double v)                  { eastings_ = v; }

    double get_northings() const                   { return northings_; }
    void   set_northings(double v)                 { northings_ = v; }

    double get_orthogonal_height() const           { return orthogonal_height_; }
    void   set_orthogonal_height(double v)         { orthogonal_height_ = v; }

    double get_x_axis_abscissa() const             { return x_axis_abscissa_; }
    void   set_x_axis_abscissa(double v)           { x_axis_abscissa_ = v; }

    double get_x_axis_ordinate() const             { return x_axis_ordinate_; }
    void   set_x_axis_ordinate(double v)           { x_axis_ordinate_ = v; }

    // Not "scale": that name belongs to Node3D
    double get_map_scale() const                   { return scale_; }
    void   set_map_scale(double v)                 { scale_ = v; }

    // ── ProjectedCRS properties ──────────────────────────────────────────
    String get_crs_name() const                    { return crs_name_; }
    void   set_crs_name(const String& v)           { crs_name_ = v; }

    String get_crs_description() const             { return crs_description_; }
    void   set_crs_description(const String& v)    { crs_description_ = v; }

    String get_geodetic_datum() const              { return geodetic_datum_; }
    void   set_geodetic_datum(const String& v)     { geodetic_datum_ = v; }

    String get_vertical_datum() const              { return vertical_datum_; }
    void   set_vertical_datum(const String& v)     { vertical_datum_ = v; }

    // ── Coordination offset (coordinate_to_origin) ───────────────────────
    double get_coordination_offset_x() const       { return coordination_offset_.x; }
    void   set_coordination_offset_x(double v)     { coordination_offset_.x = v; }

    double get_coordination_offset_y() const       { return coordination_offset_.y; }
    void   set_coordination_offset_y(double v)     { coordination_offset_.y = v; }

    double get_coordination_offset_z() const       { return coordination_offset_.z; }
    void   set_coordination_offset_z(double v)     { coordination_offset_.z = v; }

    /// Map conversion and offset stored on this node, as loader data.
    GeorreferenceData get_data() const;

protected:
    static void _bind_methods();
    bool _set(const StringName& p_name, const Variant& p_value);

private:
    // MapConversion fields
    double eastings_          = 0.0;
    double northings_         = 0.0;
    double orthogonal_height_ = 0.0;
    double x_axis_abscissa_   = 1.0;
    double x_axis_ordinate_   = 0.0;
    double scale_             = 1.0;

    // ProjectedCRS fields
    String crs_name_;
    String crs_description_;
    String geodetic_datum_;
    String vertical_datum_;

    // Doubles, not Vector3: the offset can be as large as the map coordinates
    glm::dvec3 coordination_offset_{0.0};
};

// ── Placement helpers ─────────────────────────────────────────────────────
// All matrices are in Godot axes (IFC x → +X, IFC y → −Z, IFC z → +Y).

/// Engineering → map transform described by an IfcMapConversion.
glm::dmat4 georef_map_matrix(const MapConversion& mc);

/// Transform taking the model-local space of `target` into the model-local
/// space of `reference`. Falls back to a shared engineering frame when the
/// target has no map conversion.
glm::dmat4 georef_relative_matrix(const GeorreferenceData& reference, const GeorreferenceData& target);

// Convert a GLM column-major 4×4 matrix to a Godot Transform3D.
// GLM column layout: m[col][row]; columns 0-2 are the axes, column 3 is translation.
inline Transform3D glm_to_godot_transform(const glm::dmat4& m) {
    return Transform3D(
        Basis(
            Vector3((real_t)m[0].x, (real_t)m[0].y, (real_t)m[0].z),  // X axis
            Vector3((real_t)m[1].x, (real_t)m[1].y, (real_t)m[1].z),  // Y axis
            Vector3((real_t)m[2].x, (real_t)m[2].y, (real_t)m[2].z)   // Z axis
        ),
        Vector3((real_t)m[3].x, (real_t)m[3].y, (real_t)m[3].z)       // origin
    );
}

} // namespace godot

#endif // GD_IFC_GEOREFERENCE_H
