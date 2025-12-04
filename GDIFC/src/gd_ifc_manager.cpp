#include "gd_ifc_manager.h"

using namespace godot;

void GDIFCManager::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path", "create_collision", "collision_classes"), &GDIFCManager::read_ifc, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("_thread_task", "path"), &GDIFCManager::_thread_task);

    ADD_SIGNAL(MethodInfo("ifc_read"));
}

GDIFCManager::GDIFCManager() {}
GDIFCManager::~GDIFCManager() {}

void GDIFCManager::read_ifc(String path, bool create_collision, Array collision_classes) {
    if (current_state != IDLE && current_state != DONE) {
        UtilityFunctions::print("Already loading!");
        return;
    }

    // Reset
    this->should_create_collisions = create_collision;
    this->collision_classes = collision_classes;
    this->generation_queue.clear();
    this->material_cache.clear(); // Clear cache for new file
    this->current_generation_index = 0;
    this->current_state = LOADING_THREAD;

    // Start Thread
    Callable callable = Callable(this, "_thread_task").bind(path);
    task_id = WorkerThreadPool::get_singleton()->add_task(callable, true); // High Priority
    set_process(true);
}

// ---------------------------------------------------------
// BACKGROUND THREAD (Does 99% of the work now)
// ---------------------------------------------------------
void GDIFCManager::_thread_task(String path) {

    // 1. Load File
    auto temp_ifc_manager = std::make_unique<IFCManager>();
    auto temp_file = std::make_unique<IfcParse::IfcFile>(path.utf8().get_data());
    temp_ifc_manager->read_ifc_file(path.utf8().get_data());

    if (!temp_file->good()) {
        UtilityFunctions::printerr("Failed to load IFC file.");
        this->current_state = FAILED;
        return;
    }
    // 2. Init Geometry
    temp_ifc_manager->initialize_geometry_processor();

    Vector<PrecalculatedIFCItem> temp_queue;

    // Schema check for properties
    bool is_ifc4 = (temp_file->schema()->name() == Ifc4::get_schema().name());

    // TODO: check if schema of the file is within the Ifc4/ifc2x3/future 4x3

    // 3. HEAVY LOOP: Process everything HERE, not in Main Thread
    for (auto type : temp_ifc_manager->schemaManager.GetIfcElementList()) {

        auto expressIDs = temp_ifc_manager->loader->GetExpressIDsWithType(type);

        for (uint32_t expressID : expressIDs) {

            std::string type_str = temp_ifc_manager->schemaManager.IfcTypeCodeToType(temp_ifc_manager->loader->GetLineType(expressID));

            if (type_str == "IfcOpeningElement") {
                continue;
            }

            // -- A. GEOMETRY PROCESSING --
            auto flat_mesh = temp_ifc_manager->geometry_loader->GetFlatMesh(expressID);

            // If no geometry, skip early
            if (flat_mesh.geometries.empty()) {
                continue;
            }

            PrecalculatedIFCItem item;
            item.valid = true;
            item.node_name = String(type_str.c_str()) + "_" + String::num_int64(expressID);

            // -- B. PROPERTY PARSING (Moved to Thread!) --
            // This was the main cause of lag. Now it happens in background.
            if (is_ifc4) {
                item.properties = get_ifc_property_sets<Ifc4>(*temp_file, expressID);
            } else {
                item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, expressID);
            }

            item.ifc_class = type_str.c_str();


            item.geometry = {};
            // -- C. MESH DATA PREPARATION --
            // We merge multiple sub-geometries into one ArrayMesh surface here to reduce draw calls
            // Or keep them separate if needed. This example handles the first valid geometry logic you had.

            for (auto& geom_data : flat_mesh.geometries) {
                PrecalculatedIFCItemGeometry item_geometry;
                auto ifc_geometry = temp_ifc_manager->geometry_loader->GetGeometry(geom_data.geometryExpressID);

                if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) {
                    continue;
                }

                // Resize Arrays
                int vertex_offset = item_geometry.vertices.size();
                item_geometry.vertices.resize(vertex_offset + ifc_geometry.numPoints);
                item_geometry.normals.resize(vertex_offset + ifc_geometry.numPoints);
                int index_offset = item_geometry.indices.size();
                item_geometry.indices.resize(index_offset + (ifc_geometry.numFaces * 3));

                // 1. Vertices (Math in Thread)
                for (uint32_t i = 0; i < ifc_geometry.numPoints; i++) {
                    glm::dvec3 point = ifc_geometry.GetPoint(i);
                    glm::dvec4 tv = geom_data.transformation * glm::dvec4(point, 1.0);
                    item_geometry.vertices[vertex_offset + i] = Vector3(tv.x, tv.y, tv.z);
                }

                // 2. Indices
                for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
                    bimGeometry::Face face = ifc_geometry.GetFace(i);
                    item_geometry.indices[index_offset + (i * 3) + 0] = vertex_offset + face.i2;
                    item_geometry.indices[index_offset + (i * 3) + 1] = vertex_offset + face.i1;
                    item_geometry.indices[index_offset + (i * 3) + 2] = vertex_offset + face.i0;
                }

                // 3. Normals (Math in Thread)
                for (uint32_t i = 0; i < ifc_geometry.numFaces; i++) {
                    bimGeometry::Face face = ifc_geometry.GetFace(i);
                    // Use the indices we just wrote
                    Vector3 p0 = item_geometry.vertices[vertex_offset + face.i0];
                    Vector3 p1 = item_geometry.vertices[vertex_offset + face.i1];
                    Vector3 p2 = item_geometry.vertices[vertex_offset + face.i2];

                    Vector3 normal = (p1 - p0).cross(p2 - p0).normalized(); // Expensive sqrt!

                    item_geometry.normals[vertex_offset + face.i0] = normal;
                    item_geometry.normals[vertex_offset + face.i1] = normal;
                    item_geometry.normals[vertex_offset + face.i2] = normal;
                }

                // 4. Color Info
                item_geometry.color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);

                // Determine transparency
                if (type_str == "IfcSpace") {
                    item_geometry.is_transparent = true;
                    item_geometry.color = Color(0.025, 0.037, 0.034, 0.1);
                } else {
                    item_geometry.is_transparent = (geom_data.color.a < 0.7);
                }

                if (item_geometry.vertices.size() > 0) {
                    item.geometry.push_back(item_geometry);
                }
            }

            if (!item.geometry.empty()) {
                temp_queue.push_back(item);
            }
        }
    }

    // Commit results
    this->web_ifc_manager = std::move(temp_ifc_manager);
    this->ifc_parse_file = std::move(temp_file);
    this->generation_queue = temp_queue;

    UtilityFunctions::print("Ifc Geometry calculated");
}

