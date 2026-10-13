#include "gd_ifc_manager.h"
#include <algorithm> // Required for std::sort
#include <functional> // Required for lambdas
#include <stdexcept>  // Required for std::exception
#include <mutex>

#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/classes/concave_polygon_shape3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/importer_mesh.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/scene_tree.hpp"
#include "godot_cpp/classes/static_body3d.hpp"
#include "godot_cpp/classes/triangle_mesh.hpp"

using namespace godot;

void GDIFCManager::_bind_methods() {
    ClassDB::bind_method(D_METHOD("read_ifc", "path", "create_collision", "collision_classes"), &GDIFCManager::read_ifc, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("read_ifc_base64", "base64_data", "create_collision", "collision_classes"), &GDIFCManager::read_ifc_base64, DEFVAL(false), DEFVAL(Array{}));
    ClassDB::bind_method(D_METHOD("get_gdifc_settings"), &GDIFCManager::get_gdifc_settings);
    ClassDB::bind_method(D_METHOD("set_gdifc_settings","gdifc_settings"), &GDIFCManager::set_gdifc_settings);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "geometric_settings", PROPERTY_HINT_RESOURCE_TYPE, "GDIFCLoaderSettings"),
        "set_gdifc_settings", "get_gdifc_settings");

    ClassDB::bind_method(D_METHOD("get_ifc_file_path"), &GDIFCManager::get_ifc_file_path);
    ClassDB::bind_method(D_METHOD("set_ifc_file_path","path"), &GDIFCManager::set_ifc_file_path);
    ADD_PROPERTY(PropertyInfo(Variant::STRING, "ifc_file_path", PROPERTY_HINT_GLOBAL_FILE, "*.ifc"),
        "set_ifc_file_path", "get_ifc_file_path");

    ClassDB::bind_method(D_METHOD("get_reference_georeference"), &GDIFCManager::get_reference_georeference);
    ClassDB::bind_method(D_METHOD("set_reference_georeference","path"), &GDIFCManager::set_reference_georeference);
    ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "reference_georeference", PROPERTY_HINT_NODE_PATH_VALID_TYPES, "IFCGeoreference"),
        "set_reference_georeference", "get_reference_georeference");

    ClassDB::bind_method(D_METHOD("get_ifc_model"), &GDIFCManager::get_ifc_model);
    ClassDB::bind_method(D_METHOD("get_node_by_global_id","global_id"), &GDIFCManager::get_node_by_global_id);
    ClassDB::bind_method(D_METHOD("get_ifc_object_by_global_id","global_id"), &GDIFCManager::get_ifc_object_by_global_id);
    ClassDB::bind_method(D_METHOD("get_elements_by_class","ifc_class"), &GDIFCManager::get_elements_by_class);

    ADD_SIGNAL(MethodInfo("ifc_read"));
    ADD_SIGNAL(MethodInfo("ifc_objects_ready"));
    ADD_SIGNAL(MethodInfo("ifc_load_failed"));
}

GDIFCManager::GDIFCManager() {
    geometric_settings.instantiate();
}

GDIFCManager::~GDIFCManager() {
    // A running task writes into this object, so it must finish before the members go away.
    _wait_for_task();

    // Freed mid-generation: the staging root and its children are not in the tree yet.
    if (invisible_staging_root) {
        memdelete(invisible_staging_root);
        invisible_staging_root = nullptr;
    }
}

