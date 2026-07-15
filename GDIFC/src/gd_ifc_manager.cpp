#include "gd_ifc_manager.h"
#include <algorithm> // Required for std::sort
#include <functional> // Required for lambdas
#include <stdexcept>  // Required for std::exception

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/scene_tree.hpp"

using namespace godot;

void GDIFCManager::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path", "create_collision", "collision_classes"), &GDIFCManager::read_ifc, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("read_ifc_base64", "base64_data", "create_collision", "collision_classes"), &GDIFCManager::read_ifc_base64, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("_thread_task"), &GDIFCManager::_thread_task);
    ClassDB::bind_method(D_METHOD("_metadata_thread_task"), &GDIFCManager::_metadata_thread_task);
    ClassDB::bind_method(D_METHOD("get_gdifc_settings"), &GDIFCManager::get_gdifc_settings);
    ClassDB::bind_method(D_METHOD("set_gdifc_settings","gdifc_settings"), &GDIFCManager::set_gdifc_settings);

    ClassDB::bind_method(D_METHOD("get_ifc_file_path"), &GDIFCManager::get_ifc_file_path);
    ClassDB::bind_method(D_METHOD("set_ifc_file_path","path"), &GDIFCManager::set_ifc_file_path);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "ifc_file_path", PROPERTY_HINT_GLOBAL_FILE, "*.ifc"),
        "set_ifc_file_path", "get_ifc_file_path");

    ClassDB::bind_method(D_METHOD("get_ifc_model"), &GDIFCManager::get_ifc_model);
    ClassDB::bind_method(D_METHOD("get_node_by_global_id","global_id"), &GDIFCManager::get_node_by_global_id);
    ClassDB::bind_method(D_METHOD("get_ifc_object_by_global_id","global_id"), &GDIFCManager::get_ifc_object_by_global_id);
    ClassDB::bind_method(D_METHOD("get_elements_by_class","ifc_class"), &GDIFCManager::get_elements_by_class);

    ADD_SIGNAL(MethodInfo("ifc_read"));
    ADD_SIGNAL(MethodInfo("ifc_objects_ready"));
}

GDIFCManager::GDIFCManager() {
    geometric_settings.instantiate();
}

GDIFCManager::~GDIFCManager() = default;

// ---------------------------------------------------------
// COLD RELOAD: detect saved IFC tree and re-parse metadata
// ---------------------------------------------------------
void GDIFCManager::_ready() {
    if (ifc_file_path_.is_empty()) return;

    // If "IFCModel" child already exists, this is a cold reload
    Node* model_child = nullptr;
    for (int i = 0; i < get_child_count(); i++) {
        if (get_child(i)->get_name() == StringName("IFCModel")) {
            model_child = get_child(i);
            break;
        }
    }
    if (!model_child) return;

    // Point ifc_model_node_ at the existing IFCModel
    ifc_model_node_ = Object::cast_to<IFCModel>(model_child);

    UtilityFunctions::print_rich("[color=blue]Cold reload detected — re-parsing IFC metadata from: " + ifc_file_path_);

    relink_failed_ = false;
    current_state = LOADING_THREAD;
    Callable callable = Callable(this, "_metadata_thread_task");
    task_id = WorkerThreadPool::get_singleton()->add_task(callable, true);
    set_process(true);
}

// ---------------------------------------------------------
// METADATA-ONLY THREAD TASK (no geometry, no web-ifc)
// ---------------------------------------------------------
void GDIFCManager::_metadata_thread_task() {
    // Check file existence
    if (!FileAccess::file_exists(ifc_file_path_)) {
        relink_failed_ = true;
        return;
    }

    // Read file into memory
    Ref<FileAccess> fa = FileAccess::open(ifc_file_path_, FileAccess::READ);
    if (!fa.is_valid()) {
        relink_failed_ = true;
        return;
    }
    PackedByteArray buffer = fa->get_buffer(fa->get_length());
    fa.unref();

    if (buffer.is_empty()) {
        relink_failed_ = true;
        return;
    }

    // Parse with IfcParse only (no web-ifc geometry)
    try {
        auto temp_file = std::make_shared<IfcParse::IfcFile>(
            (void*)buffer.ptr(), static_cast<int>(buffer.size()));
        if (!temp_file->good()) {
            relink_failed_ = true;
            return;
        }
        ifc_parse_file = std::move(temp_file);
        relink_failed_ = false;
    } catch (...) {
        relink_failed_ = true;
    }
}

