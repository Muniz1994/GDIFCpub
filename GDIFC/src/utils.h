#pragma once
#include <string>
#include <algorithm>
#include <vector>
#include <stack>
#include <cstdint>
#include <memory>
#include <map>
#include <variant>
#include <optional>

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <modelmanager/ModelManager.h>

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>

// Define the inner struct to hold the properties
struct PropsDetail {
    unsigned int name;
    std::string relating;
    std::string related;
    std::string key;
};


// Use a std::map to hold the PropsDetail structs
const std::map<std::string, PropsDetail> PropsNames = {
    {"aggregates", {static_cast<unsigned int>(webifc::schema::IFCRELAGGREGATES), "RelatingObject", "RelatedObjects", "children"}},
    {"spatial", {static_cast<unsigned int>(webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE), "RelatingStructure", "RelatedElements", "children"}},
    {"psets", {static_cast<unsigned int>(webifc::schema::IFCRELDEFINESBYPROPERTIES), "RelatingPropertyDefinition", "RelatedObjects", "IsDefinedBy"}},
    {"materials", {static_cast<unsigned int>(webifc::schema::IFCRELASSOCIATESMATERIAL), "RelatingMaterial", "RelatedObjects", "HasAssociations"}},
    {"type", {static_cast<unsigned int>(webifc::schema::IFCRELDEFINESBYTYPE), "RelatingType", "RelatedObjects", "IsDefinedBy"}}
};

godot::Variant ReadValue(webifc::parsing::IfcLoader* loader, webifc::parsing::IfcTokenType t);

godot::Array GetArgs(webifc::parsing::IfcLoader* loader, webifc::manager::ModelManager manager, bool inObject = false, bool inList = false);

godot::Dictionary GetLine(webifc::parsing::IfcLoader* loader, webifc::manager::ModelManager manager, uint32_t expressID);

std::vector<uint32_t> GetLineIDsWithType(webifc::parsing::IfcLoader* loader, unsigned int type);

godot::Array getRelatedProperties(webifc::manager::ModelManager manager, webifc::parsing::IfcLoader* loader, uint32_t elementID, PropsDetail propsName, bool recursive);

godot::Array getPropertySets(webifc::manager::ModelManager manager, webifc::parsing::IfcLoader* loader, uint32_t elementID = 0, bool recursive = false, bool includeTypeProperties = false);

godot::Array getTypeProperties(webifc::manager::ModelManager manager, webifc::parsing::IfcLoader* loader, uint32_t modelID, uint32_t elementID = 0, bool recursive = false);
