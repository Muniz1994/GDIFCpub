#include "gd_ifc_entity_base.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <IfcSchema.h>

using namespace godot;

// ── Static factory registry ───────────────────────────────────────────────

std::unordered_map<std::string, GDIFCEntityBase::FactoryFn>&
GDIFCEntityBase::factory_map() {
    static std::unordered_map<std::string, FactoryFn> map;
    return map;
}

void GDIFCEntityBase::register_factory(const std::string& ifc_type_name, FactoryFn fn) {
    factory_map()[ifc_type_name] = std::move(fn);
}

Ref<GDIFCEntityBase> GDIFCEntityBase::wrap(IfcUtil::IfcBaseClass* entity,
                                            std::shared_ptr<IfcParse::IfcFile> file) {
    if (entity == nullptr) {
        return {};
    }
    const std::string& type_name = entity->declaration().name();
    auto& fmap = factory_map();
    auto it = fmap.find(type_name);
    if (it != fmap.end()) {
        return it->second(entity, file);
    }
    // Fallback: plain base
    Ref<GDIFCEntityBase> obj;
    obj.instantiate();
    obj->init(entity, file);
    return obj;
}

// ── Lifecycle ─────────────────────────────────────────────────────────────

void GDIFCEntityBase::init(IfcUtil::IfcBaseClass* entity,
                            std::shared_ptr<IfcParse::IfcFile> file) {
    entity_ = entity;
    file_ = std::move(file);
}

// ── GDScript API implementation ───────────────────────────────────────────

int64_t GDIFCEntityBase::get_express_id() const {
    if (!entity_) return -1;
    return static_cast<int64_t>(entity_->id());
}

godot::String GDIFCEntityBase::get_type() const {
    if (!entity_) return godot::String();
    return godot::String(entity_->declaration().name().c_str());
}

bool GDIFCEntityBase::is_a(godot::String ifc_class) const {
    if (!entity_) return false;
    std::string name = ifc_class.utf8().get_data();
    const IfcParse::declaration* decl = &entity_->declaration();
    while (decl != nullptr) {
        if (decl->name() == name) return true;
        if (const auto* ent = decl->as_entity()) {
            decl = ent->supertype();
        } else {
            break;
        }
    }
    return false;
}

// ── Attribute access ──────────────────────────────────────────────────────

/// Convert an AttributeValue to a Variant.  Entity references are returned as
/// Ref<GDIFCEntityBase> objects (wrapping the underlying IfcBaseClass*).
/// For aggregate-of-entity, returns an Array of GDIFCEntityBase.
/// Falls back to simpler scalar conversions for plain types.
static godot::Variant gdifc_av_to_variant(const AttributeValue& av,
                                           std::shared_ptr<IfcParse::IfcFile> file) {
    // AttributeValue is NOT a boost::variant. Use av.apply_visitor() which
    // dispatches to the correct type via its own switch on av.type().
    struct Visitor {
        std::shared_ptr<IfcParse::IfcFile> file;
        explicit Visitor(std::shared_ptr<IfcParse::IfcFile> f) : file(std::move(f)) {}

        godot::Variant operator()(Blank) const { return godot::Variant(); }
        godot::Variant operator()(Derived) const { return godot::Variant(); }
        godot::Variant operator()(empty_aggregate_t) const { return godot::Array(); }
        godot::Variant operator()(empty_aggregate_of_aggregate_t) const { return godot::Array(); }
        godot::Variant operator()(int v) const { return godot::Variant(static_cast<int64_t>(v)); }
        godot::Variant operator()(bool v) const { return godot::Variant(v); }
        godot::Variant operator()(boost::logic::tribool v) const {
            if (boost::logic::indeterminate(v)) return godot::String("UNKNOWN");
            return godot::Variant(static_cast<bool>(v));
        }
        godot::Variant operator()(double v) const { return godot::Variant(v); }
        godot::Variant operator()(const std::string& v) const {
            return godot::String(v.c_str());
        }
        godot::Variant operator()(const boost::dynamic_bitset<>& v) const {
            return godot::String(std::to_string(v.size()).c_str());
        }
        godot::Variant operator()(EnumerationReference v) const {
            return godot::String(v.value());
        }
        godot::Variant operator()(IfcUtil::IfcBaseClass* v) const {
            if (v == nullptr) return godot::Variant();
            return godot::GDIFCEntityBase::wrap(v, file);
        }
        godot::Variant operator()(const std::vector<int>& v) const {
            godot::PackedInt64Array arr;
            arr.resize(static_cast<int64_t>(v.size()));
            for (size_t i = 0; i < v.size(); ++i) arr[static_cast<int64_t>(i)] = v[i];
            return arr;
        }
        godot::Variant operator()(const std::vector<double>& v) const {
            godot::PackedFloat64Array arr;
            arr.resize(static_cast<int64_t>(v.size()));
            for (size_t i = 0; i < v.size(); ++i) arr[static_cast<int64_t>(i)] = v[i];
            return arr;
        }
        godot::Variant operator()(const std::vector<std::string>& v) const {
            godot::PackedStringArray arr;
            for (const auto& s : v) arr.push_back(godot::String(s.c_str()));
            return arr;
        }
        godot::Variant operator()(const std::vector<boost::dynamic_bitset<>>&) const {
            return godot::Array();
        }
        godot::Variant operator()(const aggregate_of_instance::ptr& agg) const {
            godot::Array arr;
            if (agg) {
                for (auto* inst : *agg) {
                    arr.push_back(godot::GDIFCEntityBase::wrap(inst, file));
                }
            }
            return arr;
        }
        godot::Variant operator()(const std::vector<std::vector<int>>&) const {
            return godot::Array();
        }
        godot::Variant operator()(const std::vector<std::vector<double>>&) const {
            return godot::Array();
        }
        godot::Variant operator()(const aggregate_of_aggregate_of_instance::ptr& v) const {
            godot::Array outer;
            if (v) {
                for (const auto& inner_vec : *v) {
                    godot::Array inner;
                    for (auto* inst : inner_vec) {
                        inner.push_back(godot::GDIFCEntityBase::wrap(inst, file));
                    }
                    outer.push_back(inner);
                }
            }
            return outer;
        }
    };
    return av.apply_visitor(Visitor(std::move(file)));
}

