#ifndef GDIFCREADER_H
#define GDIFCREADER_H

#include<memory>
#include <iomanip>

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/surface_tool.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"
#include "godot_cpp/variant/array.hpp"
#include "godot_cpp/variant/variant.hpp"
#include "godot_cpp/variant/dictionary.hpp"
#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>
#include <modelmanager/ModelManager.h>
#include "utils.h"


#include <Ifc4.h>
#include <Ifc2x3.h>

#include <IfcFile.h>
#include <IfcLogger.h>
#include <FileReader.h>


#include "gd_ifc_node.h"




// Your existing class definition
class GDIFCManager : public godot::Node3D {
    GDCLASS(GDIFCManager, Node3D);

protected:
    static void _bind_methods();

public:
    GDIFCManager();
    ~GDIFCManager();

    void read_ifc(godot::String path, bool create_collision = false);

private:
    // Helper function to create Godot mesh from web-ifc data
  void create_and_add_mesh(
      webifc::geometry::IfcFlatMesh& ifc_mesh,
      std::unique_ptr<webifc::geometry::IfcGeometryProcessor>& geometryLoader,
      IFCNode* element_node,
      std::string& ifc_type,
      bool create_collision);
};

godot::Variant to_godot_variant(const AttributeValue& attr_value);

template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID);


#endif