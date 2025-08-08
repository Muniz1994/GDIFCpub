#ifndef GDIFCREADER_H
#define GDIFCREADER_H

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

// Your existing class definition
class IFCBuilding : public godot::Node3D {
    GDCLASS(IFCBuilding, Node3D);

protected:
    static void _bind_methods();

public:
    IFCBuilding();
    ~IFCBuilding();

    void read_ifc(godot::String path);

private:
    // Helper function to create Godot mesh from web-ifc data
    void create_and_add_mesh(
        webifc::geometry::IfcFlatMesh& ifc_mesh,
        webifc::geometry::IfcGeometryProcessor& geometryLoader,
        godot::Node3D* parent_node,
        godot::String& name,
        std::string& ifc_type
    );
};

#endif