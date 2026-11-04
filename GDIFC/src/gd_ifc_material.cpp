#include "gd_ifc_material.h"

#include <godot_cpp/core/class_db.hpp>

#include <unordered_set>

using namespace godot;

// Binds get_<name>/set_<name> and registers the property
#define GDIFC_BIND_PROPERTY(m_class, m_name, m_info)                                                  \
    ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                          \
    ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &m_class::set_##m_name);                 \
    ADD_PROPERTY(m_info, "set_" #m_name, "get_" #m_name)

static PropertyInfo resource_property(const char *name, const char *type) {
    return PropertyInfo(Variant::OBJECT, name, PROPERTY_HINT_RESOURCE_TYPE, type);
}

static PropertyInfo array_property(const char *name, const char *type) {
    return PropertyInfo(Variant::ARRAY, name, PROPERTY_HINT_ARRAY_TYPE, type);
}

static void call_if_valid(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn, const Ref<GDIFCMaterialDefinition> &definition) {
    if (definition.is_valid()) fn(definition);
}

template <typename T>
static void call_each(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn, const TypedArray<T> &definitions) {
    for (int64_t i = 0; i < definitions.size(); i++) {
        call_if_valid(fn, Ref<GDIFCMaterialDefinition>(definitions[i]));
    }
}

// ── GDIFCMaterialDefinition ───────────────────────────────────────────────

void GDIFCMaterialDefinition::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, description, PropertyInfo(Variant::STRING, "description"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, category, PropertyInfo(Variant::STRING, "category"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, express_id, PropertyInfo(Variant::INT, "express_id"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, properties, PropertyInfo(Variant::DICTIONARY, "properties"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, classifications, PropertyInfo(Variant::ARRAY, "classifications"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialDefinition, ifc_object,
        PropertyInfo(Variant::OBJECT, "ifc_object", PROPERTY_HINT_RESOURCE_TYPE, "GDIFCEntityBase", PROPERTY_USAGE_NONE));
}

// ── GDIFCMaterialSingle ───────────────────────────────────────────────────

void GDIFCMaterialSingle::set_visual(const Ref<Material> &v) {
    if (visual_ == v) return;
    visual_ = v;
    emit_changed();
}

void GDIFCMaterialSingle::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    for (int64_t i = 0; i < relationships_.size(); i++) {
        const Dictionary relationship = relationships_[i];
        call_each(fn, TypedArray<GDIFCMaterialSingle>(relationship.get("materials", Array())));
    }
}

void GDIFCMaterialSingle::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialSingle, visual, resource_property("visual", "Material"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialSingle, relationships, PropertyInfo(Variant::ARRAY, "relationships"));
}

// ── Layers ────────────────────────────────────────────────────────────────

void GDIFCMaterialLayer::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_if_valid(fn, material_);
}

void GDIFCMaterialLayer::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayer, material, resource_property("material", "GDIFCMaterialSingle"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayer, thickness, PropertyInfo(Variant::FLOAT, "thickness", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayer, is_ventilated, PropertyInfo(Variant::BOOL, "is_ventilated"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayer, priority, PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-1,100,1"));
}

double GDIFCMaterialLayerSet::get_total_thickness() const {
    double total = 0.0;
    for (int64_t i = 0; i < layers_.size(); i++) {
        Ref<GDIFCMaterialLayer> layer = layers_[i];
        if (layer.is_valid()) total += layer->get_thickness();
    }
    return total;
}

void GDIFCMaterialLayerSet::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_each(fn, layers_);
}

void GDIFCMaterialLayerSet::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSet, layers, array_property("layers", "GDIFCMaterialLayer"));
    ClassDB::bind_method(D_METHOD("get_total_thickness"), &GDIFCMaterialLayerSet::get_total_thickness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "total_thickness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY),
        "", "get_total_thickness");
}

void GDIFCMaterialLayerSetUsage::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_if_valid(fn, layer_set_);
}

void GDIFCMaterialLayerSetUsage::_bind_methods() {
    BIND_CONSTANT(AXIS1);
    BIND_CONSTANT(AXIS2);
    BIND_CONSTANT(AXIS3);
    BIND_CONSTANT(POSITIVE);
    BIND_CONSTANT(NEGATIVE);

    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSetUsage, layer_set, resource_property("layer_set", "GDIFCMaterialLayerSet"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSetUsage, direction, PropertyInfo(Variant::INT, "direction", PROPERTY_HINT_ENUM, "Axis 1,Axis 2,Axis 3"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSetUsage, sense, PropertyInfo(Variant::INT, "sense", PROPERTY_HINT_ENUM, "Positive,Negative"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSetUsage, offset, PropertyInfo(Variant::FLOAT, "offset"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLayerSetUsage, reference_extent, PropertyInfo(Variant::FLOAT, "reference_extent"));
}

// ── Profiles ──────────────────────────────────────────────────────────────

void GDIFCMaterialProfile::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_if_valid(fn, material_);
}

void GDIFCMaterialProfile::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfile, material, resource_property("material", "GDIFCMaterialSingle"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfile, priority, PropertyInfo(Variant::INT, "priority", PROPERTY_HINT_RANGE, "-1,100,1"));
}

void GDIFCMaterialProfileSet::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_each(fn, profiles_);
}

void GDIFCMaterialProfileSet::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfileSet, profiles, array_property("profiles", "GDIFCMaterialProfile"));
}

void GDIFCMaterialProfileSetUsage::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_if_valid(fn, profile_set_);
}

void GDIFCMaterialProfileSetUsage::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfileSetUsage, profile_set, resource_property("profile_set", "GDIFCMaterialProfileSet"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfileSetUsage, cardinal_point, PropertyInfo(Variant::INT, "cardinal_point", PROPERTY_HINT_RANGE, "0,19,1"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialProfileSetUsage, reference_extent, PropertyInfo(Variant::FLOAT, "reference_extent"));
}

// ── Constituents and lists ────────────────────────────────────────────────

void GDIFCMaterialConstituent::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_if_valid(fn, material_);
}

void GDIFCMaterialConstituent::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialConstituent, material, resource_property("material", "GDIFCMaterialSingle"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialConstituent, fraction, PropertyInfo(Variant::FLOAT, "fraction", PROPERTY_HINT_RANGE, "-1,1,0.001"));
}

void GDIFCMaterialConstituentSet::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_each(fn, constituents_);
}

void GDIFCMaterialConstituentSet::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialConstituentSet, constituents, array_property("constituents", "GDIFCMaterialConstituent"));
}

void GDIFCMaterialList::for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    call_each(fn, materials_);
}

void GDIFCMaterialList::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialList, materials, array_property("materials", "GDIFCMaterialSingle"));
}

// ── GDIFCMaterialLibrary ──────────────────────────────────────────────────

void GDIFCMaterialLibrary::for_each_definition(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {
    std::unordered_set<uint64_t> visited;
    std::function<void(const Ref<GDIFCMaterialDefinition> &)> visit = [&](const Ref<GDIFCMaterialDefinition> &definition) {
        if (!visited.insert(definition->get_instance_id()).second) return;
        fn(definition);
        definition->for_each_child(visit);
    };
    call_each(visit, definitions_);
    call_each(visit, materials_);
}

void GDIFCMaterialLibrary::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCMaterialLibrary, definitions, array_property("definitions", "GDIFCMaterialDefinition"));
    GDIFC_BIND_PROPERTY(GDIFCMaterialLibrary, materials, array_property("materials", "GDIFCMaterialSingle"));
}
