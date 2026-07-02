#ifndef GD_IFC_GEOREFERENCE_H
#define GD_IFC_GEOREFERENCE_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/variant/string.hpp>
#include <cstdint>

namespace godot {

// ── Georeferencing data structs (moved from gd_ifc_manager.h) ────────────

struct MapConversion {
    double Eastings         = 0.0;
    double Northings        = 0.0;
    double OrthogonalHeight = 0.0;
    double XAxisAbscissa    = 0.0;
    double XAxisOrdinate    = 0.0;
    double Scale            = 0.0;
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

    GeorreferenceData()
        : valid(false),
          map_conversion{0.0, 0.0, 0.0, 0.0, 0.0, 0.0},
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

    double get_scale() const                       { return scale_; }
    void   set_scale(double v)                     { scale_ = v; }

    // ── ProjectedCRS properties ──────────────────────────────────────────
    String get_crs_name() const                    { return crs_name_; }
    void   set_crs_name(const String& v)           { crs_name_ = v; }

    String get_crs_description() const             { return crs_description_; }
    void   set_crs_description(const String& v)    { crs_description_ = v; }

    String get_geodetic_datum() const              { return geodetic_datum_; }
    void   set_geodetic_datum(const String& v)     { geodetic_datum_ = v; }

    String get_vertical_datum() const              { return vertical_datum_; }
    void   set_vertical_datum(const String& v)     { vertical_datum_ = v; }

protected:
    static void _bind_methods();

private:
    // MapConversion fields (exposed as double; source struct uses int32/int16)
    double eastings_          = 0.0;
    double northings_         = 0.0;
    double orthogonal_height_ = 0.0;
    double x_axis_abscissa_   = 0.0;
    double x_axis_ordinate_   = 0.0;
    double scale_             = 0.0;

    // ProjectedCRS fields
    String crs_name_;
    String crs_description_;
    String geodetic_datum_;
    String vertical_datum_;
};

} // namespace godot

#endif // GD_IFC_GEOREFERENCE_H
