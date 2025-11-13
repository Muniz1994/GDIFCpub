#ifndef GDIFCREADER_H
#define GDIFCREADER_H

#include<memory>

#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/standard_material3d.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "godot_cpp/classes/array_mesh.hpp"
#include "godot_cpp/classes/surface_tool.hpp"
#include "godot_cpp/variant/packed_vector3_array.hpp"
#include "godot_cpp/variant/packed_int32_array.hpp"

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>
#include <modelmanager/ModelManager.h>
#include "utils.h"


#include <Ifc4.h>

#include <IfcFile.h>
#include <IfcLogger.h>
#include <FileReader.h>

#define IfcSchema Ifc4


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
        std::unique_ptr<webifc::geometry::IfcGeometryProcessor> &geometryLoader,
        godot::Node3D* parent_node,
        godot::String& name,
        std::string& ifc_type,
        std::unique_ptr<webifc::parsing::IfcLoader> &loader,
        webifc::manager::ModelManager manager,
        uint32_t expressID,
        godot::Dictionary props,
        bool create_collision = false
    );
};

#endif