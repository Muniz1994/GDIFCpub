//
// Created by engbr on 18/02/2026.
//

#include "gd_ifc_settings.h"

#include "godot_cpp/core/class_db.hpp"


void godot::GDIFCLoaderSettings::_bind_methods() {
    using namespace godot;

    // --- Coordinate To Origin ---
    ClassDB::bind_method(D_METHOD("set_coordinate_to_origin", "val"), &GDIFCLoaderSettings::setCoordinateToOrigin);
    ClassDB::bind_method(D_METHOD("get_coordinate_to_origin"), &GDIFCLoaderSettings::getCoordinateToOrigin);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "coordinate_to_origin"), "set_coordinate_to_origin", "get_coordinate_to_origin");

    // --- Circle Segments ---
    ClassDB::bind_method(D_METHOD("set_circle_segments", "val"), &GDIFCLoaderSettings::setCircleSegments);
    ClassDB::bind_method(D_METHOD("get_circle_segments"), &GDIFCLoaderSettings::getCircleSegments);
    // Added a Range hint so it appears as a slider in the inspector (min 3, max 128)
    ADD_PROPERTY(PropertyInfo(Variant::INT, "circle_segments", PROPERTY_HINT_RANGE, "3,128,1"), "set_circle_segments", "get_circle_segments");

    // --- Tape Size ---
    ClassDB::bind_method(D_METHOD("set_tape_size", "val"), &GDIFCLoaderSettings::setTapeSize);
    ClassDB::bind_method(D_METHOD("get_tape_size"), &GDIFCLoaderSettings::getTapeSize);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "tape_size"), "set_tape_size", "get_tape_size");

    // --- Memory Limit ---
    ClassDB::bind_method(D_METHOD("set_memory_limit", "val"), &GDIFCLoaderSettings::setMemoryLimit);
    ClassDB::bind_method(D_METHOD("get_memory_limit"), &GDIFCLoaderSettings::getMemoryLimit);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "memory_limit"), "set_memory_limit", "get_memory_limit");

    // --- Line Writer Buffer ---
    ClassDB::bind_method(D_METHOD("set_line_writer_buffer", "val"), &GDIFCLoaderSettings::setLineWriterBuffer);
    ClassDB::bind_method(D_METHOD("get_line_writer_buffer"), &GDIFCLoaderSettings::getLineWriterBuffer);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "line_writer_buffer"), "set_line_writer_buffer", "get_line_writer_buffer");

    // --- Tolerance Plane Intersection ---
    ClassDB::bind_method(D_METHOD("set_tolerance_plane_intersection", "val"), &GDIFCLoaderSettings::setTolerancePlaneIntersection);
    ClassDB::bind_method(D_METHOD("get_tolerance_plane_intersection"), &GDIFCLoaderSettings::getTolerancePlaneIntersection);
    // Added formatting hint for small float numbers
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tolerance_plane_intersection", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT), "set_tolerance_plane_intersection", "get_tolerance_plane_intersection");

    // --- Tolerance Plane Deviation ---
    ClassDB::bind_method(D_METHOD("set_tolerance_plane_deviation", "val"), &GDIFCLoaderSettings::setTolerancePlaneDeviation);
    ClassDB::bind_method(D_METHOD("get_tolerance_plane_deviation"), &GDIFCLoaderSettings::getTolerancePlaneDeviation);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tolerance_plane_deviation"), "set_tolerance_plane_deviation", "get_tolerance_plane_deviation");

    // --- Tolerance Back Deviation Distance ---
    ClassDB::bind_method(D_METHOD("set_tolerance_back_deviation_distance", "val"), &GDIFCLoaderSettings::setToleranceBackDeviationDistance);
    ClassDB::bind_method(D_METHOD("get_tolerance_back_deviation_distance"), &GDIFCLoaderSettings::getToleranceBackDeviationDistance);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tolerance_back_deviation_distance"), "set_tolerance_back_deviation_distance", "get_tolerance_back_deviation_distance");

    // --- Tolerance Inside Outside Perimeter ---
    ClassDB::bind_method(D_METHOD("set_tolerance_inside_outside_perimeter", "val"), &GDIFCLoaderSettings::setToleranceInsideOutsidePerimeter);
    ClassDB::bind_method(D_METHOD("get_tolerance_inside_outside_perimeter"), &GDIFCLoaderSettings::getToleranceInsideOutsidePerimeter);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tolerance_inside_outside_perimeter"), "set_tolerance_inside_outside_perimeter", "get_tolerance_inside_outside_perimeter");

    // --- Tolerance Scalar Equality ---
    ClassDB::bind_method(D_METHOD("set_tolerance_scalar_equality", "val"), &GDIFCLoaderSettings::setToleranceScalarEquality);
    ClassDB::bind_method(D_METHOD("get_tolerance_scalar_equality"), &GDIFCLoaderSettings::getToleranceScalarEquality);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "tolerance_scalar_equality"), "set_tolerance_scalar_equality", "get_tolerance_scalar_equality");

    // --- Plane Refit Iterations ---
    ClassDB::bind_method(D_METHOD("set_plane_refit_iterations", "val"), &GDIFCLoaderSettings::setPlaneRefitIterations);
    ClassDB::bind_method(D_METHOD("get_plane_refit_iterations"), &GDIFCLoaderSettings::getPlaneRefitIterations);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "plane_refit_iterations"), "set_plane_refit_iterations", "get_plane_refit_iterations");

    // --- Boolean Union Threshold ---
    ClassDB::bind_method(D_METHOD("set_boolean_union_threshold", "val"), &GDIFCLoaderSettings::setBooleanUnionThreshold);
    ClassDB::bind_method(D_METHOD("get_boolean_union_threshold"), &GDIFCLoaderSettings::getBooleanUnionThreshold);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "boolean_union_threshold"), "set_boolean_union_threshold", "get_boolean_union_threshold");
}
