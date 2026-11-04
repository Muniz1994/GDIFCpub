#include "gd_ifc_units.h"

#include "gd_ifc_entity_base.h"

#include <cctype>
#include <cstring>
#include <unordered_map>
#include <vector>

using namespace godot;

static IfcUtil::IfcBaseClass *unit_entity(IfcUtil::IfcBaseClass *entity, const char *name) {
    const IfcParse::entity *declaration = entity->declaration().as_entity();
    const ptrdiff_t index = declaration ? declaration->attribute_index(name) : -1;
    if (index < 0) return nullptr;
    const AttributeValue value = entity->get_attribute_value(index);
    return value.type() == IfcUtil::Argument_ENTITY_INSTANCE ? (IfcUtil::IfcBaseClass *)value : nullptr;
}

static std::vector<IfcUtil::IfcBaseClass *> unit_entities(IfcUtil::IfcBaseClass *entity, const char *name) {
    std::vector<IfcUtil::IfcBaseClass *> result;
    const IfcParse::entity *declaration = entity->declaration().as_entity();
    const ptrdiff_t index = declaration ? declaration->attribute_index(name) : -1;
    if (index < 0) return result;
    const AttributeValue value = entity->get_attribute_value(index);
    if (value.type() != IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE) return result;
    boost::shared_ptr<aggregate_of_instance> aggregate = value;
    if (aggregate) {
        for (IfcUtil::IfcBaseClass *item : *aggregate) {
            if (item) result.push_back(item);
        }
    }
    return result;
}

static String unit_string(IfcUtil::IfcBaseClass *entity, const char *name) {
    const IfcParse::entity *declaration = entity->declaration().as_entity();
    const ptrdiff_t index = declaration ? declaration->attribute_index(name) : -1;
    return index < 0 ? String() : gd_attr_string(entity->get_attribute_value(index));
}

static int64_t unit_int(IfcUtil::IfcBaseClass *entity, const char *name) {
    const IfcParse::entity *declaration = entity->declaration().as_entity();
    const ptrdiff_t index = declaration ? declaration->attribute_index(name) : -1;
    return index < 0 ? 0 : gd_attr_int(entity->get_attribute_value(index));
}

static String si_symbol(const String &prefix, const String &name) {
    static const std::unordered_map<std::string, const char *> PREFIXES = {
        {"EXA", "E"}, {"PETA", "P"}, {"TERA", "T"}, {"GIGA", "G"}, {"MEGA", "M"}, {"KILO", "k"},
        {"HECTO", "h"}, {"DECA", "da"}, {"DECI", "d"}, {"CENTI", "c"}, {"MILLI", "m"}, {"MICRO", "µ"},
        {"NANO", "n"}, {"PICO", "p"}, {"FEMTO", "f"}, {"ATTO", "a"},
    };
    static const std::unordered_map<std::string, const char *> NAMES = {
        {"METRE", "m"}, {"SQUARE_METRE", "m²"}, {"CUBIC_METRE", "m³"}, {"GRAM", "g"}, {"SECOND", "s"},
        {"AMPERE", "A"}, {"KELVIN", "K"}, {"MOLE", "mol"}, {"CANDELA", "cd"}, {"RADIAN", "rad"},
        {"STERADIAN", "sr"}, {"HERTZ", "Hz"}, {"NEWTON", "N"}, {"PASCAL", "Pa"}, {"JOULE", "J"},
        {"WATT", "W"}, {"COULOMB", "C"}, {"VOLT", "V"}, {"FARAD", "F"}, {"OHM", "Ω"}, {"SIEMENS", "S"},
        {"WEBER", "Wb"}, {"TESLA", "T"}, {"HENRY", "H"}, {"DEGREE_CELSIUS", "°C"}, {"LUMEN", "lm"},
        {"LUX", "lx"}, {"BECQUEREL", "Bq"}, {"GRAY", "Gy"}, {"SIEVERT", "Sv"},
    };
    const auto found_name = NAMES.find(name.utf8().get_data());
    if (found_name == NAMES.end()) return name;
    const auto found_prefix = PREFIXES.find(prefix.utf8().get_data());
    const String prefix_symbol = found_prefix == PREFIXES.end() ? String() : String::utf8(found_prefix->second);
    return prefix_symbol + String::utf8(found_name->second);
}