void GDIFCManager::_wait_for_task() {
    if (task_id == -1) return;
    WorkerThreadPool::get_singleton()->wait_for_task_completion(task_id);
    task_id = -1;
}

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

    // Point the model reference at the existing IFCModel
    if (Object::cast_to<IFCModel>(model_child)) {
        ifc_model_node_id_ = model_child->get_instance_id();
    }

    UtilityFunctions::print_rich("[color=blue]Cold reload detected — re-parsing IFC metadata from: " + ifc_file_path_);

    relink_failed_ = false;
    thread_failed_ = false;
    current_state = LOADING_THREAD;
    task_id = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &GDIFCManager::_metadata_thread_task), true);
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
        pending_ifc_parse_file = std::move(temp_file);
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

    IFCModel* model_root = get_ifc_model();
    if (!model_root) return;

    // Initialise the IFCModel node with the loaded file
    model_root->init(ifc_parse_file);

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
                        node_registry[static_cast<int>(inst->id())] = ifc_node->get_instance_id();
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

    ERR_FAIL_COND_V_MSG(current_state == LOADING_THREAD || current_state == GENERATING_NODES || current_state == RELINKING,
                        ERR_BUSY, "Already loading!");

    UtilityFunctions::print_rich("[color=blue]Started loading IFC");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    // Read the file into the shared buffer
    Ref<FileAccess> fa = FileAccess::open(_path, FileAccess::READ);
    if (!fa.is_valid()) {
        ERR_FAIL_V_MSG(Error::FAILED, String("Failed to open IFC file: ") + _path
                       + String(" (error: ") + String::num_int64((int64_t)FileAccess::get_open_error()) + String(")"));
    }
    this->pending_buffer = fa->get_buffer(fa->get_length());
    fa.unref();

    ERR_FAIL_COND_V_MSG(this->pending_buffer.is_empty(), Error::FAILED, "IFC file is empty.");

    // Store path for cold-reload persistence
    ifc_file_path_ = _path;

    return _start_loading(_create_collision, _collision_classes);
}

Error GDIFCManager::read_ifc_base64(const String &_base64_data, bool _create_collision, const Array &_collision_classes) {

    ERR_FAIL_COND_V_MSG(current_state == LOADING_THREAD || current_state == GENERATING_NODES || current_state == RELINKING,
                        ERR_BUSY, "Already loading!");
    ERR_FAIL_COND_V_MSG(_base64_data.is_empty(), Error::FAILED, "Base64 data is empty.");

    UtilityFunctions::print_rich("[color=blue]Started loading IFC from base64");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    // Decode base64 into the shared buffer
    this->pending_buffer = Marshalls::get_singleton()->base64_to_raw(_base64_data);

    ERR_FAIL_COND_V_MSG(this->pending_buffer.is_empty(), Error::FAILED, "Failed to decode base64 data.");

    // No file on disk to re-parse on cold reload
    ifc_file_path_ = String();

    return _start_loading(_create_collision, _collision_classes);
}

Error GDIFCManager::_start_loading(bool _create_collision, const Array &_collision_classes) {
    // Reset
    this->should_create_collisions = _create_collision;
    this->collision_classes = _collision_classes;
    this->generation_queue.clear();
    this->editor_cache_queue.clear();
    this->current_editor_cache_index = 0;
    this->material_cache.clear();
    this->node_registry.clear();
    this->current_generation_index = 0;
    this->thread_failed_ = false;
    this->relink_failed_ = false;
    this->current_state = LOADING_THREAD;

    // Start Thread
    task_id = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &GDIFCManager::_thread_task), true);
    set_process(true);
    return Error::OK;
}

