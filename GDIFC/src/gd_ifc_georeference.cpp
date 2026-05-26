// gd_ifc_georeference.cpp
#include "gd_ifc_georeference.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/variant.hpp>

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
    ClassDB::bind_method(D_METHOD("get_scale"),                 &IFCGeoreference::get_scale);
    ClassDB::bind_method(D_METHOD("set_scale", "v"),            &IFCGeoreference::set_scale);

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
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "scale"),
        "set_scale", "get_scale");

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
}