// ---------------------------------------------------------
// MAIN THREAD PROCESS
// ---------------------------------------------------------
void GDIFCManager::_process(double delta) {

if (current_state == LOADING_THREAD) {
        if (WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
            WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
            task_id = -1;

            // 1. Create the Staging Root
            // IMPORTANT: We do NOT add_child() yet. It exists only in memory.
            invisible_staging_root = memnew(Node3D);
            invisible_staging_root->set_name("IFC_Staging_Area");

            current_state = GENERATING_NODES;
        }
    } else if (current_state == GENERATING_NODES) {
        _process_generation_queue();
    }
}

// ---------------------------------------------------------
// DUMB & FAST GENERATOR
// ---------------------------------------------------------
void GDIFCManager::_process_generation_queue() {

    uint64_t start_time = Time::get_singleton()->get_ticks_usec();
    // Budget: 8ms (8000 usec). Keep this!
    // If you remove the budget, the game will freeze/crash 'Not Responding'.
    uint64_t time_budget = 4000;

    while (current_generation_index < generation_queue.size()) {

        const PrecalculatedIFCItem& item = generation_queue[current_generation_index];

        // 1. Create Node
        IFCNode* element_node = memnew(IFCNode);
        element_node->set_name(item.node_name);
        element_node->set_properties(item.properties);
        element_node->set_class(item.ifc_class);

        // ADD TO INVISIBLE ROOT (Not SceneTree)
        invisible_staging_root->add_child(element_node); // fast!

        for (auto geom : item.geometry) {


            // 2. Create Mesh
            Ref<ArrayMesh> mesh;
            mesh.instantiate();

            Array arrays;
            arrays.resize(Mesh::ARRAY_MAX);
            arrays[Mesh::ARRAY_VERTEX] = geom.vertices;
            arrays[Mesh::ARRAY_NORMAL] = geom.normals;
            arrays[Mesh::ARRAY_INDEX] = geom.indices;

            mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

            // 3. Material
            Ref<StandardMaterial3D> mat = _get_material(geom.color, geom.is_transparent);

            if (item.ifc_class == "IfcSpace") {
                mat->set_grow_enabled(true);
                mat->set_grow(-0.001);
            }
            // 4. Instance
            MeshInstance3D* mi = memnew(MeshInstance3D);
            mi->set_mesh(mesh);
            mi->set_surface_override_material(0, mat);

            // Collisions are safe here because element_node is not in the tree yet!
            if (this->should_create_collisions && !this->collision_classes.is_empty()) {

                if (this->collision_classes.has(item.ifc_class)) {

                    mi->create_trimesh_collision();
                }
            }

            element_node->add_child(mi);

        }


        current_generation_index++;

        // Time Slice Check
        // We still yield to let the engine render the Loading Spinner / UI
        uint64_t current_duration = Time::get_singleton()->get_ticks_usec() - start_time;
        if (current_duration > time_budget) {
            return; // Come back next frame
        }
    }

    // ---------------------------------------------------
    // FINALIZATION: THE "POP"
    // ---------------------------------------------------

    // We are done with the loop.
    // Now we add the massive invisible root to the actual scene tree.
    // This will happen in ONE FRAME.

    // Optional: Rename it to final name
    invisible_staging_root->set_name("IFCModel");

    // The Big Reveal
    add_child(invisible_staging_root, true);

    // Cleanup
    invisible_staging_root = nullptr; // Clear our pointer
    current_state = DONE;

    // File read, signal emitted
    emit_signal("ifc_read");

    set_process(false);

    UtilityFunctions::print("IFC Fully Loaded and Revealed!");
    UtilityFunctions::print(Time::get_singleton()->get_ticks_usec());
}