// ---------------------------------------------------------
// MAIN THREAD PROCESS
// ---------------------------------------------------------
void GDIFCManager::_process(double delta) {

    if (current_state == LOADING_THREAD) {
        if (WorkerThreadPool::get_singleton()->is_task_completed(task_id)) {
            _wait_for_task();

            if (thread_failed_) {
                // The task returned early and left its partial results behind
                pending_ifc_parse_file.reset();
                generation_queue.clear();
                pending_buffer = PackedByteArray();
                current_state = FAILED;
                emit_signal("ifc_load_failed");
                set_process(false);
                return;
            }

            if (pending_ifc_parse_file) {
                ifc_parse_file = std::move(pending_ifc_parse_file);
            }

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
                georreference = pending_georreference;
                invisible_staging_root = memnew(IFCModel);
                invisible_staging_root->set_name("IFC_Staging_Area");
                ifc_model_node_id_ = invisible_staging_root->get_instance_id();
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
    }
}
// ---------------------------------------------------------
// BACKGROUND THREAD
// ---------------------------------------------------------

// Fill attributes, property sets and quantity sets for one entity.
template <typename schema>
static void fill_item_metadata(IfcParse::IfcFile& file, PrecalculatedIFCItem& item) {
    item.attributes = get_ifc_object_attributes<schema>(file, item.express_id);
    item.properties = get_ifc_property_sets<schema>(file, item.express_id);
    item.quantities = get_ifc_quantity_sets<schema>(file, item.express_id);
}

struct LodOptions {
    bool enabled = false;
    float normal_merge_angle = 60.0f;
};

// Items below this triangle count (boxes, plain walls, slabs) can't be simplified
// meaningfully; skipping them avoids per-element overhead on large models.
static constexpr int LOD_MIN_TRIANGLES = 64;

// Generate LOD index arrays with Godot's ImporterMesh (the same path the glTF/scene
// importer uses). Runs on the worker thread: the ImporterMesh is temporary and only
// its index data is kept, so no RenderingServer resources are created here.
static void generate_item_lods(PrecalculatedIFCItem& item, float normal_merge_angle) {
    int64_t triangle_count = 0;
    for (const auto& geometry : item.geometry) triangle_count += geometry.indices.size() / 3;
    if (triangle_count < LOD_MIN_TRIANGLES) return;

    Ref<ImporterMesh> importer;
    importer.instantiate();

    for (const auto& geometry : item.geometry) {
        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = geometry.vertices;
        arrays[Mesh::ARRAY_NORMAL] = geometry.normals;
        arrays[Mesh::ARRAY_INDEX] = geometry.indices;
        importer->add_surface(Mesh::PRIMITIVE_TRIANGLES, arrays);
    }

    // The normal split angle is ignored by the engine since 4.2.
    importer->generate_lods(normal_merge_angle, 25.0f, Array());

    for (int i = 0; i < (int)item.geometry.size(); i++) {
        auto& geometry = item.geometry[i];
        int lod_count = importer->get_surface_lod_count(i);
        if (lod_count == 0) continue;

        // Read the arrays back so the LOD indices always match the vertex data they reference.
        Array arrays = importer->get_surface_arrays(i);
        geometry.vertices = arrays[Mesh::ARRAY_VERTEX];
        geometry.normals = arrays[Mesh::ARRAY_NORMAL];
        geometry.indices = arrays[Mesh::ARRAY_INDEX];

        for (int l = 0; l < lod_count; l++) {
            geometry.lods[importer->get_surface_lod_size(i, l)] = importer->get_surface_lod_indices(i, l);
        }
    }
}

// Tessellate an entity's flat mesh into one surface per placed geometry.
// Node transforms are currently identity, so vertices are baked in world space.
static void fill_item_geometry(webifc::geometry::IfcGeometryProcessor& geometry_loader,
                               const webifc::geometry::IfcFlatMesh& flat_mesh,
                               bool is_space, const LodOptions& lod_options,
                               PrecalculatedIFCItem& item) {
    for (const auto& geom_data : flat_mesh.geometries) {
        auto& ifc_geometry = geometry_loader.GetGeometry(geom_data.geometryExpressID);
        if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

        PrecalculatedIFCItemGeometry item_geometry;
        item_geometry.vertices.resize(ifc_geometry.numPoints);
        item_geometry.normals.resize(ifc_geometry.numPoints);
        item_geometry.indices.resize(ifc_geometry.numFaces * 3);

        Vector3* vertices = item_geometry.vertices.ptrw();
        Vector3* normals = item_geometry.normals.ptrw();
        int32_t* indices = item_geometry.indices.ptrw();

        for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
            glm::dvec4 tv = geom_data.transformation * glm::dvec4(ifc_geometry.GetPoint(k), 1.0);
            vertices[k] = Vector3((float)tv.x, (float)tv.y, (float)tv.z);
        }
        for (uint32_t k = 0; k < ifc_geometry.numFaces; k++) {
            bimGeometry::Face f = ifc_geometry.GetFace(k);
            indices[k*3+0] = f.i2;
            indices[k*3+1] = f.i1;
            indices[k*3+2] = f.i0;

            // Simple flat normals calculation
            Vector3 n = (vertices[f.i1] - vertices[f.i0]).cross(vertices[f.i2] - vertices[f.i0]).normalized();
            normals[f.i0] = n;
            normals[f.i1] = n;
            normals[f.i2] = n;
        }

        if (is_space) {
            item_geometry.is_transparent = true;
            item_geometry.color = Color(0.025, 0.037, 0.034, 0.1);
        } else {
            item_geometry.color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
            item_geometry.is_transparent = (geom_data.color.a < 0.7);
        }

        item.geometry.push_back(std::move(item_geometry));
    }

    if (lod_options.enabled && !item.geometry.empty()) {
        generate_item_lods(item, lod_options.normal_merge_angle);
    }
}

