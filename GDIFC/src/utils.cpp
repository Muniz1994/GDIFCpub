#include "utils.h"


void IFCManager::read_ifc_file(std::string path) const
{

    // 1. Open a file for reading
    std::ifstream file_stream(path);
    if (file_stream.is_open()) {
        // 3. Pass the stream object (by reference) to loadfile
        //    'file_stream' is an object of a class derived from std::istream.
        this->loader->LoadFile(file_stream);

        file_stream.close();
    }
    else {
        godot::UtilityFunctions::print("Error: Could not read ifc file");
    }
}

// old ReadValue
godot::Variant IFCManager::get_value_from_token(webifc::parsing::IfcTokenType t)
{

    switch (t)
    {
    case webifc::parsing::IfcTokenType::STRING:
    {
        return this->loader->GetDecodedStringArgument().c_str();
    }
    case webifc::parsing::IfcTokenType::ENUM:
    {
        std::string_view s = this->loader->GetStringArgument();
        if (s == "T")
        {
            return true;
        }
        if (s == "F")
        {
            return false;
        }
        if (s == "U")
        {
            return godot::Variant::NIL;
        }
        return std::string(s).c_str();
    }
    case webifc::parsing::IfcTokenType::REAL:
    {
        std::string_view s = this->loader->GetDoubleArgumentAsString();
        return std::string(s).c_str();
    }
    case webifc::parsing::IfcTokenType::INTEGER:
    {
        long d = this->loader->GetIntArgument();
        return (int64_t)d;
    }
    case webifc::parsing::IfcTokenType::REF:
    {
        uint32_t ref = this->loader->GetRefArgument();
        return ref;
    }
    default:
        // use undefined to signal val parse issue
        return godot::Variant::NIL;
    }
}

// old GetArgs
godot::Array IFCManager::get_args(bool inObject, bool inList)
{
    auto arguments = godot::Array();
    bool endOfLine = false;
    while (!loader->IsAtEnd() && !endOfLine)
    {
        webifc::parsing::IfcTokenType t = this->loader->GetTokenType();

        switch (t)
        {
        case webifc::parsing::IfcTokenType::LINE_END:
        {
            endOfLine = true;
            break;
        }
        case webifc::parsing::IfcTokenType::EMPTY:
        {
            arguments.append(godot::Variant::NIL);
            break;
        }
        case webifc::parsing::IfcTokenType::SET_BEGIN:
        {
            arguments.append(get_args(false, true));
            break;
        }
        case webifc::parsing::IfcTokenType::SET_END:
        {
            endOfLine = true;
            break;
        }
        case webifc::parsing::IfcTokenType::LABEL:
        {
            // read label
            auto obj = godot::Dictionary();
            obj["type"] = (static_cast<uint32_t>(webifc::parsing::IfcTokenType::LABEL));
            loader->StepBack();
            auto s = loader->GetStringArgument();
            auto typeCode = this->model_manager.GetSchemaManager().IfcTypeToTypeCode(s);
            obj["typecode"] = (typeCode);
            // read set open
            loader->GetTokenType();
            obj["value"] = get_args();
            arguments.append(obj);
            break;
        }
        case webifc::parsing::IfcTokenType::STRING:
        case webifc::parsing::IfcTokenType::ENUM:
        case webifc::parsing::IfcTokenType::REAL:
        case webifc::parsing::IfcTokenType::INTEGER:
        case webifc::parsing::IfcTokenType::REF:
        {
            loader->StepBack();
            godot::Dictionary obj;
            if (inObject)
                obj = get_value_from_token(t);
            else
            {
                obj = godot::Dictionary();
                obj["type"] = (static_cast<uint32_t>(t));
                obj["value"] = get_value_from_token(t);
            }
            arguments.append( obj);
            break;
        }
        default:
            break;
        }
    }
    return arguments;
}

godot::Dictionary IFCManager::get_express_line(uint32_t expressID)
{
    if (!this->loader->IsValidExpressID(expressID))
        return godot::Dictionary();

    uint32_t lineType = this->loader->GetLineType(expressID);

    if (lineType == 0)
        return godot::Dictionary();

    this->loader->MoveToArgumentOffset(expressID, 0);

    auto arguments = get_args();

    auto retVal = godot::Dictionary();
    retVal["ID"] = expressID;
    retVal["type"] = lineType;
    retVal["arguments"] = arguments;
    return retVal;
}

