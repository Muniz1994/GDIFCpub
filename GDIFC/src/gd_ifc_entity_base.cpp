#include "gd_ifc_entity_base.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <ArgumentType.h>
#include <IfcSchema.h>

#include <algorithm>
#include <cmath>

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
    return godot::String::utf8(entity_->declaration().name().c_str());
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

static godot::Variant gdifc_av_to_variant(const AttributeValue& av, std::shared_ptr<IfcParse::IfcFile> file);

// An entity as its wrapper, and an instance of a defined type, such as an IfcLabel in an IfcValue
// select, as its plain value
static godot::Variant instance_to_variant(IfcUtil::IfcBaseClass* inst, const std::shared_ptr<IfcParse::IfcFile>& file) {
    if (inst == nullptr) return godot::Variant();
    if (inst->declaration().as_type_declaration() != nullptr) {
        return gdifc_av_to_variant(inst->get_attribute_value(0), file);
    }
    return godot::GDIFCEntityBase::wrap(inst, file);
}

/// Convert an AttributeValue to a Variant.  Entity references are returned as
/// Ref<GDIFCEntityBase> objects (wrapping the underlying IfcBaseClass*), and instances of
/// defined types as their value. For aggregate-of-entity, returns an Array of them.
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
            return godot::String::utf8(v.c_str());
        }
        godot::Variant operator()(const boost::dynamic_bitset<>& v) const {
            return bits(v);
        }
        static godot::String bits(const boost::dynamic_bitset<>& v) {
            std::string s;
            boost::to_string(v, s);
            return godot::String(s.c_str());
        }
        godot::Variant operator()(EnumerationReference v) const {
            return godot::String(v.value());
        }
        godot::Variant operator()(IfcUtil::IfcBaseClass* v) const {
            return instance_to_variant(v, file);
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
            for (const auto& s : v) arr.push_back(godot::String::utf8(s.c_str()));
            return arr;
        }
        godot::Variant operator()(const std::vector<boost::dynamic_bitset<>>& v) const {
            godot::PackedStringArray arr;
            for (const auto& b : v) arr.push_back(bits(b));
            return arr;
        }
        godot::Variant operator()(const aggregate_of_instance::ptr& agg) const {
            godot::Array arr;
            if (agg) {
                for (auto* inst : *agg) {
                    arr.push_back(instance_to_variant(inst, file));
                }
            }
            return arr;
        }
        godot::Variant operator()(const std::vector<std::vector<int>>& v) const {
            godot::Array outer;
            for (const auto& inner : v) outer.push_back((*this)(inner));
            return outer;
        }
        godot::Variant operator()(const std::vector<std::vector<double>>& v) const {
            godot::Array outer;
            for (const auto& inner : v) outer.push_back((*this)(inner));
            return outer;
        }
        godot::Variant operator()(const aggregate_of_aggregate_of_instance::ptr& v) const {
            godot::Array outer;
            if (v) {
                for (const auto& inner_vec : *v) {
                    godot::Array inner;
                    for (auto* inst : inner_vec) {
                        inner.push_back(instance_to_variant(inst, file));
                    }
                    outer.push_back(inner);
                }
            }
            return outer;
        }
    };
    return av.apply_visitor(Visitor(std::move(file)));
}

godot::Variant gd_attr_to_variant(const AttributeValue& av, std::shared_ptr<IfcParse::IfcFile> file) {
    return gdifc_av_to_variant(av, std::move(file));
}

// ── Lookup and checked setting, for the generated classes ─────────────────

ptrdiff_t gd_attr_index(const IfcUtil::IfcBaseClass* e, const char* name,
                        const GDIFCAttributeAlias* aliases) {
    if (e == nullptr) return -1;
    const IfcParse::entity* decl = e->declaration().as_entity();
    if (decl == nullptr) return -1;
    const ptrdiff_t index = decl->attribute_index(name);
    if (index >= 0 || aliases == nullptr || decl->schema() == nullptr) return index;
    const std::string& schema = decl->schema()->name();
    for (; aliases->name != nullptr; ++aliases) {
        // Upper case, so is() doesn't go through its shared temporary string
        if (schema == aliases->schema && decl->is(std::string(aliases->entity_uc))) {
            const ptrdiff_t alias_index = decl->attribute_index(aliases->name);
            if (alias_index >= 0) return alias_index;
        }
    }
    return -1;
}

