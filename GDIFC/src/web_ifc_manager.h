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
#include <sstream>

#include "gd_ifc_settings.h"
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
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


class WEBIFCManager
{
public:
    godot::Ref<godot::GDIFCLoaderSettings> set;

    webifc::schema::IfcSchemaManager schemaManager;

    std::unique_ptr<webifc::parsing::IfcLoader> loader;

    webifc::manager::ModelManager model_manager;

    // Initialize geometry processor
    std::unique_ptr<webifc::geometry::IfcGeometryProcessor> geometry_loader;

    WEBIFCManager(godot::GDIFCLoaderSettings *settings)
        :
        set{},
        schemaManager{},
        model_manager(true)
    {
        if (settings != nullptr) {
            set = godot::Ref<godot::GDIFCLoaderSettings>(settings);
        } else {
            set.instantiate();
        }
        loader = std::make_unique<webifc::parsing::IfcLoader>(this->set->getTapeSize(), this->set->getMemoryLimit(), this->set->getLineWriterBuffer(), this->schemaManager);
    }

    WEBIFCManager()
        :
        set{},
        schemaManager{},
        model_manager(true)
    {
        set.instantiate();
        loader = std::make_unique<webifc::parsing::IfcLoader>(this->set->getTapeSize(), this->set->getMemoryLimit(), this->set->getLineWriterBuffer(), this->schemaManager);
    }

    void initialize_geometry_processor();

    void read_ifc_file(const char* data, size_t length) const;

    godot::Variant get_value_from_token(webifc::parsing::IfcTokenType t);

    godot::Array get_args(bool inObject = false, bool inList = false);

    godot::Dictionary get_express_line(uint32_t expressID);

    std::vector<godot::Dictionary> get_express_lines(const std::vector<uint32_t>& expressIDs);

    std::vector<uint32_t> get_express_ids_with_type(unsigned int type);

    godot::Array get_related_properties(uint32_t elementID, PropsDetail propsName, bool recursive);

    godot::Array get_property_sets(uint32_t elementID = 0, bool recursive = false, bool includeTypeProperties = false);

    godot::Array get_type_properties(uint32_t modelID, uint32_t elementID = 0, bool recursive = false);
};


// Use a std::map to hold the PropsDetail structs
const std::map<std::string, PropsDetail> PropsNames = {
    {"aggregates", {static_cast<unsigned int>(webifc::schema::IFCRELAGGREGATES), "RelatingObject", "RelatedObjects", "children"}},
    {"spatial", {static_cast<unsigned int>(webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE), "RelatingStructure", "RelatedElements", "children"}},
    {"psets", {static_cast<unsigned int>(webifc::schema::IFCRELDEFINESBYPROPERTIES), "RelatingPropertyDefinition", "RelatedObjects", "IsDefinedBy"}},
    {"materials", {static_cast<unsigned int>(webifc::schema::IFCRELASSOCIATESMATERIAL), "RelatingMaterial", "RelatedObjects", "HasAssociations"}},
    {"type", {static_cast<unsigned int>(webifc::schema::IFCRELDEFINESBYTYPE), "RelatingType", "RelatedObjects", "IsDefinedBy"}}
};