std::vector<godot::Dictionary> IFCManager::get_express_lines(const std::vector<uint32_t>& expressIDs)
{
    std::vector<godot::Dictionary> result;


    for (uint32_t expressID : expressIDs)
    {

        result.push_back(get_express_line(expressID));
    }

    return result;
}

// might be wrong cause the original one deal with types instead of a single type
std::vector<uint32_t> IFCManager::get_express_ids_with_type(unsigned int type)
{
    std::vector<uint32_t> expressIDs;

    
    auto ids = this->loader->GetExpressIDsWithType(type);
    expressIDs.insert(expressIDs.end(), ids.begin(), ids.end());


    return expressIDs;
}


godot::Array IFCManager::get_related_properties(uint32_t elementID, PropsDetail propsName, bool recursive) {
    
    auto result = godot::Array();

    godot::Array rels;

    if (elementID != 0) {

        godot::Dictionary line = get_express_line(elementID);

        if (line.has(propsName.key.c_str())) {

            rels = line[propsName.key.c_str()];
        }
    }
    else {

        auto vec = get_express_ids_with_type(propsName.name);
        for (uint32_t i = 0; i < vec.size(); ++i) {
            godot::Dictionary rel;
            rel["value"] = vec[i];
            rels.append(rel);
        }
    }

    if (rels.is_empty()) {
        return result;
    }

    for (int i = 0; i < rels.size(); ++i) {
        godot::Dictionary rel = rels[i];
        if (!rel.has("value")) {
            continue;
        }

        uint32_t relId = rel["value"];
        godot::Dictionary line = get_express_line(relId);
        if (!line.has(propsName.relating.c_str())) {
            continue;
        }

        godot::Array propSetIds = line[propsName.relating.c_str()];
        for (int x = 0; x < propSetIds.size(); ++x) {
            godot::Dictionary propSetId = propSetIds[x];
            if (!propSetId.has("value")) {
                continue;
            }
            uint32_t propId = propSetId["value"];
            result.append(get_express_line(propId));
        }
    }
    return result;
}


godot::Array IFCManager::get_property_sets(uint32_t elementID, bool recursive, bool includeTypeProperties) {
   
    if (includeTypeProperties) {

        godot::Array types = get_type_properties(elementID, false);

        godot::Array results;

        for (int i = 0; i < types.size(); ++i) {
            godot::Dictionary type = types[i];
            if (type.has("ID")) {
                uint32_t typeID = type["ID"];
                godot::Array psets = get_property_sets(typeID, recursive);
                for (int j = 0; j < psets.size(); ++j) {
                    results.append(psets[j]);
                }
            }
        }
        return results;
    }
    else {
        return get_related_properties(elementID, PropsNames.at("psets"), recursive);
    }
}

godot::Array IFCManager::get_type_properties( uint32_t modelID, uint32_t elementID, bool recursive) {
    
    if (loader->GetSchema() == IFC_SCHEMA::IFC2X3) {

        return get_related_properties(elementID, PropsNames.at("type"), recursive);
    }
    else {
        
        PropsDetail typeProps = PropsNames.at("type");

        typeProps.key = "IsTypedBy";

        return get_related_properties( elementID, typeProps, recursive);
    }
}

void IFCManager::initialize_geometry_processor()
{

    this->geometry_loader = std::make_unique<webifc::geometry::IfcGeometryProcessor>(*loader, schemaManager, set.CIRCLE_SEGMENTS, set.COORDINATE_TO_ORIGIN, set.TOLERANCE_PLANE_INTERSECTION, set.TOLERANCE_PLANE_DEVIATION, set.TOLERANCE_BACK_DEVIATION_DISTANCE, set.TOLERANCE_INSIDE_OUTSIDE_PERIMETER, set.TOLERANCE_SCALAR_EQUALITY, set.PLANE_REFIT_ITERATIONS, set.BOOLEAN_UNION_THRESHOLD);
}


// IFC-engine-API start

//std::vector<std::string> get_lines(float _model_id, std::vector<int> express_ids, bool flatten = false, bool inverse = false, std::optional<std::string> inversePropKey = std::nullopt)
//{
//
//    std::vector<std::string> output_line_data;
//
//    auto raw_lines_data = GetLines;
//
//
//
//};