// Runs on a WorkerThreadPool thread. It must not touch current_state or any member the
// main thread reads during loading; results go into pending_* members and generation_queue,
// which _process() commits after the task completes. Failures set thread_failed_.
// web-ifc keeps process-wide state (e.g. a static stack in its BVH ray test,
// global tolerances), so only one full load may run its geometry at a time.
static std::mutex web_ifc_mutex;

void GDIFCManager::_thread_task() {
    std::lock_guard<std::mutex> web_ifc_lock(web_ifc_mutex);

    const char* buf_ptr  = reinterpret_cast<const char*>(this->pending_buffer.ptr());
    int         buf_size = static_cast<int>(this->pending_buffer.size());

    // Snapshot LOD options once so edits to the settings resource mid-load aren't picked up
    LodOptions lod_options;
    if (geometric_settings.is_valid()) {
        lod_options.enabled = geometric_settings->getGenerateLods();
        lod_options.normal_merge_angle = geometric_settings->getLodNormalMergeAngle();
    }

    // Load file [web-ifc] — from memory buffer
    auto temp_ifc_manager = std::make_unique<WEBIFCManager>(geometric_settings.ptr());
    try {
        temp_ifc_manager->read_ifc_file(buf_ptr, buf_size);
    } catch (const std::exception& e) {
        thread_failed_ = true;
        ERR_FAIL_MSG(String("[web-ifc] Failed to read IFC file: ") + e.what());
    } catch (...) {
        thread_failed_ = true;
        ERR_FAIL_MSG("[web-ifc] Failed to read IFC file: unknown error.");
    }

    // Load file [IFCParse] — from memory buffer (void*, int constructor)
    std::shared_ptr<IfcParse::IfcFile> temp_file;
    try {
        temp_file = std::make_shared<IfcParse::IfcFile>((void*)buf_ptr, buf_size);
    } catch (const std::exception& e) {
        thread_failed_ = true;
        ERR_FAIL_MSG(String("[IfcParse] Failed to parse IFC file: ") + e.what());
    } catch (...) {
        thread_failed_ = true;
        ERR_FAIL_MSG("[IfcParse] Failed to parse IFC file: unknown error.");
    }

    if (!temp_file->good()) {
        thread_failed_ = true;
        ERR_FAIL_MSG("Failed to parse IFC file.");
    }

    // Schema check [IFCParse]
    std::string current_schema = temp_file->schema()->name();

    bool is_supported_schema = (current_schema == Ifc2x3::get_schema().name()) ||
                               (current_schema == Ifc4::get_schema().name()) ||
                            //    (current_schema == Ifc4x3::get_schema().name()) ||
                               (current_schema == Ifc4x3_add2::get_schema().name());

    if (!is_supported_schema) {
        thread_failed_ = true;
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
        thread_failed_ = true;
        ERR_FAIL_MSG(String("[web-ifc] Failed to initialize geometry processor: ") + e.what());
    } catch (...) {
        thread_failed_ = true;
        ERR_FAIL_MSG("[web-ifc] Failed to initialize geometry processor: unknown error.");
    }

    auto& geometry_loader = *temp_ifc_manager->geometry_loader;

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

    // Attributes, psets, quantities, GlobalId and parent for one item.
    auto fill_metadata = [&](PrecalculatedIFCItem& item) {
        switch (schema_index) {
            case 0: fill_item_metadata<Ifc4>(*temp_file, item); break;
            // case 1: fill_item_metadata<Ifc4x3>(*temp_file, item); break;
            case 2: fill_item_metadata<Ifc2x3>(*temp_file, item); break;
            case 3: fill_item_metadata<Ifc4x3_add2>(*temp_file, item); break;
            default: ;
        }

        // Extract GlobalId for scene persistence
        if (item.attributes.has("GlobalId")) {
            item.global_id = item.attributes["GlobalId"];
        }

        // Defines the parent of item
        auto parent_it = parent_map.find(item.express_id);
        if (parent_it != parent_map.end()) {
            item.parent_id = parent_it->second;
        }
    };

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

            fill_metadata(item);

            // Spatial elements can carry geometry (e.g. IfcSite terrain); IfcProject usually has none
            fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), false, lod_options, item);

            temp_queue.push_back(item);
            processed_ids.insert(id);

            } catch (const std::exception& e) {
                ERR_PRINT(String("Failed to process spatial entity ") + s_type.c_str() + " #" + String::num_int64(i) + ": " + e.what());
            } catch (...) {
                ERR_PRINT(String("Failed to process spatial entity ") + s_type.c_str() + " #" + String::num_int64(i) + ": unknown error.");
            }
        }
    }

    // PHASE B: SECONDARY PARENTS (Assemblies, elements with nested ports, spaces, etc.)
    // These can have their own geometry, so it is extracted here: the heavy loop below
    // skips every processed id.
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

        fill_metadata(item);

        if (s_type != "IfcOpeningElement") {
            fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), s_type == "IfcSpace", lod_options, item);
        }

        temp_queue.push_back(item);
        processed_ids.insert(id);

        } catch (const std::exception& e) {
            ERR_PRINT(String("Failed to process parent entity #") + String::num_int64(id) + ": " + e.what());
        } catch (...) {
            ERR_PRINT(String("Failed to process parent entity #") + String::num_int64(id) + ": unknown error.");
        }
    }

    // 3. HEAVY LOOP: Process everything else (parents were all handled above)
    for (auto type : temp_ifc_manager->schemaManager.GetIfcElementList()) {
        auto expressIDs = temp_ifc_manager->loader->GetExpressIDsWithType(type);
        for (uint32_t expressID : expressIDs) {
            try {

            if (processed_ids.find(expressID) != processed_ids.end()) continue;

            std::string type_str = temp_ifc_manager->schemaManager.IfcTypeCodeToType(temp_ifc_manager->loader->GetLineType(expressID));
            if (type_str == "IfcOpeningElement") continue;

            auto flat_mesh = geometry_loader.GetFlatMesh(expressID);
            if (flat_mesh.geometries.empty()) continue;

            PrecalculatedIFCItem item;
            item.valid = true;
            item.express_id = expressID;
            item.node_name = String(type_str.c_str()) + "_" + String::num_int64(expressID);
            item.ifc_class = type_str.c_str();

            fill_item_geometry(geometry_loader, flat_mesh, type_str == "IfcSpace", lod_options, item);
            if (item.geometry.empty()) continue;

            fill_metadata(item);
            temp_queue.push_back(item);

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
        case 0: this->pending_georreference = get_georreference<Ifc4>(*temp_file); break;
        case 3: this->pending_georreference = get_georreference<Ifc4x3_add2>(*temp_file); break;
        default: this->pending_georreference = {}; break;
    }

    // Offset web-ifc subtracted from every vertex (coordinate_to_origin), needed
    // to place this model relative to another one even without georef data
    this->pending_georreference.coordination_offset = glm::dvec3(geometry_loader.GetCoordinationMatrix()[3]);

    // Hand results to the main thread
    this->web_ifc_manager = std::move(temp_ifc_manager);
    this->pending_ifc_parse_file = std::move(temp_file);
    this->generation_queue = temp_queue;

    // Free the raw buffer — both parsers have consumed it
    this->pending_buffer = PackedByteArray();
}

// Same nodes as MeshInstance3D::create_trimesh_collision(), but built from the geometry still
// in memory: going through the mesh reads every surface buffer back from the GPU, stalling it each time.
static Ref<Shape3D> create_trimesh_collision_from_geometry(MeshInstance3D *node, const std::vector<PrecalculatedIFCItemGeometry> &geometry) {
    int64_t face_point_count = 0;
    for (const auto &item_geometry : geometry) face_point_count += item_geometry.indices.size();
    if (face_point_count == 0) return Ref<Shape3D>();

    PackedVector3Array face_points;
    face_points.resize(face_point_count);
    Vector3 *face_points_w = face_points.ptrw();
    int64_t k = 0;
    for (const auto &item_geometry : geometry) {
        const Vector3 *vertices = item_geometry.vertices.ptr();
        const int32_t *indices = item_geometry.indices.ptr();
        for (int64_t i = 0; i < item_geometry.indices.size(); i++) {
            face_points_w[k++] = vertices[indices[i]];
        }
    }

    Ref<ConcavePolygonShape3D> shape;
    shape.instantiate();
    shape->set_faces(face_points);

    StaticBody3D *static_body = memnew(StaticBody3D);
    CollisionShape3D *collision_shape = memnew(CollisionShape3D);
    collision_shape->set_shape(shape);
    static_body->add_child(collision_shape, true);
    static_body->set_name(String(node->get_name()) + "_col");
    node->add_child(static_body, true);
    return shape;
}

// ---------------------------------------------------------
// DUMB & FAST GENERATOR
// ---------------------------------------------------------
void GDIFCManager::_process_generation_queue() {


    uint64_t start_time = Time::get_singleton()->get_ticks_usec();
    uint64_t time_budget = 20000;
    const bool prebuild_editor_caches = Engine::get_singleton()->is_editor_hint();

    // In low processor mode (the editor) every frame lasts at least the sleep time anyway,
    // which is 100 ms while the editor window is unfocused. Use most of it instead of idling.
    OS *os = OS::get_singleton();
    if (os->is_in_low_processor_usage_mode()) {
        time_budget = MAX(time_budget, (uint64_t)(os->get_low_processor_usage_mode_sleep_usec() * 0.8));
    }

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
            node_registry[item.express_id] = element_node->get_instance_id();
        }

        // 3. PARENTING LOGIC
        Node* parent_node = nullptr;
        if (item.parent_id > 0 && node_registry.has(item.parent_id)) {
            parent_node = Object::cast_to<Node>(ObjectDB::get_instance(node_registry[item.parent_id]));
        }
        if (parent_node) {
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

            mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), item.geometry[i].lods);

            // 3. Material
            Ref<StandardMaterial3D> mat = _get_material(item.geometry[i].color, item.geometry[i].is_transparent, item.ifc_class == "IfcSpace");
            mesh->surface_set_material(i,mat);
        }

        if (mesh->get_surface_count() > 0) {

            element_node->set_mesh(mesh);
            if (prebuild_editor_caches) {
                editor_cache_queue.push_back(mesh);
            }

            if (this->should_create_collisions &&
                (this->collision_classes.is_empty() || this->collision_classes.has(item.ifc_class))) {
                Ref<Shape3D> shape = create_trimesh_collision_from_geometry(element_node, item.geometry);
                if (prebuild_editor_caches && shape.is_valid()) {
                    editor_cache_queue.push_back(shape);
                }
            }

        }

        element_node->add_to_group(item.ifc_class,true);

        current_generation_index++;

        uint64_t current_duration = Time::get_singleton()->get_ticks_usec() - start_time;
        if (current_duration > time_budget) {
            return;
        }
    }

    // In the editor, every owned MeshInstance3D and CollisionShape3D gets a gizmo. Its first redraw
    // builds the mesh's TriangleMesh or the shape's debug mesh, both cached and both reading data
    // back from the GPU with a full stall per buffer. Building them here, inside the time budget,
    // keeps the gizmos created when the model is revealed from all stalling in that single frame.
    // It runs after every mesh is uploaded, because stalls interleaved with the uploads cost more.
    while (current_editor_cache_index < editor_cache_queue.size()) {
        const Ref<Resource> &resource = editor_cache_queue[current_editor_cache_index];
        if (Mesh *mesh = Object::cast_to<Mesh>(resource.ptr())) {
            mesh->generate_triangle_mesh();
        } else if (Shape3D *shape = Object::cast_to<Shape3D>(resource.ptr())) {
            shape->get_debug_mesh();
        }
        current_editor_cache_index++;

        uint64_t current_duration = Time::get_singleton()->get_ticks_usec() - start_time;
        if (current_duration > time_budget) {
            return;
        }
    }
    editor_cache_queue.clear();
    current_editor_cache_index = 0;

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

        // Place the model relative to the chosen reference georeference, if any
        if (!reference_georeference_.is_empty()) {
            IFCGeoreference* reference = Object::cast_to<IFCGeoreference>(get_node_or_null(reference_georeference_));
            if (reference && reference->is_inside_tree()) {
                if (!georreference.valid) {
                    WARN_PRINT("GDIFCManager: the loaded model has no georeferencing data. "
                        "Placing it assuming it shares the reference model's engineering coordinate system.");
                }
                glm::dmat4 relative = georef_relative_matrix(reference->get_data(), georreference);
                invisible_staging_root->set_transform(
                    get_global_transform().affine_inverse() * reference->get_global_transform() * glm_to_godot_transform(relative));
            } else {
                WARN_PRINT(String("GDIFCManager: reference_georeference '") + String(reference_georeference_)
                    + "' does not point to an IFCGeoreference node. The model is placed at the origin.");
            }
        }

        add_child(invisible_staging_root, true);

        // Initialise the IFCModel node with the loaded file
        IFCModel* model = get_ifc_model();
        if (model && ifc_parse_file) {
            model->init(ifc_parse_file);
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
Ref<StandardMaterial3D> GDIFCManager::_get_material(Color color, bool transparent, bool is_space) {
    // Create a unique key string for the map
    String key = String(color.to_html()) + (transparent ? "_T" : "_O") + (is_space ? "_S" : "");

    if (material_cache.has(key)) {
        return material_cache[key];
    }

    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_albedo(color);
    mat->set_cull_mode(BaseMaterial3D::CULL_DISABLED);

    if (is_space) {
        // Shrink spaces slightly so they don't z-fight with the elements bounding them
        mat->set_grow_enabled(true);
        mat->set_grow(-0.001);
    }

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
    return Object::cast_to<IFCModel>(ObjectDB::get_instance(ifc_model_node_id_));
}

IFCNode* GDIFCManager::get_node_by_global_id(godot::String global_id) {
    if (!ifc_parse_file) return nullptr;
    try {
        auto* inst = ifc_parse_file->instance_by_guid(
            std::string(global_id.utf8().get_data()));
        if (!inst) return nullptr;
        int id = static_cast<int>(inst->id());
        if (!node_registry.has(id)) return nullptr;
        return Object::cast_to<IFCNode>(ObjectDB::get_instance(node_registry[id]));
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
                    godot::String prop_key = godot::String::utf8(prop_name_str.c_str());

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
                        props_dict[prop_key] = godot::String::utf8(p_ref->UsageName().value_or("[Reference]").c_str());
                    }
                    // Skip IfcPropertyTableValue and others for performance unless strictly needed
                }

                // Insert the constructed dictionary into the main psets dictionary
                psets[godot::String::utf8(pset_key.c_str())] = props_dict;
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
                    godot::String quant_key = godot::String::utf8(quant_name_str.c_str());

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
                qsets[godot::String::utf8(quants_key.c_str())] = quantities_dict;
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
            return Variant(godot::String::utf8(arg.c_str()));
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
            // IFC entity reference -> express ID, matching AttributeToVariantVisitor
            auto* entity = static_cast<IfcUtil::IfcBaseClass*>(arg);
            return entity ? Variant((int64_t)entity->id()) : Variant();
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
                godot_array.push_back(godot::String::utf8(item.c_str()));
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
                    (double)map_conversion->XAxisAbscissa().value_or(1.0), // IFC default: no rotation
                    (double)map_conversion->XAxisOrdinate().value_or(0.0),
                    (double)map_conversion->Scale().value_or(1.0)          // IFC default: no scaling
                },
                ProjectedCRS{
                    godot::String::utf8(crs_name.c_str()),
                    godot::String::utf8(projected_crs->Description().value_or("").c_str()),
                    godot::String::utf8(projected_crs->GeodeticDatum().value_or("").c_str()),
                    godot::String::utf8(projected_crs->VerticalDatum().value_or("").c_str())
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
