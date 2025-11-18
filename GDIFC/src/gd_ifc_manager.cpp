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
    std::unique_ptr<webifc::geometry::IfcGeometryProcessor>& geometryLoader,
    godot::Node3D* parent_node,
    godot::String& name,
    std::string& ifc_type,
    std::unique_ptr<webifc::parsing::IfcLoader>& loader,
    webifc::manager::ModelManager manager,
    uint32_t expressID,
    godot::Dictionary props,
    bool create_collision) {

    // rem --------------------------------------

    IFCNode* element_node = memnew(IFCNode);

    element_node->set_name(ifc_type.c_str());

    element_node->set_properties(props);

    parent_node->add_child(element_node, true);

    // ---------------

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
            vertices[i] = godot::Vector3(transformed_vertex.x,transformed_vertex.y,transformed_vertex.z);
        }

        // Get indices
        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
            bimGeometry::Face face = ifc_geometry.GetFace(i);
            indices[static_cast<int64_t>(i) * 3 + 0] = face.i2;
            indices[static_cast<int64_t>(i) * 3 + 1] = face.i1;
            indices[static_cast<int64_t>(i) * 3 + 2] = face.i0;
        }

        // Calculate normals
        normals.resize(ifc_geometry.numPoints);

        for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {

            bimGeometry::Face face = ifc_geometry.GetFace(i);
            Vector3 p0 = vertices[face.i0];
            Vector3 p1 = vertices[face.i1];
            Vector3 p2 = vertices[face.i2];
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

    IfcParse::IfcFile file(path.utf8().get_data());

    if (!file.good()) {
        UtilityFunctions::print("IFC file not loaded into IfcParser loader.");
    }
    else {
        UtilityFunctions::print("IFC file successfully loaded into IfcParser loader.");
    }

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

            auto props = godot::Dictionary{};

            

            if (file.schema()->name() == Ifc4::get_schema().name())
            {
                props = get_ifc_property_sets<Ifc4>(file, expressID);
            }
            else if (file.schema()->name() == Ifc2x3::get_schema().name())
            {
                props = get_ifc_property_sets<Ifc2x3>(file, expressID);
            }
            
            // Create the meshes
            create_and_add_mesh(flat_mesh, ifc_manager.geometry_loader, main_node, name, ifc_type, ifc_manager.loader, ifc_manager.model_manager, expressID, props, create_collision);
        }
    }

    UtilityFunctions::print("IFC file processing complete.");
}


godot::Variant to_godot_variant(const AttributeValue& attr_value) {
    // We use a lambda as the visitor, leveraging C++17 'if constexpr' for type dispatch.
    return attr_value.apply_visitor([&](auto&& arg) -> Variant {
        // Decay the type to handle const/reference correctly
        using T = std::decay_t<decltype(arg)>;

        // 1. PRIMITIVE TYPES
        if constexpr (std::is_same_v<T, int>) {
            return Variant(arg);
        }
        else if constexpr (std::is_same_v<T, double>) {
            return Variant(arg);
        }
        else if constexpr (std::is_same_v<T, std::string>) {
            // Note: Godot's String type is usually a dedicated type, but here we pass the C++ string
            return Variant(godot::String(arg.c_str()));
        }
        else if constexpr (std::is_same_v<T, bool>) {
            return Variant(arg);
        }

        // 2. IFC/BOOST SPECIFIC TYPES
        else if constexpr (std::is_same_v<T, boost::tribool>) {
            // FIX: Compare to bool 'true' and 'false', not int '1' and '0'
            if (arg == true) return Variant("TRUE");
            if (arg == false) return Variant("FALSE");
            return Variant("UNKNOWN");
        }
        else if constexpr (std::is_same_v<T, EnumerationReference>) {
            // Enumerations are best represented as Godot Strings
            return Variant(godot::String(arg.value()));
        }
        else if constexpr (std::is_convertible_v<T, IfcUtil::IfcBaseClass*>) {
            // IFC Entity Instance pointer -> Should be wrapped in a Godot Object
            // A helper function (e.g., entity_to_godot_object) would be called here.
            // For the mockup, we return a Variant initialized with the pointer.
            return Variant(static_cast<IfcUtil::IfcBaseClass*>(arg));
        }

        // 3. AGGREGATE (VECTOR) TYPES
        else if constexpr (
            std::is_same_v<T, std::vector<int>> ||
            std::is_same_v<T, std::vector<double>>
            ) {
            godot::Array godot_array; // <-- FIX: Use godot::Array
            for (const auto& item : arg) {
                godot_array.push_back(Variant(item)); // This is fine for int/double
            }
            return Variant(godot_array); // <-- FIX: Construct Variant from godot::Array
        }
        // FIX: Handle string vectors separately
        else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            godot::Array godot_array; // <-- FIX: Use godot::Array
            for (const auto& item : arg) {
                // <-- FIX (C2440): Convert inner string to godot::String
                godot_array.push_back(godot::String(item.c_str()));
            }
            return Variant(godot_array); // <-- FIX: Construct Variant from godot::Array
        }

        // 4. COMPLEX/NULL TYPES
        else if constexpr (
            std::is_same_v<T, Derived> ||
            std::is_same_v<T, Blank> ||
            std::is_same_v<T, empty_aggregate_t> ||
            std::is_same_v<T, empty_aggregate_of_aggregate_t>
            ) {
            // These represent derived, blank, or empty values -> return NIL
            return Variant();
        }

        // 5. FALLBACK / UNHANDLED TYPES
        else {
            // Handle types like boost::dynamic_bitset, aggregate_of_aggregate, etc.
            // For now, we return a NIL Variant.
            return Variant();
        }
        });
}

