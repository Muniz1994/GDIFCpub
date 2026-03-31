#ifndef GD_IFC_ENTITY_BASE_H
#define GD_IFC_ENTITY_BASE_H

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>

#include <IfcBaseClass.h>
#include <IfcFile.h>
#include <IfcSchema.h>
#include <aggregate_of_instance.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace godot {

class GDIFCEntityBase : public RefCounted {
    GDCLASS(GDIFCEntityBase, RefCounted)

public:
    // Wrapped C++ IFC entity (raw pointer; lifetime owned by IfcFile)
    IfcUtil::IfcBaseClass* entity_ = nullptr;
    // Keep file alive while this object lives
    std::shared_ptr<IfcParse::IfcFile> file_;

    // ── Factory ────────────────────────────────────────────────────────────
    using FactoryFn = std::function<Ref<GDIFCEntityBase>(
        IfcUtil::IfcBaseClass*, std::shared_ptr<IfcParse::IfcFile>)>;

    static void register_factory(const std::string& ifc_type_name, FactoryFn fn);

    /// Wrap an IfcBaseClass* in the most-derived registered GD class.
    /// Falls back to a plain GDIFCEntityBase if the type is not registered.
    static Ref<GDIFCEntityBase> wrap(IfcUtil::IfcBaseClass* entity,
                                     std::shared_ptr<IfcParse::IfcFile> file);

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void init(IfcUtil::IfcBaseClass* entity, std::shared_ptr<IfcParse::IfcFile> file);

    // ── GDScript API ───────────────────────────────────────────────────────
    int64_t get_express_id() const;
    godot::String get_type() const;
    bool is_a(godot::String ifc_class) const;

    godot::Variant    get_attribute(godot::String attr_name);
    godot::PackedStringArray get_attribute_names();
    godot::Dictionary get_all_attributes();
    godot::Array      get_inverse(godot::String inverse_attr_name);

protected:
    static void _bind_methods();

private:
    static std::unordered_map<std::string, FactoryFn>& factory_map();
};

} // namespace godot

// ── Free helper functions used by generated getters/setters ────────────────
//
//  Named gd_attr_* for read access and gd_set_* for write access.
//  Defined inline here so the generated translation units can use them
//  without additional link-time dependencies on entity_base.cpp.
//
//  NOTE: entity_ and file_ are public on GDIFCEntityBase so helpers can
//        access them directly.

#include <Argument.h>
#include <IfcEntityInstanceData.h>
#include <boost/logic/tribool.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>

// ── Scalar getters ─────────────────────────────────────────────────────────
// AttributeValue is NOT a boost::variant — use av.type() + explicit cast operators.

inline godot::String gd_attr_string(const AttributeValue& av) {
    auto t = av.type();
    if (t == IfcUtil::Argument_STRING) {
        return godot::String(((std::string)av).c_str());
    }
    if (t == IfcUtil::Argument_ENUMERATION) {
        EnumerationReference ref = av;
        return godot::String(ref.value());
    }
    return godot::String();
}

inline int64_t gd_attr_int(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_INT) {
        return static_cast<int64_t>((int)av);
    }
    return 0;
}

inline double gd_attr_double(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_DOUBLE) {
        return (double)av;
    }
    return 0.0;
}

inline bool gd_attr_bool(const AttributeValue& av) {
    auto t = av.type();
    if (t == IfcUtil::Argument_BOOL) {
        return (bool)av;
    }
    if (t == IfcUtil::Argument_LOGICAL) {
        boost::logic::tribool tb = av;
        if (!boost::logic::indeterminate(tb)) {
            return static_cast<bool>(tb);
        }
    }
    return false;
}

// ── Entity-reference getter ────────────────────────────────────────────────

inline godot::Ref<godot::GDIFCEntityBase> gd_attr_entity(
    const AttributeValue& av,
    std::shared_ptr<IfcParse::IfcFile> file)
{
    if (av.type() == IfcUtil::Argument_ENTITY_INSTANCE) {
        IfcUtil::IfcBaseClass* ptr = av;
        if (ptr != nullptr) {
            return godot::GDIFCEntityBase::wrap(ptr, file);
        }
    }
    return {};
}

// ── Aggregate getters ──────────────────────────────────────────────────────

inline godot::PackedInt64Array gd_attr_agg_int(const AttributeValue& av) {
    godot::PackedInt64Array result;
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_INT) {
        std::vector<int> v = av;
        result.resize(static_cast<int64_t>(v.size()));
        for (size_t i = 0; i < v.size(); ++i) {
            result[static_cast<int64_t>(i)] = static_cast<int64_t>(v[i]);
        }
    }
    return result;
}

