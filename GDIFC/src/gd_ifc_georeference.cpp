// gd_ifc_georeference.cpp
#include "gd_ifc_georeference.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <cmath>

using namespace godot;

void IFCGeoreference::_bind_methods() {
    // ── MapConversion ────────────────────────────────────────────────────
    ClassDB::bind_method(D_METHOD("get_eastings"),              &IFCGeoreference::get_eastings);
    ClassDB::bind_method(D_METHOD("set_eastings", "v"),         &IFCGeoreference::set_eastings);
    ClassDB::bind_method(D_METHOD("get_northings"),             &IFCGeoreference::get_northings);
    ClassDB::bind_method(D_METHOD("set_northings", "v"),        &IFCGeoreference::set_northings);
    ClassDB::bind_method(D_METHOD("get_orthogonal_height"),     &IFCGeoreference::get_orthogonal_height);
    ClassDB::bind_method(D_METHOD("set_orthogonal_height","v"), &IFCGeoreference::set_orthogonal_height);
    ClassDB::bind_method(D_METHOD("get_x_axis_abscissa"),       &IFCGeoreference::get_x_axis_abscissa);
    ClassDB::bind_method(D_METHOD("set_x_axis_abscissa", "v"),  &IFCGeoreference::set_x_axis_abscissa);
    ClassDB::bind_method(D_METHOD("get_x_axis_ordinate"),       &IFCGeoreference::get_x_axis_ordinate);
    ClassDB::bind_method(D_METHOD("set_x_axis_ordinate", "v"),  &IFCGeoreference::set_x_axis_ordinate);
    ClassDB::bind_method(D_METHOD("get_map_scale"),             &IFCGeoreference::get_map_scale);
    ClassDB::bind_method(D_METHOD("set_map_scale", "v"),        &IFCGeoreference::set_map_scale);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "eastings"),
        "set_eastings", "get_eastings");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "northings"),
        "set_northings", "get_northings");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "orthogonal_height"),
        "set_orthogonal_height", "get_orthogonal_height");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "x_axis_abscissa"),
        "set_x_axis_abscissa", "get_x_axis_abscissa");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "x_axis_ordinate"),
        "set_x_axis_ordinate", "get_x_axis_ordinate");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "map_scale"),
        "set_map_scale", "get_map_scale");

    // ── ProjectedCRS ─────────────────────────────────────────────────────
    ClassDB::bind_method(D_METHOD("get_crs_name"),                  &IFCGeoreference::get_crs_name);
    ClassDB::bind_method(D_METHOD("set_crs_name", "v"),             &IFCGeoreference::set_crs_name);
    ClassDB::bind_method(D_METHOD("get_crs_description"),           &IFCGeoreference::get_crs_description);
    ClassDB::bind_method(D_METHOD("set_crs_description", "v"),      &IFCGeoreference::set_crs_description);
    ClassDB::bind_method(D_METHOD("get_geodetic_datum"),            &IFCGeoreference::get_geodetic_datum);
    ClassDB::bind_method(D_METHOD("set_geodetic_datum", "v"),       &IFCGeoreference::set_geodetic_datum);
    ClassDB::bind_method(D_METHOD("get_vertical_datum"),            &IFCGeoreference::get_vertical_datum);
    ClassDB::bind_method(D_METHOD("set_vertical_datum", "v"),       &IFCGeoreference::set_vertical_datum);

    ADD_PROPERTY(PropertyInfo(Variant::STRING, "crs_name"),
        "set_crs_name", "get_crs_name");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "crs_description"),
        "set_crs_description", "get_crs_description");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "geodetic_datum"),
        "set_geodetic_datum", "get_geodetic_datum");
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "vertical_datum"),
        "set_vertical_datum", "get_vertical_datum");

    // ── Coordination offset ──────────────────────────────────────────────
    ClassDB::bind_method(D_METHOD("get_coordination_offset_x"),      &IFCGeoreference::get_coordination_offset_x);
    ClassDB::bind_method(D_METHOD("set_coordination_offset_x", "v"), &IFCGeoreference::set_coordination_offset_x);
    ClassDB::bind_method(D_METHOD("get_coordination_offset_y"),      &IFCGeoreference::get_coordination_offset_y);
    ClassDB::bind_method(D_METHOD("set_coordination_offset_y", "v"), &IFCGeoreference::set_coordination_offset_y);
    ClassDB::bind_method(D_METHOD("get_coordination_offset_z"),      &IFCGeoreference::get_coordination_offset_z);
    ClassDB::bind_method(D_METHOD("set_coordination_offset_z", "v"), &IFCGeoreference::set_coordination_offset_z);

    ADD_GROUP("Coordination", "coordination_offset_");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coordination_offset_x"),
        "set_coordination_offset_x", "get_coordination_offset_x");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coordination_offset_y"),
        "set_coordination_offset_y", "get_coordination_offset_y");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "coordination_offset_z"),
        "set_coordination_offset_z", "get_coordination_offset_z");
}