// Fills a dictionary with **all** properties of an IFC instance

template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID)
{
    godot::Dictionary psets;

    // 1. Early exit validation
    auto instance = file.instance_by_id(expressID);
    if (!instance) return psets;

    auto object = instance->template as<typename schema::IfcObject>();
    if (!object) return psets;

    // 2. Get the list of all relationships (Generic list)
    // IfcOpenShell: IsDefinedBy returns a aggregate/list, not a single object
    auto rels_defines = object->IsDefinedBy();
    if (!rels_defines) return psets;

    // 3. Iterate generic relationships
    for (auto rel_generic : *rels_defines)
    {
        // OPTIMIZATION: Single dynamic_cast capture
        // Check if this specific relationship is "DefinesByProperties"
        if (auto rel = rel_generic->template as<typename schema::IfcRelDefinesByProperties>())
        {
            auto p_set_select = rel->RelatingPropertyDefinition();
            if (!p_set_select) continue;

            // Check if it is actually an IfcPropertySet (could be ElementQuantity, etc.)
            if (auto p_set = p_set_select->template as<typename schema::IfcPropertySet>())
            {
                auto p_set_name_opt = p_set->Name();
                if (!p_set_name_opt.has_value()) continue;

                std::string pset_key = p_set_name_opt.value();

                auto props = p_set->HasProperties();
                if (!props) continue;

                godot::Dictionary props_dict;

                for (auto prop : *props)
                {
                    if (!prop) continue;

                    // Cache the property name to avoid repeated lookups
                    // Godot Dictionaries use Variants as keys. Passing a const char* // creates a String automatically.
                    std::string prop_name_str = prop->Name();
                    const char* prop_key = prop_name_str.c_str();

                    // OPTIMIZATION: Order by probability
                    // IfcPropertySingleValue is the vast majority (90%+) of cases. Check it first.

                    if (auto p_single = prop->template as<typename schema::IfcPropertySingleValue>())
                    {
                        // Handle Single Value
                        if (auto n_value = p_single->NominalValue()) {
                            // Determine underlying primitive via generic access
                            if (auto final_value = n_value->template as<IfcUtil::IfcBaseClass>()) {
                                props_dict[prop_key] = to_godot_variant(final_value->get_attribute_value(0));
                            }
                            else {
                                props_dict[prop_key] = "[Invalid Value]";
                            }
                        }
                        else {
                            props_dict[prop_key] = godot::Variant(); // Null/Nil
                        }
                    }
                    else if (auto p_bounded = prop->template as<typename schema::IfcPropertyBoundedValue>())
                    {
                        // Handle Bounded Value
                        godot::Array bounds;

                        // Lower
                        if (auto lb = p_bounded->LowerBoundValue()) {
                            if (auto val = lb->template as<IfcUtil::IfcBaseClass>())
                                bounds.push_back(to_godot_variant(val->get_attribute_value(0)));
                        }
                        else {
                            bounds.push_back(godot::Variant());
                        }

                        // Upper
                        if (auto ub = p_bounded->UpperBoundValue()) {
                            if (auto val = ub->template as<IfcUtil::IfcBaseClass>())
                                bounds.push_back(to_godot_variant(val->get_attribute_value(0)));
                        }
                        else {
                            bounds.push_back(godot::Variant());
                        }

                        props_dict[prop_key] = bounds;
                    }
                    else if (auto p_enum = prop->template as<typename schema::IfcPropertyEnumeratedValue>())
                    {
                        // Handle Enumerated Value
                        godot::Array enum_arr;

                        if (auto enum_values = p_enum->EnumerationValues()) {
                            for (auto& enum_val : *enum_values.get()) {
                                if (auto val = enum_val->template as<IfcUtil::IfcBaseClass>()) {
                                    enum_arr.push_back(to_godot_variant(val->get_attribute_value(0)));
                                }
                            }
                        }
                        // IfcOpenShell usually resolves Reference automatically, 
                        // but if Values are empty, check Reference:
                        else if (auto enum_ref = p_enum->EnumerationReference()) {
                            // Logic to pull defaults from the reference could go here
                            // depending on schema version, usually implies looking up IfcPropertyEnumeration
                        }
                        props_dict[prop_key] = enum_arr;
                    }
                    else if (auto p_list = prop->template as<typename schema::IfcPropertyListValue>())
                    {
                        // Handle List Value
                        godot::Array list_arr;
                        if (auto list_values = p_list->ListValues()) {
                            for (auto& list_val : *list_values.get()) {
                                if (auto val = list_val->template as<IfcUtil::IfcBaseClass>()) {
                                    list_arr.push_back(to_godot_variant(val->get_attribute_value(0)));
                                }
                            }
                        }
                        props_dict[prop_key] = list_arr;
                    }
                    else if (auto p_ref = prop->template as<typename schema::IfcPropertyReferenceValue>())
                    {
                        props_dict[prop_key] = p_ref->UsageName().value_or("[Reference]").c_str();
                    }
                    // Skip IfcPropertyTableValue and others for performance unless strictly needed
                }

                // Insert the constructed dictionary into the main psets dictionary
                psets[pset_key.c_str()] = props_dict;
            }
        }
    }

    return psets;
}



