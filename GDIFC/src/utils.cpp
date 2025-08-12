#include "utils.h"

godot::Variant ReadValue(webifc::parsing::IfcLoader* loader, webifc::parsing::IfcTokenType t)
{

    switch (t)
    {
    case webifc::parsing::IfcTokenType::STRING:
    {
        return loader->GetDecodedStringArgument().c_str();
    }
    case webifc::parsing::IfcTokenType::ENUM:
    {
        std::string_view s = loader->GetStringArgument();
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
        std::string_view s = loader->GetDoubleArgumentAsString();
        return std::string(s).c_str();
    }
    case webifc::parsing::IfcTokenType::INTEGER:
    {
        long d = loader->GetIntArgument();
        return (int64_t)d;
    }
    case webifc::parsing::IfcTokenType::REF:
    {
        uint32_t ref = loader->GetRefArgument();
        return ref;
    }
    default:
        // use undefined to signal val parse issue
        return godot::Variant::NIL;
    }
}



godot::Array GetArgs(webifc::parsing::IfcLoader *loader, webifc::manager::ModelManager manager, bool inObject, bool inList)
{
    auto arguments = godot::Array();
    bool endOfLine = false;
    while (!loader->IsAtEnd() && !endOfLine)
    {
        webifc::parsing::IfcTokenType t = loader->GetTokenType();

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
            arguments.append(GetArgs(loader, manager, false, true));
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
            auto typeCode = manager.GetSchemaManager().IfcTypeToTypeCode(s);
            obj["typecode"] = (typeCode);
            // read set open
            loader->GetTokenType();
            obj["value"] = GetArgs(loader, manager);
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
                obj = ReadValue(loader, t);
            else
            {
                obj = godot::Dictionary();
                obj["type"] = (static_cast<uint32_t>(t));
                obj["value"] = ReadValue(loader, t);
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

godot::Dictionary GetLine(webifc::parsing::IfcLoader* loader, webifc::manager::ModelManager manager, uint32_t expressID)
{
    /*if (!manager.IsModelOpen(modelID))
        return emscripten::val::object();*/
    if (!loader->IsValidExpressID(expressID))
        return godot::Dictionary();
    uint32_t lineType = loader->GetLineType(expressID);
    if (lineType == 0)
        return godot::Dictionary();

    loader->MoveToArgumentOffset(expressID, 0);

    auto arguments = GetArgs(loader,manager);

    auto retVal = godot::Dictionary();
    retVal["ID"] = expressID;
    retVal["type"] = lineType;
    retVal["arguments"] = arguments;
    return retVal;
}