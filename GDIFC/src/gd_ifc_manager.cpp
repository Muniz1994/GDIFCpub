#include "gd_ifc_manager.h"
#include <algorithm> // Required for std::sort
#include <functional> // Required for lambdas

using namespace godot;

void GDIFCManager::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path", "create_collision", "collision_classes"), &GDIFCManager::read_ifc, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("_thread_task", "path"), &GDIFCManager::_thread_task);
    ClassDB::bind_method(D_METHOD("get_gdifc_settings"), &GDIFCManager::get_gdifc_settings);
    ClassDB::bind_method(D_METHOD("set_gdifc_settings","gdifc_settings"), &GDIFCManager::set_gdifc_settings);

    ADD_SIGNAL(MethodInfo("ifc_read"));
}

GDIFCManager::GDIFCManager() = default;

GDIFCManager::~GDIFCManager() = default;

Error GDIFCManager::read_ifc(const String &_path, bool _create_collision, const Array &_collision_classes) {

    UtilityFunctions::print_rich("[color=blue]Started loading IFC");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    ERR_FAIL_COND_V_MSG((current_state != IDLE && current_state != DONE),Error::FAILED,"Already loading!");

    // Reset
    this->should_create_collisions = _create_collision;
    this->collision_classes = _collision_classes;
    this->generation_queue.clear();
    this->material_cache.clear();
    this->current_generation_index = 0;
    this->current_state = LOADING_THREAD;

    // Start Thread
    Callable callable = Callable(this, "_thread_task").bind(_path);
    task_id = WorkerThreadPool::get_singleton()->add_task(callable, true); // High Priority
    set_process(true);
    return Error::OK;
}

