#ifndef GD_IFC_COLLISION_H
#define GD_IFC_COLLISION_H

#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/mesh_convex_decomposition_settings.hpp>
#include <godot_cpp/classes/resource.hpp>
#include <godot_cpp/classes/shape3d.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <string>
#include <vector>

namespace IfcParse {
class schema_definition;
}

// Per-class collision. A GDIFCCollisionMatrix holds one GDIFCCollisionRule per IFC class, each
// with the shape built for the class and the physics layers of its StaticBody3D. Elements whose
// class matches no rule get the matrix's default rule, or no collision without one.

namespace godot {

class GDIFCCollisionRule : public Resource {
    GDCLASS(GDIFCCollisionRule, Resource)

public:
    // The shape types of Godot's scene importer, in its order where they overlap. Collision
    // matrices save them as numbers, so new types go at the end. Likewise, a body type option
    // added later must default to StaticBody3D, so saved matrices keep their meaning.
    enum ShapeType {
        SHAPE_NONE,
        SHAPE_TRIMESH,
        SHAPE_SIMPLE_CONVEX,
        SHAPE_BOX,
        SHAPE_DECOMPOSE_CONVEX,
    };

    /// Exact IFC class name, e.g. IfcWall. Also the resource name, so the Inspector's rule list shows it.
    String get_ifc_class() const { return ifc_class_; }
    void set_ifc_class(const String &v);

    /// Also matches subtypes of the class, e.g. IfcWallStandardCase for IfcWall
    bool get_include_subtypes() const { return include_subtypes_; }
    void set_include_subtypes(bool v) { include_subtypes_ = v; }

    ShapeType get_shape_type() const { return shape_type_; }
    void set_shape_type(ShapeType v);

    /// Physics layers of the StaticBody3D. Static bodies don't scan, so there is no mask.
    uint32_t get_collision_layer() const { return collision_layer_; }
    void set_collision_layer(uint32_t v) { collision_layer_ = v; }

    /// Decompose Convex only. Empty uses up to 16 hulls and a maximum concavity of 0.01.
    Ref<MeshConvexDecompositionSettings> get_decomposition_settings() const { return decomposition_settings_; }
    void set_decomposition_settings(const Ref<MeshConvexDecompositionSettings> &v) { decomposition_settings_ = v; }

protected:
    static void _bind_methods();
    void _validate_property(PropertyInfo &p_property) const;

private:
    String ifc_class_;
    bool include_subtypes_ = true;
    ShapeType shape_type_ = SHAPE_TRIMESH;
    uint32_t collision_layer_ = 1;
    Ref<MeshConvexDecompositionSettings> decomposition_settings_;
};

class GDIFCCollisionMatrix : public Resource {
    GDCLASS(GDIFCCollisionMatrix, Resource)

public:
    TypedArray<GDIFCCollisionRule> get_rules() const { return rules_; }
    void set_rules(const TypedArray<GDIFCCollisionRule> &v) { rules_ = v; }

    /// For classes no rule matches. Its class is ignored. Empty gives them no collision.
    Ref<GDIFCCollisionRule> get_default_rule() const { return default_rule_; }
    void set_default_rule(const Ref<GDIFCCollisionRule> &v) { default_rule_ = v; }

    /// The rule an element of this class gets in a file of this schema (IFC2X3, IFC4 or
    /// IFC4X3_ADD2), or null for no collision. Subtypes are only resolved for those schemas.
    Ref<GDIFCCollisionRule> get_rule_for_class(const String &ifc_class, const String &schema = "IFC4") const;

protected:
    static void _bind_methods();

private:
    TypedArray<GDIFCCollisionRule> rules_;
    Ref<GDIFCCollisionRule> default_rule_;
};

// The collision settings of one load, copied when it starts so edits to the resources mid-load
// aren't picked up. Built from a matrix, or from the class list of read_ifc().
class GDIFCCollisionPlan {
public:
    struct Rule {
        std::string ifc_class_uc; // upper case, as IfcParse names classes
        bool include_subtypes = true;
        GDIFCCollisionRule::ShapeType shape_type = GDIFCCollisionRule::SHAPE_TRIMESH;
        uint32_t collision_layer = 1;
        Ref<MeshConvexDecompositionSettings> decomposition_settings;
    };