// Scenes saved before the rename stored the map scale as a float "scale"
bool IFCGeoreference::_set(const StringName& p_name, const Variant& p_value) {
    if (p_name == StringName("scale") && p_value.get_type() == Variant::FLOAT) {
        scale_ = p_value;
        return true;
    }
    return false;
}

void IFCGeoreference::init(const GeorreferenceData& data) {
    eastings_          = static_cast<double>(data.map_conversion.Eastings);
    northings_         = static_cast<double>(data.map_conversion.Northings);
    orthogonal_height_ = static_cast<double>(data.map_conversion.OrthogonalHeight);
    x_axis_abscissa_   = static_cast<double>(data.map_conversion.XAxisAbscissa);
    x_axis_ordinate_   = static_cast<double>(data.map_conversion.XAxisOrdinate);
    scale_             = static_cast<double>(data.map_conversion.Scale);

    crs_name_        = data.projected_crs.Name;
    crs_description_ = data.projected_crs.Description;
    geodetic_datum_  = data.projected_crs.GeodeticDatum;
    vertical_datum_  = data.projected_crs.VerticalDatum;

    coordination_offset_ = data.coordination_offset;
}

GeorreferenceData IFCGeoreference::get_data() const {
    MapConversion mc;
    mc.Eastings         = eastings_;
    mc.Northings        = northings_;
    mc.OrthogonalHeight = orthogonal_height_;
    mc.XAxisAbscissa    = x_axis_abscissa_;
    mc.XAxisOrdinate    = x_axis_ordinate_;
    mc.Scale            = scale_;

    GeorreferenceData data(mc, ProjectedCRS{crs_name_, crs_description_, geodetic_datum_, vertical_datum_});
    data.coordination_offset = coordination_offset_;
    return data;
}

// ── Placement helpers ─────────────────────────────────────────────────────

glm::dmat4 godot::georef_map_matrix(const MapConversion& mc) {
    // Eastings → +X, Northings → −Z, height → +Y
    const glm::dvec3 origin(mc.Eastings, mc.OrthogonalHeight, -mc.Northings);
    // The x-axis direction is counter-clockwise from east around IFC +Z,
    // which is a positive rotation around Godot +Y
    const double angle = std::atan2(mc.XAxisOrdinate, mc.XAxisAbscissa);
    const double scale = mc.Scale > 0.0 ? mc.Scale : 1.0;

    glm::dmat4 m = glm::translate(glm::dmat4(1.0), origin);
    m = glm::rotate(m, angle, glm::dvec3(0.0, 1.0, 0.0));
    m = glm::scale(m, glm::dvec3(scale));
    return m;
}

glm::dmat4 godot::georef_relative_matrix(const GeorreferenceData& reference, const GeorreferenceData& target) {
    // Model-local = C * engineering, so engineering = C⁻¹ * model-local
    const glm::dmat4 ref_coord      = glm::translate(glm::dmat4(1.0), reference.coordination_offset);
    const glm::dmat4 target_uncoord = glm::translate(glm::dmat4(1.0), -target.coordination_offset);

    if (!reference.valid || !target.valid) {
        // No map conversion to go through: assume both share one engineering frame
        return ref_coord * target_uncoord;
    }

    // Both map matrices are inverted/multiplied in double, so the large
    // eastings/northings cancel before anything is cast to float
    const glm::dmat4 eng_to_eng = glm::inverse(georef_map_matrix(reference.map_conversion))
                                * georef_map_matrix(target.map_conversion);
    return ref_coord * eng_to_eng * target_uncoord;
}