// ---------------------------------------------------------
// MATERIAL CACHING (Optimization)
// ---------------------------------------------------------
Ref<StandardMaterial3D> GDIFCManager::_get_material(Color color, bool transparent) {
    // Create a unique key string for the map
    String key = String(color.to_html()) + (transparent ? "_T" : "_O");

    if (material_cache.has(key)) {
        return material_cache[key];
    }

    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_albedo(color);

    if (transparent) {
        mat->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
        // Add specific transparent logic from your old code here if needed
    }

    material_cache[key] = mat;
    return mat;
}


godot::Variant to_godot_variant(const AttributeValue& attr_value) {
    // We use a lambda as the visitor, leveraging C++17 'if constexpr' for type dispatch.
    return attr_value.apply_visitor([&](auto&& arg) -> Variant {
        // Decay the type to handle const/reference correctly
        using T = std::decay_t<decltype(arg)>;

        // 1. PRIMITIVE TYPES
        if constexpr (std::is_same_v<T, int>) {
            return Variant(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            return Variant(arg);
        } else if constexpr (std::is_same_v<T, std::string>) {
            // Note: Godot's String type is usually a dedicated type, but here we pass the C++ string
            return Variant(godot::String(arg.c_str()));
        } else if constexpr (std::is_same_v<T, bool>) {
            return Variant(arg);
        }

        // 2. IFC/BOOST SPECIFIC TYPES
        else if constexpr (std::is_same_v<T, boost::tribool>) {
            // FIX: Compare to bool 'true' and 'false', not int '1' and '0'
            if (arg == true) {
                return Variant("TRUE");
            }
            if (arg == false) {
                return Variant("FALSE");
            }
            return Variant("UNKNOWN");
        } else if constexpr (std::is_same_v<T, EnumerationReference>) {
            // Enumerations are best represented as Godot Strings
            return Variant(godot::String(arg.value()));
        } else if constexpr (std::is_convertible_v<T, IfcUtil::IfcBaseClass*>) {
            // IFC Entity Instance pointer -> Should be wrapped in a Godot Object
            // A helper function (e.g., entity_to_godot_object) would be called here.
            // For the mockup, we return a Variant initialized with the pointer.
            return Variant(static_cast<IfcUtil::IfcBaseClass*>(arg));
        }

        // 3. AGGREGATE (VECTOR) TYPES
        else if constexpr (
            std::is_same_v<T, std::vector<int>> ||
            std::is_same_v<T, std::vector<double>>) {
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
            std::is_same_v<T, empty_aggregate_of_aggregate_t>) {
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


template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID) {
    godot::Dictionary psets;

    // 1. Early exit validation
    auto instance = file.instance_by_id(expressID);
    if (!instance) {
        return psets;
    }

    auto object = instance->template as<typename schema::IfcObject>();
    if (!object) {
        return psets;
    }

    // 2. Get the list of all relationships (Generic list)
    // IfcOpenShell: IsDefinedBy returns a aggregate/list, not a single object
    auto rels_defines = object->IsDefinedBy();
    if (!rels_defines) {
        return psets;
    }

    // 3. Iterate generic relationships
    for (auto rel_generic : *rels_defines) {
        // OPTIMIZATION: Single dynamic_cast capture
        // Check if this specific relationship is "DefinesByProperties"
        if (auto rel = rel_generic->template as<typename schema::IfcRelDefinesByProperties>()) {
            auto p_set_select = rel->RelatingPropertyDefinition();
            if (!p_set_select) {
                continue;
            }

            // Check if it is actually an IfcPropertySet (could be ElementQuantity, etc.)
            if (auto p_set = p_set_select->template as<typename schema::IfcPropertySet>()) {
                auto p_set_name_opt = p_set->Name();
                if (!p_set_name_opt.has_value()) {
                    continue;
                }

                std::string pset_key = p_set_name_opt.value();

                auto props = p_set->HasProperties();
                if (!props) {
                    continue;
                }

                godot::Dictionary props_dict;

                for (auto prop : *props) {
                    if (!prop) {
                        continue;
                    }

                    // Cache the property name to avoid repeated lookups
                    // Godot Dictionaries use Variants as keys. Passing a const char* // creates a String automatically.
                    std::string prop_name_str = prop->Name();
                    const char* prop_key = prop_name_str.c_str();

                    // OPTIMIZATION: Order by probability
                    // IfcPropertySingleValue is the vast majority (90%+) of cases. Check it first.

                    if (auto p_single = prop->template as<typename schema::IfcPropertySingleValue>()) {
                        // Handle Single Value
                        if (auto n_value = p_single->NominalValue()) {
                            // Determine underlying primitive via generic access
                            if (auto final_value = n_value->template as<IfcUtil::IfcBaseClass>()) {
                                props_dict[prop_key] = to_godot_variant(final_value->get_attribute_value(0));
                            } else {
                                props_dict[prop_key] = "[Invalid Value]";
                            }
                        } else {
                            props_dict[prop_key] = godot::Variant(); // Null/Nil
                        }
                    } else if (auto p_bounded = prop->template as<typename schema::IfcPropertyBoundedValue>()) {
                        // Handle Bounded Value
                        godot::Array bounds;

                        // Lower
                        if (auto lb = p_bounded->LowerBoundValue()) {
                            if (auto val = lb->template as<IfcUtil::IfcBaseClass>()) {
                                bounds.push_back(to_godot_variant(val->get_attribute_value(0)));
                            }
                        } else {
                            bounds.push_back(godot::Variant());
                        }

                        // Upper
                        if (auto ub = p_bounded->UpperBoundValue()) {
                            if (auto val = ub->template as<IfcUtil::IfcBaseClass>()) {
                                bounds.push_back(to_godot_variant(val->get_attribute_value(0)));
                            }
                        } else {
                            bounds.push_back(godot::Variant());
                        }

                        props_dict[prop_key] = bounds;
                    } else if (auto p_enum = prop->template as<typename schema::IfcPropertyEnumeratedValue>()) {
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
                    } else if (auto p_list = prop->template as<typename schema::IfcPropertyListValue>()) {
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
                    } else if (auto p_ref = prop->template as<typename schema::IfcPropertyReferenceValue>()) {
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


godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID) {

    godot::Dictionary attributes;

    auto instance = file.instance_by_id(expressID);
    if (!instance) {
        return attributes;
    }

    auto object = instance->as<typename Ifc4::IfcObject>();
    if (!object) {
        return attributes;
    }

    attributes["GlobalId"] = object->GlobalId().c_str();
    attributes["Name"] = object->Name()->c_str();
    attributes["Description"] = object->Description()->c_str();

    return attributes;
}