    void set_from_matrix(const Ref<GDIFCCollisionMatrix> &matrix);
    // Trimesh on layer 1 for exactly these classes, or for every element when the list is empty
    void set_from_classes(bool create_collision, const Array &classes);
    // Subtypes are matched with this schema. Null matches exact classes only.
    void set_schema(const IfcParse::schema_definition *schema);

    bool is_enabled() const { return enabled_; }
    // The rule for elements of this class, or null for no collision. Cached per class.
    const Rule *rule_for(const String &ifc_class);

private:
    int _find_rule(const std::string &ifc_class_uc) const;

    bool enabled_ = false;
    std::vector<Rule> rules_;
    bool has_default_ = false;
    Rule default_;
    const IfcParse::schema_definition *schema_ = nullptr;
    // Rule index per class: >= 0 a rule, -1 the default, -2 no collision
    HashMap<String, int> cache_;
};

// Shapes for one element, built from its geometry still in memory. Reading the vertices back
// from an ArrayMesh would stall the GPU once per surface.
namespace collision_shapes {

struct Surface {
    PackedVector3Array vertices;
    PackedInt32Array indices;
};

struct PlacedShape {
    Ref<Shape3D> shape;
    Vector3 position; // of the CollisionShape3D, in the element's local space
};

// Empty when the geometry has no triangles or the shape can't be built
std::vector<PlacedShape> build(GDIFCCollisionRule::ShapeType shape_type, const std::vector<Surface> &surfaces,
                               const Ref<MeshConvexDecompositionSettings> &decomposition_settings);

} // namespace collision_shapes

// A Mesh that only holds triangle arrays in memory, so Godot's convex hull and convex
// decomposition can run on IFC geometry without an ArrayMesh. Internal.
class GDIFCGeometryMesh : public Mesh {
    GDCLASS(GDIFCGeometryMesh, Mesh)

public:
    void add_surface(const PackedVector3Array &vertices, const PackedInt32Array &indices);

    int32_t _get_surface_count() const override { return (int32_t)surfaces_.size(); }
    int32_t _surface_get_array_len(int32_t p_index) const override;
    int32_t _surface_get_array_index_len(int32_t p_index) const override;
    Array _surface_get_arrays(int32_t p_index) const override;
    TypedArray<Array> _surface_get_blend_shape_arrays(int32_t p_index) const override { return TypedArray<Array>(); }
    Dictionary _surface_get_lods(int32_t p_index) const override { return Dictionary(); }
    uint32_t _surface_get_format(int32_t p_index) const override { return ARRAY_FORMAT_VERTEX | ARRAY_FORMAT_INDEX; }
    uint32_t _surface_get_primitive_type(int32_t p_index) const override { return PRIMITIVE_TRIANGLES; }
    void _surface_set_material(int32_t p_index, const Ref<Material> &p_material) override {}
    Ref<Material> _surface_get_material(int32_t p_index) const override { return Ref<Material>(); }
    int32_t _get_blend_shape_count() const override { return 0; }
    StringName _get_blend_shape_name(int32_t p_index) const override { return StringName(); }
    void _set_blend_shape_name(int32_t p_index, const StringName &p_name) override {}
    AABB _get_aabb() const override { return aabb_; }

protected:
    static void _bind_methods() {}

private:
    struct Surface {
        PackedVector3Array vertices;
        PackedInt32Array indices;
    };
    std::vector<Surface> surfaces_;
    AABB aabb_;
    bool has_aabb_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(GDIFCCollisionRule::ShapeType);

#endif // GD_IFC_COLLISION_H
