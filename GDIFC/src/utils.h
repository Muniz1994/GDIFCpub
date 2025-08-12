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

godot::Variant ReadValue(webifc::parsing::IfcLoader* loader, webifc::parsing::IfcTokenType t);

godot::Array GetArgs(webifc::parsing::IfcLoader* loader, webifc::manager::ModelManager manager, bool inObject = false, bool inList = false);

godot::Dictionary GetLine(webifc::parsing::IfcLoader* loader, webifc::manager::ModelManager manager, uint32_t expressID);