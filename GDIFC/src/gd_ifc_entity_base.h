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
//  gd_attr_* convert an attribute value read from an entity; gd_attr_index finds where the
//  entity holds an attribute, and gd_set_attribute sets one.
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
        return godot::String::utf8(((std::string)av).c_str());
    }
    if (t == IfcUtil::Argument_ENUMERATION) {
        EnumerationReference ref = av;
        return godot::String(ref.value());
    }
    // As STEP spells them
    if (t == IfcUtil::Argument_LOGICAL) {
        boost::logic::tribool tb = av;
        return boost::logic::indeterminate(tb) ? "UNKNOWN" : (tb ? "TRUE" : "FALSE");
    }
    if (t == IfcUtil::Argument_BOOL) {
        return (bool)av ? "TRUE" : "FALSE";
    }
    if (t == IfcUtil::Argument_BINARY) {
        boost::dynamic_bitset<> bits = av;
        std::string s;
        boost::to_string(bits, s);
        return godot::String(s.c_str());
    }
    return godot::String();
}

// Numbers of either kind: files write reals without a decimal point, which parse as integers,
// and some write integers, such as timestamps, as reals

inline int64_t gd_attr_int(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_INT) {
        return static_cast<int64_t>((int)av);
    }
    if (av.type() == IfcUtil::Argument_DOUBLE) {
        return static_cast<int64_t>((double)av);
    }
    return 0;
}

inline double gd_attr_double(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_DOUBLE) {
        return (double)av;
    }
    if (av.type() == IfcUtil::Argument_INT) {
        return static_cast<double>((int)av);
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

template <typename Packed, typename From>
Packed gd_packed_from(const std::vector<From>& v) {
    Packed result;
    result.resize(static_cast<int64_t>(v.size()));
    for (size_t i = 0; i < v.size(); ++i) {
        result[static_cast<int64_t>(i)] = static_cast<std::decay_t<decltype(result[0])>>(v[i]);
    }
    return result;
}

inline godot::PackedInt64Array gd_attr_agg_int(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_INT) {
        return gd_packed_from<godot::PackedInt64Array>((std::vector<int>)av);
    }
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_DOUBLE) {
        return gd_packed_from<godot::PackedInt64Array>((std::vector<double>)av);
    }
    return godot::PackedInt64Array();
}

inline godot::PackedFloat64Array gd_attr_agg_double(const AttributeValue& av) {
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_DOUBLE) {
        return gd_packed_from<godot::PackedFloat64Array>((std::vector<double>)av);
    }
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_INT) {
        return gd_packed_from<godot::PackedFloat64Array>((std::vector<int>)av);
    }
    return godot::PackedFloat64Array();
}

inline godot::PackedStringArray gd_attr_agg_string(const AttributeValue& av) {
    godot::PackedStringArray result;
    if (av.type() == IfcUtil::Argument_AGGREGATE_OF_STRING) {
        std::vector<std::string> v = av;
        for (const auto& s : v) {
            result.push_back(godot::String::utf8(s.c_str()));
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

/// Any attribute value, as get_attribute() returns it. Used for nested aggregates and for
/// attributes whose type differs between schemas.
godot::Variant gd_attr_to_variant(const AttributeValue& av, std::shared_ptr<IfcParse::IfcFile> file);

/// Aggregates of aggregates, as an Array of arrays
inline godot::Array gd_attr_array(const AttributeValue& av, std::shared_ptr<IfcParse::IfcFile> file) {
    const godot::Variant v = gd_attr_to_variant(av, std::move(file));
    return v.get_type() == godot::Variant::ARRAY ? godot::Array(v) : godot::Array();
}

// ── Attribute lookup ───────────────────────────────────────────────────────
//
//  The generated classes cover IFC2X3, IFC4 and IFC4X3 together, so an attribute of a class can
//  be missing from the schema of a given file, or sit at another position in it. Attributes are
//  looked up by name in the entity's own schema: a missing one reads as empty and can't be set.

/// An attribute IFC4X3 renamed: in files of `schema`, entities of `entity_uc` (upper case) and
/// its subtypes hold it under `name`. Lists of aliases end with an entry whose name is null.
struct GDIFCAttributeAlias {
    const char* schema;
    const char* entity_uc;
    const char* name;
};

/// Position of the attribute in the entity's own schema, or -1 when that schema doesn't have it
ptrdiff_t gd_attr_index(const IfcUtil::IfcBaseClass* e, const char* name,
                        const GDIFCAttributeAlias* aliases = nullptr);

/// Sets the attribute, converted to the type its schema declares. Reports an error and leaves the
/// entity unchanged when its schema lacks the attribute, the attribute is derived, or the value
/// doesn't fit. Null unsets it.
void gd_set_attribute(IfcUtil::IfcBaseClass* e, const char* name, const godot::Variant& value,
                      const GDIFCAttributeAlias* aliases = nullptr);

// ── Generated registration declaration ────────────────────────────────────
void register_all_gd_ifc_entities();

#endif // GD_IFC_ENTITY_BASE_H
