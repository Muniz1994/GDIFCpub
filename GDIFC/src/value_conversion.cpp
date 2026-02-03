#include "value_conversion.h"

// --- Main Conversion Function ---
Variant convert_attribute_value(const AttributeValue& val) {
    if (val.isNull()) {
        return Variant();
    }
    return val.apply_visitor(AttributeToVariantVisitor());
}