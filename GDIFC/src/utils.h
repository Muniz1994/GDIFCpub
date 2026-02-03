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
#include <fstream>

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

struct GDIFCLoaderSettings
{
    bool COORDINATE_TO_ORIGIN = false;
    uint16_t CIRCLE_SEGMENTS = 12;
    uint32_t TAPE_SIZE = 67108864; // probably no need for anyone other than web-ifc devs to change this
    uint32_t MEMORY_LIMIT = 2147483648;
    uint16_t LINEWRITER_BUFFER = 10000;
    double TOLERANCE_PLANE_INTERSECTION = 1.0E-01;
    double TOLERANCE_PLANE_DEVIATION = 3.0E-04;
    double TOLERANCE_BACK_DEVIATION_DISTANCE = 3.0E-04;
    double TOLERANCE_INSIDE_OUTSIDE_PERIMETER = 1.0E-10;
    double TOLERANCE_SCALAR_EQUALITY = 1.0E-04;
    uint16_t PLANE_REFIT_ITERATIONS = 10;
    uint16_t BOOLEAN_UNION_THRESHOLD = 150;
};


class IFCManager
{
public:
    GDIFCLoaderSettings set;

    webifc::schema::IfcSchemaManager schemaManager;

    std::unique_ptr<webifc::parsing::IfcLoader> loader;

    webifc::manager::ModelManager model_manager;

    // Initialize geometry processor
    std::unique_ptr<webifc::geometry::IfcGeometryProcessor> geometry_loader;

    IFCManager()
        :
        set{},
        schemaManager{},
        model_manager(true),
        loader(std::make_unique<webifc::parsing::IfcLoader>(this->set.TAPE_SIZE, this->set.MEMORY_LIMIT, this->set.LINEWRITER_BUFFER, this->schemaManager))


    {
         // Pass the dereferenced pointer (the actual object)

        
    }

    void initialize_geometry_processor();

    void read_ifc_file(std::string path) const;

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