// ---------------------------------------------------------
// MAIN THREAD PROCESS (Unchanged, for context)
// ---------------------------------------------------------
void GDIFCManager::_process(double delta) {

    if (current_state == LOADING_THREAD) {
        if (WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
            WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
            task_id = -1;
            invisible_staging_root = memnew(Node3D);
            invisible_staging_root->set_name("IFC_Staging_Area");
            current_state = GENERATING_NODES;
        }
    } else if (current_state == GENERATING_NODES) {
        _process_generation_queue();
    }
}
// ---------------------------------------------------------
// BACKGROUND THREAD
// ---------------------------------------------------------
void GDIFCManager::_thread_task(const String& _path) {

    // 1. Load File
    auto temp_ifc_manager = std::make_unique<WEBIFCManager>(*geometric_settings);

    temp_ifc_manager->read_ifc_file(_path.utf8().get_data());

    auto schema = temp_ifc_manager->get_args();

    auto temp_file = std::make_unique<IfcParse::IfcFile>(_path.utf8().get_data());

    if (!temp_file->good()) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("Failed to load IFC file.");
    }

    // Schema check
    std::string current_schema = temp_file->schema()->name();

    bool is_supported_schema = (current_schema == Ifc2x3::get_schema().name()) ||
                               (current_schema == Ifc4::get_schema().name()) ||
                               (current_schema == Ifc4x3::get_schema().name()) ||
                               (current_schema == Ifc4x3_add2::get_schema().name());

    if (!is_supported_schema) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("Schema not supported.");
    }

    // [Assuming standard schema map logic here]
    std::unordered_map<std::string,int> schema_map{
        {Ifc4::get_schema().name(),0},
        {Ifc4x3::get_schema().name(),1},
        {Ifc2x3::get_schema().name(),2},
        {Ifc4x3_add2::get_schema().name(),3}
    };

    int schema_index = schema_map[current_schema];

    // 2. Init Geometry
    temp_ifc_manager->initialize_geometry_processor();

    Vector<PrecalculatedIFCItem> temp_queue;

    // --- BUILD HIERARCHY MAP ---
    std::unordered_map<int, int> parent_map;

    switch (schema_index) {
        case 0: parent_map = build_spatial_hierarchy<Ifc4>(*temp_file); break;
        case 1: parent_map = build_spatial_hierarchy<Ifc4x3>(*temp_file); break;
        case 2: parent_map = build_spatial_hierarchy<Ifc2x3>(*temp_file); break;
        case 3: parent_map = build_spatial_hierarchy<Ifc4x3_add2>(*temp_file); break;
        default: ;
    }

    std::unordered_set<int> active_parents;
    for (const auto& pair : parent_map) active_parents.insert(pair.second);

    std::unordered_set<int> processed_ids;

    // --- PRE-PROCESS SPATIAL STRUCTURE ---

    // PHASE A: PRIMARY STRUCTURE (Project, Site, Building, Storey)
    std::vector<std::string> spatial_types = {"IfcProject", "IfcSite", "IfcBuilding", "IfcBuildingStorey"};

    for (const auto& s_type : spatial_types) {
        auto instances = temp_file->instances_by_type(s_type);
        if (!instances) continue;

        for(int i=0; i<instances->size(); i++) {
            auto ent = instances->operator[](i);
            int id = ent->id();

            if (processed_ids.find(id) != processed_ids.end()) continue;

            PrecalculatedIFCItem item;
            item.valid = true;
            item.express_id = id;
            item.node_name = String(s_type.c_str()) + "_" + String::num_int64(id);
            item.ifc_class = s_type.c_str();

            // ... (Attribute/Property loading omitted for brevity - same as before) ...
            switch (schema_index)
            {
                case 0: item.attributes = get_ifc_object_attributes<Ifc4>(*temp_file, id); break;
                case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, id); break;
                case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, id); break;
                case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, id); break;
                default: ;
            }

            switch (schema_index)
            {
                case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, id); break;
                case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, id); break;
                case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, id); break;
                case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, id); break;
                default: ;
            }

            if (parent_map.find(id) != parent_map.end()) {
                item.parent_id = parent_map[id];
            }

            // [FIX START] Extract Geometry for Spatial Elements (IfcSite Terrain)
            // ------------------------------------------------------------------
            item.geometry = {};
            auto flat_mesh = temp_ifc_manager->geometry_loader->GetFlatMesh(id);

            // Only process if geometry actually exists (IfcProject will usually skip this)
            if (!flat_mesh.geometries.empty()) {
                for (auto& geom_data : flat_mesh.geometries) {
                    PrecalculatedIFCItemGeometry item_geometry;
                    auto ifc_geometry = temp_ifc_manager->geometry_loader->GetGeometry(geom_data.geometryExpressID);

                    if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

                    // Resize & Fill Vertices/Normals/Indices (Standard logic)
                    int v_off = item_geometry.vertices.size();
                    int i_off = item_geometry.indices.size();

                    item_geometry.vertices.resize(v_off + ifc_geometry.numPoints);
                    item_geometry.normals.resize(v_off + ifc_geometry.numPoints);
                    item_geometry.indices.resize(i_off + (ifc_geometry.numFaces * 3));

                    for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
                        glm::dvec4 tv = geom_data.transformation * glm::dvec4(ifc_geometry.GetPoint(k), 1.0);
                        item_geometry.vertices[v_off + k] = Vector3(tv.x, tv.y, tv.z);
                    }
                    for (uint32_t k = 0; k < ifc_geometry.numFaces; k++) {
                        bimGeometry::Face f = ifc_geometry.GetFace(k);
                        item_geometry.indices[i_off + k*3+0] = v_off + f.i2;
                        item_geometry.indices[i_off + k*3+1] = v_off + f.i1;
                        item_geometry.indices[i_off + k*3+2] = v_off + f.i0;

                        // Simple flat normals calculation
                        Vector3 p0 = item_geometry.vertices[v_off + f.i0];
                        Vector3 p1 = item_geometry.vertices[v_off + f.i1];
                        Vector3 p2 = item_geometry.vertices[v_off + f.i2];
                        Vector3 n = (p1 - p0).cross(p2 - p0).normalized();
                        item_geometry.normals[v_off + f.i0] = n;
                        item_geometry.normals[v_off + f.i1] = n;
                        item_geometry.normals[v_off + f.i2] = n;
                    }

                    item_geometry.color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
                    item_geometry.is_transparent = (geom_data.color.a < 0.7);

                    if (item_geometry.vertices.size() > 0) item.geometry.push_back(item_geometry);
                }
            }
            // [FIX END] ---------------------------------------------------------

            temp_queue.push_back(item);
            processed_ids.insert(id);
        }
    }

    // PHASE B: SECONDARY PARENTS (Assemblies, etc.)
    // ... (This section remains identical to your previous code) ...
    std::vector<int> other_parents;
    for (int p_id : active_parents) {
        if (processed_ids.find(p_id) == processed_ids.end()) other_parents.push_back(p_id);
    }
    std::sort(other_parents.begin(), other_parents.end());

    for (int id : other_parents) {
        auto ent = temp_file->instance_by_id(id);
        if (!ent) continue;

        std::string s_type = ent->declaration().name();

        // Note: You can technically add the geometry extraction block here too if you expect Assemblies to have direct geometry.
        // For brevity, assuming B-Phase parents are mostly empty containers.
        PrecalculatedIFCItem item;
        item.valid = true;
        item.express_id = id;
        item.node_name = String(s_type.c_str()) + "_" + String::num_int64(id);
        item.ifc_class = s_type.c_str();

        switch (schema_index)
        {
            case 0: item.attributes = get_ifc_object_attributes<Ifc4>(*temp_file, id); break;
            case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, id); break;
            case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, id); break;
            case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, id); break;
            default: ;
        }

        switch (schema_index)
        {
            case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, id); break;
            case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, id); break;
            case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, id); break;
            case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, id); break;
            default: ;
        }

        if (parent_map.find(id) != parent_map.end()) {
            item.parent_id = parent_map[id];
        }

        temp_queue.push_back(item);
        processed_ids.insert(id);
    }

    // 3. HEAVY LOOP: Process everything else
    for (auto type : temp_ifc_manager->schemaManager.GetIfcElementList()) {
        auto expressIDs = temp_ifc_manager->loader->GetExpressIDsWithType(type);
        for (uint32_t expressID : expressIDs) {

            if (processed_ids.find(expressID) != processed_ids.end()) continue;

            std::string type_str = temp_ifc_manager->schemaManager.IfcTypeCodeToType(temp_ifc_manager->loader->GetLineType(expressID));
            if (type_str == "IfcOpeningElement") continue;

            auto flat_mesh = temp_ifc_manager->geometry_loader->GetFlatMesh(expressID);
            bool has_geometry = !flat_mesh.geometries.empty();
            bool is_container_node = (active_parents.find(expressID) != active_parents.end());

            if (!has_geometry && !is_container_node) continue;

            PrecalculatedIFCItem item;
            item.valid = true;
            item.express_id = expressID;
            item.node_name = String(type_str.c_str()) + "_" + String::num_int64(expressID);
            item.ifc_class = type_str.c_str();

            switch (schema_index)
            {
                case 0: item.attributes = get_ifc_object_attributes<Ifc4>(*temp_file, expressID); break;
                case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, expressID); break;
                case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, expressID); break;
                case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, expressID); break;
                default: ;
            }

            switch (schema_index)
            {
                case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, expressID); break;
                case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, expressID); break;
                case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, expressID); break;
                case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, expressID); break;
                default: ;
            }

            if (parent_map.count(expressID)) item.parent_id = parent_map[expressID];

            // Geometry (Standard Loop)
            item.geometry = {};
            for (auto& geom_data : flat_mesh.geometries) {
                PrecalculatedIFCItemGeometry item_geometry;
                auto ifc_geometry = temp_ifc_manager->geometry_loader->GetGeometry(geom_data.geometryExpressID);
                if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

                int v_off = item_geometry.vertices.size();
                int i_off = item_geometry.indices.size();
                item_geometry.vertices.resize(v_off + ifc_geometry.numPoints);
                item_geometry.normals.resize(v_off + ifc_geometry.numPoints);
                item_geometry.indices.resize(i_off + (ifc_geometry.numFaces * 3));

                for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
                    glm::dvec4 tv = geom_data.transformation * glm::dvec4(ifc_geometry.GetPoint(k), 1.0);
                    item_geometry.vertices[v_off + k] = Vector3(tv.x, tv.y, tv.z);
                }
                for (uint32_t k = 0; k < ifc_geometry.numFaces; k++) {
                    bimGeometry::Face f = ifc_geometry.GetFace(k);
                    item_geometry.indices[i_off + k*3+0] = v_off + f.i2;
                    item_geometry.indices[i_off + k*3+1] = v_off + f.i1;
                    item_geometry.indices[i_off + k*3+2] = v_off + f.i0;

                    Vector3 p0 = item_geometry.vertices[v_off + f.i0];
                    Vector3 p1 = item_geometry.vertices[v_off + f.i1];
                    Vector3 p2 = item_geometry.vertices[v_off + f.i2];
                    Vector3 n = (p1 - p0).cross(p2 - p0).normalized();
                    item_geometry.normals[v_off + f.i0] = n;
                    item_geometry.normals[v_off + f.i1] = n;
                    item_geometry.normals[v_off + f.i2] = n;
                }
                item_geometry.color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
                if (type_str == "IfcSpace") {
                    item_geometry.is_transparent = true;
                    item_geometry.color = Color(0.025, 0.037, 0.034, 0.1);
                } else {
                    item_geometry.is_transparent = (geom_data.color.a < 0.7);
                }

                if (item_geometry.vertices.size() > 0) item.geometry.push_back(item_geometry);
            }

            if (!item.geometry.empty() || is_container_node) {
                temp_queue.push_back(item);
            }
        }
    }

    // =========================================================
    // [PREVIOUS FIX] TOPOLOGICAL SORT & ORPHAN RESCUE
    // =========================================================

    // 1. Orphan Rescue
    int project_id = -1;
    for (const auto& item : temp_queue) { if (item.ifc_class == "IfcProject") { project_id = item.express_id; break; } }

    if (project_id > 0) {
        for (int i = 0; i < temp_queue.size(); i++) {
            if (temp_queue[i].ifc_class == "IfcSite" && temp_queue[i].parent_id <= 0) {
                temp_queue.write[i].parent_id = project_id;
                parent_map[temp_queue[i].express_id] = project_id;
            }
        }
    }

    // 2. Depth Sort
    std::unordered_map<int, int> depth_cache;
    std::function<int(int)> get_depth = [&](int id) -> int {
        if (id <= 0) return 0;
        if (depth_cache.count(id)) return depth_cache[id];
        if (parent_map.find(id) == parent_map.end()) { depth_cache[id] = 0; return 0; }
        int p = parent_map[id];
        if (p == 0 || p == id) { depth_cache[id] = 0; return 0; }
        int d = 1 + get_depth(p);
        depth_cache[id] = d;
        return d;
    };

    PrecalculatedIFCItem* ptr = temp_queue.ptrw();
    std::sort(ptr, ptr + temp_queue.size(), [&](const PrecalculatedIFCItem& a, const PrecalculatedIFCItem& b) {
        int da = get_depth(a.express_id);
        int db = get_depth(b.express_id);
        if (da != db) return da < db;
        return a.express_id < b.express_id;
    });

    // Commit results
    this->web_ifc_manager = std::move(temp_ifc_manager);
    this->ifc_parse_file = std::move(temp_file);
    this->generation_queue = temp_queue;
}