inline godot::PackedFloat64Array gd_attr_agg_double(const AttributeValue& av) {
    godot::PackedFloat64Array result;
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_DOUBLE) {
        std::vector<double> v = av;
        result.resize(static_cast<int64_t>(v.size()));
        for (size_t i = 0; i < v.size(); ++i) {
            result[static_cast<int64_t>(i)] = v[i];
        }
    }
    return result;
}

inline godot::PackedStringArray gd_attr_agg_string(const AttributeValue& av) {
    godot::PackedStringArray result;
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_STRING) {
        std::vector<std::string> v = av;
        for (const auto& s : v) {
            result.push_back(godot::String(s.c_str()));
        }
    }
    return result;
}

inline godot::Array gd_attr_agg_entity(
    const AttributeValue& av,
    std::shared_ptr<IfcParse::IfcFile> file)
{
    godot::Array result;
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE) {
        boost::shared_ptr<aggregate_of_instance> agg = av;
        if (agg) {
            for (auto* inst : *agg) {
                result.push_back(godot::GDIFCEntityBase::wrap(inst, file));
            }
        }
    }
    return result;
}

/// Fallback for aggregate-of-aggregate and other complex types.
/// Returns an empty Variant for now; use get_attribute(name) for dynamic access.
inline godot::Variant gd_attr_variant(const AttributeValue& /*av*/) {
    return godot::Variant();
}

// ── Scalar setters ─────────────────────────────────────────────────────────

inline void gd_set_string(IfcUtil::IfcBaseClass* e, size_t idx, godot::String v) {
    e->set_attribute_value(idx, std::string(v.utf8().get_data()));
}

inline void gd_set_int(IfcUtil::IfcBaseClass* e, size_t idx, int64_t v) {
    e->set_attribute_value(idx, static_cast<int>(v));
}

inline void gd_set_double(IfcUtil::IfcBaseClass* e, size_t idx, double v) {
    e->set_attribute_value(idx, v);
}

inline void gd_set_bool(IfcUtil::IfcBaseClass* e, size_t idx, bool v) {
    e->set_attribute_value(idx, v);
}

// ── Entity-reference setter ───────────────────────────────────────────────

inline void gd_set_entity(IfcUtil::IfcBaseClass* e, size_t idx,
                          godot::Ref<godot::GDIFCEntityBase> v)
{
    if (v.is_valid() && v->entity_ != nullptr) {
        e->set_attribute_value(idx, v->entity_);
    } else {
        e->unset_attribute_value(idx);
    }
}

// ── Aggregate setters ──────────────────────────────────────────────────────

inline void gd_set_agg_int(IfcUtil::IfcBaseClass* e, size_t idx,
                            godot::PackedInt64Array v)
{
    std::vector<int> vec;
    vec.reserve(static_cast<size_t>(v.size()));
    for (int64_t i = 0; i < v.size(); ++i) {
        vec.push_back(static_cast<int>(v[i]));
    }
    e->set_attribute_value(idx, vec);
}

inline void gd_set_agg_double(IfcUtil::IfcBaseClass* e, size_t idx,
                               godot::PackedFloat64Array v)
{
    std::vector<double> vec;
    vec.reserve(static_cast<size_t>(v.size()));
    for (int64_t i = 0; i < v.size(); ++i) {
        vec.push_back(v[i]);
    }
    e->set_attribute_value(idx, vec);
}

inline void gd_set_agg_string(IfcUtil::IfcBaseClass* e, size_t idx,
                               godot::PackedStringArray v)
{
    std::vector<std::string> vec;
    vec.reserve(static_cast<size_t>(v.size()));
    for (int64_t i = 0; i < v.size(); ++i) {
        vec.push_back(std::string(v[i].utf8().get_data()));
    }
    e->set_attribute_value(idx, vec);
}

inline void gd_set_agg_entity(IfcUtil::IfcBaseClass* e, size_t idx,
                               godot::Array v)
{
    auto agg = boost::make_shared<aggregate_of_instance>();
    for (int64_t i = 0; i < v.size(); ++i) {
        const godot::Variant& item = v[i];
        auto* obj = godot::Object::cast_to<godot::GDIFCEntityBase>(
            item.operator godot::Object*());
        if (obj != nullptr && obj->entity_ != nullptr) {
            agg->push(obj->entity_);
        }
    }
    e->set_attribute_value(idx, agg);
}

// ── Generated registration declaration ────────────────────────────────────
void register_all_gd_ifc_entities();

#endif // GD_IFC_ENTITY_BASE_H
