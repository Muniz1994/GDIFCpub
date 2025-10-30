// gdifcreader.cpp
#include "gd_ifc_manager.h"
#include "gd_ifc_node.h"

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"



using namespace godot;

// Binds the new method for use in Godot scripts
void GDIFCManager::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path", "create_collision"), &GDIFCManager::read_ifc);
};



GDIFCManager::GDIFCManager() {};
GDIFCManager::~GDIFCManager() {};

// Helper function to convert web-ifc's glm::dvec3 to Godot's Vector3
static Vector3 glm_to_godot_vec3(const glm::dvec3& v) {
    // Correct swizzle for position and orientation.
    // However, this might invert the winding order.
    Vector3 position = Vector3(v.x, v.y, v.z);

    // Negate the Z-component to flip the triangle winding order,
    // which will correct the normals.
    return Vector3(position.x, position.y, -position.z);
}

// Helper function to create a mesh from web-ifc data
void GDIFCManager::create_and_add_mesh(
    webifc::geometry::IfcFlatMesh& ifc_mesh,
    std::unique_ptr<webifc::geometry::IfcGeometryProcessor> &geometryLoader,
    godot::Node3D* parent_node,
    godot::String& name,
    std::string& ifc_type,
    std::unique_ptr<webifc::parsing::IfcLoader> &loader,
    webifc::manager::ModelManager manager,
    uint32_t expressID,
    bool create_collision) {

    IFCNode* element_node = memnew(IFCNode);

    element_node->set_name(ifc_type.c_str());

    parent_node->add_child(element_node, true);

 /*   godot::Dictionary attrs = (loader, manager, expressID);*/

    //godot::Array props = getPropertySets(manager, loader, expressID, true, true);

    //element_node->set_properties(props);

    //element_node->set_attributes(attrs);

    for (auto& geom_data : ifc_mesh.geometries) {

        auto ifc_geometry = geometryLoader->GetGeometry(geom_data.geometryExpressID);

        // CRITICAL FIX: Check if the geometry object is valid before accessing it.
        if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) {
            continue; // Skip invalid geometry
        }

        PackedVector3Array vertices;
        PackedInt32Array indices;
        PackedVector3Array normals;

        vertices.resize(ifc_geometry.numPoints);
        normals.resize(ifc_geometry.numPoints);
        indices.resize(static_cast<int64_t>(ifc_geometry.numFaces) * 3);

        // Get vertices and apply transformation
        for (uint32_t i = 0; i < ifc_geometry.numPoints; i++) {
            glm::dvec3 point = ifc_geometry.GetPoint(i);
            glm::dvec4 transformed_vertex = geom_data.transformation * glm::dvec4(point, 1.0);
            vertices[i] = glm_to_godot_vec3(glm::dvec3(transformed_vertex));
        }

        // Get indices
        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
            bimGeometry::Face face = ifc_geometry.GetFace(i);
            indices[static_cast<int64_t>(i) * 3 + 0] = face.i0;
            indices[static_cast<int64_t>(i) * 3 + 1] = face.i1;
            indices[static_cast<int64_t>(i) * 3 + 2] = face.i2;
        }

        // Calculate normals
        normals.resize(ifc_geometry.numPoints);
        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
            bimGeometry::Face face = ifc_geometry.GetFace(i);
            Vector3 p0 = vertices[face.i2];
            Vector3 p1 = vertices[face.i1];
            Vector3 p2 = vertices[face.i0];
            Vector3 normal = (p1 - p0).cross(p2 - p0).normalized();

            normals[face.i0] = normal;
            normals[face.i1] = normal;
            normals[face.i2] = normal;
        }

        Ref<ArrayMesh> gd_array_mesh;
        gd_array_mesh.instantiate();

        godot::Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = vertices;
        arrays[Mesh::ARRAY_NORMAL] = normals;
        arrays[Mesh::ARRAY_INDEX] = indices;

        gd_array_mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

        // Define a default material
        Ref<godot::StandardMaterial3D> element_material;
        element_material.instantiate();
       

        if (ifc_type == "IfcSpace") {

            element_material->set_transparency(godot::BaseMaterial3D::TRANSPARENCY_ALPHA);
            godot::Color material_color = godot::Color(0.025, 0.037, 0.034, 0.1);
            element_material->set_albedo(material_color);
            element_material->set_shading_mode(godot::BaseMaterial3D::SHADING_MODE_UNSHADED);
            element_material->set_grow_enabled(true);
            element_material->set_grow(-0.001);
            element_material->set_render_priority(-2);
            
        }
        else if (ifc_type == "IfcWindow" || ifc_type == "IfcDoor" || ifc_type == "IfcWall" || ifc_type == "IfcPlate")
        {
            if (geom_data.color.a < 0.7)
            {
                element_material->set_transparency(godot::BaseMaterial3D::TRANSPARENCY_ALPHA);
            }
            godot::Color material_color = godot::Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
            element_material->set_albedo(material_color);
            element_material->set_render_priority(-1);
        }
        else 
        {
            godot::Color material_color = godot::Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
            element_material->set_albedo(material_color);
        }
        
        //UtilityFunctions::print(material_color);

        MeshInstance3D* gd_mesh = memnew(MeshInstance3D);
        gd_mesh->set_mesh(gd_array_mesh);
        gd_mesh->set_surface_override_material(0, element_material);
        gd_mesh->set_name(ifc_type.c_str() + String("_geom"));

        if (create_collision)
        {
            gd_mesh->create_trimesh_collision();
        }

        element_node->add_child(gd_mesh, true);
    }
}

// Main function to load the IFC file using web-ifc
void GDIFCManager::read_ifc(godot::String path, bool create_collision) {

    UtilityFunctions::print("Starting IFC file read...");

    IFCManager ifc_manager = IFCManager();

    ifc_manager.read_ifc_file(path.utf8().get_data());

    UtilityFunctions::print("IFC file successfully loaded into web-ifc loader.");

    // Create a new Node3D to hold the entire scene
    Node3D* main_node = memnew(Node3D);
    main_node->set_name("IFCModel");
    add_child(main_node, true);

    ifc_manager.initialize_geometry_processor();

    // Iterate through all geometric elements and create meshes
    for (auto type : ifc_manager.schemaManager.GetIfcElementList()) { 

        auto expressIDs = ifc_manager.loader->GetExpressIDsWithType(type);


        for (uint32_t expressID : expressIDs) {


            auto flat_mesh = ifc_manager.geometry_loader->GetFlatMesh(expressID);

            auto ifc_type = ifc_manager.schemaManager.IfcTypeCodeToType(ifc_manager.loader->GetLineType(expressID));


            // Safety check for empty meshes
            if (flat_mesh.geometries.size() == 0) {
                continue;
            }

            if (ifc_type == "IfcOpeningElement") {
                continue;
            }

            // Define the name of each node
            String class_name = String(ifc_type.c_str());

            String name = class_name + "_" + String::num_int64(expressID);

            // Create the meshes
            create_and_add_mesh(flat_mesh, ifc_manager.geometry_loader, main_node, name, ifc_type, ifc_manager.loader, ifc_manager.model_manager, expressID, create_collision);
        }
    }

    UtilityFunctions::print("IFC file processing complete.");
}



