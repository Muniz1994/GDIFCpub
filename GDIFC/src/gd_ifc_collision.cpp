#include "gd_ifc_collision.h"

#include <godot_cpp/classes/box_shape3d.hpp>
#include <godot_cpp/classes/collision_shape3d.hpp>
#include <godot_cpp/classes/concave_polygon_shape3d.hpp>
#include <godot_cpp/classes/convex_polygon_shape3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/static_body3d.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <IfcSchema.h>

#include <climits>

using namespace godot;

// Binds get_<name>/set_<name> and registers the property
#define GDIFC_BIND_PROPERTY(m_class, m_name, m_info)                                                  \
    ClassDB::bind_method(D_METHOD("get_" #m_name), &m_class::get_##m_name);                          \
    ClassDB::bind_method(D_METHOD("set_" #m_name, "value"), &m_class::set_##m_name);                 \
    ADD_PROPERTY(m_info, "set_" #m_name, "get_" #m_name)

// Box sides are at least this long, so flat elements (e.g. a plate modelled as one face) still
// make a valid box
static constexpr real_t MIN_BOX_SIZE = 0.001;
// Decomposition when a Decompose Convex rule has no settings of its own. Godot's defaults, one
// hull and a concavity of 1, leave a railing as a single hull.
static constexpr int DEFAULT_MAX_CONVEX_HULLS = 16;
static constexpr real_t DEFAULT_MAX_CONCAVITY = 0.01;

// ── Class matching ────────────────────────────────────────────────────────

static bool same_class(const std::string &a_uc, const std::string &b_uc) {
    if (a_uc == b_uc) return true;
    // IFC4X3 renamed IfcBuildingElement to IfcBuiltElement, so a rule for either works with every schema
    static const std::string building = "IFCBUILDINGELEMENT";
    static const std::string built = "IFCBUILTELEMENT";
    return (a_uc == building && b_uc == built) || (a_uc == built && b_uc == building);
}

// Steps from the class up to the rule's class: 0 for the class itself, -1 when the rule doesn't match
static int match_distance(const std::string &class_uc, const GDIFCCollisionPlan::Rule &rule,
                          const IfcParse::schema_definition *schema) {
    if (rule.ifc_class_uc.empty()) return -1;
    if (same_class(class_uc, rule.ifc_class_uc)) return 0;
    if (!rule.include_subtypes || !schema) return -1;

    const IfcParse::entity *entity = nullptr;
    try {
        entity = schema->declaration_by_name(class_uc)->as_entity();
    } catch (...) {
        return -1; // not a class of this schema
    }
    int steps = 1;
    for (const IfcParse::entity *super = entity ? entity->supertype() : nullptr; super; super = super->supertype(), steps++) {
        if (same_class(super->name_uc(), rule.ifc_class_uc)) return steps;
    }
    return -1;
}

// The rule closest to the class in its hierarchy, the first one on a tie. -1 when none matches.
static int find_rule_index(const std::string &class_uc, const std::vector<GDIFCCollisionPlan::Rule> &rules,
                           const IfcParse::schema_definition *schema) {
    int best = -1;
    int best_distance = INT_MAX;
    for (int i = 0; i < (int)rules.size(); i++) {
        const int distance = match_distance(class_uc, rules[i], schema);
        if (distance >= 0 && distance < best_distance) {
            best = i;
            best_distance = distance;
        }
    }
    return best;
}

static std::string upper_utf8(const String &s) {
    return std::string(s.strip_edges().to_upper().utf8().get_data());
}

static GDIFCCollisionPlan::Rule plan_rule(const Ref<GDIFCCollisionRule> &rule) {
    GDIFCCollisionPlan::Rule plan;
    if (rule.is_null()) {
        plan.shape_type = GDIFCCollisionRule::SHAPE_NONE; // an empty array slot matches nothing
        return plan;
    }
    plan.ifc_class_uc = upper_utf8(rule->get_ifc_class());
    plan.include_subtypes = rule->get_include_subtypes();
    plan.shape_type = rule->get_shape_type();
    plan.collision_layer = rule->get_collision_layer();
    plan.decomposition_settings = rule->get_decomposition_settings();
    return plan;
}

// ── GDIFCCollisionRule ────────────────────────────────────────────────────

void GDIFCCollisionRule::set_ifc_class(const String &v) {
    if (get_name().is_empty() || get_name() == ifc_class_) {
        set_name(v);
    }
    ifc_class_ = v;
    emit_changed();
}

void GDIFCCollisionRule::set_shape_type(ShapeType v) {
    shape_type_ = v;
    notify_property_list_changed();
    emit_changed();
}

void GDIFCCollisionRule::_validate_property(PropertyInfo &p_property) const {
    if (p_property.name == StringName("decomposition_settings") && shape_type_ != SHAPE_DECOMPOSE_CONVEX) {
        p_property.usage = PROPERTY_USAGE_NO_EDITOR;
    }
}

void GDIFCCollisionRule::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCCollisionRule, ifc_class, PropertyInfo(Variant::STRING, "ifc_class"));
    GDIFC_BIND_PROPERTY(GDIFCCollisionRule, include_subtypes, PropertyInfo(Variant::BOOL, "include_subtypes"));
    GDIFC_BIND_PROPERTY(GDIFCCollisionRule, shape_type,
        PropertyInfo(Variant::INT, "shape_type", PROPERTY_HINT_ENUM, "None,Trimesh,Simple Convex,Box,Decompose Convex"));
    GDIFC_BIND_PROPERTY(GDIFCCollisionRule, collision_layer, PropertyInfo(Variant::INT, "collision_layer", PROPERTY_HINT_LAYERS_3D_PHYSICS));
    GDIFC_BIND_PROPERTY(GDIFCCollisionRule, decomposition_settings,
        PropertyInfo(Variant::OBJECT, "decomposition_settings", PROPERTY_HINT_RESOURCE_TYPE, "MeshConvexDecompositionSettings"));

    BIND_ENUM_CONSTANT(SHAPE_NONE);
    BIND_ENUM_CONSTANT(SHAPE_TRIMESH);
    BIND_ENUM_CONSTANT(SHAPE_SIMPLE_CONVEX);
    BIND_ENUM_CONSTANT(SHAPE_BOX);
    BIND_ENUM_CONSTANT(SHAPE_DECOMPOSE_CONVEX);
}

// ── GDIFCCollisionMatrix ──────────────────────────────────────────────────

Ref<GDIFCCollisionRule> GDIFCCollisionMatrix::get_rule_for_class(const String &ifc_class, const String &schema) const {
    std::string schema_name = upper_utf8(schema);
    if (schema_name == "IFC4X3") schema_name = "IFC4X3_ADD2"; // the IFC 4.3 schema GDIFC reads
    const IfcParse::schema_definition *schema_definition = nullptr;
    try {
        schema_definition = IfcParse::schema_by_name(schema_name);
    } catch (...) {}

    std::vector<GDIFCCollisionPlan::Rule> plan_rules;
    for (int64_t i = 0; i < rules_.size(); i++) {
        plan_rules.push_back(plan_rule(rules_[i]));
    }
    const int index = find_rule_index(upper_utf8(ifc_class), plan_rules, schema_definition);
    Ref<GDIFCCollisionRule> rule = index >= 0 ? Ref<GDIFCCollisionRule>(rules_[index]) : default_rule_;
    if (rule.is_valid() && rule->get_shape_type() == GDIFCCollisionRule::SHAPE_NONE) {
        return Ref<GDIFCCollisionRule>();
    }
    return rule;
}

void GDIFCCollisionMatrix::_bind_methods() {
    GDIFC_BIND_PROPERTY(GDIFCCollisionMatrix, rules, PropertyInfo(Variant::ARRAY, "rules", PROPERTY_HINT_ARRAY_TYPE, "GDIFCCollisionRule"));
    GDIFC_BIND_PROPERTY(GDIFCCollisionMatrix, default_rule,
        PropertyInfo(Variant::OBJECT, "default_rule", PROPERTY_HINT_RESOURCE_TYPE, "GDIFCCollisionRule"));
    ClassDB::bind_method(D_METHOD("get_rule_for_class", "ifc_class", "schema"), &GDIFCCollisionMatrix::get_rule_for_class, DEFVAL("IFC4"));
}

// ── GDIFCCollisionPlan ────────────────────────────────────────────────────

void GDIFCCollisionPlan::set_from_matrix(const Ref<GDIFCCollisionMatrix> &matrix) {
    rules_.clear();
    cache_.clear();
    enabled_ = matrix.is_valid();
    has_default_ = false;
    if (!enabled_) return;

    const TypedArray<GDIFCCollisionRule> rules = matrix->get_rules();
    for (int64_t i = 0; i < rules.size(); i++) {
        rules_.push_back(plan_rule(rules[i]));
    }
    const Ref<GDIFCCollisionRule> default_rule = matrix->get_default_rule();
    if (default_rule.is_valid()) {
        has_default_ = true;
        default_ = plan_rule(default_rule);
    }
}

void GDIFCCollisionPlan::set_from_classes(bool create_collision, const Array &classes) {
    rules_.clear();
    cache_.clear();
    enabled_ = create_collision;
    has_default_ = classes.is_empty();
    default_ = Rule();
    for (int64_t i = 0; i < classes.size(); i++) {
        Rule rule;
        rule.ifc_class_uc = upper_utf8(classes[i]);
        rule.include_subtypes = false; // the list has always been matched by exact class name
        rules_.push_back(rule);
    }
}

void GDIFCCollisionPlan::set_schema(const IfcParse::schema_definition *schema) {
    schema_ = schema;
    cache_.clear();
}

int GDIFCCollisionPlan::_find_rule(const std::string &ifc_class_uc) const {
    const int index = find_rule_index(ifc_class_uc, rules_, schema_);
    if (index >= 0) return index;
    return has_default_ ? -1 : -2;
}

const GDIFCCollisionPlan::Rule *GDIFCCollisionPlan::rule_for(const String &ifc_class) {
    if (!enabled_) return nullptr;

    int index;
    if (const int *cached = cache_.getptr(ifc_class)) {
        index = *cached;
    } else {
        index = _find_rule(upper_utf8(ifc_class));
        cache_.insert(ifc_class, index);
    }

    const Rule *rule = index >= 0 ? &rules_[index] : (index == -1 ? &default_ : nullptr);
    if (!rule || rule->shape_type == GDIFCCollisionRule::SHAPE_NONE) return nullptr;
    return rule;
}

// ── Shapes ────────────────────────────────────────────────────────────────

// Same faces as Mesh::create_trimesh_shape()
static Ref<Shape3D> build_trimesh(const std::vector<collision_shapes::Surface> &surfaces) {
    int64_t face_point_count = 0;
    for (const auto &surface : surfaces) face_point_count += surface.indices.size();
    if (face_point_count == 0) return Ref<Shape3D>();

    PackedVector3Array face_points;
    face_points.resize(face_point_count);
    Vector3 *face_points_w = face_points.ptrw();
    int64_t k = 0;
    for (const auto &surface : surfaces) {
        const Vector3 *vertices = surface.vertices.ptr();
        const int32_t *indices = surface.indices.ptr();
        for (int64_t i = 0; i < surface.indices.size(); i++) {
            face_points_w[k++] = vertices[indices[i]];
        }
    }

    Ref<ConcavePolygonShape3D> shape;
    shape.instantiate();
    shape->set_faces(face_points);
    return shape;
}

// The bounding box in the element's local space, which follows its axes for most IFC elements
static collision_shapes::PlacedShape build_box(const std::vector<collision_shapes::Surface> &surfaces) {
    AABB aabb;
    bool has_point = false;
    for (const auto &surface : surfaces) {
        const Vector3 *vertices = surface.vertices.ptr();
        const int32_t *indices = surface.indices.ptr();
        for (int64_t i = 0; i < surface.indices.size(); i++) {
            if (has_point) {
                aabb.expand_to(vertices[indices[i]]);
            } else {
                aabb = AABB(vertices[indices[i]], Vector3());
                has_point = true;
            }
        }
    }
    if (!has_point) return {};

    Ref<BoxShape3D> shape;
    shape.instantiate();
    shape->set_size(aabb.size.max(Vector3(MIN_BOX_SIZE, MIN_BOX_SIZE, MIN_BOX_SIZE)));
    return { shape, aabb.get_center() };
}

static Ref<GDIFCGeometryMesh> geometry_mesh(const std::vector<collision_shapes::Surface> &surfaces) {
    Ref<GDIFCGeometryMesh> mesh;
    mesh.instantiate();
    for (const auto &surface : surfaces) {
        if (!surface.indices.is_empty()) mesh->add_surface(surface.vertices, surface.indices);
    }
    return mesh;
}

// The hull's own vertices only, found by Godot's ConvexHullComputer
static Ref<Shape3D> build_simple_convex(const std::vector<collision_shapes::Surface> &surfaces) {
    Ref<GDIFCGeometryMesh> mesh = geometry_mesh(surfaces);
    if (mesh->get_surface_count() == 0) return Ref<Shape3D>();
    return mesh->create_convex_shape(true, false);
}

// V-HACD through MeshInstance3D, the only way the extension API exposes it. V-HACD is only in
// editor builds, so this only works at import.
static std::vector<collision_shapes::PlacedShape> build_decomposed(const std::vector<collision_shapes::Surface> &surfaces,
                                                                   const Ref<MeshConvexDecompositionSettings> &rule_settings) {
    std::vector<collision_shapes::PlacedShape> shapes;
    Ref<GDIFCGeometryMesh> mesh = geometry_mesh(surfaces);
    if (mesh->get_surface_count() == 0) return shapes;

    Ref<MeshConvexDecompositionSettings> settings = rule_settings;
    if (settings.is_null()) {
        settings.instantiate();
        settings->set_max_convex_hulls(DEFAULT_MAX_CONVEX_HULLS);
        settings->set_max_concavity(DEFAULT_MAX_CONCAVITY);
    }

    MeshInstance3D *mesh_instance = memnew(MeshInstance3D);
    mesh_instance->set_mesh(mesh);
    mesh_instance->create_multiple_convex_collisions(settings);
    for (int i = 0; i < mesh_instance->get_child_count(); i++) {
        StaticBody3D *body = Object::cast_to<StaticBody3D>(mesh_instance->get_child(i));
        if (!body) continue;
        for (int j = 0; j < body->get_child_count(); j++) {
            CollisionShape3D *collision_shape = Object::cast_to<CollisionShape3D>(body->get_child(j));
            if (collision_shape && collision_shape->get_shape().is_valid()) {
                shapes.push_back({ collision_shape->get_shape(), collision_shape->get_position() });
            }
        }
    }
    memdelete(mesh_instance);
    return shapes;
}

std::vector<collision_shapes::PlacedShape> collision_shapes::build(GDIFCCollisionRule::ShapeType shape_type,
        const std::vector<Surface> &surfaces, const Ref<MeshConvexDecompositionSettings> &decomposition_settings) {
    std::vector<PlacedShape> shapes;
    switch (shape_type) {
        case GDIFCCollisionRule::SHAPE_NONE:
            break;
        case GDIFCCollisionRule::SHAPE_TRIMESH:
            shapes.push_back({ build_trimesh(surfaces), Vector3() });
            break;
        case GDIFCCollisionRule::SHAPE_SIMPLE_CONVEX:
            shapes.push_back({ build_simple_convex(surfaces), Vector3() });
            break;
        case GDIFCCollisionRule::SHAPE_BOX:
            shapes.push_back(build_box(surfaces));
            break;
        case GDIFCCollisionRule::SHAPE_DECOMPOSE_CONVEX:
            shapes = build_decomposed(surfaces, decomposition_settings);
            // V-HACD gives up on some geometry; one hull still collides
            if (shapes.empty()) shapes.push_back({ build_simple_convex(surfaces), Vector3() });
            break;
    }
    if (shapes.size() == 1 && shapes[0].shape.is_null()) shapes.clear();
    return shapes;
}

// ── GDIFCGeometryMesh ─────────────────────────────────────────────────────

void GDIFCGeometryMesh::add_surface(const PackedVector3Array &vertices, const PackedInt32Array &indices) {
    surfaces_.push_back({ vertices, indices });
    for (int64_t i = 0; i < vertices.size(); i++) {
        if (has_aabb_) {
            aabb_.expand_to(vertices[i]);
        } else {
            aabb_ = AABB(vertices[i], Vector3());
            has_aabb_ = true;
        }
    }
}

int32_t GDIFCGeometryMesh::_surface_get_array_len(int32_t p_index) const {
    ERR_FAIL_INDEX_V(p_index, (int32_t)surfaces_.size(), 0);
    return (int32_t)surfaces_[p_index].vertices.size();
}

int32_t GDIFCGeometryMesh::_surface_get_array_index_len(int32_t p_index) const {
    ERR_FAIL_INDEX_V(p_index, (int32_t)surfaces_.size(), 0);
    return (int32_t)surfaces_[p_index].indices.size();
}

Array GDIFCGeometryMesh::_surface_get_arrays(int32_t p_index) const {
    ERR_FAIL_INDEX_V(p_index, (int32_t)surfaces_.size(), Array());
    Array arrays;
    arrays.resize(ARRAY_MAX);
    arrays[ARRAY_VERTEX] = surfaces_[p_index].vertices;
    arrays[ARRAY_INDEX] = surfaces_[p_index].indices;
    return arrays;
}