// ---------------------------------------------------------
// RELINK: walk existing IFCNode tree and restore ifc_object_
// ---------------------------------------------------------
void GDIFCManager::_relink_ifc_objects() {
    if (!ifc_parse_file) return;

    Node* model_root = nullptr;
    for (int i = 0; i < get_child_count(); i++) {
        if (get_child(i)->get_name() == StringName("IFCModel")) {
            model_root = get_child(i);
            break;
        }
    }
    if (!model_root) return;

    // Initialise the IFCModel node with the loaded file
    if (ifc_model_node_ && ifc_parse_file) {
        ifc_model_node_->init(ifc_parse_file);
    }

    node_registry.clear();

    std::function<void(Node*)> walk = [&](Node* n) {
        IFCNode* ifc_node = Object::cast_to<IFCNode>(n);
        if (ifc_node) {
            String gid = ifc_node->get_global_id();
            if (!gid.is_empty()) {
                try {
                    auto* inst = ifc_parse_file->instance_by_guid(
                        std::string(gid.utf8().get_data()));
                    if (inst) {
                        ifc_node->set_ifc_object(GDIFCEntityBase::wrap(inst, ifc_parse_file));
                        node_registry[static_cast<int>(inst->id())] = ifc_node;
                    }
                } catch (...) {}
            }
        }
        for (int i = 0; i < n->get_child_count(); i++) {
            walk(n->get_child(i));
        }
    };
    walk(model_root);
}

Error GDIFCManager::read_ifc(const String &_path, bool _create_collision, const Array &_collision_classes) {

    UtilityFunctions::print_rich("[color=blue]Started loading IFC");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    // Store path for cold-reload persistence
    ifc_file_path_ = _path;

    ERR_FAIL_COND_V_MSG((current_state != IDLE && current_state != DONE),Error::FAILED,"Already loading!");

    // Read the file into the shared buffer
    Ref<FileAccess> fa = FileAccess::open(_path, FileAccess::READ);
    if (!fa.is_valid()) {
        ERR_FAIL_V_MSG(Error::FAILED, String("Failed to open IFC file: ") + _path
                       + String(" (error: ") + String::num_int64((int64_t)FileAccess::get_open_error()) + String(")"));
    }
    this->pending_buffer = fa->get_buffer(fa->get_length());
    fa.unref();

    ERR_FAIL_COND_V_MSG(this->pending_buffer.is_empty(), Error::FAILED, "IFC file is empty.");

    // Reset
    this->should_create_collisions = _create_collision;
    this->collision_classes = _collision_classes;
    this->generation_queue.clear();
    this->material_cache.clear();
    this->current_generation_index = 0;
    this->current_state = LOADING_THREAD;

    // Start Thread
    Callable callable = Callable(this, "_thread_task");
    task_id = WorkerThreadPool::get_singleton()->add_task(callable, true);
    set_process(true);
    return Error::OK;
}

Error GDIFCManager::read_ifc_base64(const String &_base64_data, bool _create_collision, const Array &_collision_classes) {

    UtilityFunctions::print_rich("[color=blue]Started loading IFC from base64");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    ERR_FAIL_COND_V_MSG((current_state != IDLE && current_state != DONE), Error::FAILED, "Already loading!");
    ERR_FAIL_COND_V_MSG(_base64_data.is_empty(), Error::FAILED, "Base64 data is empty.");

    // Decode base64 into the shared buffer
    this->pending_buffer = Marshalls::get_singleton()->base64_to_raw(_base64_data);

    ERR_FAIL_COND_V_MSG(this->pending_buffer.is_empty(), Error::FAILED, "Failed to decode base64 data.");

    // Reset
    this->should_create_collisions = _create_collision;
    this->collision_classes = _collision_classes;
    this->generation_queue.clear();
    this->material_cache.clear();
    this->current_generation_index = 0;
    this->current_state = LOADING_THREAD;

    // Start Thread
    Callable callable = Callable(this, "_thread_task");
    task_id = WorkerThreadPool::get_singleton()->add_task(callable, true);
    set_process(true);
    return Error::OK;
}

// ---------------------------------------------------------
// MAIN THREAD PROCESS
// ---------------------------------------------------------
void GDIFCManager::_process(double delta) {

    if (current_state == LOADING_THREAD) {
        if (WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
            WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
            task_id = -1;

            // If we're doing a metadata-only reload, skip node generation
            if (relink_failed_) {
                // File was missing — warn and finish
                WARN_PRINT(String("GDIFCManager: IFC file not found at '") + ifc_file_path_
                    + "'. The model scene is intact but ifc_object properties are unavailable. "
                    + "Call read_ifc() with a valid path to restore them.");
                current_state = DONE;
                emit_signal("ifc_objects_ready");
                set_process(false);
            } else if (!generation_queue.is_empty()) {
                // Fresh load path — create staging root and generate nodes
                invisible_staging_root = memnew(IFCModel);
                invisible_staging_root->set_name("IFC_Staging_Area");
                ifc_model_node_ = Object::cast_to<IFCModel>(invisible_staging_root);
                current_state = GENERATING_NODES;
            } else {
                // Metadata-only reload — move to relinking
                current_state = RELINKING;
            }
        }
    } else if (current_state == GENERATING_NODES) {
        _process_generation_queue();
    } else if (current_state == RELINKING) {
        _relink_ifc_objects();
        current_state = DONE;
        emit_signal("ifc_objects_ready");
        set_process(false);
        UtilityFunctions::print("IFC objects relinked from saved scene.");
    } else if (current_state == FAILED) {
        set_process(false);
    }
}
// ---------------------------------------------------------
// BACKGROUND THREAD
// ---------------------------------------------------------

