#ifndef GD_IFC_MATERIAL_H
#define GD_IFC_MATERIAL_H

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "gd_ifc_entity_base.h"

#include <functional>

// IFC material definitions, read with GDIFCLoaderSettings.ifc_materials. They hold only the IFC
// data: the look is a plain Godot Material on GDIFCMaterialSingle.visual, so any material can
// replace it. Every element using a definition shares the same resource.
//
// The IFC Name is the resource name. Lengths are in the IFC file's length unit, like quantities.
// Optional IFC numbers that are not given are -1 (priority, fraction) or 0 (reference extent,
// cardinal point).

namespace godot {

class GDIFCMaterialDefinition : public Resource {
    GDCLASS(GDIFCMaterialDefinition, Resource)

public:
    String get_description() const { return description_; }
    void set_description(const String &v) { description_ = v; }

    String get_category() const { return category_; }
    void set_category(const String &v) { category_ = v; }

    /// EXPRESS id in the IFC file. Materials have no GlobalId.
    int64_t get_express_id() const { return express_id_; }
    void set_express_id(int64_t v) { express_id_ = v; }

    /// Material property sets: set name -> {property name -> value}, like IFCNode.properties
    Dictionary get_properties() const { return properties_; }
    void set_properties(const Dictionary &v) { properties_ = v; }

    /// Value type and unit of each property: set name -> {property name -> detail}, like IFCNode.property_details
    Dictionary get_property_details() const { return property_details_; }
    void set_property_details(const Dictionary &v) { property_details_ = v; }

    /// Classification and library references, one Dictionary each
    Array get_classifications() const { return classifications_; }
    void set_classifications(const Array &v) { classifications_ = v; }

    /// Typed IFC entity, relinked on load like IFCNode.ifc_object. Not saved.
    Ref<GDIFCEntityBase> get_ifc_object() const { return ifc_object_; }
    void set_ifc_object(const Ref<GDIFCEntityBase> &v) { ifc_object_ = v; }

    /// Calls `fn` with each definition this one references directly
    virtual void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const {}

protected:
    static void _bind_methods();

private:
    String description_;
    String category_;
    int64_t express_id_ = 0;
    Dictionary properties_;
    Dictionary property_details_;
    Array classifications_;
    Ref<GDIFCEntityBase> ifc_object_;
};

/// IfcMaterial. The only definition with a look of its own.
class GDIFCMaterialSingle : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialSingle, GDIFCMaterialDefinition)

public:
    /// The surfaces coloured by this material share this Material
    Ref<Material> get_visual() const { return visual_; }
    void set_visual(const Ref<Material> &v);

    /// IfcMaterialRelationship where this material is the whole: {name, description, expression, materials}
    Array get_relationships() const { return relationships_; }
    void set_relationships(const Array &v) { relationships_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<Material> visual_;
    Array relationships_;
};

/// IfcMaterialLayer (and IfcMaterialLayerWithOffsets)
class GDIFCMaterialLayer : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialLayer, GDIFCMaterialDefinition)

public:
    /// Null for an air gap
    Ref<GDIFCMaterialSingle> get_material() const { return material_; }
    void set_material(const Ref<GDIFCMaterialSingle> &v) { material_ = v; }

    double get_thickness() const { return thickness_; }
    void set_thickness(double v) { thickness_ = v; }

    bool get_is_ventilated() const { return is_ventilated_; }
    void set_is_ventilated(bool v) { is_ventilated_ = v; }

    int64_t get_priority() const { return priority_; }
    void set_priority(int64_t v) { priority_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<GDIFCMaterialSingle> material_;
    double thickness_ = 0.0;
    bool is_ventilated_ = false;
    int64_t priority_ = -1;
};

/// IfcMaterialLayerSet. Layers are in IFC order.
class GDIFCMaterialLayerSet : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialLayerSet, GDIFCMaterialDefinition)

public:
    TypedArray<GDIFCMaterialLayer> get_layers() const { return layers_; }
    void set_layers(const TypedArray<GDIFCMaterialLayer> &v) { layers_ = v; }

    double get_total_thickness() const;

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCMaterialLayer> layers_;
};

/// IfcMaterialLayerSetUsage: how a layer set is placed on one element
class GDIFCMaterialLayerSetUsage : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialLayerSetUsage, GDIFCMaterialDefinition)