// ---------------------------------------------------------
// DUMB & FAST GENERATOR
// ---------------------------------------------------------
void GDIFCManager::_process_generation_queue() {


    uint64_t start_time = Time::get_singleton()->get_ticks_usec();
    uint64_t time_budget = 20000;

    while (current_generation_index < generation_queue.size()) {

        const PrecalculatedIFCItem& item = generation_queue[current_generation_index];

        // 1. Create Node
        IFCNode* element_node = memnew(IFCNode);
        element_node->set_name(item.node_name);
        element_node->set_properties(item.properties);
        element_node->set_attributes(item.attributes);
        element_node->set_ifc_class(item.ifc_class);

        // 2. REGISTER NODE
        if (item.express_id > 0) {
            node_registry[item.express_id] = element_node;
        }

        // 3. PARENTING LOGIC
        if (item.parent_id > 0 && node_registry.has(item.parent_id)) {
            Node* parent_node = node_registry[item.parent_id];
            parent_node->add_child(element_node);
        }
        else {
            invisible_staging_root->add_child(element_node);
        }

        // 2. Create Mesh
        Ref<ArrayMesh> mesh;
        mesh.instantiate();



        // [NOTE] This loop handles cases where geometry is empty (Containers) by doing nothing
        // but leaving the element_node in the tree as a parent for future children.
        for (int i = 0; i < item.geometry.size(); i++) {

            Array arrays;
            arrays.resize(Mesh::ARRAY_MAX);
            arrays[Mesh::ARRAY_VERTEX] = item.geometry[i].vertices;
            arrays[Mesh::ARRAY_NORMAL] = item.geometry[i].normals;
            arrays[Mesh::ARRAY_INDEX] = item.geometry[i].indices;

            mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

            // 3. Material
            Ref<StandardMaterial3D> mat = _get_material(item.geometry[i].color, item.geometry[i].is_transparent);

            mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);

            mesh->surface_set_material(i,mat);


            if (item.ifc_class == "IfcSpace") {
                mat->set_grow_enabled(true);
                mat->set_grow(-0.001);
            }

        }


        if (mesh->get_surface_count() > 0) {
            MeshInstance3D* mi = memnew(MeshInstance3D);
            mi->set_mesh(mesh);
            mi->set_name("object_geometry");

            if (this->should_create_collisions && !this->collision_classes.is_empty()) {
                if (this->collision_classes.has(item.ifc_class)) {
                    mi->create_trimesh_collision();
                }
            } else if (this->should_create_collisions && this->collision_classes.is_empty()) {
                mi->create_trimesh_collision();
            }

            element_node->add_child(mi);
        }



        element_node->add_to_group(item.ifc_class,true);

        current_generation_index++;

        uint64_t current_duration = Time::get_singleton()->get_ticks_usec() - start_time;
        if (current_duration > time_budget) {
            return;
        }
    }

    if (current_generation_index >= generation_queue.size()) {
        node_registry.clear();
        invisible_staging_root->set_name("IFCModel");
        add_child(invisible_staging_root, true);
        invisible_staging_root = nullptr;
        current_state = DONE;
        emit_signal("ifc_read");
        set_process(false);
        UtilityFunctions::print("IFC Fully Loaded and Revealed!");
        UtilityFunctions::print((Time::get_singleton()->get_ticks_usec() - start_loading_time)/1000000," secs to load");
    }
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


