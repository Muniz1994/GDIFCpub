#ifndef GD_IFC_UNITS_H
#define GD_IFC_UNITS_H

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>

#include <IfcFile.h>

#include <map>
#include <string>

namespace godot {

// The units of an IFC file, from its IfcProject's IfcUnitAssignment, as symbols such as "mm", "m²"
// or "W/(m²·K)". Read by name, so one table covers every schema.
class IfcUnitTable {
public:
    IfcUnitTable() = default;
    explicit IfcUnitTable(IfcParse::IfcFile &file);

    /// The project's unit for a value type such as "IfcLengthMeasure", or empty when it has none
    String for_measure(const std::string &value_type) const;

    /// {unit type: symbol}, such as {"LENGTHUNIT": "mm", "MONETARYUNIT": "EUR"}
    Dictionary to_dictionary() const;

    /// Symbol of an IfcNamedUnit, IfcDerivedUnit or IfcMonetaryUnit, or empty for anything else
    static String symbol(IfcUtil::IfcBaseClass *unit);

private:
    std::map<std::string, String> by_type_;
};

} // namespace godot

#endif // GD_IFC_UNITS_H