// Convert a GLM column-major 4×4 matrix to a Godot Transform3D.
// GLM column layout: m[col][row]; columns 0-2 are the axes, column 3 is translation.
static inline godot::Transform3D glm_to_godot_transform(const glm::dmat4& m) {
    return godot::Transform3D(
        godot::Basis(
            godot::Vector3((float)m[0].x, (float)m[0].y, (float)m[0].z),  // X axis
            godot::Vector3((float)m[1].x, (float)m[1].y, (float)m[1].z),  // Y axis
            godot::Vector3((float)m[2].x, (float)m[2].y, (float)m[2].z)   // Z axis
        ),
        godot::Vector3((float)m[3].x, (float)m[3].y, (float)m[3].z)       // origin
    );
}

void GDIFCManager::_thread_task() {

    const char* buf_ptr  = reinterpret_cast<const char*>(this->pending_buffer.ptr());
    int         buf_size = static_cast<int>(this->pending_buffer.size());

    // Load file [web-ifc] — from memory buffer
    auto temp_ifc_manager = std::make_unique<WEBIFCManager>(geometric_settings.ptr());
    try {
        temp_ifc_manager->read_ifc_file(buf_ptr, buf_size);
    } catch (const std::exception& e) {
        this->current_state = FAILED;
        ERR_FAIL_MSG(String("[web-ifc] Failed to read IFC file: ") + e.what());
    } catch (...) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("[web-ifc] Failed to read IFC file: unknown error.");
    }

    // Load file [IFCParse] — from memory buffer (void*, int constructor)
    std::shared_ptr<IfcParse::IfcFile> temp_file;
    try {
        temp_file = std::make_shared<IfcParse::IfcFile>((void*)buf_ptr, buf_size);
    } catch (const std::exception& e) {
        this->current_state = FAILED;
        ERR_FAIL_MSG(String("[IfcParse] Failed to parse IFC file: ") + e.what());
    } catch (...) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("[IfcParse] Failed to parse IFC file: unknown error.");
    }

    if (!temp_file->good()) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("Failed to parse IFC file.");
    }

    // Schema check [IFCParse]
    std::string current_schema = temp_file->schema()->name();

    bool is_supported_schema = (current_schema == Ifc2x3::get_schema().name()) ||
                               (current_schema == Ifc4::get_schema().name()) ||
                            //    (current_schema == Ifc4x3::get_schema().name()) ||
                               (current_schema == Ifc4x3_add2::get_schema().name());

    if (!is_supported_schema) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("Schema not supported.");
    }

    std::unordered_map<std::string,int> schema_map{
        {Ifc4::get_schema().name(),0},
        // {Ifc4x3::get_schema().name(),1},
        {Ifc2x3::get_schema().name(),2},
        {Ifc4x3_add2::get_schema().name(),3}
    };

    int schema_index = schema_map[current_schema];

    // Init Geometry [web-ifc]
    try {
        temp_ifc_manager->initialize_geometry_processor();
    } catch (const std::exception& e) {
        this->current_state = FAILED;
        ERR_FAIL_MSG(String("[web-ifc] Failed to initialize geometry processor: ") + e.what());
    } catch (...) {
        this->current_state = FAILED;
        ERR_FAIL_MSG("[web-ifc] Failed to initialize geometry processor: unknown error.");
    }

    Vector<PrecalculatedIFCItem> temp_queue;

    // Build hierarchy parent map (EXPRESSID, EXPRESSID), of elements based on its parent if they exist
    std::unordered_map<int, int> parent_map;

    try {
        switch (schema_index) {
            case 0: parent_map = build_spatial_hierarchy<Ifc4>(*temp_file); break;
            // case 1: parent_map = build_spatial_hierarchy<Ifc4x3>(*temp_file); break;
            case 2: parent_map = build_spatial_hierarchy<Ifc2x3>(*temp_file); break;
            case 3: parent_map = build_spatial_hierarchy<Ifc4x3_add2>(*temp_file); break;
            default: ;
        }
    } catch (const std::exception& e) {
        ERR_PRINT(String("[IfcParse] Failed to build spatial hierarchy: ") + e.what());
    } catch (...) {
        ERR_PRINT("[IfcParse] Failed to build spatial hierarchy: unknown error.");
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
            try {
            auto ent = instances->operator[](i);

            int id = ent->id();

            if (processed_ids.find(id) != processed_ids.end()) continue;

            PrecalculatedIFCItem item;
            item.valid = true;
            item.express_id = id;
            item.node_name = String(s_type.c_str()) + "_" + String::num_int64(id); // Godot demands an unique name per node at the same tree level
            item.ifc_class = s_type.c_str();

            //  Get attributes
            switch (schema_index)
            {
                case 0: item.attributes = get_ifc_object_attributes<Ifc4>(*temp_file, id); break;
                // case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, id); break;
                case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, id); break;
                case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, id); break;
                default: ;
            }

            //  Get psets
            switch (schema_index)
            {
                case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, id); break;
                // case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, id); break;
                case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, id); break;
                case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, id); break;
                default: ;
            }

            switch (schema_index)
            {
                case 0: item.quantities = get_ifc_quantity_sets<Ifc4>(*temp_file, id); break;
                // case 1: item.quantities = get_ifc_quantity_sets<Ifc4x3>(*temp_file, id); break;
                case 2: item.quantities = get_ifc_quantity_sets<Ifc2x3>(*temp_file, id); break;
                case 3: item.quantities = get_ifc_quantity_sets<Ifc4x3_add2>(*temp_file, id); break;
                default: ;
            }

            //  TODO: Get quantities


            // Extract GlobalId for scene persistence
            if (item.attributes.has("GlobalId")) {
                item.global_id = item.attributes["GlobalId"];
            }

            // Defines the parent of item
            if (parent_map.find(id) != parent_map.end()) {
                item.parent_id = parent_map[id];
            }

            // ------------------------------------------------------------------
            // Extract Geometry for Spatial Elements (IfcSite Terrain)
            // ------------------------------------------------------------------
            item.geometry = {};
            auto flat_mesh = temp_ifc_manager->geometry_loader->GetFlatMesh(id);

            // Only process if geometry actually exists (IfcProject will usually skip this)
            if (!flat_mesh.geometries.empty()) {
                glm::dmat4 node_mat(1.0);
                bool has_node_transform = false;
                for (auto& geom_data : flat_mesh.geometries) {
                    PrecalculatedIFCItemGeometry item_geometry;
                    auto ifc_geometry = temp_ifc_manager->geometry_loader->GetGeometry(geom_data.geometryExpressID);

                    if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

                    // First geometry defines the node's world-space placement.
                    // if (!has_node_transform) {
                    //     node_mat = geom_data.transformation;
                    //     item.node_transform = glm_to_godot_transform(node_mat);
                    //     has_node_transform = true;
                    // }

                    // Express vertices in the node's local space.
                    glm::dmat4 local_mat = glm::inverse(node_mat) * geom_data.transformation;

                    // Resize & Fill Vertices/Normals/Indices (Standard logic)
                    int v_off = item_geometry.vertices.size();
                    int i_off = item_geometry.indices.size();

                    item_geometry.vertices.resize(v_off + ifc_geometry.numPoints);
                    item_geometry.normals.resize(v_off + ifc_geometry.numPoints);
                    item_geometry.indices.resize(i_off + (ifc_geometry.numFaces * 3));

                    for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
                        glm::dvec4 tv = local_mat * glm::dvec4(ifc_geometry.GetPoint(k), 1.0);
                        item_geometry.vertices[v_off + k] = Vector3((float)tv.x, (float)tv.y, (float)tv.z);
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

            temp_queue.push_back(item);
            processed_ids.insert(id);

            } catch (const std::exception& e) {
                ERR_PRINT(String("Failed to process spatial entity ") + s_type.c_str() + " #" + String::num_int64(i) + ": " + e.what());
            } catch (...) {
                ERR_PRINT(String("Failed to process spatial entity ") + s_type.c_str() + " #" + String::num_int64(i) + ": unknown error.");
            }
        }
    }

    // PHASE B: SECONDARY PARENTS (Assemblies, etc.)
    std::vector<int> other_parents;
    for (int p_id : active_parents) {
        if (processed_ids.find(p_id) == processed_ids.end()) other_parents.push_back(p_id);
    }
    std::sort(other_parents.begin(), other_parents.end());

    for (int id : other_parents) {
        try {
        auto ent = temp_file->instance_by_id(id);
        if (!ent) continue;

        std::string s_type = ent->declaration().name();

        PrecalculatedIFCItem item;
        item.valid = true;
        item.express_id = id;
        item.node_name = String(s_type.c_str()) + "_" + String::num_int64(id);
        item.ifc_class = s_type.c_str();

        switch (schema_index)
        {
            case 0: item.attributes = get_ifc_object_attributes<Ifc4>(*temp_file, id); break;
            // case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, id); break;
            case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, id); break;
            case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, id); break;
            default: ;
        }

        switch (schema_index)
        {
            case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, id); break;
            // case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, id); break;
            case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, id); break;
            case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, id); break;
            default: ;
        }

        switch (schema_index)
        {
            case 0: item.quantities = get_ifc_quantity_sets<Ifc4>(*temp_file, id); break;
            // case 1: item.quantities = get_ifc_quantity_sets<Ifc4x3>(*temp_file, id); break;
            case 2: item.quantities = get_ifc_quantity_sets<Ifc2x3>(*temp_file, id); break;
            case 3: item.quantities = get_ifc_quantity_sets<Ifc4x3_add2>(*temp_file, id); break;
            default: ;
        }

        if (parent_map.find(id) != parent_map.end()) {
            item.parent_id = parent_map[id];
        }

        // Extract GlobalId for scene persistence
        if (item.attributes.has("GlobalId")) {
            item.global_id = item.attributes["GlobalId"];
        }

        temp_queue.push_back(item);
        processed_ids.insert(id);

        } catch (const std::exception& e) {
            ERR_PRINT(String("Failed to process parent entity #") + String::num_int64(id) + ": " + e.what());
        } catch (...) {
            ERR_PRINT(String("Failed to process parent entity #") + String::num_int64(id) + ": unknown error.");
        }
    }

    // 3. HEAVY LOOP: Process everything else
    for (auto type : temp_ifc_manager->schemaManager.GetIfcElementList()) {
        auto expressIDs = temp_ifc_manager->loader->GetExpressIDsWithType(type);
        for (uint32_t expressID : expressIDs) {
            try {

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
                // case 1: item.attributes = get_ifc_object_attributes<Ifc4x3>(*temp_file, expressID); break;
                case 2: item.attributes = get_ifc_object_attributes<Ifc2x3>(*temp_file, expressID); break;
                case 3: item.attributes = get_ifc_object_attributes<Ifc4x3_add2>(*temp_file, expressID); break;
                default: ;
            }

            switch (schema_index)
            {
                case 0: item.properties = get_ifc_property_sets<Ifc4>(*temp_file, expressID); break;
                // case 1: item.properties = get_ifc_property_sets<Ifc4x3>(*temp_file, expressID); break;
                case 2: item.properties = get_ifc_property_sets<Ifc2x3>(*temp_file, expressID); break;
                case 3: item.properties = get_ifc_property_sets<Ifc4x3_add2>(*temp_file, expressID); break;
                default: ;
            }

            switch (schema_index)
            {
                case 0: item.quantities = get_ifc_quantity_sets<Ifc4>(*temp_file, expressID); break;
                // case 1: item.quantities = get_ifc_quantity_sets<Ifc4x3>(*temp_file, expressID); break;
                case 2: item.quantities = get_ifc_quantity_sets<Ifc2x3>(*temp_file, expressID); break;
                case 3: item.quantities = get_ifc_quantity_sets<Ifc4x3_add2>(*temp_file, expressID); break;
                default: ;
            }

            if (parent_map.count(expressID)) item.parent_id = parent_map[expressID];

            // Extract GlobalId for scene persistence
            if (item.attributes.has("GlobalId")) {
                item.global_id = item.attributes["GlobalId"];
            }

            // Geometry (Standard Loop)
            item.geometry = {};
            glm::dmat4 node_mat(1.0);
            bool has_node_transform = false;
            for (auto& geom_data : flat_mesh.geometries) {
                PrecalculatedIFCItemGeometry item_geometry;
                auto ifc_geometry = temp_ifc_manager->geometry_loader->GetGeometry(geom_data.geometryExpressID);
                if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

                // First geometry defines the node's world-space placement.
                // if (!has_node_transform) {
                //     node_mat = geom_data.transformation;
                //     item.node_transform = glm_to_godot_transform(node_mat);
                //     has_node_transform = true;
                // }

                // Express vertices in the node's local space.
                glm::dmat4 local_mat = glm::inverse(node_mat) * geom_data.transformation;

                int v_off = item_geometry.vertices.size();
                int i_off = item_geometry.indices.size();
                item_geometry.vertices.resize(v_off + ifc_geometry.numPoints);
                item_geometry.normals.resize(v_off + ifc_geometry.numPoints);
                item_geometry.indices.resize(i_off + (ifc_geometry.numFaces * 3));

                for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
                    glm::dvec4 tv = local_mat * glm::dvec4(ifc_geometry.GetPoint(k), 1.0);
                    item_geometry.vertices[v_off + k] = Vector3((float)tv.x, (float)tv.y, (float)tv.z);
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

            } catch (const std::exception& e) {
                ERR_PRINT(String("Failed to process element #") + String::num_int64(expressID) + ": " + e.what());
            } catch (...) {
                ERR_PRINT(String("Failed to process element #") + String::num_int64(expressID) + ": unknown error.");
            }
        }
    }

    // =========================================================
    // TOPOLOGICAL SORT & ORPHAN RESCUE
    // =========================================================

    // 1. Orphan IfcSite Rescue
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
    std::unordered_set<int> in_progress;

    std::function<int(int)> get_depth = [&](int id) -> int {
        if (id <= 0) return 0;
        if (depth_cache.count(id)) return depth_cache[id];
        if (parent_map.find(id) == parent_map.end()) { depth_cache[id] = 0; return 0; }
        int p = parent_map[id];
        if (p == 0 || p == id) { depth_cache[id] = 0; return 0; }
        // Cycle guard: if we're already computing depth for this node, break the cycle
        if (in_progress.count(id)) { depth_cache[id] = 0; return 0; }
        in_progress.insert(id);
        int d = 1 + get_depth(p);
        in_progress.erase(id);
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

    // Get georeference data for IFC4+ schemas
    switch (schema_index) {
        case 0: this->georreference = get_georreference<Ifc4>(*temp_file); break;
        case 3: this->georreference = get_georreference<Ifc4x3_add2>(*temp_file); break;
        default: this->georreference = {}; break;
    }

    // Commit results
    this->web_ifc_manager = std::move(temp_ifc_manager);
    this->ifc_parse_file = std::move(temp_file);
    this->generation_queue = temp_queue;

    // Free the raw buffer — both parsers have consumed it
    this->pending_buffer = PackedByteArray();
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
        element_node->set_transform(item.node_transform);
        element_node->set_properties(item.properties);
        element_node->set_quantities(item.quantities);
        element_node->set_attributes(item.attributes);
        element_node->set_ifc_class(item.ifc_class);
        element_node->set_global_id(item.global_id);

        // Wrap the IFC entity with the typed GD object
        if (ifc_parse_file) {
            try {
                auto* inst = ifc_parse_file->instance_by_id(item.express_id);
                if (inst) {
                    element_node->set_ifc_object(GDIFCEntityBase::wrap(inst, ifc_parse_file));
                }
            } catch (...) {}
        }

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

            element_node->set_mesh(mesh);

            if (this->should_create_collisions && !this->collision_classes.is_empty()) {
                if (this->collision_classes.has(item.ifc_class)) {
                    element_node->create_trimesh_collision();
                }
            } else if (this->should_create_collisions && this->collision_classes.is_empty()) {
                element_node->create_trimesh_collision();
            }

        }

        element_node->add_to_group(item.ifc_class,true);

        current_generation_index++;

        uint64_t current_duration = Time::get_singleton()->get_ticks_usec() - start_time;
        if (current_duration > time_budget) {
            return;
        }
    }

    if (current_generation_index >= generation_queue.size()) {
        invisible_staging_root->set_name("IFCModel");

        // IFC4+: create Georeference node, or warn if georef data is absent
        bool is_ifc4plus = ifc_parse_file &&
            ifc_parse_file->schema()->name() != Ifc2x3::get_schema().name();
        if (is_ifc4plus) {
            if (georreference.valid) {
                IFCGeoreference* georef_node = memnew(IFCGeoreference);
                georef_node->set_name("Georeference");
                georef_node->init(georreference);
                invisible_staging_root->add_child(georef_node);
            } else {
                WARN_PRINT("IFC4+ file loaded without georeferencing data (IfcProjectedCRS/IfcMapConversion not found).");
            }
        }

        add_child(invisible_staging_root, true);

        // Initialise the IFCModel node with the loaded file
        if (ifc_model_node_ && ifc_parse_file) {
            ifc_model_node_->init(ifc_parse_file);
        }

        // Set ownership recursively so all nodes are selectable in the editor
        Node* scene_owner = get_owner() != nullptr ? get_owner() : this;
        std::function<void(Node*)> set_owner_recursive = [&](Node* n) {
            n->set_owner(scene_owner);
            for (int i = 0; i < n->get_child_count(); i++) {
                set_owner_recursive(n->get_child(i));
            }
        };
        set_owner_recursive(invisible_staging_root);

        invisible_staging_root = nullptr;
        current_state = DONE;
        emit_signal("ifc_read");
        emit_signal("ifc_objects_ready");
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

IFCModel* GDIFCManager::get_ifc_model() {
    return ifc_model_node_;
}

IFCNode* GDIFCManager::get_node_by_global_id(godot::String global_id) {
    if (!ifc_parse_file) return nullptr;
    try {
        auto* inst = ifc_parse_file->instance_by_guid(
            std::string(global_id.utf8().get_data()));
        if (!inst) return nullptr;
        int id = static_cast<int>(inst->id());
        if (!node_registry.has(id)) return nullptr;
        return Object::cast_to<IFCNode>(node_registry[id]);
    } catch (...) {
        return nullptr;
    }
}

Ref<GDIFCEntityBase> GDIFCManager::get_ifc_object_by_global_id(godot::String global_id) {
    if (!ifc_parse_file) return {};
    try {
        auto* inst = ifc_parse_file->instance_by_guid(
            std::string(global_id.utf8().get_data()));
        if (!inst) return {};
        return GDIFCEntityBase::wrap(inst, ifc_parse_file);
    } catch (...) {
        return {};
    }
}

godot::Array GDIFCManager::get_elements_by_class(godot::String ifc_class) {
    if (!get_tree()) return godot::Array();
    return get_tree()->get_nodes_in_group(ifc_class);
}

template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID) {
    godot::Dictionary psets;
    try {

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

    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to get property sets for entity #") + godot::String::num_int64(expressID) + ": " + e.what());
    } catch (...) {
        ERR_PRINT(godot::String("Failed to get property sets for entity #") + godot::String::num_int64(expressID) + ": unknown error.");
    }
    return psets;
}