public:
    enum Direction { AXIS1, AXIS2, AXIS3 };
    enum Sense { POSITIVE, NEGATIVE };

    Ref<GDIFCMaterialLayerSet> get_layer_set() const { return layer_set_; }
    void set_layer_set(const Ref<GDIFCMaterialLayerSet> &v) { layer_set_ = v; }

    int get_direction() const { return direction_; }
    void set_direction(int v) { direction_ = v; }

    int get_sense() const { return sense_; }
    void set_sense(int v) { sense_ = v; }

    double get_offset() const { return offset_; }
    void set_offset(double v) { offset_ = v; }

    double get_reference_extent() const { return reference_extent_; }
    void set_reference_extent(double v) { reference_extent_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<GDIFCMaterialLayerSet> layer_set_;
    int direction_ = AXIS2;
    int sense_ = POSITIVE;
    double offset_ = 0.0;
    double reference_extent_ = 0.0;
};

/// IfcMaterialProfile (and IfcMaterialProfileWithOffsets). The profile itself is on ifc_object.
class GDIFCMaterialProfile : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialProfile, GDIFCMaterialDefinition)

public:
    Ref<GDIFCMaterialSingle> get_material() const { return material_; }
    void set_material(const Ref<GDIFCMaterialSingle> &v) { material_ = v; }

    int64_t get_priority() const { return priority_; }
    void set_priority(int64_t v) { priority_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<GDIFCMaterialSingle> material_;
    int64_t priority_ = -1;
};

/// IfcMaterialProfileSet
class GDIFCMaterialProfileSet : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialProfileSet, GDIFCMaterialDefinition)

public:
    TypedArray<GDIFCMaterialProfile> get_profiles() const { return profiles_; }
    void set_profiles(const TypedArray<GDIFCMaterialProfile> &v) { profiles_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCMaterialProfile> profiles_;
};

/// IfcMaterialProfileSetUsage (and IfcMaterialProfileSetUsageTapering, whose end set is on ifc_object)
class GDIFCMaterialProfileSetUsage : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialProfileSetUsage, GDIFCMaterialDefinition)

public:
    Ref<GDIFCMaterialProfileSet> get_profile_set() const { return profile_set_; }
    void set_profile_set(const Ref<GDIFCMaterialProfileSet> &v) { profile_set_ = v; }

    int64_t get_cardinal_point() const { return cardinal_point_; }
    void set_cardinal_point(int64_t v) { cardinal_point_ = v; }

    double get_reference_extent() const { return reference_extent_; }
    void set_reference_extent(double v) { reference_extent_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<GDIFCMaterialProfileSet> profile_set_;
    int64_t cardinal_point_ = 0;
    double reference_extent_ = 0.0;
};

/// IfcMaterialConstituent
class GDIFCMaterialConstituent : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialConstituent, GDIFCMaterialDefinition)

public:
    Ref<GDIFCMaterialSingle> get_material() const { return material_; }
    void set_material(const Ref<GDIFCMaterialSingle> &v) { material_ = v; }

    double get_fraction() const { return fraction_; }
    void set_fraction(double v) { fraction_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    Ref<GDIFCMaterialSingle> material_;
    double fraction_ = -1.0;
};

/// IfcMaterialConstituentSet
class GDIFCMaterialConstituentSet : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialConstituentSet, GDIFCMaterialDefinition)

public:
    TypedArray<GDIFCMaterialConstituent> get_constituents() const { return constituents_; }
    void set_constituents(const TypedArray<GDIFCMaterialConstituent> &v) { constituents_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCMaterialConstituent> constituents_;
};

/// IfcMaterialList (IFC2X3, deprecated in IFC4)
class GDIFCMaterialList : public GDIFCMaterialDefinition {
    GDCLASS(GDIFCMaterialList, GDIFCMaterialDefinition)

public:
    TypedArray<GDIFCMaterialSingle> get_materials() const { return materials_; }
    void set_materials(const TypedArray<GDIFCMaterialSingle> &v) { materials_ = v; }

    void for_each_child(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const override;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCMaterialSingle> materials_;
};

/// Every material definition of a loaded model, on GDIFCManager.material_library
class GDIFCMaterialLibrary : public Resource {
    GDCLASS(GDIFCMaterialLibrary, Resource)

public:
    /// The definitions elements are associated with, by express id
    TypedArray<GDIFCMaterialDefinition> get_definitions() const { return definitions_; }
    void set_definitions(const TypedArray<GDIFCMaterialDefinition> &v) { definitions_ = v; }

    /// Every IfcMaterial, including those inside sets, by express id
    TypedArray<GDIFCMaterialSingle> get_materials() const { return materials_; }
    void set_materials(const TypedArray<GDIFCMaterialSingle> &v) { materials_ = v; }

    /// Calls `fn` once with every definition in the library, whatever its depth
    void for_each_definition(const std::function<void(const Ref<GDIFCMaterialDefinition> &)> &fn) const;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCMaterialDefinition> definitions_;
    TypedArray<GDIFCMaterialSingle> materials_;
};

} // namespace godot

#endif // GD_IFC_MATERIAL_H