// Conversion based units are named by the exporter, so only the usual spellings get a symbol
static String conversion_symbol(const String &name) {
    static const std::unordered_map<std::string, const char *> SYMBOLS = {
        {"inch", "in"}, {"foot", "ft"}, {"yard", "yd"}, {"mile", "mi"},
        {"square inch", "in²"}, {"square foot", "ft²"}, {"square yard", "yd²"}, {"acre", "ac"},
        {"cubic inch", "in³"}, {"cubic foot", "ft³"}, {"cubic yard", "yd³"}, {"litre", "l"},
        {"ounce", "oz"}, {"pound", "lb"}, {"degree", "°"}, {"degree fahrenheit", "°F"},
    };
    const auto found = SYMBOLS.find(name.strip_edges().to_lower().utf8().get_data());
    return found == SYMBOLS.end() ? name : String::utf8(found->second);
}

static String with_exponent(const String &symbol, int64_t exponent) {
    if (exponent == 1) return symbol;
    if (exponent == 2) return symbol + String::utf8("²");
    if (exponent == 3) return symbol + String::utf8("³");
    return symbol + String("^") + String::num_int64(exponent);
}

// W, m^-2 and K^-1 -> "W/(m²·K)"
static String derived_symbol(IfcUtil::IfcBaseClass *unit) {
    PackedStringArray numerator, denominator;
    for (IfcUtil::IfcBaseClass *element : unit_entities(unit, "Elements")) {
        IfcUtil::IfcBaseClass *named = unit_entity(element, "Unit");
        const int64_t exponent = unit_int(element, "Exponent");
        const String symbol = named ? IfcUnitTable::symbol(named) : String();
        if (symbol.is_empty() || exponent == 0) return String();
        if (exponent > 0) {
            numerator.push_back(with_exponent(symbol, exponent));
        } else {
            denominator.push_back(with_exponent(symbol, -exponent));
        }
    }
    const String dot = String::utf8("·");
    String result = numerator.is_empty() ? String("1") : String(dot).join(numerator);
    if (denominator.size() == 1) {
        result += String("/") + denominator[0];
    } else if (denominator.size() > 1) {
        result += String("/(") + String(dot).join(denominator) + String(")");
    }
    return result;
}

String IfcUnitTable::symbol(IfcUtil::IfcBaseClass *unit) {
    if (!unit) return String();
    const IfcParse::declaration &type = unit->declaration();
    if (type.is("IfcSIUnit")) return si_symbol(unit_string(unit, "Prefix"), unit_string(unit, "Name"));
    if (type.is("IfcConversionBasedUnit")) return conversion_symbol(unit_string(unit, "Name"));
    if (type.is("IfcContextDependentUnit")) return unit_string(unit, "Name");
    if (type.is("IfcDerivedUnit")) return derived_symbol(unit);
    if (type.is("IfcMonetaryUnit")) return unit_string(unit, "Currency");
    return String();
}

IfcUnitTable::IfcUnitTable(IfcParse::IfcFile &file) {
    IfcUtil::IfcBaseClass *assignment = nullptr;
    try {
        aggregate_of_instance::ptr projects = file.instances_by_type("IfcProject");
        if (projects && projects->size() > 0) {
            assignment = unit_entity(*projects->begin(), "UnitsInContext");
        }
    } catch (...) {
        return;
    }
    if (!assignment) return;

    for (IfcUtil::IfcBaseClass *unit : unit_entities(assignment, "Units")) {
        const String symbol = IfcUnitTable::symbol(unit);
        if (symbol.is_empty()) continue;
        const std::string unit_type = unit->declaration().is("IfcMonetaryUnit")
            ? std::string("MONETARYUNIT")
            : std::string(unit_string(unit, "UnitType").utf8().get_data());
        if (unit_type.empty() || unit_type == "USERDEFINED") continue;
        by_type_.emplace(unit_type, symbol); // the first unit of a type wins
    }
}

// IfcLengthMeasure and IfcPositiveLengthMeasure -> LENGTHUNIT. The unit types are named after
// their measures, except for thermal conductivity.
String IfcUnitTable::for_measure(const std::string &value_type) const {
    if (by_type_.empty()) return String();
    std::string name = value_type;
    if (name.rfind("Ifc", 0) == 0) name = name.substr(3);
    const std::string suffix = "Measure";
    if (name.size() <= suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
        return String();
    }
    name = name.substr(0, name.size() - suffix.size());
    for (const char *qualifier : {"Positive", "NonNegative"}) {
        if (name.rfind(qualifier, 0) == 0) name = name.substr(std::strlen(qualifier));
    }
    for (char &c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    if (name == "THERMALCONDUCTIVITY") name = "THERMALCONDUCTANCE";

    const auto found = by_type_.find(name + "UNIT");
    return found == by_type_.end() ? String() : found->second;
}

Dictionary IfcUnitTable::to_dictionary() const {
    Dictionary result;
    for (const auto &entry : by_type_) {
        result[String(entry.first.c_str())] = entry.second;
    }
    return result;
}