template <typename schema>
godot::Dictionary get_ifc_quantity_sets(IfcParse::IfcFile& file, int expressID) {
    godot::Dictionary qsets;
    try {

    // 1. Early exit validation
    auto instance = file.instance_by_id(expressID);
    if (!instance) {
        return qsets;
    }

    auto object = instance->template as<typename schema::IfcObject>();
    if (!object) {
        return qsets;
    }

    // 2. Get the list of all relationships (Generic list)
    // IfcOpenShell: IsDefinedBy returns a aggregate/list, not a single object
    auto rels_defines = object->IsDefinedBy();
    if (!rels_defines) {
        return qsets;
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
            if (auto p_set = p_set_select->template as<typename schema::IfcElementQuantity>()) {
                auto p_set_name_opt = p_set->Name();
                if (!p_set_name_opt.has_value()) {
                    continue;
                }

                std::string quants_key = p_set_name_opt.value();

                auto quants = p_set->Quantities();
                if (!quants) {
                    continue;
                }

                godot::Dictionary quantities_dict;

                for (auto quant : *quants) {
                    if (!quant) {
                        continue;
                    }

                    // Cache the property name to avoid repeated lookups
                    // Godot Dictionaries use Variants as keys. Passing a const char* // creates a String automatically.
                    std::string quant_name_str = quant->Name();
                    const char* quant_key = quant_name_str.c_str();

                    if (auto q_length = quant->template as<typename schema::IfcQuantityLength>()) {
                        // Handle Single Value
                        if (auto n_value = q_length->LengthValue()) {
                            quantities_dict[quant_key] = n_value;
                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }
                    } else if (auto q_area = quant->template as<typename schema::IfcQuantityArea>()) {
                        if (auto n_value = q_area->AreaValue()) {

                            quantities_dict[quant_key] = n_value;

                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }
                    } else if (auto q_volume = quant->template as<typename schema::IfcQuantityVolume>()) {
                        if (auto n_value = q_volume->VolumeValue()) {
                            quantities_dict[quant_key] = n_value;
                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }
                    } else if (auto q_weight = quant->template as<typename schema::IfcQuantityWeight>()) {
                        if (auto n_value = q_weight->WeightValue()) {
                            quantities_dict[quant_key] = n_value;
                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }

                    } else if (auto q_count = quant->template as<typename schema::IfcQuantityCount>()) {
                        if (auto n_value = q_count->CountValue()) {
                            quantities_dict[quant_key] = n_value;
                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }

                    } else if (auto q_time = quant->template as<typename schema::IfcQuantityTime>()) {
                        if (auto n_value = q_time->TimeValue()) {
                            quantities_dict[quant_key] = n_value;
                        } else {
                            quantities_dict[quant_key] = godot::Variant(); // Null/Nil
                        }
                    }
                }

                // Insert the constructed dictionary into the main psets dictionary
                qsets[quants_key.c_str()] = quantities_dict;
            }
        }
    }

    return qsets;

    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to get quantity sets for entity #") + godot::String::num_int64(expressID) + ": " + e.what());
    } catch (...) {
        ERR_PRINT(godot::String("Failed to get quantity sets for entity #") + godot::String::num_int64(expressID) + ": unknown error.");
    }
    return qsets;
}