namespace {

// Whether an entity of `actual` fits where `declared` is expected: the entity type itself, or a
// select that includes it
bool accepts(const IfcParse::declaration* declared, const IfcParse::declaration& actual) {
    if (declared == nullptr) return false;
    if (declared == &actual) return true;
    if (declared->as_entity() != nullptr) return actual.is(*declared);
    if (const auto* select = declared->as_select_type()) {
        for (const auto* option : select->select_list()) {
            if (accepts(option, actual)) return true;
        }
    }
    return false;
}

const IfcParse::declaration* declared_type(const IfcParse::parameter_type* type) {
    const auto* named = type != nullptr ? type->as_named_type() : nullptr;
    return named != nullptr ? named->declared_type() : nullptr;
}

const IfcParse::parameter_type* element_type(const IfcParse::parameter_type* type) {
    const auto* aggregate = type != nullptr ? type->as_aggregation_type() : nullptr;
    return aggregate != nullptr ? aggregate->type_of_element() : nullptr;
}

// The IFC entity a Variant holds, if any
IfcUtil::IfcBaseClass* entity_of(const godot::Variant& v) {
    if (v.get_type() != godot::Variant::OBJECT) return nullptr;
    auto* obj = godot::Object::cast_to<godot::GDIFCEntityBase>(v.operator godot::Object*());
    return obj != nullptr ? obj->entity_ : nullptr;
}

bool is_number(const godot::Variant& v) {
    return v.get_type() == godot::Variant::INT || v.get_type() == godot::Variant::FLOAT;
}

// A whole number, from an int or a float without a fractional part
bool to_int(const godot::Variant& v, int& out) {
    if (v.get_type() == godot::Variant::INT) {
        out = static_cast<int>(static_cast<int64_t>(v));
        return true;
    }
    if (v.get_type() == godot::Variant::FLOAT) {
        const double d = v;
        if (d != std::floor(d)) return false;
        out = static_cast<int>(d);
        return true;
    }
    return false;
}

bool is_string(const godot::Variant& v) {
    return v.get_type() == godot::Variant::STRING || v.get_type() == godot::Variant::STRING_NAME;
}

// A String or StringName. A Variant only converts to the type it holds.
godot::String as_string(const godot::Variant& v) {
    return v.get_type() == godot::Variant::STRING_NAME ? godot::String(godot::StringName(v)) : godot::String(v);
}

std::string utf8(const godot::Variant& v) {
    return std::string(as_string(v).utf8().get_data());
}

// Strings of 0s and 1s, as gd_attr_string returns them
bool to_bits(const godot::Variant& v, boost::dynamic_bitset<>& out) {
    if (!is_string(v)) return false;
    const std::string s = utf8(v);
    if (s.find_first_not_of("01") != std::string::npos) return false;
    out = boost::dynamic_bitset<>(s);
    return true;
}

// The elements of an Array or packed array, as an Array
bool elements(const godot::Variant& v, godot::Array& out) {
    switch (v.get_type()) {
        case godot::Variant::ARRAY: out = godot::Array(v); return true;
        case godot::Variant::PACKED_INT32_ARRAY: out = godot::Array(godot::PackedInt32Array(v)); return true;
        case godot::Variant::PACKED_INT64_ARRAY: out = godot::Array(godot::PackedInt64Array(v)); return true;
        case godot::Variant::PACKED_FLOAT32_ARRAY: out = godot::Array(godot::PackedFloat32Array(v)); return true;
        case godot::Variant::PACKED_FLOAT64_ARRAY: out = godot::Array(godot::PackedFloat64Array(v)); return true;
        case godot::Variant::PACKED_STRING_ARRAY: out = godot::Array(godot::PackedStringArray(v)); return true;
        default: return false;
    }
}

template <typename T, typename Convert>
bool to_vector(const godot::Variant& v, std::vector<T>& out, Convert convert) {
    godot::Array items;
    if (!elements(v, items)) return false;
    out.clear();
    out.reserve(static_cast<size_t>(items.size()));
    for (int64_t i = 0; i < items.size(); ++i) {
        T item;
        if (!convert(items[i], item)) return false;
        out.push_back(std::move(item));
    }
    return true;
}

bool to_double(const godot::Variant& v, double& out) {
    if (!is_number(v)) return false;
    out = v;
    return true;
}

bool to_std_string(const godot::Variant& v, std::string& out) {
    if (!is_string(v)) return false;
    out = utf8(v);
    return true;
}

bool set_converted(IfcUtil::IfcBaseClass* e, size_t index, const IfcParse::parameter_type* type,
                   const godot::Variant& value);

// The defined types a select includes, directly or through the selects it includes
void value_types(const IfcParse::declaration* declared, std::vector<const IfcParse::type_declaration*>& out) {
    if (declared == nullptr) return;
    if (const auto* type = declared->as_type_declaration()) {
        out.push_back(type);
    } else if (const auto* select = declared->as_select_type()) {
        for (const auto* option : select->select_list()) value_types(option, out);
    }
}

// An instance of a defined type, such as an IfcLabel, holding `value`. Null when it doesn't fit.
IfcUtil::IfcBaseClass* make_value(const IfcParse::type_declaration* type, const godot::Variant& value) {
    const IfcParse::schema_definition* schema = type->schema();
    if (schema == nullptr) return nullptr;
    IfcUtil::IfcBaseClass* instance = schema->instantiate(type, IfcEntityInstanceData(in_memory_attribute_storage(1)));
    if (instance == nullptr) return nullptr;
    bool fits = false;
    try {
        fits = set_converted(instance, 0, type->declared_type(), value);
    } catch (const std::exception&) {
    }
    if (!fits) {
        delete instance;
        return nullptr;
    }
    return instance;
}

// A plain value for a select of defined types, such as IfcValue: of the type of the value it
// replaces when it fits, otherwise IfcLabel, IfcInteger, IfcReal or IfcBoolean after its Variant
// type, when the select includes it
IfcUtil::IfcBaseClass* value_for(const IfcParse::declaration* declared, const IfcUtil::IfcBaseClass* current,
                                 const godot::Variant& value) {
    std::vector<const IfcParse::type_declaration*> types;
    value_types(declared, types);
    if (types.empty()) return nullptr;
    if (current != nullptr) {
        const auto* type = current->declaration().as_type_declaration();
        if (type != nullptr && std::find(types.begin(), types.end(), type) != types.end()) {
            if (IfcUtil::IfcBaseClass* instance = make_value(type, value)) return instance;
        }
    }
    const char* preferred = nullptr;
    switch (value.get_type()) {
        case godot::Variant::STRING:
        case godot::Variant::STRING_NAME: preferred = "IfcLabel"; break;
        case godot::Variant::INT: preferred = "IfcInteger"; break;
        case godot::Variant::FLOAT: preferred = "IfcReal"; break;
        case godot::Variant::BOOL: preferred = "IfcBoolean"; break;
        default: return nullptr;
    }
    for (const auto* type : types) {
        if (type->name() == preferred) return make_value(type, value);
    }
    return nullptr;
}

// Entities of the same file that fit the declared type, for an aggregate. Plain values become
// instances of defined types when the declared type is a select of them.
bool to_instances(const godot::Variant& v, const IfcParse::parameter_type* type, const IfcParse::IfcFile* file,
                  aggregate_of_instance::ptr& out) {
    godot::Array items;
    if (!elements(v, items)) return false;
    out = boost::make_shared<aggregate_of_instance>();
    const IfcParse::declaration* declared = declared_type(type);
    for (int64_t i = 0; i < items.size(); ++i) {
        IfcUtil::IfcBaseClass* item = entity_of(items[i]);
        if (item == nullptr) {
            if (items[i].get_type() == godot::Variant::OBJECT) return false;
            item = value_for(declared, nullptr, items[i]);
            if (item == nullptr) return false;
        } else if (item->file_ != file || !accepts(declared, item->declaration())) {
            return false;
        }
        out->push(item);
    }
    return true;
}

// Converts `value` to the type the attribute declares and sets it. False when it doesn't fit.
bool set_converted(IfcUtil::IfcBaseClass* e, size_t index, const IfcParse::parameter_type* type,
                   const godot::Variant& value) {
    switch (IfcUtil::from_parameter_type(type)) {
        case IfcUtil::Argument_INT: {
            int v;
            if (!to_int(value, v)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_DOUBLE: {
            double v;
            if (!to_double(value, v)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_BOOL:
            if (value.get_type() != godot::Variant::BOOL) return false;
            e->set_attribute_value(index, static_cast<bool>(value));
            return true;
        case IfcUtil::Argument_LOGICAL: {
            boost::logic::tribool v;
            if (value.get_type() == godot::Variant::BOOL) {
                v = static_cast<bool>(value);
            } else if (is_string(value)) {
                const godot::String s = as_string(value).to_upper();
                if (s == "TRUE" || s == "T") v = true;
                else if (s == "FALSE" || s == "F") v = false;
                else if (s == "UNKNOWN" || s == "U") v = boost::logic::indeterminate;
                else return false;
            } else {
                return false;
            }
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_STRING: {
            std::string v;
            if (!to_std_string(value, v)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_BINARY: {
            boost::dynamic_bitset<> v;
            if (!to_bits(value, v)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_ENUMERATION: {
            const IfcParse::declaration* declared = declared_type(type);
            const auto* enumeration = declared != nullptr ? declared->as_enumeration_type() : nullptr;
            if (enumeration == nullptr || !is_string(value)) return false;
            const std::string item = utf8(as_string(value).to_upper());
            const auto& items = enumeration->enumeration_items();
            const auto it = std::find(items.begin(), items.end(), item);
            if (it == items.end()) return false;
            e->set_attribute_value(index, EnumerationReference(enumeration, std::distance(items.begin(), it)));
            return true;
        }
        case IfcUtil::Argument_ENTITY_INSTANCE: {
            IfcUtil::IfcBaseClass* v = entity_of(value);
            if (v == nullptr) {
                // A plain value, for a select of defined types such as IfcValue
                if (value.get_type() == godot::Variant::OBJECT) return false;
                const AttributeValue current = e->get_attribute_value(index);
                v = value_for(declared_type(type), current.type() == IfcUtil::Argument_ENTITY_INSTANCE
                    ? static_cast<IfcUtil::IfcBaseClass*>(current) : nullptr, value);
                if (v == nullptr) return false;
            } else if (v->file_ != e->file_ || !accepts(declared_type(type), v->declaration())) {
                return false;
            }
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_INT: {
            std::vector<int> v;
            if (!to_vector(value, v, to_int)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_DOUBLE: {
            std::vector<double> v;
            if (!to_vector(value, v, to_double)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_STRING: {
            std::vector<std::string> v;
            if (!to_vector(value, v, to_std_string)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_BINARY: {
            std::vector<boost::dynamic_bitset<>> v;
            if (!to_vector(value, v, to_bits)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE: {
            aggregate_of_instance::ptr v;
            if (!to_instances(value, element_type(type), e->file_, v)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_INT: {
            std::vector<std::vector<int>> v;
            auto inner = [](const godot::Variant& item, std::vector<int>& out) { return to_vector(item, out, to_int); };
            if (!to_vector(value, v, inner)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_DOUBLE: {
            std::vector<std::vector<double>> v;
            auto inner = [](const godot::Variant& item, std::vector<double>& out) { return to_vector(item, out, to_double); };
            if (!to_vector(value, v, inner)) return false;
            e->set_attribute_value(index, v);
            return true;
        }
        case IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE: {
            const IfcParse::parameter_type* inner_type = element_type(element_type(type));
            godot::Array rows;
            if (!elements(value, rows)) return false;
            auto v = boost::make_shared<aggregate_of_aggregate_of_instance>();
            for (int64_t i = 0; i < rows.size(); ++i) {
                aggregate_of_instance::ptr row;
                if (!to_instances(rows[i], inner_type, e->file_, row)) return false;
                v->push(row);
            }
            e->set_attribute_value(index, v);
            return true;
        }
        default:
            return false;
    }
}

godot::String schema_name(const IfcParse::entity& decl) {
    return decl.schema() != nullptr ? godot::String(decl.schema()->name().c_str()) : godot::String("its schema");
}

// Whether a select includes anything but entities
bool select_holds_values(const IfcParse::declaration* declared) {
    if (declared->as_entity() != nullptr) return false;
    if (const auto* select = declared->as_select_type()) {
        return std::any_of(select->select_list().begin(), select->select_list().end(), select_holds_values);
    }
    return true;
}

} // namespace

bool gd_holds_values(const IfcParse::parameter_type* type) {
    if (type == nullptr) return false;
    if (const auto* aggregate = type->as_aggregation_type()) return gd_holds_values(aggregate->type_of_element());
    const IfcParse::declaration* declared = declared_type(type);
    return declared == nullptr || select_holds_values(declared);
}

godot::Variant gd_get_attribute(const IfcUtil::IfcBaseClass* e, const char* name,
                                std::shared_ptr<IfcParse::IfcFile> file, const GDIFCAttributeAlias* aliases) {
    const ptrdiff_t index = gd_attr_index(e, name, aliases);
    if (index < 0) return godot::Variant();
    const AttributeValue av = e->get_attribute_value(static_cast<size_t>(index));
    const IfcUtil::ArgumentType held = av.type();
    if (held == IfcUtil::Argument_NULL || held == IfcUtil::Argument_DERIVED) return godot::Variant();

    // The type the entity's own schema declares, which may differ from the class's
    const IfcParse::entity* decl = e->declaration().as_entity();
    switch (IfcUtil::from_parameter_type(decl->attribute_by_index(static_cast<size_t>(index))->type_of_attribute())) {
        case IfcUtil::Argument_INT: return gd_attr_int(av);
        case IfcUtil::Argument_DOUBLE: return gd_attr_double(av);
        case IfcUtil::Argument_BOOL: return gd_attr_bool(av);
        case IfcUtil::Argument_LOGICAL:
        case IfcUtil::Argument_BINARY:
        case IfcUtil::Argument_STRING:
        case IfcUtil::Argument_ENUMERATION: return gd_attr_string(av);
        case IfcUtil::Argument_AGGREGATE_OF_INT: return gd_attr_agg_int(av);
        case IfcUtil::Argument_AGGREGATE_OF_DOUBLE: return gd_attr_agg_double(av);
        case IfcUtil::Argument_AGGREGATE_OF_STRING: return gd_attr_agg_string(av);
        default: return gdifc_av_to_variant(av, std::move(file));
    }
}

void gd_set_attribute(IfcUtil::IfcBaseClass* e, const char* name, const godot::Variant& value,
                      const GDIFCAttributeAlias* aliases) {
    if (e == nullptr) return;
    const IfcParse::entity* decl = e->declaration().as_entity();
    if (decl == nullptr) return;
    const godot::String type = decl->name().c_str();

    const ptrdiff_t index = gd_attr_index(e, name, aliases);
    if (index < 0) {
        godot::UtilityFunctions::push_error(godot::vformat("%s has no attribute %s in %s.", type, name, schema_name(*decl)));
        return;
    }
    if (decl->derived()[index]) {
        godot::UtilityFunctions::push_error(godot::vformat("%s.%s is derived in %s, so it can't be set.", type, name, schema_name(*decl)));
        return;
    }
    try {
        if (value.get_type() == godot::Variant::NIL ||
            (value.get_type() == godot::Variant::OBJECT && value.operator godot::Object*() == nullptr)) {
            e->set_attribute_value(index, Blank{});
            return;
        }
        if (!set_converted(e, index, decl->attribute_by_index(index)->type_of_attribute(), value)) {
            godot::UtilityFunctions::push_error(godot::vformat("Can't set %s.%s to %s: it doesn't fit the attribute's type in %s.",
                                                               type, name, value.stringify(), schema_name(*decl)));
        }
    } catch (const std::exception& ex) {
        godot::UtilityFunctions::push_error(godot::vformat("Can't set %s.%s: %s", type, name, ex.what()));
    }
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
        result.push_back(godot::String::utf8(attr->name().c_str()));
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
        auto key = godot::String::utf8(all_attrs[i]->name().c_str());
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
