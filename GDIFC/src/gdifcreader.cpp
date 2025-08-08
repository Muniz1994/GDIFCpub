// gdifcreader.cpp
#include "gdifcreader.h"

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"


using namespace godot;

// Binds the new method for use in Godot scripts
void IFCBuilding::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path"), &IFCBuilding::read_ifc);
};

IFCBuilding::IFCBuilding() {};
IFCBuilding::~IFCBuilding() {};

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
void IFCBuilding::create_and_add_mesh(
    webifc::geometry::IfcFlatMesh& ifc_mesh,
    webifc::geometry::IfcGeometryProcessor& geometryLoader,
    godot::Node3D* parent_node,
    godot::String& name,
    std::string& ifc_type) {

    Node3D* element_node = memnew(Node3D);
    element_node->set_name(ifc_type.c_str());

    parent_node->add_child(element_node, true);

    for (auto& geom_data : ifc_mesh.geometries) {

        auto ifc_geometry = geometryLoader.GetGeometry(geom_data.geometryExpressID);

        // CRITICAL FIX: Check if the geometry object is valid before accessing it.
        if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) {
            continue; // Skip invalid geometry
        }

        PackedVector3Array vertices;
        PackedInt32Array indices;
        PackedVector3Array normals;

        vertices.resize(ifc_geometry.numPoints);
        normals.resize(ifc_geometry.numPoints);
        indices.resize(ifc_geometry.numFaces * 3);

        // Get vertices and apply transformation
        for (uint32_t i = 0; i < ifc_geometry.numPoints; i++) {
            glm::dvec3 point = ifc_geometry.GetPoint(i);
            glm::dvec4 transformed_vertex = geom_data.transformation * glm::dvec4(point, 1.0);
            vertices[i] = glm_to_godot_vec3(glm::dvec3(transformed_vertex));
        }

        // Get indices
        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
            bimGeometry::Face face = ifc_geometry.GetFace(i);
            indices[i * 3 + 0] = face.i0;
            indices[i * 3 + 1] = face.i1;
            indices[i * 3 + 2] = face.i2;
        }

        // Calculate normals (as before)
        normals.resize(ifc_geometry.numPoints);
        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
            bimGeometry::Face face = ifc_geometry.GetFace(i);
            Vector3 p0 = vertices[face.i0];
            Vector3 p1 = vertices[face.i1];
            Vector3 p2 = vertices[face.i2];
            Vector3 normal = (p1 - p0).cross(p2 - p0).normalized();

            normals[face.i0] = -normal;
            normals[face.i1] = -normal;
            normals[face.i2] = -normal;
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

            godot::Color material_color = godot::Color(0.009,0.153,0.195, 0.502);
            element_material->set_transparency(godot::BaseMaterial3D::TRANSPARENCY_ALPHA);
            element_material->set_albedo(material_color);
            element_material->set_shading_mode(godot::BaseMaterial3D::SHADING_MODE_UNSHADED);
        }
        else if (ifc_type == "IfcWindow")
        {
            godot::Color material_color = godot::Color(geom_data.color[0], geom_data.color[1], geom_data.color[2], geom_data.color[3]);
            element_material->set_albedo(material_color);
            element_material->set_transparency(godot::BaseMaterial3D::TRANSPARENCY_ALPHA);
        }
        else 
        {
            godot::Color material_color = godot::Color(geom_data.color[0], geom_data.color[1], geom_data.color[2]);
            element_material->set_albedo(material_color);
        }
        
        //UtilityFunctions::print(material_color);

        MeshInstance3D* gd_mesh = memnew(MeshInstance3D);
        gd_mesh->set_mesh(gd_array_mesh);
        gd_mesh->set_surface_override_material(0, element_material);
        gd_mesh->set_name(ifc_type.c_str() + String("_geom"));

        element_node->add_child(gd_mesh, true);
    }
}

// Main function to load the IFC file using web-ifc
void IFCBuilding::read_ifc(godot::String path) {
    UtilityFunctions::print("Starting IFC file read...");

    webifc::manager::LoaderSettings set;

    webifc::schema::IfcSchemaManager schemaManager;
    webifc::parsing::IfcLoader loader(set.TAPE_SIZE, set.MEMORY_LIMIT, set.LINEWRITER_BUFFER, schemaManager);


    // Read the file content as raw bytes, which is safer
    Ref<FileAccess> file = FileAccess::open(path, FileAccess::READ);
    if (!file.is_valid()) {
        UtilityFunctions::print("Error: Could not open file at path ", path);
        return;
    }
    PackedByteArray file_data = file->get_buffer(file->get_length());

    // Load the IFC file into the web-ifc loader
    loader.LoadFile([&](char* dest, size_t sourceOffset, size_t destSize) -> size_t {
        uint32_t length = MIN((size_t)file_data.size() - sourceOffset, destSize);
        if (length > 0) {
            memcpy(dest, &file_data.ptr()[sourceOffset], length);
        }
        return length;
        });

    UtilityFunctions::print("IFC file successfully loaded into web-ifc loader.");

    // Initialize geometry processor
    webifc::geometry::IfcGeometryProcessor geometryLoader(
        loader, schemaManager, set.CIRCLE_SEGMENTS, set.COORDINATE_TO_ORIGIN,
        set.tolerancePlaneIntersection, set.toleranceBoundaryPoint,
        set.toleranceInsideOutsideToPlane, set.toleranceInsideOutside,
        set.toleranceScalarEquality, set.addPlaneIterations);

    // Create a new Node3D to hold the entire scene
    Node3D* main_node = memnew(Node3D);
    main_node->set_name("IFCModel_Root");
    add_child(main_node, true);



    // Iterate through all geometric elements and create meshes
    for (auto type : schemaManager.GetIfcElementList()) {

        auto expressIDs = loader.GetExpressIDsWithType(type);



        for (uint32_t expressID : expressIDs) {

            auto flat_mesh = geometryLoader.GetFlatMesh(expressID);

            auto ifc_type = schemaManager.IfcTypeCodeToType(loader.GetLineType(expressID));

            if (ifc_type == "IfcBuilding") {
                UtilityFunctions::print("yeeeeeeeeeeeeeeeeeeeeeaaaaaaaaaaaaaaaaaaaaaaaa");
            }

            if (ifc_type == "IfcBuildingStorey") {
                UtilityFunctions::print("yeeeeeeeeeeeeeeeeeeeeeaaaaaaaaaaaaaaaaaaaaaaaa");
            }

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
            create_and_add_mesh(flat_mesh, geometryLoader, main_node, name, ifc_type);
        }
    }

    UtilityFunctions::print("IFC file processing complete.");
}