template <typename schema>
godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID) {

    godot::Dictionary attributes;
    try {

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

    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to get attributes for entity #") + godot::String::num_int64(expressID) + ": " + e.what());
    } catch (...) {
        ERR_PRINT(godot::String("Failed to get attributes for entity #") + godot::String::num_int64(expressID) + ": unknown error.");
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
    if constexpr (std::is_same_v<schema, Ifc2x3>) {
        return {};
    } else {
        try {
            auto instances = file.instances_by_type("IfcMapConversion");
            if (!instances || instances->size() == 0) return {};

            auto* obj = (*instances)[0];
            auto* map_conversion = obj->template as<typename schema::IfcMapConversion>();
            if (!map_conversion) return {};

            // Cast TargetCRS to IfcProjectedCRS* for full attribute access.
            // VerticalDatum is on the base class in IFC4, on IfcProjectedCRS in IFC4x3_add2.
            auto* projected_crs = map_conversion->TargetCRS()
                                      ->template as<typename schema::IfcProjectedCRS>();
            if (!projected_crs) return {};

            // Name is non-optional (std::string) in IFC4 but optional in IFC4x3_add2.
            std::string crs_name;
            if constexpr (std::is_same_v<schema, Ifc4>) {
                crs_name = projected_crs->Name();
            } else {
                crs_name = projected_crs->Name().value_or("");
            }

            return GeorreferenceData(
                MapConversion{
                    (double)map_conversion->Eastings(),
                    (double)map_conversion->Northings(),
                    (double)map_conversion->OrthogonalHeight(),
                    (double)map_conversion->XAxisAbscissa().value_or(0.0),
                    (double)map_conversion->XAxisOrdinate().value_or(0.0),
                    (double)map_conversion->Scale().value_or(0.0)
                },
                ProjectedCRS{
                    crs_name.c_str(),
                    projected_crs->Description().value_or("").c_str(),
                    projected_crs->GeodeticDatum().value_or("").c_str(),
                    projected_crs->VerticalDatum().value_or("").c_str()
                }
            );
        } catch (const std::exception& e) {
            ERR_PRINT(godot::String("Failed to get georeference data: ") + e.what());
        } catch (...) {
            ERR_PRINT("Failed to get georeference data: unknown error.");
        }
        return {};
    }
}

template <typename schema>
std::unordered_map<int, int> build_spatial_hierarchy(IfcParse::IfcFile &file) {

    std::unordered_map<int, int> child_to_parent;

    // Handle Aggregation
    try {
    auto rel_aggregates = file.instances_by_type("IfcRelAggregates");

    if (rel_aggregates) {
        for (int i = 0; i < rel_aggregates->size(); i++) {
            auto rel = rel_aggregates->operator[](i)->as<typename schema::IfcRelAggregates>();
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
    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to process IfcRelAggregates: ") + e.what());
    } catch (...) {
        ERR_PRINT("Failed to process IfcRelAggregates: unknown error.");
    }

    // Handle Spatial Containment
    try {
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
    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to process IfcRelContainedInSpatialStructure: ") + e.what());
    } catch (...) {
        ERR_PRINT("Failed to process IfcRelContainedInSpatialStructure: unknown error.");
    }

    // Handle nesting
    try {
    auto rel_nests = file.instances_by_type("IfcRelNests");
    if (rel_nests) {
        for (int i = 0; i < rel_nests->size(); i++) {
            auto rel = rel_nests->operator[](i)->as<typename schema::IfcRelNests>();
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
    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to process IfcRelNests: ") + e.what());
    } catch (...) {
        ERR_PRINT("Failed to process IfcRelNests: unknown error.");
    }

    try {
    if (schema::get_schema().name() == Ifc4x3_add2::get_schema().name()) {
        auto rel_adheres = file.instances_by_type("IfcRelAdheresToElement");

        if (rel_adheres) {
            for (int i = 0; i < rel_adheres->size(); i++) {
                auto rel = rel_adheres->operator[](i)->as<typename Ifc4x3_add2::IfcRelAdheresToElement>();
                if (!rel) continue;

                auto parent = rel->RelatingElement();
                auto children = rel->RelatedSurfaceFeatures();

                if (parent && children) {
                    int parent_id = parent->id();
                    for (auto child : *children) {
                        child_to_parent[child->id()] = parent_id;
                    }
                }
            }
        }

    }
    } catch (const std::exception& e) {
        ERR_PRINT(godot::String("Failed to process IfcRelAdheresToElement: ") + e.what());
    } catch (...) {
        ERR_PRINT("Failed to process IfcRelAdheresToElement: unknown error.");
    }


    return child_to_parent;
}