godot::Variant GDIFCEntityBase::get_attribute(godot::String attr_name) {
    if (!entity_) return godot::Variant();
    const auto* ent_decl = entity_->declaration().as_entity();
    if (!ent_decl) return godot::Variant();

    std::string name = attr_name.utf8().get_data();
    const auto& all_attrs = ent_decl->all_attributes();
    for (size_t i = 0; i < all_attrs.size(); ++i) {
        if (all_attrs[i]->name() == name) {
            return gdifc_av_to_variant(entity_->get_attribute_value(i), file_);
        }
    }
    return godot::Variant();
}

godot::PackedStringArray GDIFCEntityBase::get_attribute_names() {
    godot::PackedStringArray result;
    if (!entity_) return result;
    const auto* ent_decl = entity_->declaration().as_entity();
    if (!ent_decl) return result;
    for (const auto* attr : ent_decl->all_attributes()) {
        result.push_back(godot::String(attr->name().c_str()));
    }
    return result;
}

godot::Dictionary GDIFCEntityBase::get_all_attributes() {
    godot::Dictionary result;
    if (!entity_) return result;
    const auto* ent_decl = entity_->declaration().as_entity();
    if (!ent_decl) return result;
    const auto& all_attrs = ent_decl->all_attributes();
    for (size_t i = 0; i < all_attrs.size(); ++i) {
        auto key = godot::String(all_attrs[i]->name().c_str());
        result[key] = gdifc_av_to_variant(entity_->get_attribute_value(i), file_);
    }
    return result;
}

godot::Array GDIFCEntityBase::get_inverse(godot::String inverse_attr_name) {
    godot::Array result;
    if (!entity_ || !file_) return result;

    const auto* ent_decl = entity_->declaration().as_entity();
    if (!ent_decl) return result;

    std::string name = inverse_attr_name.utf8().get_data();
    const auto& inv_attrs = ent_decl->all_inverse_attributes();
    for (const auto* inv : inv_attrs) {
        if (inv->name() == name) {
            const auto* ref_decl = inv->entity_reference();
            const auto* fwd_attr = inv->attribute_reference();
            ptrdiff_t attr_idx = ref_decl->attribute_index(fwd_attr);
            if (attr_idx < 0) return result;
            auto instances = file_->getInverse(
                static_cast<int>(entity_->id()),
                ref_decl,
                static_cast<int>(attr_idx));
            if (instances) {
                for (auto* inst : *instances) {
                    result.push_back(GDIFCEntityBase::wrap(inst, file_));
                }
            }
            return result;
        }
    }
    return result;
}

// ── Bind methods ──────────────────────────────────────────────────────────

void GDIFCEntityBase::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_express_id"), &GDIFCEntityBase::get_express_id);
    ClassDB::bind_method(D_METHOD("get_type"), &GDIFCEntityBase::get_type);
    ClassDB::bind_method(D_METHOD("is_a", "ifc_class"), &GDIFCEntityBase::is_a);
    ClassDB::bind_method(D_METHOD("get_attribute", "attr_name"),
                         &GDIFCEntityBase::get_attribute);
    ClassDB::bind_method(D_METHOD("get_attribute_names"), &GDIFCEntityBase::get_attribute_names);
    ClassDB::bind_method(D_METHOD("get_all_attributes"), &GDIFCEntityBase::get_all_attributes);
    ClassDB::bind_method(D_METHOD("get_inverse", "inverse_attr_name"),
                         &GDIFCEntityBase::get_inverse);
}
