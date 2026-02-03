//
// Created by engbr on 03/02/2026.
//

#ifndef GODOTEXTENSION_VALUE_CONVERSION_H
#define GODOTEXTENSION_VALUE_CONVERSION_H

#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp> // Added for string conversion
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <boost/logic/tribool.hpp>
#include <boost/dynamic_bitset.hpp>
#include <IfcEntityInstanceData.h>
#include <IfcBaseClass.h>

using namespace godot;

// Visitor struct to handle conversion for every type in AttributeValue
struct AttributeToVariantVisitor {

    // --- 1. Null / Blank / Derived ---
    Variant operator()(Blank) const {
        return Variant(); // NIL
    }
    Variant operator()(Derived) const {
        return Variant(); // NIL (typically represented as null in Godot for logic checks)
    }

    // --- 2. Simple Atomic Types ---
    Variant operator()(int v) const {
        return Variant((int64_t)v);
    }
    Variant operator()(bool v) const {
        return Variant(v);
    }
    Variant operator()(double v) const {
        return Variant(v);
    }
    Variant operator()(const std::string& v) const {
        return Variant(String(v.c_str()));
    }

    // --- 3. Logic & Binary ---
    Variant operator()(boost::logic::tribool v) const {
        if (boost::logic::indeterminate(v)) {
            return Variant(); // NIL represents the 'U' (Unknown) state
        }
        return Variant((bool)v);
    }

    Variant operator()(const boost::dynamic_bitset<>& v) const {
        // Godot doesn't have a BitSet, so we convert to a binary String (e.g., "01010")
        std::string s;
        boost::to_string(v, s);
        return Variant(String(s.c_str()));
    }

    // --- 4. IFC Specifics ---
    Variant operator()(const EnumerationReference& v) const {
        // FIXED: Used .value() instead of .name()
        // value() returns const char* from the enumeration lookup
        return Variant(String(v.value()));
    }

    Variant operator()(const IfcUtil::IfcBaseClass* v) const {
        if (!v) return Variant();

        // Returning the ID (int64) is the safest way to handle references
        // without a complex Godot Object wrapper system.
        return Variant((int64_t)v->id());
    }

    // --- 5. Aggregates (Vectors) -> PackedArrays ---

    Variant operator()(const std::vector<int>& v) const {
        PackedInt64Array arr;
        arr.resize(v.size());
        for(size_t i = 0; i < v.size(); ++i) {
            arr[i] = (int64_t)v[i];
        }
        return Variant(arr);
    }

    Variant operator()(const std::vector<double>& v) const {
        PackedFloat64Array arr;
        arr.resize(v.size());
        for(size_t i = 0; i < v.size(); ++i) {
            arr[i] = v[i];
        }
        return Variant(arr);
    }

    Variant operator()(const std::vector<std::string>& v) const {
        PackedStringArray arr;
        arr.resize(v.size());
        for(size_t i = 0; i < v.size(); ++i) {
            arr[i] = String(v[i].c_str());
        }
        return Variant(arr);
    }

    Variant operator()(const std::vector<boost::dynamic_bitset<>>& v) const {
        // Array of Strings (because PackedStringArray is more efficient than Array for strings)
        PackedStringArray arr;
        arr.resize(v.size());
        for(size_t i = 0; i < v.size(); ++i) {
            std::string s;
            boost::to_string(v[i], s);
            arr[i] = String(s.c_str());
        }
        return Variant(arr);
    }

    Variant operator()(const boost::shared_ptr<aggregate_of_instance>& v) const {
        // Returns an Array of Entity IDs (Integers)
        // If you want PackedInt64Array, you can change the return type,
        // but Standard Arrays are safer if mixing nulls is possible.
        Array arr;
        if (!v) return arr;
        for(auto it = v->begin(); it != v->end(); ++it) {
            IfcUtil::IfcBaseClass* entity = *it;
            if (entity) arr.push_back((int64_t)entity->id());
            else arr.push_back(Variant());
        }
        return Variant(arr);
    }

    // --- 6. Nested Aggregates ---

    Variant operator()(const std::vector<std::vector<int>>& v) const {
        Array outer_arr;
        for(const auto& inner : v) {
            PackedInt64Array p_inner;
            p_inner.resize(inner.size());
            for(size_t i = 0; i < inner.size(); ++i) p_inner[i] = (int64_t)inner[i];
            outer_arr.push_back(p_inner);
        }
        return Variant(outer_arr);
    }

    Variant operator()(const std::vector<std::vector<double>>& v) const {
        Array outer_arr;
        for(const auto& inner : v) {
            PackedFloat64Array p_inner;
            p_inner.resize(inner.size());
            for(size_t i = 0; i < inner.size(); ++i) p_inner[i] = inner[i];
            outer_arr.push_back(p_inner);
        }
        return Variant(outer_arr);
    }

    Variant operator()(const boost::shared_ptr<aggregate_of_aggregate_of_instance>& v) const {
        Array outer_arr;
        if (!v) return outer_arr;

        // Iterate over the outer aggregate
        for(auto it_outer = v->begin(); it_outer != v->end(); ++it_outer) {
             // Depending on how aggregate_of_aggregate is implemented in your version of IfcOpenShell,
             // you might need to dereference it_outer to get a shared_ptr<aggregate_of_instance>
             // or a vector. Assuming standard nested structure:

             // Recursively call the logic for single aggregate here or build Array manually
             Array inner_arr;
             auto inner_list = *it_outer; // Attempt to get the inner list

             // Note: If 'inner_list' is a distinct type, you might need a specific loop here.
             // This is a generic implementation assuming standard container behavior.
             for(auto it_inner = inner_list.begin(); it_inner != inner_list.end(); ++it_inner) {
                 IfcUtil::IfcBaseClass* entity = *it_inner;
                 if (entity) inner_arr.push_back((int64_t)entity->id());
                 else inner_arr.push_back(Variant());
             }
             outer_arr.push_back(inner_arr);
        }
        return Variant(outer_arr);
    }

    // --- 7. Empty Aggregates ---
    Variant operator()(empty_aggregate_t) const { return Array(); }
    Variant operator()(empty_aggregate_of_aggregate_t) const { return Array(); }
};

Variant convert_attribute_value(const AttributeValue& val);

#endif //GODOTEXTENSION_VALUE_CONVERSION_H