GeorreferenceData GDIFCManager::get_georreference_data() {
    return georreference;
}


void GDIFCManager::set_georreference_data(GeorreferenceData data) {
    georreference = data;
}

Ref<GDIFCLoaderSettings> GDIFCManager::get_gdifc_settings() {
    return geometric_settings;
}

void GDIFCManager::set_gdifc_settings(Ref<GDIFCLoaderSettings> gdifc_settings) {

    geometric_settings = gdifc_settings;
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

template <typename schema>
godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID) {

    godot::Dictionary attributes;

    auto instance = file.instance_by_id(expressID);

    if (!instance) {
        return attributes;
    }

    auto object = instance->as<typename schema::IfcObject>();
    if (!object) {
        return attributes;
    }

    auto attrs = object->declaration().as_entity()->all_attributes();
    auto iter = attrs.begin();
    size_t idx = 0;

    for (; iter != attrs.end(); ++iter, ++idx) {
        attributes[(*iter)->name().c_str()] = convert_attribute_value(object->get_attribute_value(idx));
    }


    return attributes;
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
godot::GeorreferenceData get_georreference(IfcParse::IfcFile &file) {
    if (std::is_same_v<schema,Ifc2x3>)
    {return {};}
    else {
        aggregate_of_instance project_list = *file.instances_by_type("IfcMapConversion");

        IfcUtil::IfcBaseClass* obj;

        if (0 < project_list.size()) {
            obj = project_list[0];

        }
        else {
            return {};
        }

        auto map_conversion = obj->template as<typename schema::IfcMapConversion>();

        auto projected_crs = map_conversion->TargetCRS();

        return GeorreferenceData(MapConversion{
                                     (int32_t)map_conversion->Eastings(),
                                     (int32_t)map_conversion->Northings(),
                                     (int32_t)map_conversion->OrthogonalHeight(),
                                     (int32_t)map_conversion->XAxisAbscissa().value_or(0),
                                     (int32_t)map_conversion->XAxisOrdinate().value_or(0),
                                     (int16_t)map_conversion->Scale().value_or(0)},
                                 ProjectedCRS{
                                     projected_crs->Name().c_str(),
                                     projected_crs->Description().value_or("").c_str(),
                                     projected_crs->GeodeticDatum().value_or("").c_str(),
                                     projected_crs->VerticalDatum().value_or("").c_str()}
        );



    }
}

template <typename schema>
std::unordered_map<int, int> build_spatial_hierarchy(IfcParse::IfcFile &file) {
    std::unordered_map<int, int> child_to_parent;

    // 1. Handle Aggregation (Site -> Building -> Storey)
    // Uses IfcRelAggregates
    auto rel_aggregates = file.instances_by_type("IfcRelAggregates");
    if (rel_aggregates) {
        for (int i = 0; i < rel_aggregates->size(); i++) {
            auto rel = rel_aggregates->operator[](i)->as<typename schema::IfcRelAggregates>(); // cast works for 2x3 too usually due to inheritance or use generic base
            if (!rel) continue;

            auto parent = rel->RelatingObject();
            auto children = rel->RelatedObjects();

            if (parent && children) {
                int parent_id = parent->id();
                for (auto child : *children) {
                    child_to_parent[child->id()] = parent_id;
                }
            }
        }
    }

    // 2. Handle Spatial Containment (Storey -> Wall/Window/etc)
    // Uses IfcRelContainedInSpatialStructure
    auto rel_contained = file.instances_by_type("IfcRelContainedInSpatialStructure");
    if (rel_contained) {
        for (int i = 0; i < rel_contained->size(); i++) {
            auto rel = rel_contained->operator[](i)->as<typename schema::IfcRelContainedInSpatialStructure>();
            if (!rel) continue;

            auto parent = rel->RelatingStructure(); // The Spatial Structure (e.g. Storey)
            auto children = rel->RelatedElements(); // The Physical Elements (e.g. Wall)

            if (parent && children) {
                int parent_id = parent->id();
                for (auto child : *children) {
                    child_to_parent[child->id()] = parent_id;
                }
            }
        }
    }

    auto rel_nests = file.instances_by_type("IfcRelNests");
    if (rel_nests) {
        for (int i = 0; i < rel_nests->size(); i++) {
            auto rel = rel_nests->operator[](i)->as<typename schema::IfcRelNests>();
            if (!rel) continue;

            auto parent = rel->RelatingObject(); // The Spatial Structure (e.g. Storey)
            auto children = rel->RelatedObjects(); // The Physical Elements (e.g. Wall)

            if (parent && children) {
                int parent_id = parent->id();
                for (auto child : *children) {
                    child_to_parent[child->id()] = parent_id;
                }
            }
        }
    }

    if (schema::get_schema().name() == Ifc4x3_add2::get_schema().name()) {
        auto rel_adheres = file.instances_by_type("IfcRelAdheresToElement");

        if (rel_adheres) {
            for (int i = 0; i < rel_adheres->size(); i++) {
                auto rel = rel_adheres->operator[](i)->as<typename Ifc4x3_add2::IfcRelAdheresToElement>();
                if (!rel) continue;

                auto parent = rel->RelatingElement(); // The Spatial Structure (e.g. Storey)
                auto children = rel->RelatedSurfaceFeatures(); // The Physical Elements (e.g. Wall)

                if (parent && children) {
                    int parent_id = parent->id();
                    for (auto child : *children) {
                        child_to_parent[child->id()] = parent_id;
                    }
                }
            }
        }

    }


    return child_to_parent;
}
