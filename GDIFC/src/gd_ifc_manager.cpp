#include "gd_ifc_manager.h"
#include <algorithm> // Required for std::sort
#include <cctype>     // Required for std::toupper
#include <cstring>    // Required for strlen
#include <functional> // Required for lambdas
#include <stdexcept>  // Required for std::exception
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_set>

#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/classes/concave_polygon_shape3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/image_texture.hpp"
#include "godot_cpp/classes/importer_mesh.hpp"
#include "godot_cpp/classes/os.hpp"
#include "godot_cpp/classes/project_settings.hpp"
#include "godot_cpp/classes/resource_loader.hpp"
#include "godot_cpp/classes/rendering_server.hpp"
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

    ClassDB::bind_method(D_METHOD("get_material_library"), &GDIFCManager::get_material_library);
    ClassDB::bind_method(D_METHOD("set_material_library","library"), &GDIFCManager::set_material_library);
    ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "material_library", PROPERTY_HINT_RESOURCE_TYPE, "GDIFCMaterialLibrary"),
        "set_material_library", "get_material_library");
    ClassDB::bind_method(D_METHOD("find_material_definitions", "name"), &GDIFCManager::find_material_definitions);
    ClassDB::bind_method(D_METHOD("set_material_visual", "material_name", "visual"), &GDIFCManager::set_material_visual);

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

    ClassDB::bind_method(D_METHOD("get_load_progress"), &GDIFCManager::get_load_progress);
    ClassDB::bind_method(D_METHOD("get_load_stage"), &GDIFCManager::get_load_stage);

    ADD_SIGNAL(MethodInfo("ifc_read"));
    ADD_SIGNAL(MethodInfo("ifc_objects_ready"));
    ADD_SIGNAL(MethodInfo("ifc_load_failed"));
    ADD_SIGNAL(MethodInfo("load_progress_changed", PropertyInfo(Variant::FLOAT, "progress"), PropertyInfo(Variant::STRING, "stage")));
}

GDIFCManager::GDIFCManager() {
    geometric_settings.instantiate();
}

GDIFCManager::~GDIFCManager() {
    // Running tasks write into this object, so they must finish before the members go away.
    _wait_for_task();
    _wait_for_metadata_task();
    if (lod_group_id != -1) {
        WorkerThreadPool::get_singleton()->wait_for_group_task_completion(lod_group_id);
        lod_group_id = -1;
    }

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

void GDIFCManager::_wait_for_metadata_task() {
    if (metadata_task_id == -1) return;
    WorkerThreadPool::get_singleton()->wait_for_task_completion(metadata_task_id);
    metadata_task_id = -1;
}

bool GDIFCManager::_is_loading() const {
    return current_state == LOADING_THREAD || current_state == GENERATING_LODS || current_state == GENERATING_NODES
        || current_state == RELINKING || (metadata_state != META_IDLE && metadata_state != META_DONE);
}

// Per-frame time budget for main-thread work. In low processor mode (the editor) every frame
// lasts at least the sleep time anyway, which is 100 ms while the editor window is unfocused,
// so most of it is used instead of idling.
static uint64_t get_frame_time_budget() {
    uint64_t time_budget = 20000;
    OS *os = OS::get_singleton();
    if (os->is_in_low_processor_usage_mode()) {
        time_budget = MAX(time_budget, (uint64_t)(os->get_low_processor_usage_mode_sleep_usec() * 0.8));
    }
    return time_budget;
}

// ---------------------------------------------------------
// LOAD PROGRESS
// ---------------------------------------------------------

struct StageRange {
    float start;
    float end;
};

// Share of the time until the model is shown that each stage covers, from stage durations
// measured in the editor. The reveal range includes the editor's own work after the reveal,
// when nothing is redrawn. Outside the editor there are no gizmo caches and the reveal is short.
// Property sets and typed objects load after the reveal and are not part of the progress.
static StageRange get_stage_range(int stage, bool in_editor) {
    if (in_editor) {
        switch (stage) {
            case GDIFCManager::STAGE_PARSING: return {0.0f, 0.06f};
            case GDIFCManager::STAGE_GEOMETRY: return {0.06f, 0.23f};
            case GDIFCManager::STAGE_LODS: return {0.23f, 0.28f};
            case GDIFCManager::STAGE_NODES: return {0.28f, 0.36f};
            case GDIFCManager::STAGE_EDITOR_CACHES: return {0.36f, 0.79f};
            case GDIFCManager::STAGE_REVEAL: return {0.79f, 1.0f};
            case GDIFCManager::STAGE_DONE: return {1.0f, 1.0f};
            default: return {0.0f, 0.0f};
        }
    }
    switch (stage) {
        case GDIFCManager::STAGE_PARSING: return {0.0f, 0.19f};
        case GDIFCManager::STAGE_GEOMETRY: return {0.19f, 0.68f};
        case GDIFCManager::STAGE_LODS: return {0.68f, 0.79f};
        case GDIFCManager::STAGE_NODES: return {0.79f, 0.98f};
        case GDIFCManager::STAGE_REVEAL: return {0.98f, 1.0f};
        case GDIFCManager::STAGE_DONE: return {1.0f, 1.0f};
        default: return {0.0f, 0.0f};
    }
}

// web-ifc's parse runs at roughly 220 MB/s (measured on three models), in bytes per microsecond
static constexpr double PARSE_BYTES_PER_USEC = 220.0;

String GDIFCManager::get_load_stage() const {
    switch (reported_stage_) {
        case STAGE_PARSING: return "Parsing";
        case STAGE_GEOMETRY: return "Processing geometry";
        case STAGE_LODS: return "Generating LODs";
        case STAGE_NODES: return "Creating nodes";
        case STAGE_EDITOR_CACHES: return "Preparing editor";
        case STAGE_REVEAL: return "Adding to scene";
        case STAGE_DONE: return "Done";
        case STAGE_FAILED: return "Failed";
        default: return "";
    }
}

// Called by the worker when it enters a stage with `total` steps
void GDIFCManager::_set_worker_progress(LoadStage stage, uint32_t total) {
    progress_done_.store(0);
    progress_total_.store(total);
    load_stage_.store(stage);
}

// Main thread only. Emits load_progress_changed when the stage changes or progress moves by 0.5%.
void GDIFCManager::_update_load_progress() {
    const int stage = load_stage_.load();

    float fraction = 0.0f;
    switch (stage) {
        case STAGE_PARSING: {
            // web-ifc's read is one opaque call, estimated from the file size and capped so it
            // never looks finished. Setting up the geometry processor after it is quick.
            const uint32_t done = progress_done_.load();
            const uint32_t total = progress_total_.load();
            if (total > 0 && done >= total) {
                fraction = 1.0f;
            } else if (total > 0 && done >= 1) {
                fraction = 0.95f;
            } else if (total > 0) {
                const double expected = MAX(1.0, (double)parse_bytes_ / PARSE_BYTES_PER_USEC);
                const double elapsed = (double)(Time::get_singleton()->get_ticks_usec() - parse_step_start_usec_.load());
                fraction = 0.9f * (float)MIN(0.95, elapsed / expected);
            }
        } break;
        case STAGE_GEOMETRY:
        case STAGE_LODS: {
            const uint32_t total = progress_total_.load();
            fraction = total > 0 ? (float)MIN(progress_done_.load(), total) / (float)total : 0.0f;
        } break;
        case STAGE_NODES:
            fraction = generation_queue.is_empty() ? 1.0f : (float)current_generation_index / (float)generation_queue.size();
            break;
        case STAGE_EDITOR_CACHES:
            fraction = editor_cache_queue.is_empty() ? 1.0f : (float)current_editor_cache_index / (float)editor_cache_queue.size();
            break;
        default:
            break;
    }

    float progress = load_progress_;
    if (stage != STAGE_IDLE && stage != STAGE_FAILED) {
        const StageRange range = get_stage_range(stage, Engine::get_singleton()->is_editor_hint());
        // The worker can be read mid-way through switching stages, so never step backwards
        progress = MAX(load_progress_, range.start + (range.end - range.start) * fraction);
    }

    if (stage == reported_stage_ && progress - load_progress_ < 0.005f && !(progress >= 1.0f && load_progress_ < 1.0f)) {
        return;
    }
    load_progress_ = progress;
    reported_stage_ = stage;
    emit_signal("load_progress_changed", load_progress_, get_load_stage());
}

// ---------------------------------------------------------
// COLD RELOAD: detect saved IFC tree and re-parse metadata
// ---------------------------------------------------------
void GDIFCManager::_ready() {
    // A saved model's materials can be replaced even when its .ifc file can't be found
    _watch_material_library();

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
    _apply_instance_reference_placement();

    UtilityFunctions::print_rich("[color=blue]Cold reload detected — re-parsing IFC metadata from: " + ifc_file_path_);

    relink_failed_ = false;
    thread_failed_ = false;
    current_state = LOADING_THREAD;
    load_progress_ = 0.0f;
    reported_stage_ = STAGE_IDLE;
    _set_worker_progress(STAGE_PARSING, 0);
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

    _relink_material_library();
}

Error GDIFCManager::read_ifc(const String &_path, bool _create_collision, const Array &_collision_classes) {

    ERR_FAIL_COND_V_MSG(_is_loading(), ERR_BUSY, "Already loading!");

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

    ERR_FAIL_COND_V_MSG(_is_loading(), ERR_BUSY, "Already loading!");
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

// The schemas GDIFC reads properties and typed objects from. web-ifc reads geometry from more.
static const char *SUPPORTED_IFC_SCHEMAS[] = {"IFC2X3", "IFC4", "IFC4X3_ADD2"};

// Reads the identifiers in FILE_SCHEMA from the header, without a full parse
static std::vector<std::string> read_file_schemas(const PackedByteArray &buffer) {
    // The header sits at the top of the file
    const size_t head_size = std::min<size_t>(buffer.size(), 64 * 1024);
    std::string head(reinterpret_cast<const char *>(buffer.ptr()), head_size);
    std::transform(head.begin(), head.end(), head.begin(), [](unsigned char c) { return (char)std::toupper(c); });

    std::vector<std::string> schemas;
    size_t pos = 0;
    while ((pos = head.find("FILE_SCHEMA", pos)) != std::string::npos) {
        pos += strlen("FILE_SCHEMA");
        // Skip mentions of the keyword inside strings, e.g. in FILE_DESCRIPTION
        const size_t open_paren = head.find_first_not_of(" \t\r\n", pos);
        if (open_paren == std::string::npos || head[open_paren] != '(') continue;

        const size_t end = head.find(';', open_paren);
        if (end == std::string::npos) break;
        size_t cursor = open_paren;
        while (true) {
            const size_t open = head.find('\'', cursor);
            if (open == std::string::npos || open > end) break;
            const size_t close = head.find('\'', open + 1);
            if (close == std::string::npos || close > end) break;
            schemas.push_back(head.substr(open + 1, close - open - 1));
            cursor = close + 1;
        }
        break;
    }
    return schemas;
}

// IfcParse can't read files with other schemas. Loading goes on, since web-ifc still reads the
// geometry, but the user is told clearly why properties and typed objects will be missing.
static void warn_if_schema_unsupported(const PackedByteArray &buffer) {
    const std::vector<std::string> schemas = read_file_schemas(buffer);
    String problem;
    String hint = "Re-export the model with one of the supported schemas.";

    if (schemas.empty()) {
        problem = "The file header declares no schema in FILE_SCHEMA. The file is not a valid IFC-SPF file or its header is damaged.";
        hint = "Check that the file is a .ifc (STEP) file and that its HEADER section is complete.";
    } else if (schemas.size() > 1) {
        String list;
        for (const std::string &schema : schemas) {
            list += String(list.is_empty() ? "'" : ", '") + schema.c_str() + "'";
        }
        problem = String("The file declares several schemas (") + list + "). Only one is allowed.";
    } else {
        const std::string &schema = schemas.front();
        for (const char *supported : SUPPORTED_IFC_SCHEMAS) {
            if (schema == supported) return;
        }
        problem = String("The file declares schema '") + schema.c_str() + "', which GDIFC can't read.";
        if (schema.rfind("IFC4X3", 0) == 0) {
            hint = "Most exporters can write IFC 4.3 as IFC4X3_ADD2 (IFC 4.3 ADD2).";
        }
    }

    const String details = problem
        + " Supported schemas: IFC2X3 (IFC 2x3 TC1), IFC4 (IFC 4 ADD2 TC1) and IFC4X3_ADD2 (IFC 4.3 ADD2)."
        + " Geometry still loads, but attributes, property sets, quantities, typed objects and georeferencing won't be available. "
        + hint;
    // The schema name comes from the file, so its brackets are escaped for BBCode
    UtilityFunctions::print_rich(String("[b][color=orange]Unsupported IFC file: properties and typed objects will be unavailable. Only the geometry will load.[/color][/b]\n")
        + "[color=orange]" + details.replace("[", "[lb]") + "[/color]");
    WARN_PRINT(details);
}

Error GDIFCManager::_start_loading(bool _create_collision, const Array &_collision_classes) {
    _reset_load_state(_create_collision, _collision_classes);

    // Start Threads
    task_id = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &GDIFCManager::_thread_task), true);
    metadata_task_id = WorkerThreadPool::get_singleton()->add_task(callable_mp(this, &GDIFCManager::_metadata_parse_task), true);
    set_process(true);
    _update_load_progress();
    return Error::OK;
}

// Clears the previous load and prepares both pipelines for pending_buffer
void GDIFCManager::_reset_load_state(bool _create_collision, const Array &_collision_classes) {
    this->should_create_collisions = _create_collision;
    this->collision_classes = _collision_classes;
    this->generation_queue.clear();
    this->editor_cache_queue.clear();
    this->current_editor_cache_index = 0;
    this->material_cache.clear();
    this->ifc_materials_.clear();
    this->_unwatch_material_library();
    this->material_library_.unref();
    this->metadata_material_library_.unref();
    this->metadata_material_styles_.clear();
    this->metadata_colour_styles_.clear();
    this->node_registry.clear();
    this->current_generation_index = 0;
    this->thread_failed_ = false;
    this->relink_failed_ = false;
    this->current_state = LOADING_THREAD;

    // Snapshot LOD options once so edits to the settings resource mid-load aren't picked up
    this->lod_enabled_ = geometric_settings.is_valid() && geometric_settings->getGenerateLods();
    this->lod_merge_angle_ = geometric_settings.is_valid() ? geometric_settings->getLodNormalMergeAngle() : 60.0f;
    this->ifc_materials_enabled_ = geometric_settings.is_valid() && geometric_settings->getIfcMaterials();
    this->ifc_textures_enabled_ = this->ifc_materials_enabled_ && geometric_settings->getIfcTextures();

    // Parsing has two steps: web-ifc's read, then the geometry processor
    this->parse_bytes_ = this->pending_buffer.size();
    this->load_progress_ = 0.0f;
    this->reported_stage_ = STAGE_IDLE;
    this->parse_step_start_usec_.store(Time::get_singleton()->get_ticks_usec());
    _set_worker_progress(STAGE_PARSING, 2);

    warn_if_schema_unsupported(this->pending_buffer);

    // IfcParse gets its own reference to the file bytes and parses them alongside web-ifc
    this->metadata_buffer_ = this->pending_buffer;
    this->metadata_file_.reset();
    this->metadata_schema_index_ = -1;
    this->metadata_ids_.clear();
    this->metadata_results_.clear();
    this->metadata_apply_index_ = 0;
    this->metadata_georreference_ = GeorreferenceData();
    this->metadata_state = META_PARSING;
}

// Runs the same pipeline as read_ifc() to completion before returning, with the model added
// as a child and every generated node owned by this manager, ready to be packed into a scene.
// Used by the editor importer, which has to produce the scene within one call.
Error GDIFCManager::read_ifc_blocking(const String &_path, bool _create_collision, const Array &_collision_classes) {
    ERR_FAIL_COND_V_MSG(_is_loading(), ERR_BUSY, "Already loading!");

    start_loading_time = Time::get_singleton()->get_ticks_usec();

    Ref<FileAccess> fa = FileAccess::open(_path, FileAccess::READ);
    if (!fa.is_valid()) {
        ERR_FAIL_V_MSG(Error::FAILED, String("Failed to open IFC file: ") + _path
                       + String(" (error: ") + String::num_int64((int64_t)FileAccess::get_open_error()) + String(")"));
    }
    this->pending_buffer = fa->get_buffer(fa->get_length());
    fa.unref();
    ERR_FAIL_COND_V_MSG(this->pending_buffer.is_empty(), Error::FAILED, "IFC file is empty.");

    ifc_file_path_ = _path;
    _reset_load_state(_create_collision, _collision_classes);

    // web-ifc and IfcParse still parse side by side
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();
    task_id = pool->add_task(callable_mp(this, &GDIFCManager::_thread_task), true);
    metadata_task_id = pool->add_task(callable_mp(this, &GDIFCManager::_metadata_parse_task), true);
    _wait_for_task();
    _wait_for_metadata_task();

    if (thread_failed_) {
        pending_ifc_parse_file.reset();
        generation_queue.clear();
        pending_buffer = PackedByteArray();
        metadata_file_.reset();
        current_state = FAILED;
        metadata_state = META_IDLE;
        load_stage_.store(STAGE_FAILED);
        return Error::FAILED;
    }
    georreference = pending_georreference;

    if (lod_enabled_ && !generation_queue.is_empty()) {
        current_state = GENERATING_LODS;
        lod_items_ = generation_queue.ptrw();
        _set_worker_progress(STAGE_LODS, generation_queue.size());
        lod_group_id = pool->add_group_task(
            callable_mp(this, &GDIFCManager::_lod_group_task), generation_queue.size(), -1, true, "GDIFC LOD generation");
        pool->wait_for_group_task_completion(lod_group_id);
        lod_group_id = -1;
        lod_items_ = nullptr;
    }

    current_state = GENERATING_NODES;
    load_stage_.store(STAGE_NODES);
    IFCModel *model = memnew(IFCModel);
    model->set_name("IFCModel");
    ifc_model_node_id_ = model->get_instance_id();
    invisible_staging_root = model;
    for (int i = 0; i < generation_queue.size(); i++) {
        _create_item_node(generation_queue[i], false);
    }
    invisible_staging_root = nullptr;
    add_child(model, true);

    metadata_ids_.clear();
    metadata_ids_.reserve(generation_queue.size());
    for (int i = 0; i < generation_queue.size(); i++) {
        metadata_ids_.push_back(generation_queue[i].express_id);
    }
    generation_queue.clear();
    current_state = DONE;
    load_stage_.store(STAGE_DONE);

    if (metadata_file_) {
        _metadata_extract_task();
        for (PrecalculatedIFCMetadata &item : metadata_results_) {
            _apply_metadata_item(item);
        }
    }
    _finish_metadata();

    std::function<void(Node*)> set_owner_recursive = [&](Node* n) {
        n->set_owner(this);
        for (int i = 0; i < n->get_child_count(); i++) {
            set_owner_recursive(n->get_child(i));
        }
    };
    set_owner_recursive(model);

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
                // The task returned early and left its partial results behind.
                // ifc_load_failed is emitted once the metadata task has stopped too.
                pending_ifc_parse_file.reset();
                generation_queue.clear();
                pending_buffer = PackedByteArray();
                current_state = FAILED;
                load_stage_.store(STAGE_FAILED);
                _update_load_progress();
            } else if (metadata_state == META_IDLE) {
                // Cold reload: metadata only, no node generation
                if (pending_ifc_parse_file) {
                    ifc_parse_file = std::move(pending_ifc_parse_file);
                }
                if (relink_failed_) {
                    // File was missing — warn and finish
                    WARN_PRINT(String("GDIFCManager: IFC file not found at '") + ifc_file_path_
                        + "'. The model scene is intact but ifc_object properties are unavailable. "
                        + "Call read_ifc() with a valid path to restore them.");
                    current_state = DONE;
                    load_stage_.store(STAGE_DONE);
                    _update_load_progress();
                    emit_signal("ifc_objects_ready");
                } else {
                    current_state = RELINKING;
                }
            } else {
                // Fresh load — create the staging root, then LODs (if enabled) and nodes
                georreference = pending_georreference;
                metadata_ids_.clear();
                metadata_ids_.reserve(generation_queue.size());
                for (int i = 0; i < generation_queue.size(); i++) {
                    metadata_ids_.push_back(generation_queue[i].express_id);
                }

                invisible_staging_root = memnew(IFCModel);
                invisible_staging_root->set_name("IFC_Staging_Area");
                ifc_model_node_id_ = invisible_staging_root->get_instance_id();

                if (lod_enabled_ && !generation_queue.is_empty()) {
                    // Each item is independent, so LODs are generated on every core. The queue
                    // isn't touched on the main thread until the group completes.
                    lod_items_ = generation_queue.ptrw();
                    _set_worker_progress(STAGE_LODS, generation_queue.size());
                    lod_group_id = WorkerThreadPool::get_singleton()->add_group_task(
                        callable_mp(this, &GDIFCManager::_lod_group_task), generation_queue.size(), -1, true, "GDIFC LOD generation");
                    current_state = GENERATING_LODS;
                } else {
                    current_state = GENERATING_NODES;
                    load_stage_.store(STAGE_NODES);
                }
            }
        }
    } else if (current_state == GENERATING_LODS) {
        if (WorkerThreadPool::get_singleton()->is_group_task_completed(lod_group_id)) {
            WorkerThreadPool::get_singleton()->wait_for_group_task_completion(lod_group_id);
            lod_group_id = -1;
            lod_items_ = nullptr;
            current_state = GENERATING_NODES;
            load_stage_.store(STAGE_NODES);
        }
    } else if (current_state == GENERATING_NODES) {
        _process_generation_queue();
    } else if (current_state == RELINKING) {
        _relink_ifc_objects();
        current_state = DONE;
        load_stage_.store(STAGE_DONE);
        _update_load_progress();
        emit_signal("ifc_objects_ready");
        UtilityFunctions::print("IFC objects relinked from saved scene.");
    }

    _process_metadata();

    // Processing stops this frame, since nothing is loading any more, so this fires once
    if (current_state == FAILED && metadata_state == META_IDLE) {
        emit_signal("ifc_load_failed");
    }

    if (current_state == LOADING_THREAD || current_state == GENERATING_LODS || current_state == GENERATING_NODES || current_state == RELINKING) {
        _update_load_progress();
    }
    if (!_is_loading()) {
        set_process(false);
    }
}

// ---------------------------------------------------------
// DEFERRED METADATA
// ---------------------------------------------------------

// Parses the file with IfcParse alongside web-ifc. Writes only metadata_* members.
void GDIFCManager::_metadata_parse_task() {
    try {
        auto file = std::make_shared<IfcParse::IfcFile>((void*)metadata_buffer_.ptr(), static_cast<int>(metadata_buffer_.size()));
        if (!file->good()) {
            ERR_PRINT("[IfcParse] Failed to parse IFC file. Properties and typed objects are unavailable.");
        } else {
            const std::string schema_name = file->schema()->name();
            if (schema_name == Ifc4::get_schema().name()) {
                metadata_schema_index_ = 0;
            } else if (schema_name == Ifc2x3::get_schema().name()) {
                metadata_schema_index_ = 2;
            } else if (schema_name == Ifc4x3_add2::get_schema().name()) {
                metadata_schema_index_ = 3;
            } else {
                ERR_PRINT(String("[IfcParse] Schema ") + schema_name.c_str()
                    + " is not supported. Properties and typed objects are unavailable.");
            }
            if (metadata_schema_index_ >= 0) {
                metadata_file_ = std::move(file);
            }
        }
    } catch (const std::exception& e) {
        ERR_PRINT(String("[IfcParse] Failed to parse IFC file: ") + e.what());
    } catch (...) {
        ERR_PRINT("[IfcParse] Failed to parse IFC file: unknown error.");
    }

    // Both parsers have consumed the bytes; web-ifc's task drops its own reference
    metadata_buffer_ = PackedByteArray();
}

// ---------------------------------------------------------
// IFC MATERIAL DEFINITIONS
// ---------------------------------------------------------

template <typename schema, typename Properties>
static godot::Dictionary convert_ifc_properties(const Properties& props);

// IfcParse attributes read by name, so one reader covers every schema: a schema without an
// attribute (IFC2X3 layers have no Name, for example) leaves the field at its default.
static ptrdiff_t ifc_attribute_index(IfcUtil::IfcBaseClass* entity, const char* name) {
    const IfcParse::entity* declaration = entity->declaration().as_entity();
    return declaration ? declaration->attribute_index(name) : -1;
}

static String ifc_string(IfcUtil::IfcBaseClass* entity, const char* name) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    return index < 0 ? String() : gd_attr_string(entity->get_attribute_value(index));
}

static double ifc_double(IfcUtil::IfcBaseClass* entity, const char* name, double fallback) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return fallback;
    const AttributeValue value = entity->get_attribute_value(index);
    return value.type() == IfcUtil::Argument_DOUBLE ? (double)value : fallback;
}

static int64_t ifc_int(IfcUtil::IfcBaseClass* entity, const char* name, int64_t fallback) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return fallback;
    const AttributeValue value = entity->get_attribute_value(index);
    return value.type() == IfcUtil::Argument_INT ? (int64_t)(int)value : fallback;
}

static bool ifc_bool(IfcUtil::IfcBaseClass* entity, const char* name) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    return index >= 0 && gd_attr_bool(entity->get_attribute_value(index));
}

static IfcUtil::IfcBaseClass* ifc_entity(IfcUtil::IfcBaseClass* entity, const char* name) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return nullptr;
    const AttributeValue value = entity->get_attribute_value(index);
    return value.type() == IfcUtil::Argument_ENTITY_INSTANCE ? (IfcUtil::IfcBaseClass*)value : nullptr;
}

static std::vector<IfcUtil::IfcBaseClass*> ifc_entities(IfcUtil::IfcBaseClass* entity, const char* name) {
    std::vector<IfcUtil::IfcBaseClass*> result;
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return result;
    const AttributeValue value = entity->get_attribute_value(index);
    if (value.type() != IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE) return result;
    boost::shared_ptr<aggregate_of_instance> aggregate = value;
    if (aggregate) {
        for (IfcUtil::IfcBaseClass* item : *aggregate) {
            if (item) result.push_back(item);
        }
    }
    return result;
}

// Every attribute but `skip`, for IFC2X3 material properties, which are entities with fixed attributes
static Dictionary ifc_attributes(IfcUtil::IfcBaseClass* entity, const char* skip) {
    Dictionary result;
    const IfcParse::entity* declaration = entity->declaration().as_entity();
    if (!declaration) return result;
    const auto attributes = declaration->all_attributes();
    for (size_t i = 0; i < attributes.size(); i++) {
        if (attributes[i]->name() == skip) continue;
        result[String::utf8(attributes[i]->name().c_str())] = convert_attribute_value(entity->get_attribute_value(i));
    }
    return result;
}

// Instances of an IFC class and its subtypes in file order. Empty when the schema has no such class.
static std::vector<IfcUtil::IfcBaseClass*> ifc_instances(IfcParse::IfcFile& file, const char* ifc_class) {
    std::vector<IfcUtil::IfcBaseClass*> result;
    try {
        if (aggregate_of_instance::ptr instances = file.instances_by_type(ifc_class)) {
            result.assign(instances->begin(), instances->end());
        }
    } catch (...) {
        return result;
    }
    std::sort(result.begin(), result.end(), [](IfcUtil::IfcBaseClass* a, IfcUtil::IfcBaseClass* b) { return a->id() < b->id(); });
    return result;
}

template <typename T>
static Ref<T> material_as(const Ref<GDIFCMaterialDefinition>& definition) {
    return Ref<T>(Object::cast_to<T>(definition.ptr()));
}

// Builds one GDIFCMaterialDefinition per IFC material definition, shared by every element using it.
// Runs on the metadata task. The relationships it follows are each collected in one pass, and an
// element associated more than once keeps its first material, as in the web-ifc task.
template <typename schema>
class MaterialReader {
public:
    explicit MaterialReader(const std::shared_ptr<IfcParse::IfcFile>& file) : file_(file) {
        IfcParse::IfcFile& f = *file_;
        for (IfcUtil::IfcBaseClass* rel : ifc_instances(f, "IfcRelAssociatesMaterial")) {
            IfcUtil::IfcBaseClass* material = ifc_entity(rel, "RelatingMaterial");
            if (!material) continue;
            for (IfcUtil::IfcBaseClass* object : ifc_entities(rel, "RelatedObjects")) {
                element_material_.try_emplace((int)object->id(), material);
            }
        }
        // Occurrences without a material of their own use their type's
        for (IfcUtil::IfcBaseClass* rel : ifc_instances(f, "IfcRelDefinesByType")) {
            IfcUtil::IfcBaseClass* type = ifc_entity(rel, "RelatingType");
            if (!type) continue;
            const std::vector<IfcUtil::IfcBaseClass*> objects = ifc_entities(rel, "RelatedObjects");
            for (IfcUtil::IfcBaseClass* object : objects) {
                element_type_.try_emplace((int)object->id(), (int)type->id());
            }
            auto type_material = element_material_.find((int)type->id());
            if (type_material == element_material_.end()) continue;
            IfcUtil::IfcBaseClass* material = type_material->second; // emplacing below may rehash
            for (IfcUtil::IfcBaseClass* object : objects) {
                element_material_.try_emplace((int)object->id(), material);
            }
        }
        for (IfcUtil::IfcBaseClass* properties : ifc_instances(f, "IfcMaterialProperties")) {
            if (IfcUtil::IfcBaseClass* material = ifc_entity(properties, "Material")) {
                properties_[(int)material->id()].push_back(properties);
            }
        }
        for (IfcUtil::IfcBaseClass* rel : ifc_instances(f, "IfcMaterialClassificationRelationship")) {
            if (IfcUtil::IfcBaseClass* material = ifc_entity(rel, "ClassifiedMaterial")) {
                for (IfcUtil::IfcBaseClass* classification : ifc_entities(rel, "MaterialClassifications")) {
                    references_[(int)material->id()].push_back(classification);
                }
            }
        }
        // IFC4: classification and library references of any resource, materials included
        for (IfcUtil::IfcBaseClass* rel : ifc_instances(f, "IfcExternalReferenceRelationship")) {
            IfcUtil::IfcBaseClass* reference = ifc_entity(rel, "RelatingReference");
            if (!reference) continue;
            for (IfcUtil::IfcBaseClass* object : ifc_entities(rel, "RelatedResourceObjects")) {
                references_[(int)object->id()].push_back(reference);
            }
        }
        for (IfcUtil::IfcBaseClass* rel : ifc_instances(f, "IfcMaterialRelationship")) {
            if (IfcUtil::IfcBaseClass* material = ifc_entity(rel, "RelatingMaterial")) {
                relationships_[(int)material->id()].push_back(rel);
            }
        }
    }

    // The element's material definition: its own association, else its type's. Null if it has none.
    Ref<GDIFCMaterialDefinition> for_element(int express_id) {
        // The type's materials are in the library too: shape aspects on the type's geometry can show
        // them on an occurrence with a material set of its own
        if (auto type = element_type_.find(express_id); type != element_type_.end()) {
            if (auto type_material = element_material_.find(type->second); type_material != element_material_.end()) {
                read(type_material->second);
            }
        }
        auto material = element_material_.find(express_id);
        if (material == element_material_.end()) return {};
        Ref<GDIFCMaterialDefinition> definition = read(material->second);
        if (definition.is_valid()) associated_.insert((int)definition->get_express_id());
        return definition;
    }

    // The definitions elements were associated with, and every IfcMaterial read
    Ref<GDIFCMaterialLibrary> make_library() const {
        TypedArray<GDIFCMaterialDefinition> definitions;
        TypedArray<GDIFCMaterialSingle> materials;
        for (const auto& [id, definition] : definitions_) {
            if (associated_.count(id)) definitions.push_back(definition);
            if (Ref<GDIFCMaterialSingle> material = material_as<GDIFCMaterialSingle>(definition); material.is_valid()) {
                materials.push_back(material);
            }
        }
        Ref<GDIFCMaterialLibrary> library;
        library.instantiate();
        library->set_definitions(definitions);
        library->set_materials(materials);
        return library;
    }

private:
    std::shared_ptr<IfcParse::IfcFile> file_;
    std::unordered_map<int, IfcUtil::IfcBaseClass*> element_material_;
    std::unordered_map<int, int> element_type_; // occurrence -> its type
    std::unordered_map<int, std::vector<IfcUtil::IfcBaseClass*>> properties_;
    std::unordered_map<int, std::vector<IfcUtil::IfcBaseClass*>> references_;
    std::unordered_map<int, std::vector<IfcUtil::IfcBaseClass*>> relationships_;
    // Ordered by express id, so the library is in file order
    std::map<int, Ref<GDIFCMaterialDefinition>> definitions_;
    std::unordered_set<int> associated_;

    // An empty definition of the class matching the IFC entity, or null for anything else
    static Ref<GDIFCMaterialDefinition> create(const IfcParse::declaration& type) {
        // is() matches subtypes, e.g. IfcMaterialLayerWithOffsets is an IfcMaterialLayer
        if (type.is("IfcMaterial")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialSingle));
        if (type.is("IfcMaterialLayer")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialLayer));
        if (type.is("IfcMaterialLayerSet")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialLayerSet));
        if (type.is("IfcMaterialLayerSetUsage")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialLayerSetUsage));
        if (type.is("IfcMaterialProfile")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialProfile));
        if (type.is("IfcMaterialProfileSet")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialProfileSet));
        if (type.is("IfcMaterialProfileSetUsage")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialProfileSetUsage));
        if (type.is("IfcMaterialConstituent")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialConstituent));
        if (type.is("IfcMaterialConstituentSet")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialConstituentSet));
        if (type.is("IfcMaterialList")) return Ref<GDIFCMaterialDefinition>(memnew(GDIFCMaterialList));
        return {};
    }

    Ref<GDIFCMaterialDefinition> read(IfcUtil::IfcBaseClass* entity) {
        if (!entity) return {};
        const int id = (int)entity->id();
        auto cached = definitions_.find(id);
        if (cached != definitions_.end()) return cached->second;

        const IfcParse::declaration& type = entity->declaration();
        Ref<GDIFCMaterialDefinition> definition = create(type);
        if (definition.is_null()) return {};
        // Cached before its members are read, so each is read once
        definitions_[id] = definition;

        definition->set_express_id(id);
        definition->set_name(ifc_string(entity, type.is("IfcMaterialLayerSet") ? "LayerSetName" : "Name"));
        definition->set_description(ifc_string(entity, "Description"));
        definition->set_category(ifc_string(entity, "Category"));
        definition->set_properties(read_properties(id));
        definition->set_classifications(read_references(id));
        definition->set_ifc_object(GDIFCEntityBase::wrap(entity, file_));
        read_members(entity, definition.ptr());
        return definition;
    }

    Ref<GDIFCMaterialSingle> read_material(IfcUtil::IfcBaseClass* entity) {
        return material_as<GDIFCMaterialSingle>(read(entity));
    }

    template <typename T>
    TypedArray<T> read_all(IfcUtil::IfcBaseClass* entity, const char* name) {
        TypedArray<T> result;
        for (IfcUtil::IfcBaseClass* member : ifc_entities(entity, name)) {
            if (Ref<T> definition = material_as<T>(read(member)); definition.is_valid()) {
                result.push_back(definition);
            }
        }
        return result;
    }

    void read_members(IfcUtil::IfcBaseClass* entity, GDIFCMaterialDefinition* definition) {
        if (auto* material = Object::cast_to<GDIFCMaterialSingle>(definition)) {
            material->set_relationships(read_relationships((int)entity->id()));
        } else if (auto* layer = Object::cast_to<GDIFCMaterialLayer>(definition)) {
            layer->set_material(read_material(ifc_entity(entity, "Material")));
            layer->set_thickness(ifc_double(entity, "LayerThickness", 0.0));
            layer->set_is_ventilated(ifc_bool(entity, "IsVentilated"));
            layer->set_priority(ifc_int(entity, "Priority", -1));
        } else if (auto* layer_set = Object::cast_to<GDIFCMaterialLayerSet>(definition)) {
            layer_set->set_layers(read_all<GDIFCMaterialLayer>(entity, "MaterialLayers"));
        } else if (auto* layer_usage = Object::cast_to<GDIFCMaterialLayerSetUsage>(definition)) {
            Ref<GDIFCMaterialLayerSet> used = material_as<GDIFCMaterialLayerSet>(read(ifc_entity(entity, "ForLayerSet")));
            layer_usage->set_layer_set(used);
            const String direction = ifc_string(entity, "LayerSetDirection");
            layer_usage->set_direction(direction == "AXIS1" ? GDIFCMaterialLayerSetUsage::AXIS1
                : direction == "AXIS3" ? GDIFCMaterialLayerSetUsage::AXIS3 : GDIFCMaterialLayerSetUsage::AXIS2);
            layer_usage->set_sense(ifc_string(entity, "DirectionSense") == "NEGATIVE"
                ? GDIFCMaterialLayerSetUsage::NEGATIVE : GDIFCMaterialLayerSetUsage::POSITIVE);
            layer_usage->set_offset(ifc_double(entity, "OffsetFromReferenceLine", 0.0));
            layer_usage->set_reference_extent(ifc_double(entity, "ReferenceExtent", 0.0));
            // A usage has no name of its own
            if (used.is_valid()) layer_usage->set_name(used->get_name());
        } else if (auto* profile = Object::cast_to<GDIFCMaterialProfile>(definition)) {
            profile->set_material(read_material(ifc_entity(entity, "Material")));
            profile->set_priority(ifc_int(entity, "Priority", -1));
        } else if (auto* profile_set = Object::cast_to<GDIFCMaterialProfileSet>(definition)) {
            profile_set->set_profiles(read_all<GDIFCMaterialProfile>(entity, "MaterialProfiles"));
        } else if (auto* profile_usage = Object::cast_to<GDIFCMaterialProfileSetUsage>(definition)) {
            Ref<GDIFCMaterialProfileSet> used = material_as<GDIFCMaterialProfileSet>(read(ifc_entity(entity, "ForProfileSet")));
            profile_usage->set_profile_set(used);
            profile_usage->set_cardinal_point(ifc_int(entity, "CardinalPoint", 0));
            profile_usage->set_reference_extent(ifc_double(entity, "ReferenceExtent", 0.0));
            if (used.is_valid()) profile_usage->set_name(used->get_name());
        } else if (auto* constituent = Object::cast_to<GDIFCMaterialConstituent>(definition)) {
            constituent->set_material(read_material(ifc_entity(entity, "Material")));
            constituent->set_fraction(ifc_double(entity, "Fraction", -1.0));
        } else if (auto* constituent_set = Object::cast_to<GDIFCMaterialConstituentSet>(definition)) {
            constituent_set->set_constituents(read_all<GDIFCMaterialConstituent>(entity, "MaterialConstituents"));
        } else if (auto* list = Object::cast_to<GDIFCMaterialList>(definition)) {
            list->set_materials(read_all<GDIFCMaterialSingle>(entity, "Materials"));
        }
    }

    // Material property sets, by name. IFC4 and later hold IfcProperty lists like property sets;
    // IFC2X3 also has typed entities (thermal, mechanical, ...) whose attributes are the properties.
    Dictionary read_properties(int id) {
        Dictionary sets;
        auto found = properties_.find(id);
        if (found == properties_.end()) return sets;
        for (IfcUtil::IfcBaseClass* entity : found->second) {
            String name = ifc_string(entity, "Name");
            if (name.is_empty()) name = String(entity->declaration().name().c_str());
            if constexpr (std::is_same_v<schema, Ifc2x3>) {
                if (auto* extended = entity->template as<Ifc2x3::IfcExtendedMaterialProperties>()) {
                    sets[name] = convert_ifc_properties<schema>(extended->ExtendedProperties());
                } else {
                    sets[name] = ifc_attributes(entity, "Material");
                }
            } else {
                if (auto* extended = entity->template as<typename schema::IfcMaterialProperties>()) {
                    sets[name] = convert_ifc_properties<schema>(extended->Properties());
                }
            }
        }
        return sets;
    }

    // Classification and library references: their type and whichever identifying fields they have
    Array read_references(int id) {
        Array result;
        auto found = references_.find(id);
        if (found == references_.end()) return result;
        static const char* const FIELDS[][2] = {
            {"Identification", "identification"}, // IFC4
            {"ItemReference", "item_reference"},  // IFC2X3
            {"Name", "name"},
            {"Location", "location"},
            {"Description", "description"},
        };
        for (IfcUtil::IfcBaseClass* reference : found->second) {
            Dictionary entry;
            entry["type"] = String(reference->declaration().name().c_str());
            for (const auto& field : FIELDS) {
                const String value = ifc_string(reference, field[0]);
                if (!value.is_empty()) entry[field[1]] = value;
            }
            // IFC2X3 classification notations hold their codes as facets
            PackedStringArray facets;
            for (IfcUtil::IfcBaseClass* facet : ifc_entities(reference, "NotationFacets")) {
                facets.push_back(ifc_string(facet, "NotationValue"));
            }
            if (!facets.is_empty()) entry["notation"] = String(".").join(facets);
            if (IfcUtil::IfcBaseClass* source = ifc_entity(reference, "ReferencedSource")) {
                const String source_name = ifc_string(source, "Name");
                if (!source_name.is_empty()) entry["source"] = source_name;
            }
            result.push_back(entry);
        }
        return result;
    }

    // IFC4 IfcMaterialRelationship, where this material is the whole made of the related materials
    Array read_relationships(int id) {
        Array result;
        auto found = relationships_.find(id);
        if (found == relationships_.end()) return result;
        for (IfcUtil::IfcBaseClass* rel : found->second) {
            Dictionary entry;
            entry["name"] = ifc_string(rel, "Name");
            entry["description"] = ifc_string(rel, "Description");
            entry["expression"] = ifc_string(rel, "Expression");
            entry["materials"] = read_all<GDIFCMaterialSingle>(rel, "RelatedMaterials");
            result.push_back(entry);
        }
        return result;
    }
};

static std::vector<double> ifc_doubles(IfcUtil::IfcBaseClass* entity, const char* name) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return {};
    const AttributeValue value = entity->get_attribute_value(index);
    if (value.type() != IfcUtil::Argument_AGGREGATE_OF_DOUBLE) return {};
    return (std::vector<double>)value;
}

// A STEP binary as bytes, in file order. IfcParse stores its first bit at the highest index.
static PackedByteArray binary_to_bytes(const boost::dynamic_bitset<>& bits) {
    PackedByteArray bytes;
    bytes.resize((int64_t)((bits.size() + 7) / 8));
    bytes.fill(0);
    uint8_t* out = bytes.ptrw();
    for (size_t p = 0; p < bits.size(); p++) {
        if (bits[bits.size() - 1 - p]) out[p / 8] |= (uint8_t)(0x80 >> (p % 8));
    }
    return bytes;
}

static PackedByteArray ifc_binary(IfcUtil::IfcBaseClass* entity, const char* name) {
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return {};
    const AttributeValue value = entity->get_attribute_value(index);
    if (value.type() != IfcUtil::Argument_BINARY) return {};
    return binary_to_bytes((boost::dynamic_bitset<>)value);
}

static std::vector<PackedByteArray> ifc_binaries(IfcUtil::IfcBaseClass* entity, const char* name) {
    std::vector<PackedByteArray> result;
    const ptrdiff_t index = ifc_attribute_index(entity, name);
    if (index < 0) return result;
    const AttributeValue value = entity->get_attribute_value(index);
    if (value.type() != IfcUtil::Argument_AGGREGATE_OF_BINARY) return result;
    for (const boost::dynamic_bitset<>& bits : (std::vector<boost::dynamic_bitset<>>)value) {
        result.push_back(binary_to_bytes(bits));
    }
    return result;
}

bool PrecalculatedIFCStyle::same_look(const PrecalculatedIFCStyle& other) const {
    if (roughness != other.roughness || metallic != other.metallic || textures.size() != other.textures.size()) return false;
    for (size_t i = 0; i < textures.size(); i++) {
        const Texture& a = textures[i];
        const Texture& b = other.textures[i];
        if (a.slot != b.slot || a.key != b.key || a.repeat != b.repeat || a.uv_scale != b.uv_scale || a.uv_offset != b.uv_offset) return false;
    }
    return true;
}

// web-ifc's colour for geometry without any style. Materials shared by it are never given a style,
// since a white style can't be told apart from no style.
static const char* const UNSTYLED_COLOUR_KEY = "ffffffff_O";

// Reads the looks of surface styles for the generated materials: the style of each IfcMaterial,
// and the style behind each colour of the materials shared per colour, when every style of that
// colour looks the same. Runs on the metadata task.
class StyleReader {
public:
    StyleReader(IfcParse::IfcFile& file, bool with_textures, const String& ifc_path)
        : file_(file), with_textures_(with_textures), ifc_dir_(ifc_path.get_base_dir()) {}

    void read(std::unordered_map<uint32_t, PrecalculatedIFCStyle>& material_styles,
              std::vector<std::pair<String, PrecalculatedIFCStyle>>& colour_styles) {
        // The style an IfcMaterial is presented with
        for (IfcUtil::IfcBaseClass* representation : ifc_instances(file_, "IfcMaterialDefinitionRepresentation")) {
            IfcUtil::IfcBaseClass* material = ifc_entity(representation, "RepresentedMaterial");
            if (!material || !material->declaration().is("IfcMaterial") || material_styles.count((uint32_t)material->id())) continue;
            if (IfcUtil::IfcBaseClass* style = first_surface_style(representation)) {
                PrecalculatedIFCStyle look = read_style(style);
                if (!look.is_empty()) material_styles[(uint32_t)material->id()] = std::move(look);
            }
        }

        // The style behind each colour, keyed like _get_material's colour materials
        struct ColourGroup {
            String key;
            PrecalculatedIFCStyle look;
            bool ambiguous = false;
        };
        std::vector<ColourGroup> groups;
        std::unordered_map<std::string, size_t> group_by_key;
        for (IfcUtil::IfcBaseClass* style : ifc_instances(file_, "IfcSurfaceStyle")) {
            const String key = colour_key(style);
            if (key.is_empty() || key == UNSTYLED_COLOUR_KEY) continue;
            PrecalculatedIFCStyle look = read_style(style);
            auto [found, inserted] = group_by_key.try_emplace(std::string(key.utf8().get_data()), groups.size());
            if (inserted) {
                groups.push_back({ key, std::move(look), false });
            } else if (!groups[found->second].look.same_look(look)) {
                groups[found->second].ambiguous = true;
            }
        }
        for (ColourGroup& group : groups) {
            if (!group.ambiguous && !group.look.is_empty()) colour_styles.emplace_back(group.key, std::move(group.look));
        }

        if (!missing_.empty()) {
            WARN_PRINT(vformat("%d IFC texture(s) could not be found, such as '%s'. Textures are looked for at their path and next to the IFC file; web addresses are not downloaded.",
                (int64_t)missing_.size(), missing_.front()));
        }
    }

private:
    struct LoadedTexture {
        String key;
        Ref<Texture2D> texture;
        Ref<Image> image;
    };

    IfcParse::IfcFile& file_;
    bool with_textures_;
    String ifc_dir_;
    std::unordered_map<int, PrecalculatedIFCStyle> looks_;          // by IfcSurfaceStyle
    std::unordered_map<std::string, LoadedTexture> textures_;      // by path or embedding entity
    std::vector<String> missing_;

    // The first IfcSurfaceStyle of an IfcMaterialDefinitionRepresentation
    static IfcUtil::IfcBaseClass* first_surface_style(IfcUtil::IfcBaseClass* representation) {
        for (IfcUtil::IfcBaseClass* styled_representation : ifc_entities(representation, "Representations")) {
            for (IfcUtil::IfcBaseClass* item : ifc_entities(styled_representation, "Items")) {
                for (IfcUtil::IfcBaseClass* style : ifc_entities(item, "Styles")) {
                    if (style->declaration().is("IfcSurfaceStyle")) return style;
                    // IFC2X3 wraps styles in an IfcPresentationStyleAssignment
                    for (IfcUtil::IfcBaseClass* assigned : ifc_entities(style, "Styles")) {
                        if (assigned->declaration().is("IfcSurfaceStyle")) return assigned;
                    }
                }
            }
        }
        return nullptr;
    }

    // The colour key web-ifc's colour for this style gives in _get_material: its first shading's
    // SurfaceColour, with alpha 1 - Transparency. Empty when it has none.
    static String colour_key(IfcUtil::IfcBaseClass* style) {
        for (IfcUtil::IfcBaseClass* element : ifc_entities(style, "Styles")) {
            if (!element->declaration().is("IfcSurfaceStyleShading")) continue;
            IfcUtil::IfcBaseClass* colour = ifc_entity(element, "SurfaceColour");
            if (!colour || !colour->declaration().is("IfcColourRgb")) continue;
            const double transparency = ifc_double(element, "Transparency", -1.0);
            const Color color(ifc_double(colour, "Red", 0.0), ifc_double(colour, "Green", 0.0), ifc_double(colour, "Blue", 0.0),
                transparency >= 0.0 ? 1.0 - transparency : 1.0);
            return color.to_html() + (color.a < 0.7 ? "_T" : "_O");
        }
        return String();
    }

    const PrecalculatedIFCStyle& read_style(IfcUtil::IfcBaseClass* style) {
        auto [cached, inserted] = looks_.try_emplace((int)style->id());
        if (!inserted) return cached->second;
        PrecalculatedIFCStyle& look = cached->second;
        bool rendering_read = false;
        for (IfcUtil::IfcBaseClass* element : ifc_entities(style, "Styles")) {
            if (!rendering_read && element->declaration().is("IfcSurfaceStyleRendering")) {
                read_rendering(element, look);
                rendering_read = true;
            } else if (with_textures_ && element->declaration().is("IfcSurfaceStyleWithTextures")) {
                for (IfcUtil::IfcBaseClass* texture : ifc_entities(element, "Textures")) {
                    read_texture(texture, look);
                }
            }
        }
        return look;
    }

    // Roughness and metallic from IfcSurfaceStyleRendering. The albedo stays web-ifc's colour.
    static void read_rendering(IfcUtil::IfcBaseClass* rendering, PrecalculatedIFCStyle& look) {
        // IfcSpecularExponent or IfcSpecularRoughness, a value wrapped in its type
        if (IfcUtil::IfcBaseClass* highlight = ifc_entity(rendering, "SpecularHighlight")) {
            const AttributeValue value = highlight->get_attribute_value(0);
            if (value.type() == IfcUtil::Argument_DOUBLE) {
                const double number = value;
                const std::string& type = highlight->declaration().name();
                if (type == "IfcSpecularRoughness") {
                    look.roughness = (float)CLAMP(number, 0.0, 1.0);
                } else if (type == "IfcSpecularExponent" && number >= 0.0) {
                    // Blinn-Phong exponent n to a GGX alpha of sqrt(2 / (n + 2)); roughness is sqrt(alpha)
                    look.roughness = (float)std::pow(2.0 / (number + 2.0), 0.25);
                }
            }
        }
        const String method = ifc_string(rendering, "ReflectanceMethod");
        if (method == "METAL") {
            look.metallic = 1.0f;
        } else if (method == "MIRROR") {
            look.metallic = 1.0f;
            if (look.roughness < 0.0f) look.roughness = 0.0f;
        } else if (method == "GLASS") {
            if (look.roughness < 0.0f) look.roughness = 0.05f;
        }
    }

    // The material slot of an IfcSurfaceTexture: IFC2X3 TextureType or IFC4 Mode. False for
    // purposes StandardMaterial3D has no texture for.
    static bool texture_slot(IfcUtil::IfcBaseClass* texture, PrecalculatedIFCStyle::Slot& slot) {
        String purpose = ifc_string(texture, "TextureType");
        if (purpose.is_empty()) purpose = ifc_string(texture, "Mode");
        purpose = purpose.to_upper();
        if (purpose.contains("NORMAL")) {
            slot = PrecalculatedIFCStyle::NORMAL;
        } else if (purpose.contains("BUMP") || purpose.contains("HEIGHT") || purpose.contains("DISPLACEMENT")) {
            slot = PrecalculatedIFCStyle::HEIGHT;
        } else if (purpose.contains("SELFILLUMINATION") || purpose.contains("EMISSI")) {
            slot = PrecalculatedIFCStyle::EMISSION;
        } else if (purpose.contains("OPACITY") || purpose.contains("TRANSPARENT") || purpose.contains("REFLECTION")
                || purpose.contains("SHININESS") || purpose.contains("SPECULAR") || purpose.contains("ROUGH") || purpose.contains("METAL")) {
            return false;
        } else {
            // TEXTURE, NOTDEFINED, no purpose, or an X3D blend mode such as MODULATE
            slot = PrecalculatedIFCStyle::ALBEDO;
        }
        return true;
    }

    void read_texture(IfcUtil::IfcBaseClass* entity, PrecalculatedIFCStyle& look) {
        PrecalculatedIFCStyle::Slot slot;
        if (!texture_slot(entity, slot)) return;
        for (const PrecalculatedIFCStyle::Texture& existing : look.textures) {
            if (existing.slot == slot) return; // the first texture of a slot wins
        }
        const LoadedTexture* loaded = load(entity);
        if (!loaded) return;

        PrecalculatedIFCStyle::Texture texture;
        texture.slot = slot;
        texture.key = loaded->key;
        texture.texture = loaded->texture;
        texture.image = loaded->image;
        texture.repeat = ifc_bool(entity, "RepeatS") && ifc_bool(entity, "RepeatT");
        // IfcCartesianTransformationOperator2D: scale and origin. Rotation is not applied.
        if (IfcUtil::IfcBaseClass* transform = ifc_entity(entity, "TextureTransform")) {
            const double scale = ifc_double(transform, "Scale", 1.0);
            texture.uv_scale = Vector2((real_t)scale, (real_t)ifc_double(transform, "Scale2", scale));
            if (IfcUtil::IfcBaseClass* origin = ifc_entity(transform, "LocalOrigin")) {
                const std::vector<double> coordinates = ifc_doubles(origin, "Coordinates");
                if (coordinates.size() >= 2) texture.uv_offset = Vector2((real_t)coordinates[0], (real_t)coordinates[1]);
            }
        }
        look.textures.push_back(texture);
    }

    // The file an IfcImageTexture refers to: its path, else the same path or file name next to the
    // IFC file. Inside the project the path is a res:// path. Empty when it can't be found.
    String resolve(const String& url) const {
        String path = url.strip_edges().replace("\\", "/");
        if (path.is_empty() || path.begins_with("http://") || path.begins_with("https://")) return String();
        if (path.begins_with("file://")) {
            path = path.substr(7);
            // file:///C:/... on Windows
            if (path.length() > 3 && path[0] == '/' && path[2] == ':') path = path.substr(1);
        }
        std::vector<String> candidates;
        if (path.is_absolute_path()) candidates.push_back(path);
        if (!ifc_dir_.is_empty()) {
            if (path.is_relative_path()) candidates.push_back(ifc_dir_.path_join(path));
            candidates.push_back(ifc_dir_.path_join(path.get_file()));
        }
        for (const String& candidate : candidates) {
            if (FileAccess::file_exists(candidate)) return ProjectSettings::get_singleton()->localize_path(candidate);
        }
        return String();
    }

    // A texture's image, decoded once per file or embedding entity. Null when it can't be read.
    const LoadedTexture* load(IfcUtil::IfcBaseClass* entity) {
        LoadedTexture loaded;
        const IfcParse::declaration& type = entity->declaration();
        if (type.is("IfcImageTexture")) {
            String url = ifc_string(entity, "URLReference"); // IFC4
            if (url.is_empty()) url = ifc_string(entity, "UrlReference"); // IFC2X3
            const String path = resolve(url);
            if (path.is_empty()) {
                if (std::find(missing_.begin(), missing_.end(), url) == missing_.end()) missing_.push_back(url);
                return nullptr;
            }
            loaded.key = path;
        } else if (type.is("IfcBlobTexture") || type.is("IfcPixelTexture")) {
            loaded.key = String("#") + String::num_int64(entity->id());
        } else {
            return nullptr;
        }

        const std::string cache_key = loaded.key.utf8().get_data();
        auto cached = textures_.find(cache_key);
        if (cached != textures_.end()) return cached->second.key.is_empty() ? nullptr : &cached->second;

        if (type.is("IfcImageTexture")) {
            // A texture imported in the project keeps its import settings
            if (loaded.key.begins_with("res://") && ResourceLoader::get_singleton()->exists(loaded.key, "Texture2D")) {
                loaded.texture = ResourceLoader::get_singleton()->load(loaded.key, "Texture2D");
            } else {
                loaded.image = Image::load_from_file(loaded.key);
            }
        } else if (type.is("IfcBlobTexture")) {
            loaded.image = decode_blob(entity);
        } else {
            loaded.image = decode_pixels(entity);
        }
        if (loaded.image.is_valid() && !loaded.image->is_empty()) {
            loaded.image->generate_mipmaps();
        } else if (loaded.texture.is_null()) {
            ERR_PRINT(String("Failed to read IFC texture ") + loaded.key + ".");
            textures_[cache_key] = LoadedTexture(); // tried once
            return nullptr;
        }
        return &(textures_[cache_key] = loaded);
    }

    // IFC4 IfcBlobTexture: an image file in RasterFormat (PNG, JPG, ...) held in RasterCode
    static Ref<Image> decode_blob(IfcUtil::IfcBaseClass* entity) {
        const String format = ifc_string(entity, "RasterFormat").to_upper();
        const PackedByteArray bytes = ifc_binary(entity, "RasterCode");
        if (bytes.is_empty()) return {};
        Ref<Image> image;
        image.instantiate();
        Error err = ERR_FILE_UNRECOGNIZED;
        if (format == "PNG") err = image->load_png_from_buffer(bytes);
        else if (format == "JPG" || format == "JPEG") err = image->load_jpg_from_buffer(bytes);
        else if (format == "BMP") err = image->load_bmp_from_buffer(bytes);
        else if (format == "TGA") err = image->load_tga_from_buffer(bytes);
        else if (format == "WEBP") err = image->load_webp_from_buffer(bytes);
        return err == OK ? image : Ref<Image>();
    }

    // IfcPixelTexture: Width x Height pixels of 1 to 4 components, from the lower left corner
    static Ref<Image> decode_pixels(IfcUtil::IfcBaseClass* entity) {
        const int64_t width = ifc_int(entity, "Width", 0);
        const int64_t height = ifc_int(entity, "Height", 0);
        const int64_t components = ifc_int(entity, "ColourComponents", 0);
        const std::vector<PackedByteArray> pixels = ifc_binaries(entity, "Pixel");
        static const Image::Format FORMATS[] = { Image::FORMAT_L8, Image::FORMAT_LA8, Image::FORMAT_RGB8, Image::FORMAT_RGBA8 };
        if (width <= 0 || height <= 0 || components < 1 || components > 4 || (int64_t)pixels.size() != width * height) return {};

        PackedByteArray data;
        data.resize(width * height * components);
        uint8_t* out = data.ptrw();
        for (size_t i = 0; i < pixels.size(); i++) {
            if (pixels[i].size() < components) return {};
            memcpy(out + i * components, pixels[i].ptr(), (size_t)components);
        }
        Ref<Image> image = Image::create_from_data((int32_t)width, (int32_t)height, false, FORMATS[components - 1], data);
        if (image.is_valid()) image->flip_y(); // Godot images start at the top left
        return image;
    }
};

// Material definition of every item, and the library holding them. Writes only `items` and `library`.
template <typename schema>
static void extract_material_definitions(const std::shared_ptr<IfcParse::IfcFile>& file,
                                         std::vector<PrecalculatedIFCMetadata>& items, Ref<GDIFCMaterialLibrary>& library) {
    MaterialReader<schema> reader(file);
    for (PrecalculatedIFCMetadata& item : items) {
        try {
            item.material = reader.for_element(item.express_id);
        } catch (const std::exception& e) {
            ERR_PRINT(String("Failed to read the IFC material of #") + String::num_int64(item.express_id) + ": " + e.what());
        } catch (...) {
            ERR_PRINT(String("Failed to read the IFC material of #") + String::num_int64(item.express_id) + ": unknown error.");
        }
    }
    library = reader.make_library();
}

template <typename schema>
static void extract_item_metadata(IfcParse::IfcFile& file, PrecalculatedIFCMetadata& item) {
    item.attributes = get_ifc_object_attributes<schema>(file, item.express_id);
    item.properties = get_ifc_property_sets<schema>(file, item.express_id);
    item.quantities = get_ifc_quantity_sets<schema>(file, item.express_id);
}

// Attributes, property sets and quantities of every generated node, plus georeferencing.
// Writes only metadata_results_ and metadata_georreference_.
void GDIFCManager::_metadata_extract_task() {
    IfcParse::IfcFile& file = *metadata_file_;
    metadata_results_.resize(metadata_ids_.size());

    for (size_t i = 0; i < metadata_ids_.size(); i++) {
        PrecalculatedIFCMetadata& item = metadata_results_[i];
        item.express_id = metadata_ids_[i];
        try {
            switch (metadata_schema_index_) {
                case 0: extract_item_metadata<Ifc4>(file, item); break;
                case 2: extract_item_metadata<Ifc2x3>(file, item); break;
                case 3: extract_item_metadata<Ifc4x3_add2>(file, item); break;
                default: ;
            }
        } catch (const std::exception& e) {
            ERR_PRINT(String("Failed to read the properties of #") + String::num_int64(item.express_id) + ": " + e.what());
        } catch (...) {
            ERR_PRINT(String("Failed to read the properties of #") + String::num_int64(item.express_id) + ": unknown error.");
        }
    }

    // IFC material definitions, shared by the items using them
    if (ifc_materials_enabled_) {
        try {
            switch (metadata_schema_index_) {
                case 0: extract_material_definitions<Ifc4>(metadata_file_, metadata_results_, metadata_material_library_); break;
                case 2: extract_material_definitions<Ifc2x3>(metadata_file_, metadata_results_, metadata_material_library_); break;
                case 3: extract_material_definitions<Ifc4x3_add2>(metadata_file_, metadata_results_, metadata_material_library_); break;
                default: ;
            }
        } catch (const std::exception& e) {
            ERR_PRINT(String("[IfcParse] Failed to read IFC material definitions: ") + e.what());
        } catch (...) {
            ERR_PRINT("[IfcParse] Failed to read IFC material definitions: unknown error.");
        }

        // Roughness, metallic and textures of the surface styles, for the generated materials
        try {
            StyleReader(*metadata_file_, ifc_textures_enabled_, ifc_file_path_).read(metadata_material_styles_, metadata_colour_styles_);
        } catch (const std::exception& e) {
            ERR_PRINT(String("[IfcParse] Failed to read IFC surface styles: ") + e.what());
        } catch (...) {
            ERR_PRINT("[IfcParse] Failed to read IFC surface styles: unknown error.");
        }
    }

    // Georeferencing data exists in IFC4 and later
    try {
        switch (metadata_schema_index_) {
            case 0: metadata_georreference_ = get_georreference<Ifc4>(file); break;
            case 3: metadata_georreference_ = get_georreference<Ifc4x3_add2>(file); break;
            default: ;
        }
    } catch (...) {
        ERR_PRINT("[IfcParse] Failed to read georeferencing data.");
    }
}

// Main thread. Advances the metadata pipeline: parse (alongside geometry), extract (once the
// node ids are known), then apply to the nodes once they are revealed.
void GDIFCManager::_process_metadata() {
    WorkerThreadPool *pool = WorkerThreadPool::get_singleton();

    switch (metadata_state) {
        case META_PARSING: {
            if (!pool->is_task_completed(metadata_task_id)) return;
            if (current_state == FAILED) {
                _wait_for_metadata_task();
                metadata_file_.reset();
                metadata_state = META_IDLE;
                return;
            }
            // The extract task needs the ids of the generated items
            if (current_state == LOADING_THREAD) return;
            _wait_for_metadata_task();
            if (!metadata_file_) {
                metadata_state = META_APPLYING; // nothing to extract; still places the model
                return;
            }
            metadata_task_id = pool->add_task(callable_mp(this, &GDIFCManager::_metadata_extract_task), false);
            metadata_state = META_EXTRACTING;
        } break;

        case META_EXTRACTING: {
            if (!pool->is_task_completed(metadata_task_id)) return;
            _wait_for_metadata_task();
            metadata_state = META_APPLYING;
        } break;

        case META_APPLYING: {
            // Applied to the nodes after they are revealed, so they never hold up the reveal
            if (current_state != DONE) return;

            const uint64_t start_time = Time::get_singleton()->get_ticks_usec();
            const uint64_t time_budget = get_frame_time_budget();
            while (metadata_apply_index_ < metadata_results_.size()) {
                _apply_metadata_item(metadata_results_[metadata_apply_index_++]);

                if (Time::get_singleton()->get_ticks_usec() - start_time > time_budget) {
                    return;
                }
            }
            _finish_metadata();
        } break;

        default:
            break;
    }
}

// Main thread. Hands one item's attributes, property sets, quantities and typed object to its node,
// then releases the dictionaries.
void GDIFCManager::_apply_metadata_item(PrecalculatedIFCMetadata& item) {
    if (node_registry.has(item.express_id)) {
        IFCNode* node = Object::cast_to<IFCNode>(ObjectDB::get_instance(node_registry[item.express_id]));
        if (node) {
            node->set_attributes(item.attributes);
            node->set_properties(item.properties);
            node->set_quantities(item.quantities);
            if (item.material.is_valid()) {
                node->set_material_definition(item.material);
            }
            try {
                auto* inst = metadata_file_->instance_by_id(item.express_id);
                if (inst) {
                    node->set_ifc_object(GDIFCEntityBase::wrap(inst, metadata_file_));
                }
            } catch (...) {}
        }
    }
    item = PrecalculatedIFCMetadata();
}

// Places the IFCModel relative to reference_georeference, given the loaded model's georeferencing
void GDIFCManager::_apply_reference_placement(const GeorreferenceData& model_georeference) {
    IFCModel* model = get_ifc_model();
    if (!model || reference_georeference_.is_empty()) return;

    IFCGeoreference* reference = Object::cast_to<IFCGeoreference>(get_node_or_null(reference_georeference_));
    if (reference && reference->is_inside_tree()) {
        if (!model_georeference.valid) {
            WARN_PRINT("GDIFCManager: the loaded model has no georeferencing data. "
                "Placing it assuming it shares the reference model's engineering coordinate system.");
        }
        glm::dmat4 relative = georef_relative_matrix(reference->get_data(), model_georeference);
        model->set_transform(
            get_global_transform().affine_inverse() * reference->get_global_transform() * glm_to_godot_transform(relative));
    } else {
        WARN_PRINT(String("GDIFCManager: reference_georeference '") + String(reference_georeference_)
            + "' does not point to an IFCGeoreference node. The model is placed at the origin.");
    }
}

// An imported .ifc scene keeps no placement of its own: an instance follows its
// reference_georeference whenever it enters the scene or the reference changes.
void GDIFCManager::_apply_instance_reference_placement() {
    if (get_scene_file_path().get_extension().to_lower() != "ifc") return; // only instances of imported .ifc scenes
    IFCModel* model = get_ifc_model();
    if (!model) return;

    if (reference_georeference_.is_empty()) {
        model->set_transform(Transform3D());
        return;
    }
    GeorreferenceData model_georeference;
    for (int i = 0; i < model->get_child_count(); i++) {
        if (IFCGeoreference* georef = Object::cast_to<IFCGeoreference>(model->get_child(i))) {
            model_georeference = georef->get_data();
            break;
        }
    }
    _apply_reference_placement(model_georeference);
}

void GDIFCManager::set_reference_georeference(const NodePath &path) {
    reference_georeference_ = path;
    if (is_node_ready()) {
        _apply_instance_reference_placement();
    }
}

// Main thread. Commits the IfcParse file, adds the Georeference node, places the model relative
// to the reference georeference and emits ifc_objects_ready.
void GDIFCManager::_finish_metadata() {
    const bool is_ifc4plus = metadata_file_ && metadata_schema_index_ != 2;
    if (metadata_file_) {
        ifc_parse_file = metadata_file_;
        GeorreferenceData data = metadata_georreference_;
        data.coordination_offset = georreference.coordination_offset;
        georreference = data;
    }
    metadata_file_.reset();
    metadata_results_.clear();
    metadata_ids_.clear();

    if (metadata_material_library_.is_valid()) {
        material_library_ = metadata_material_library_;
        metadata_material_library_.unref();
        _bind_material_visuals();
        _apply_material_styles();
        _watch_material_library();
    }

    IFCModel* model = get_ifc_model();
    if (model) {
        if (ifc_parse_file) {
            model->init(ifc_parse_file);
        }

        // IFC4+: create Georeference node, or warn if georef data is absent
        if (is_ifc4plus) {
            if (georreference.valid) {
                IFCGeoreference* georef_node = memnew(IFCGeoreference);
                georef_node->set_name("Georeference");
                georef_node->init(georreference);
                model->add_child(georef_node);
                georef_node->set_owner(get_owner() != nullptr ? get_owner() : this);
            } else {
                WARN_PRINT("IFC4+ file loaded without georeferencing data (IfcProjectedCRS/IfcMapConversion not found).");
            }
        }

        // Place the model relative to the chosen reference georeference, if any
        _apply_reference_placement(georreference);
    }

    metadata_state = META_DONE;
    emit_signal("ifc_objects_ready");
    UtilityFunctions::print(vformat("IFC properties and typed objects ready after %.2f s",
        (Time::get_singleton()->get_ticks_usec() - start_loading_time) / 1000000.0));
}

// Main thread. Gives every IfcMaterial its Godot material: the one its surfaces already share, or
// a new one in its colour when none of its surfaces are shown (a hidden layer, for example).
void GDIFCManager::_bind_material_visuals() {
    const TypedArray<GDIFCMaterialSingle> materials = material_library_->get_materials();
    for (int64_t i = 0; i < materials.size(); i++) {
        Ref<GDIFCMaterialSingle> material = materials[i];
        if (material.is_null() || material->get_visual().is_valid()) continue;

        const uint32_t id = (uint32_t)material->get_express_id();
        Color color(1, 1, 1, 1);
        auto found = ifc_materials_.find(id);
        if (found != ifc_materials_.end()) {
            color = found->second.color;
        }
        Ref<StandardMaterial3D> visual = _get_material(color, color.a < 0.7, false, id);
        if (visual->get_name().is_empty()) {
            visual->set_name(material->get_name());
        }
        material->set_visual(visual);
    }
}

// Main thread. Gives the generated materials the look of their surface style beyond its colour.
// They are shared, so every surface using one changes with it.
void GDIFCManager::_apply_material_styles() {
    // An image becomes one ImageTexture, however many materials use it
    HashMap<String, Ref<Texture2D>> textures;
    auto texture_of = [&](const PrecalculatedIFCStyle::Texture& texture) -> Ref<Texture2D> {
        if (texture.texture.is_valid()) return texture.texture;
        if (texture.image.is_null()) return {};
        if (const Ref<Texture2D>* created = textures.getptr(texture.key)) return *created;
        Ref<Texture2D> created = ImageTexture::create_from_image(texture.image);
        textures[texture.key] = created;
        return created;
    };

    auto apply = [&](const Ref<StandardMaterial3D>& material, const PrecalculatedIFCStyle& style) {
        if (style.roughness >= 0.0f) material->set_roughness(style.roughness);
        if (style.metallic >= 0.0f) material->set_metallic(style.metallic);
        bool textured = false;
        for (const PrecalculatedIFCStyle::Texture& texture : style.textures) {
            Ref<Texture2D> image = texture_of(texture);
            if (image.is_null()) continue;
            switch (texture.slot) {
                case PrecalculatedIFCStyle::ALBEDO: {
                    // As in X3D, which IFC textures follow, a colour texture replaces the surface colour
                    material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, image);
                    material->set_albedo(Color(1, 1, 1, material->get_albedo().a));
                } break;
                case PrecalculatedIFCStyle::NORMAL:
                    material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
                    material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, image);
                    break;
                case PrecalculatedIFCStyle::HEIGHT:
                    material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
                    material->set_texture(BaseMaterial3D::TEXTURE_HEIGHTMAP, image);
                    break;
                case PrecalculatedIFCStyle::EMISSION:
                    material->set_feature(BaseMaterial3D::FEATURE_EMISSION, true);
                    material->set_emission(Color(1, 1, 1));
                    material->set_texture(BaseMaterial3D::TEXTURE_EMISSION, image);
                    break;
            }
            // A material has one UV transform; the first texture's is used
            if (!textured) {
                textured = true;
                material->set_uv1_scale(Vector3(texture.uv_scale.x, texture.uv_scale.y, 1));
                material->set_uv1_offset(Vector3(texture.uv_offset.x, texture.uv_offset.y, 0));
                material->set_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT, texture.repeat);
            }
        }
    };

    // A material without a style of its own, shown through a shape aspect in a styled colour, takes
    // the look of that colour's style, as its surfaces would have without IFC materials
    HashMap<String, const PrecalculatedIFCStyle*> colour_styles;
    for (const auto& [key, style] : metadata_colour_styles_) {
        colour_styles[key] = &style;
    }
    std::vector<std::pair<Ref<StandardMaterial3D>, const PrecalculatedIFCStyle*>> styled;
    for (const KeyValue<String, Ref<StandardMaterial3D>>& cached : material_cache) {
        if (!cached.key.begins_with("M")) continue;
        auto own = metadata_material_styles_.find((uint32_t)cached.key.substr(1).to_int());
        if (own != metadata_material_styles_.end()) {
            styled.emplace_back(cached.value, &own->second);
            continue;
        }
        const Ref<StandardMaterial3D>& material = cached.value;
        const String colour_key = material->get_albedo().to_html()
            + (material->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED ? "_T" : "_O");
        if (const PrecalculatedIFCStyle* const* style = colour_styles.getptr(colour_key)) {
            styled.emplace_back(material, *style);
        }
    }
    // Applied once every material's key is read, since a texture turns the albedo white
    for (const auto& [material, style] : styled) {
        apply(material, *style);
    }
    for (const auto& [key, style] : metadata_colour_styles_) {
        if (const Ref<StandardMaterial3D>* material = material_cache.getptr(key)) {
            apply(*material, style);
        }
    }
    metadata_material_styles_.clear();
    metadata_colour_styles_.clear();
}

// Main thread. Watches every IfcMaterial of the library, so replacing its visual reaches its surfaces.
// Each surface's mesh material is its IfcMaterial's visual, before and after the scene is saved.
void GDIFCManager::_watch_material_library() {
    _unwatch_material_library();
    if (material_library_.is_null()) return;

    const TypedArray<GDIFCMaterialSingle> materials = material_library_->get_materials();
    for (int64_t i = 0; i < materials.size(); i++) {
        Ref<GDIFCMaterialSingle> material = materials[i];
        if (material.is_null()) continue;
        const uint64_t id = material->get_instance_id();
        if (material_watchers_.has(id)) continue;
        applied_visuals_[id] = material->get_visual();
        Callable watcher = callable_mp(this, &GDIFCManager::_on_ifc_material_changed).bind(id);
        material->connect("changed", watcher);
        material_watchers_[id] = watcher;
    }
}

void GDIFCManager::_unwatch_material_library() {
    for (const KeyValue<uint64_t, Callable> &watcher : material_watchers_) {
        Object *material = ObjectDB::get_instance(ObjectID(watcher.key));
        if (material && material->is_connected("changed", watcher.value)) {
            material->disconnect("changed", watcher.value);
        }
    }
    material_watchers_.clear();
    applied_visuals_.clear();
    material_surfaces_.clear();
    material_surfaces_found_ = false;
}

// Finds every surface showing a watched material's visual. Done once, on the first replacement,
// while every visual is still the one its surfaces have.
void GDIFCManager::_find_material_surfaces() {
    material_surfaces_found_ = true;
    HashMap<uint64_t, uint64_t> material_by_visual;
    for (const KeyValue<uint64_t, Ref<Material>> &applied : applied_visuals_) {
        if (applied.value.is_valid()) {
            material_by_visual[applied.value->get_instance_id()] = applied.key;
        }
    }
    if (material_by_visual.is_empty()) return;

    IFCModel *model = get_ifc_model();
    if (!model) return;
    TypedArray<Node> nodes = model->find_children("*", "IFCNode", true, false);
    for (int64_t i = 0; i < nodes.size(); i++) {
        IFCNode *node = Object::cast_to<IFCNode>(nodes[i]);
        Ref<Mesh> mesh = node ? node->get_mesh() : Ref<Mesh>();
        if (mesh.is_null()) continue;
        for (int surface = 0; surface < mesh->get_surface_count(); surface++) {
            Ref<Material> visual = mesh->surface_get_material(surface);
            if (visual.is_null()) continue;
            const uint64_t *material = material_by_visual.getptr(visual->get_instance_id());
            if (material) {
                MaterialSurface found;
                found.node = ObjectID(node->get_instance_id());
                found.surface = surface;
                material_surfaces_[*material].push_back(found);
            }
        }
    }
}

// A GDIFCMaterialSingle emitted changed: gives its new visual to the surfaces still showing its old one.
// A surface the user gave another material, on the mesh, is left as it is; a node's surface
// override is never touched, so one set by hand keeps showing.
void GDIFCManager::_on_ifc_material_changed(uint64_t material_id) {
    GDIFCMaterialSingle *material = Object::cast_to<GDIFCMaterialSingle>(ObjectDB::get_instance(ObjectID(material_id)));
    Ref<Material> *applied = applied_visuals_.getptr(material_id);
    if (!material || !applied) return;
    const Ref<Material> visual = material->get_visual();
    if (*applied == visual) return; // changed for another reason, such as its name

    if (!material_surfaces_found_) {
        _find_material_surfaces();
    }
    const Ref<Material> previous = *applied;
    if (const Vector<MaterialSurface> *surfaces = material_surfaces_.getptr(material_id)) {
        for (const MaterialSurface &surface : *surfaces) {
            IFCNode *node = Object::cast_to<IFCNode>(ObjectDB::get_instance(surface.node));
            Ref<Mesh> mesh = node ? node->get_mesh() : Ref<Mesh>();
            if (mesh.is_null() || surface.surface >= mesh->get_surface_count()) continue;
            if (mesh->surface_get_material(surface.surface) != previous) continue;
            mesh->surface_set_material(surface.surface, visual);
        }
    }
    *applied = visual;
}

void GDIFCManager::set_material_library(const Ref<GDIFCMaterialLibrary> &library) {
    material_library_ = library;
    if (is_node_ready()) {
        _watch_material_library();
    }
}

godot::Array GDIFCManager::find_material_definitions(const String &name) const {
    godot::Array result;
    if (material_library_.is_null()) return result;
    material_library_->for_each_definition([&](const Ref<GDIFCMaterialDefinition> &definition) {
        if (definition->get_name() == name) {
            result.push_back(definition);
        }
    });
    return result;
}

// By name, since exporters often write one material several times under the same name
int GDIFCManager::set_material_visual(const String &material_name, const Ref<Material> &visual) {
    int changed = 0;
    if (material_library_.is_null()) return changed;
    const TypedArray<GDIFCMaterialSingle> materials = material_library_->get_materials();
    for (int64_t i = 0; i < materials.size(); i++) {
        Ref<GDIFCMaterialSingle> material = materials[i];
        if (material.is_valid() && material->get_name() == material_name) {
            material->set_visual(visual);
            changed++;
        }
    }
    return changed;
}

// Restores ifc_object on every saved material definition, by express id
void GDIFCManager::_relink_material_library() {
    if (material_library_.is_null() || !ifc_parse_file) return;
    material_library_->for_each_definition([&](const Ref<GDIFCMaterialDefinition>& definition) {
        try {
            if (auto* inst = ifc_parse_file->instance_by_id((int)definition->get_express_id())) {
                definition->set_ifc_object(GDIFCEntityBase::wrap(inst, ifc_parse_file));
            }
        } catch (...) {}
    });
}

// ---------------------------------------------------------
// BACKGROUND THREAD
// ---------------------------------------------------------

// Items below this triangle count (boxes, plain walls, slabs) can't be simplified
// meaningfully; skipping them avoids per-element overhead on large models.
static constexpr int LOD_MIN_TRIANGLES = 64;

// Generate LOD index arrays with Godot's ImporterMesh (the same path the glTF/scene
// importer uses). Runs on worker threads, one item each: the ImporterMesh is temporary and
// only its index data is kept, so no RenderingServer resources are created here.
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
        arrays[Mesh::ARRAY_TEX_UV] = geometry.uvs;
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
        geometry.uvs = arrays[Mesh::ARRAY_TEX_UV];
        geometry.indices = arrays[Mesh::ARRAY_INDEX];

        for (int l = 0; l < lod_count; l++) {
            geometry.lods[importer->get_surface_lod_size(i, l)] = importer->get_surface_lod_indices(i, l);
        }
    }
}

// A string argument of an entity, or empty when it isn't a string (e.g. $)
static String read_string_argument(const webifc::parsing::IfcLoader& loader, uint32_t express_id, uint32_t argument) {
    loader.MoveToArgumentOffset(express_id, argument);
    if (loader.GetTokenType() != webifc::parsing::IfcTokenType::STRING) return String();
    loader.StepBack();
    return String::utf8(loader.GetDecodedStringArgument().c_str());
}

// An IfcMaterial that can colour an element's surfaces
struct MaterialLeaf {
    uint32_t express_id = 0;
    bool has_color = false;
    uint32_t rgba32 = 0; // converted like the surface colours in fill_item_geometry
};

// web-ifc's colour for geometry with no colour at all
static constexpr uint32_t UNCOLORED_RGBA32 = 0xFFFFFFFF;

// The layers of an element's IfcMaterialLayerSetUsage, to cut its geometry into. web-ifc gives a
// layered wall or slab one solid in the colour of a single layer.
struct LayerSplit {
    glm::dvec3 direction;            // across the layers in world space, from the first layer to the last
    std::vector<double> boundaries;  // each boundary between layers, as a fraction of the total thickness
    std::vector<uint32_t> materials; // IfcMaterial of each layer from the first, 0 for an air gap
};

// Finds the IfcMaterial behind each surface colour, following web-ifc's colour rules: an element
// uses its own material association, else its type's, and a set resolves to its materials.
// Runs on the web-ifc task, and only when IFC materials are read.
class MaterialResolver {
public:
    // Every material returned by leaves_for(), by express id
    std::unordered_map<uint32_t, PrecalculatedIFCMaterial> materials;

    MaterialResolver(const webifc::parsing::IfcLoader& loader, const webifc::geometry::IfcGeometryLoader& geometry_loader)
        : loader_(loader), geometry_loader_(geometry_loader) {
        // IfcRelAssociatesMaterial: RelatedObjects (4), RelatingMaterial (5). In file order, so an
        // object associated more than once keeps its first material.
        std::vector<uint32_t> associations = loader_.GetExpressIDsWithType(webifc::schema::IFCRELASSOCIATESMATERIAL);
        std::sort(associations.begin(), associations.end());
        for (uint32_t rel : associations) {
            const uint32_t material = read_ref(rel, 5);
            if (material == 0) continue;
            for (uint32_t object : read_refs(rel, 4)) {
                element_material_.try_emplace(object, material);
            }
        }

        // IfcRelDefinesByType: RelatedObjects (4), RelatingType (5). Occurrences without a material
        // of their own use their type's.
        std::vector<uint32_t> type_rels = loader_.GetExpressIDsWithType(webifc::schema::IFCRELDEFINESBYTYPE);
        std::sort(type_rels.begin(), type_rels.end());
        for (uint32_t rel : type_rels) {
            const uint32_t type = read_ref(rel, 5);
            const std::vector<uint32_t> objects = read_refs(rel, 4);
            for (uint32_t object : objects) {
                element_type_.try_emplace(object, type);
            }
            auto type_material = element_material_.find(type);
            if (type_material == element_material_.end()) continue;
            const uint32_t material = type_material->second; // emplacing below may rehash
            for (uint32_t object : objects) {
                element_material_.try_emplace(object, material);
            }
        }

        // IfcShapeAspect: ShapeRepresentations (0), Name (1). It names the representation items
        // making one part of a product or type; a constituent or profile of the same name is that
        // part's material.
        std::vector<uint32_t> aspects = loader_.GetExpressIDsWithType(webifc::schema::IFCSHAPEASPECT);
        std::sort(aspects.begin(), aspects.end());
        for (uint32_t aspect : aspects) {
            const std::string name = read_text(aspect, 1);
            if (name.empty()) continue;
            for (uint32_t representation : read_refs(aspect, 0)) {
                // IfcShapeRepresentation: Items (3)
                for (uint32_t item : read_refs(representation, 3)) {
                    aspect_names_.try_emplace(item, name);
                }
            }
        }
    }

    // The element's materials in the order web-ifc picks their colour, or nullptr if it has none.
    // Elements sharing a material or set share one cached list.
    const std::vector<MaterialLeaf>* leaves_for(uint32_t element_id) {
        auto material = element_material_.find(element_id);
        if (material == element_material_.end()) return nullptr;

        auto [cached, inserted] = leaves_by_material_.try_emplace(material->second);
        if (inserted) {
            std::vector<uint32_t> ids;
            collect(material->second, ids, 0);
            for (uint32_t id : ids) {
                cached->second.push_back(make_leaf(id));
            }
        }
        return cached->second.empty() ? nullptr : &cached->second;
    }

    // The layers to cut the element's geometry into: those of its own IfcMaterialLayerSetUsage,
    // when it has two or more. A layer set without a usage isn't placed on the element.
    std::optional<LayerSplit> layer_split_for(uint32_t element_id) {
        auto material = element_material_.find(element_id);
        if (material == element_material_.end()
                || loader_.GetLineType(material->second) != webifc::schema::IFCMATERIALLAYERSETUSAGE) return {};
        auto [cached, inserted] = layers_by_usage_.try_emplace(material->second);
        if (inserted) cached->second = read_usage_layers(material->second);
        const UsageLayers& layers = cached->second;
        if (layers.split.materials.size() < 2) return {};

        // The usage's axis is in the element's coordinate system: its placement, turned Z-up to
        // Y-up like its geometry. The scale and the coordinate_to_origin offset don't change a direction.
        glm::dmat4 frame = webifc::geometry::NormalizeIFC;
        if (const uint32_t placement = read_ref(element_id, 5)) {
            frame = frame * geometry_loader_.GetLocalPlacement(placement);
        }
        glm::dvec3 axis(0.0);
        axis[layers.axis] = layers.positive ? 1.0 : -1.0;
        const glm::dvec3 direction = glm::dvec3(frame * glm::dvec4(axis, 0.0));
        if (glm::length(direction) < 1e-9) return {};

        LayerSplit split = layers.split;
        split.direction = glm::normalize(direction);
        return split;
    }

    // Constituent and profile names of the element's material set and its type's -> their
    // IfcMaterial, for its shape aspects. The aspects of a type's geometry are named after the
    // type's parts, which an occurrence with a set of its own can name differently; its own
    // names come first. Null when neither material is a constituent or profile set.
    const std::unordered_map<std::string, uint32_t>* parts_for(uint32_t element_id) {
        uint32_t own = 0;
        uint32_t of_type = 0;
        if (auto material = element_material_.find(element_id); material != element_material_.end()) own = material->second;
        if (auto type = element_type_.find(element_id); type != element_type_.end()) {
            if (auto material = element_material_.find(type->second); material != element_material_.end()) of_type = material->second;
        }
        if (own == 0 && of_type == 0) return nullptr;

        auto [cached, inserted] = parts_by_materials_.try_emplace(((uint64_t)own << 32) | of_type);
        if (inserted) {
            if (own != 0) read_parts(own, cached->second);
            if (of_type != 0 && of_type != own) read_parts(of_type, cached->second);
        }
        return cached->second.empty() ? nullptr : &cached->second;
    }

    // Whether a surface of this colour can use the material's Godot material; see shows()
    bool shows_material(uint32_t material, uint32_t rgba32) {
        if (!materials.count(material)) make_leaf(material);
        const PrecalculatedIFCMaterial& known = materials[material];
        MaterialLeaf leaf;
        leaf.express_id = material;
        leaf.has_color = known.has_color;
        leaf.rgba32 = known.color.to_rgba32();
        return shows(leaf, rgba32);
    }

    // The IfcMaterial a shape aspect names for a representation item, or 0
    uint32_t aspect_material(const std::unordered_map<std::string, uint32_t>& parts, uint32_t item) const {
        auto aspect = aspect_names_.find(item);
        if (aspect == aspect_names_.end()) return 0;
        auto part = parts.find(aspect->second);
        return part == parts.end() ? 0 : part->second;
    }

    // Whether a surface of this colour can use the material's Godot material, which has one colour.
    // A material without a colour of its own takes the first colour it is shown in.
    bool shows(const MaterialLeaf& leaf, uint32_t rgba32) {
        if (leaf.has_color) return leaf.rgba32 == rgba32;
        auto [adopted, inserted] = adopted_colours_.try_emplace(leaf.express_id, rgba32);
        return adopted->second == rgba32;
    }

    // The colour every surface of the material has: its own, else the one it took, else white
    Color surface_colour(uint32_t material) {
        auto found = materials.find(material);
        if (found != materials.end() && found->second.has_color) return found->second.color;
        auto [adopted, inserted] = adopted_colours_.try_emplace(material, UNCOLORED_RGBA32);
        return adopted->second == UNCOLORED_RGBA32 ? Color(1, 1, 1, 1) : Color::hex(adopted->second);
    }

private:
    // A layer set usage's layers, the same for every element using it
    struct UsageLayers {
        int axis = 1; // AXIS1..AXIS3 -> 0..2
        bool positive = true;
        LayerSplit split; // without its direction
    };

    const webifc::parsing::IfcLoader& loader_;
    const webifc::geometry::IfcGeometryLoader& geometry_loader_;
    // Element or type express id -> its IfcMaterialSelect
    std::unordered_map<uint32_t, uint32_t> element_material_;
    // IfcMaterialSelect -> its materials
    std::unordered_map<uint32_t, std::vector<MaterialLeaf>> leaves_by_material_;
    std::unordered_map<uint32_t, UsageLayers> layers_by_usage_;
    // Representation item -> the name of the shape aspect it belongs to
    std::unordered_map<uint32_t, std::string> aspect_names_;
    // Occurrence -> its type
    std::unordered_map<uint32_t, uint32_t> element_type_;
    // An element's own and its type's IfcMaterialSelect -> their constituents' and profiles' materials by name
    std::unordered_map<uint64_t, std::unordered_map<std::string, uint32_t>> parts_by_materials_;
    // IfcMaterial without a colour of its own -> the colour (RGBA8) its surfaces have
    std::unordered_map<uint32_t, uint32_t> adopted_colours_;

    std::string read_text(uint32_t express_id, uint32_t argument) const {
        loader_.MoveToArgumentOffset(express_id, argument);
        if (loader_.GetTokenType() != webifc::parsing::IfcTokenType::STRING) return std::string();
        loader_.StepBack();
        return loader_.GetDecodedStringArgument();
    }

    // IfcMaterialConstituentSet / IfcMaterialProfileSet: their parts (2), each with Name (0) and
    // Material (2). A profile set usage refers to its set (0).
    void read_parts(uint32_t select, std::unordered_map<std::string, uint32_t>& parts) const {
        using namespace webifc::schema;
        uint32_t type = loader_.GetLineType(select);
        if (type == IFCMATERIALPROFILESETUSAGE || type == IFCMATERIALPROFILESETUSAGETAPERING) {
            select = read_ref(select, 0);
            if (select == 0) return;
            type = loader_.GetLineType(select);
        }
        if (type != IFCMATERIALCONSTITUENTSET && type != IFCMATERIALPROFILESET) return;
        for (uint32_t part : read_refs(select, 2)) {
            const std::string name = read_text(part, 0);
            const uint32_t material = read_ref(part, 2);
            if (!name.empty() && material != 0 && loader_.GetLineType(material) == IFCMATERIAL) {
                parts.try_emplace(name, material);
            }
        }
    }

    std::string read_enum(uint32_t express_id, uint32_t argument) const {
        loader_.MoveToArgumentOffset(express_id, argument);
        if (loader_.GetTokenType() != webifc::parsing::IfcTokenType::ENUM) return std::string();
        loader_.StepBack();
        return std::string(loader_.GetStringArgument());
    }

    double read_real(uint32_t express_id, uint32_t argument) const {
        loader_.MoveToArgumentOffset(express_id, argument);
        if (loader_.GetTokenType() != webifc::parsing::IfcTokenType::REAL) return 0.0;
        loader_.StepBack();
        return loader_.GetDoubleArgument();
    }

    // IfcMaterialLayerSetUsage: ForLayerSet (0), LayerSetDirection (1), DirectionSense (2).
    // IfcMaterialLayer: Material (0, $ for an air gap), LayerThickness (1).
    UsageLayers read_usage_layers(uint32_t usage) {
        using namespace webifc::schema;
        UsageLayers result;
        const std::string direction = read_enum(usage, 1);
        result.axis = direction == "AXIS1" ? 0 : direction == "AXIS3" ? 2 : 1;
        result.positive = read_enum(usage, 2) != "NEGATIVE";

        std::vector<double> thicknesses;
        for (uint32_t layer : read_refs(read_ref(usage, 0), 0)) {
            const uint32_t type = loader_.GetLineType(layer);
            if (type != IFCMATERIALLAYER && type != IFCMATERIALLAYERWITHOFFSETS) continue;
            const double thickness = read_real(layer, 1);
            if (!(thickness > 0.0)) continue;
            uint32_t material = read_ref(layer, 0);
            if (material != 0 && loader_.GetLineType(material) != IFCMATERIAL) material = 0;
            // Known to the main thread by name and colour, like the materials of leaves_for()
            if (material != 0 && !materials.count(material)) make_leaf(material);
            thicknesses.push_back(thickness);
            result.split.materials.push_back(material);
        }

        double total = 0.0;
        for (double thickness : thicknesses) total += thickness;
        if (thicknesses.size() < 2 || !(total > 0.0)) {
            result.split = LayerSplit();
            return result;
        }
        double cumulative = 0.0;
        for (size_t i = 0; i + 1 < thicknesses.size(); i++) {
            cumulative += thicknesses[i];
            result.split.boundaries.push_back(cumulative / total);
        }
        return result;
    }

    uint32_t read_ref(uint32_t express_id, uint32_t argument) const {
        loader_.MoveToArgumentOffset(express_id, argument);
        if (loader_.GetTokenType() != webifc::parsing::IfcTokenType::REF) return 0;
        loader_.StepBack();
        return loader_.GetRefArgument();
    }

    std::vector<uint32_t> read_refs(uint32_t express_id, uint32_t argument) const {
        std::vector<uint32_t> refs;
        loader_.MoveToArgumentOffset(express_id, argument);
        if (loader_.GetTokenType() != webifc::parsing::IfcTokenType::SET_BEGIN) return refs;
        loader_.StepBack();
        for (uint32_t offset : loader_.GetSetArgument()) {
            if (loader_.GetTokenType(offset) == webifc::parsing::IfcTokenType::REF) {
                refs.push_back(loader_.GetRefArgument(offset));
            }
        }
        return refs;
    }

    // Appends the IfcMaterials of a material select, in the order web-ifc tries them for a colour.
    // Layers and profiles with offsets and tapered profile usages are followed too, although
    // web-ifc takes no colour from them, so their materials can still match a styled colour.
    void collect(uint32_t express_id, std::vector<uint32_t>& out, int depth) const {
        using namespace webifc::schema;
        if (express_id == 0 || depth > 4) return;

        switch (loader_.GetLineType(express_id)) {
            case IFCMATERIAL:
                if (std::find(out.begin(), out.end(), express_id) == out.end()) out.push_back(express_id);
                break;
            case IFCMATERIALLAYERSETUSAGE:
            case IFCMATERIALPROFILESETUSAGE:
            case IFCMATERIALPROFILESETUSAGETAPERING:
            case IFCMATERIALLAYER:
            case IFCMATERIALLAYERWITHOFFSETS:
                collect(read_ref(express_id, 0), out, depth + 1);
                break;
            case IFCMATERIALLAYERSET:
                for (uint32_t layer : read_refs(express_id, 0)) collect(layer, out, depth + 1);
                break;
            case IFCMATERIALLIST: {
                // web-ifc shows the last listed material that has a colour
                const std::vector<uint32_t> materials = read_refs(express_id, 0);
                for (auto it = materials.rbegin(); it != materials.rend(); ++it) collect(*it, out, depth + 1);
            } break;
            case IFCMATERIALCONSTITUENTSET:
            case IFCMATERIALPROFILESET:
                for (uint32_t member : read_refs(express_id, 2)) collect(member, out, depth + 1);
                break;
            case IFCMATERIALCONSTITUENT:
            case IFCMATERIALPROFILE:
            case IFCMATERIALPROFILEWITHOFFSETS:
                collect(read_ref(express_id, 2), out, depth + 1);
                break;
            default:
                break;
        }
    }

    MaterialLeaf make_leaf(uint32_t express_id) {
        MaterialLeaf leaf;
        leaf.express_id = express_id;
        PrecalculatedIFCMaterial material;
        material.name = read_string_argument(loader_, express_id, 0);
        // The same colour web-ifc gives geometry coloured by this material
        if (std::optional<glm::dvec4> color = geometry_loader_.GetColor(express_id)) {
            material.color = Color(color->r, color->g, color->b, color->a);
            material.has_color = true;
            leaf.has_color = true;
            leaf.rgba32 = material.color.to_rgba32();
        }
        materials.try_emplace(express_id, material);
        return leaf;
    }
};

// The IfcMaterial a surface colour shows, or null for none. Uncoloured geometry shows the first
// material that has no colour of its own.
static const MaterialLeaf* match_material(const std::vector<MaterialLeaf>& leaves, uint32_t rgba32) {
    for (const MaterialLeaf& leaf : leaves) {
        if (leaf.has_color && leaf.rgba32 == rgba32) return &leaf;
    }
    if (rgba32 == UNCOLORED_RGBA32) {
        for (const MaterialLeaf& leaf : leaves) {
            if (!leaf.has_color) return &leaf;
        }
    }
    return nullptr;
}

// What an element's IFC materials tell fill_item_geometry, with IFC materials on
struct ElementMaterials {
    MaterialResolver* resolver = nullptr;
    const std::vector<MaterialLeaf>* leaves = nullptr;                    // its materials, if any
    const std::unordered_map<std::string, uint32_t>* parts = nullptr;     // constituent or profile names
    std::optional<LayerSplit> layer_split;                                // layers to cut its geometry into
};

// An element's materials. A lookup that can't be read is left out, so the element keeps one
// material per colour for it.
static ElementMaterials element_materials(MaterialResolver& resolver, uint32_t express_id) {
    ElementMaterials result;
    result.resolver = &resolver;
    try { result.leaves = resolver.leaves_for(express_id); } catch (...) {}
    try { result.parts = resolver.parts_for(express_id); } catch (...) {}
    try { result.layer_split = resolver.layer_split_for(express_id); } catch (...) {}
    return result;
}

// The IfcMaterial a placed geometry shows: the part a shape aspect names for its item, else the
// material with its colour. A material is only used when it keeps the surface's colour.
static uint32_t surface_material(const ElementMaterials& materials, uint32_t item, uint32_t rgba32) {
    if (materials.parts) {
        // The part may be one of the type's, which the element's own materials don't list
        const uint32_t part = materials.resolver->aspect_material(*materials.parts, item);
        if (part != 0 && materials.resolver->shows_material(part, rgba32)) return part;
    }
    if (!materials.leaves) return 0;
    const MaterialLeaf* leaf = match_material(*materials.leaves, rgba32);
    return leaf && materials.resolver->shows(*leaf, rgba32) ? leaf->express_id : 0;
}

// Box-projected UV of a vertex: projected onto the plane of the dominant normal axis, 1 UV unit per meter
static Vector2 box_uv(const glm::dvec3& p, const glm::dvec3& n) {
    const glm::dvec3 a = glm::abs(n);
    if (a.y >= a.x && a.y >= a.z) {
        return Vector2((float)p.x, (float)(n.y >= 0.0 ? p.z : -p.z));
    } else if (a.x >= a.z) {
        return Vector2((float)(n.x >= 0.0 ? -p.z : p.z), (float)-p.y);
    }
    return Vector2((float)(n.z >= 0.0 ? p.x : -p.x), (float)-p.y);
}

// Appends one triangle with its own three vertices
static void append_triangle(PrecalculatedIFCItemGeometry& geometry, const glm::dvec3 (&corners)[3], const glm::dvec3& n) {
    const int64_t base = geometry.vertices.size();
    geometry.vertices.resize(base + 3);
    geometry.normals.resize(base + 3);
    geometry.uvs.resize(base + 3);
    const int64_t index_base = geometry.indices.size();
    geometry.indices.resize(index_base + 3);
    for (int c = 0; c < 3; c++) {
        const glm::dvec3& p = corners[c];
        geometry.vertices.set(base + c, Vector3((float)p.x, (float)p.y, (float)p.z));
        geometry.normals.set(base + c, Vector3((float)n.x, (float)n.y, (float)n.z));
        geometry.uvs.set(base + c, box_uv(p, n));
        geometry.indices.set(index_base + c, (int32_t)(base + c));
    }
}

// The part of a convex polygon where side * (dot(p, direction) - limit) >= 0
static std::vector<glm::dvec3> clip_polygon(const std::vector<glm::dvec3>& polygon, const glm::dvec3& direction, double limit, double side) {
    std::vector<glm::dvec3> clipped;
    for (size_t i = 0; i < polygon.size(); i++) {
        const glm::dvec3& a = polygon[i];
        const glm::dvec3& b = polygon[(i + 1) % polygon.size()];
        const double da = side * (glm::dot(a, direction) - limit);
        const double db = side * (glm::dot(b, direction) - limit);
        if (da >= 0.0) clipped.push_back(a);
        if ((da >= 0.0) != (db >= 0.0)) clipped.push_back(a + (b - a) * (da / (da - db)));
    }
    return clipped;
}

// Tessellate an entity's flat mesh into one surface per material: placed geometries sharing a
// color are appended to the same surface, since an element can have hundreds of them and a mesh
// holds at most RenderingServer::MAX_MESH_SURFACES surfaces. With materials (IFC materials on),
// geometries showing one of the element's IfcMaterials get a surface per material instead, and
// those coloured by its layer set are cut into one surface per layer.
// Node transforms are currently identity, so vertices are baked in world space.
static void fill_item_geometry(webifc::geometry::IfcGeometryProcessor& geometry_loader,
                               const webifc::geometry::IfcFlatMesh& flat_mesh,
                               bool is_space, const ElementMaterials* materials, PrecalculatedIFCItem& item) {
    const LayerSplit* layer_split = materials && materials->layer_split ? &*materials->layer_split : nullptr;
    // Same key as _get_material -> index in item.geometry. A material surface has the top bit
    // set and the IfcMaterial id; a colour surface has the RGBA8 color and transparency.
    std::unordered_map<uint64_t, size_t> surface_by_material;
    // The surface of a material or colour, created on first use. The reference is only valid
    // until the next call.
    auto surface_for = [&](uint32_t material_id, const Color& color, bool is_transparent) -> PrecalculatedIFCItemGeometry& {
        const uint64_t key = material_id != 0
            ? ((uint64_t)1 << 63) | material_id
            : ((uint64_t)color.to_rgba32() << 1) | (is_transparent ? 1 : 0);
        auto [surface, inserted] = surface_by_material.try_emplace(key, item.geometry.size());
        if (inserted) {
            PrecalculatedIFCItemGeometry new_geometry;
            new_geometry.color = color;
            new_geometry.is_transparent = is_transparent;
            new_geometry.material_id = material_id;
            item.geometry.push_back(std::move(new_geometry));
        }
        return item.geometry[surface->second];
    };

    // Geometries coloured by the layer set, cut into layers once all of them are known
    std::vector<const webifc::geometry::IfcPlacedGeometry*> layered;

    for (const auto& geom_data : flat_mesh.geometries) {
        auto& ifc_geometry = geometry_loader.GetGeometry(geom_data.geometryExpressID);
        if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

        Color color;
        bool is_transparent;
        uint32_t material_id = 0;
        if (is_space) {
            is_transparent = true;
            color = Color(0.025, 0.037, 0.034, 0.1);
        } else {
            color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
            is_transparent = (geom_data.color.a < 0.7);
            if (materials) {
                material_id = surface_material(*materials, geom_data.geometryExpressID, color.to_rgba32());
            }
        }

        if (layer_split && material_id != 0
                && std::find(layer_split->materials.begin(), layer_split->materials.end(), material_id) != layer_split->materials.end()) {
            layered.push_back(&geom_data);
            continue;
        }

        PrecalculatedIFCItemGeometry& item_geometry = surface_for(material_id, color, is_transparent);

        // Positions are kept in double until the UVs are computed, so large (georeferenced)
        // coordinates don't make the texture jitter.
        std::vector<glm::dvec3> positions(ifc_geometry.numPoints);
        for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
            positions[k] = glm::dvec3(geom_data.transformation * glm::dvec4(ifc_geometry.GetPoint(k), 1.0));
        }

        // Flat normals and box-projected UVs, both per face. A vertex shared by faces facing
        // different directions is duplicated, otherwise the last face would overwrite them.
        std::vector<glm::dvec3> out_positions = positions;
        std::vector<glm::dvec3> out_normals(positions.size(), glm::dvec3(0.0));
        std::vector<bool> assigned(positions.size(), false);
        std::vector<int32_t> out_indices(ifc_geometry.numFaces * 3);

        for (uint32_t k = 0; k < ifc_geometry.numFaces; k++) {
            bimGeometry::Face f = ifc_geometry.GetFace(k);
            const int face[3] = { f.i2, f.i1, f.i0 };

            glm::dvec3 n = glm::cross(positions[f.i1] - positions[f.i0], positions[f.i2] - positions[f.i0]);
            const double len = glm::length(n);
            n = len > 0.0 ? n / len : glm::dvec3(0.0, 1.0, 0.0);

            for (int c = 0; c < 3; c++) {
                uint32_t v = face[c];
                if (!assigned[v]) {
                    assigned[v] = true;
                    out_normals[v] = n;
                } else if (glm::dot(out_normals[v], n) < 0.999) {
                    v = (uint32_t)out_positions.size();
                    out_positions.push_back(positions[face[c]]);
                    out_normals.push_back(n);
                }
                out_indices[k*3+c] = (int32_t)v;
            }
        }

        // Append after what earlier geometries with this material already wrote
        const int64_t vertex_base = item_geometry.vertices.size();
        const int64_t index_base = item_geometry.indices.size();
        const int64_t vertex_count = (int64_t)out_positions.size();
        item_geometry.vertices.resize(vertex_base + vertex_count);
        item_geometry.normals.resize(vertex_base + vertex_count);
        item_geometry.uvs.resize(vertex_base + vertex_count);
        item_geometry.indices.resize(index_base + (int64_t)out_indices.size());

        Vector3* vertices = item_geometry.vertices.ptrw() + vertex_base;
        Vector3* normals = item_geometry.normals.ptrw() + vertex_base;
        Vector2* uvs = item_geometry.uvs.ptrw() + vertex_base;
        int32_t* indices = item_geometry.indices.ptrw() + index_base;

        for (int64_t k = 0; k < vertex_count; k++) {
            const glm::dvec3& p = out_positions[k];
            const glm::dvec3& n = out_normals[k];
            vertices[k] = Vector3((float)p.x, (float)p.y, (float)p.z);
            normals[k] = Vector3((float)n.x, (float)n.y, (float)n.z);
            uvs[k] = box_uv(p, n);
        }
        for (size_t k = 0; k < out_indices.size(); k++) {
            indices[k] = out_indices[k] + (int32_t)vertex_base;
        }
    }

    if (layered.empty()) return;

    // Triangles of the layered geometries, in world space, with their flat normals
    struct Triangle {
        glm::dvec3 corners[3];
        glm::dvec3 normal;
        Color color;
        bool is_transparent;
    };
    std::vector<Triangle> triangles;
    const glm::dvec3& direction = layer_split->direction;
    double u_min = std::numeric_limits<double>::max();
    double u_max = std::numeric_limits<double>::lowest();
    for (const webifc::geometry::IfcPlacedGeometry* geom_data : layered) {
        auto& ifc_geometry = geometry_loader.GetGeometry(geom_data->geometryExpressID);
        const Color color(geom_data->color.r, geom_data->color.g, geom_data->color.b, geom_data->color.a);
        std::vector<glm::dvec3> positions(ifc_geometry.numPoints);
        for (uint32_t k = 0; k < ifc_geometry.numPoints; k++) {
            positions[k] = glm::dvec3(geom_data->transformation * glm::dvec4(ifc_geometry.GetPoint(k), 1.0));
            const double u = glm::dot(positions[k], direction);
            u_min = MIN(u_min, u);
            u_max = MAX(u_max, u);
        }
        for (uint32_t k = 0; k < ifc_geometry.numFaces; k++) {
            bimGeometry::Face f = ifc_geometry.GetFace(k);
            Triangle triangle;
            // Same winding and normal as the unsplit path
            triangle.corners[0] = positions[f.i2];
            triangle.corners[1] = positions[f.i1];
            triangle.corners[2] = positions[f.i0];
            glm::dvec3 n = glm::cross(positions[f.i1] - positions[f.i0], positions[f.i2] - positions[f.i0]);
            const double len = glm::length(n);
            triangle.normal = len > 0.0 ? n / len : glm::dvec3(0.0, 1.0, 0.0);
            triangle.color = color;
            triangle.is_transparent = geom_data->color.a < 0.7;
            triangles.push_back(triangle);
        }
    }

    // Each layer covers its share of the element's extent across the layers, from the first layer
    // at u_min. The outer layers reach past it, so nothing is left out.
    const size_t layer_count = layer_split->materials.size();
    const double span = u_max - u_min;
    std::vector<double> lows(layer_count, std::numeric_limits<double>::lowest());
    std::vector<double> highs(layer_count, std::numeric_limits<double>::max());
    if (span > 1e-9) {
        for (size_t i = 0; i + 1 < layer_count; i++) {
            const double boundary = u_min + layer_split->boundaries[i] * span;
            highs[i] = boundary;
            lows[i + 1] = boundary;
        }
    } else {
        // No extent across the layers: all of it goes to the first one
        for (size_t i = 1; i < layer_count; i++) lows[i] = std::numeric_limits<double>::max();
    }

    for (const Triangle& triangle : triangles) {
        double t_min = std::numeric_limits<double>::max();
        double t_max = std::numeric_limits<double>::lowest();
        for (const glm::dvec3& corner : triangle.corners) {
            const double u = glm::dot(corner, direction);
            t_min = MIN(t_min, u);
            t_max = MAX(t_max, u);
        }
        // A triangle across the layers' direction (a wall's face) lies in one layer; one that only
        // touches a boundary isn't part of the layer on the other side
        const bool flat = t_max - t_min <= 1e-9;
        for (size_t layer = 0; layer < layer_count; layer++) {
            const bool overlaps = flat ? (t_min >= lows[layer] && t_min < highs[layer]) : (t_max > lows[layer] && t_min < highs[layer]);
            if (!overlaps) continue;
            const uint32_t material_id = layer_split->materials[layer];
            // An air gap keeps the geometry's own colour
            const Color color = material_id != 0 ? materials->resolver->surface_colour(material_id) : triangle.color;
            const bool is_transparent = material_id != 0 ? color.a < 0.7 : triangle.is_transparent;
            PrecalculatedIFCItemGeometry& surface = surface_for(material_id, color, is_transparent);

            if (flat || (t_min >= lows[layer] && t_max <= highs[layer])) {
                append_triangle(surface, triangle.corners, triangle.normal);
                continue;
            }
            std::vector<glm::dvec3> piece(std::begin(triangle.corners), std::end(triangle.corners));
            if (lows[layer] > std::numeric_limits<double>::lowest()) piece = clip_polygon(piece, direction, lows[layer], 1.0);
            if (highs[layer] < std::numeric_limits<double>::max()) piece = clip_polygon(piece, direction, highs[layer], -1.0);
            for (size_t k = 1; k + 1 < piece.size(); k++) {
                const glm::dvec3 corners[3] = { piece[0], piece[k], piece[k + 1] };
                if (glm::length(glm::cross(corners[1] - corners[0], corners[2] - corners[0])) < 1e-12) continue; // sliver
                append_triangle(surface, corners, triangle.normal);
            }
        }
    }
}

void GDIFCManager::_lod_group_task(uint32_t index) {
    PrecalculatedIFCItem& item = lod_items_[index];
    if (!item.geometry.empty()) {
        generate_item_lods(item, lod_merge_angle_);
    }
    progress_done_.fetch_add(1, std::memory_order_relaxed);
}

// Child -> parent express ids from the decomposition and containment relationships, read from
// web-ifc's parsed file. Applied in this order, so a later relationship wins for the same child.
// Only links between products and the project are kept: the same relationships also nest tasks,
// work schedules, cost items and type objects, which would otherwise become nodes.
static std::unordered_map<int, int> build_parent_map(const webifc::parsing::IfcLoader& loader,
        const webifc::schema::IfcSchemaManager& schema_manager, bool is_ifc4x3) {
    using webifc::parsing::IfcTokenType;

    // web-ifc's element list holds every IfcProduct subtype, spatial elements included
    auto is_scene_entity = [&](uint32_t express_id) {
        const uint32_t type = loader.GetLineType(express_id);
        return type == webifc::schema::IFCPROJECT || schema_manager.IsIfcElement(type);
    };

    struct Relation {
        uint32_t type;
        uint32_t parent_argument;
        uint32_t children_argument;
    };
    std::vector<Relation> relations = {
        {webifc::schema::IFCRELAGGREGATES, 4, 5},
        {webifc::schema::IFCRELCONTAINEDINSPATIALSTRUCTURE, 5, 4},
        {webifc::schema::IFCRELNESTS, 4, 5},
    };
    if (is_ifc4x3) {
        relations.push_back({webifc::schema::IFCRELADHERESTOELEMENT, 4, 5});
    }

    std::unordered_map<int, int> child_to_parent;
    for (const Relation& relation : relations) {
        std::vector<uint32_t> relation_ids = loader.GetExpressIDsWithType(relation.type);
        std::sort(relation_ids.begin(), relation_ids.end()); // file order

        for (uint32_t relation_id : relation_ids) {
            // Check every token type first: web-ifc's readers assume it
            loader.MoveToArgumentOffset(relation_id, relation.parent_argument);
            if (loader.GetTokenType() != IfcTokenType::REF) continue;
            loader.StepBack();
            const int parent_id = static_cast<int>(loader.GetRefArgument());
            if (!is_scene_entity(parent_id)) continue;

            loader.MoveToArgumentOffset(relation_id, relation.children_argument);
            if (loader.GetTokenType() != IfcTokenType::SET_BEGIN) continue;
            loader.StepBack();
            for (uint32_t offset : loader.GetSetArgument()) {
                if (loader.GetTokenType(offset) != IfcTokenType::REF) continue;
                const uint32_t child_id = loader.GetRefArgument(offset);
                if (!is_scene_entity(child_id)) continue;
                child_to_parent[static_cast<int>(child_id)] = parent_id;
            }
        }
    }
    return child_to_parent;
}

// IfcRoot's GlobalId is the first argument of every rooted entity
static String read_global_id(const webifc::parsing::IfcLoader& loader, uint32_t express_id) {
    return read_string_argument(loader, express_id, 0);
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

    // Load file [web-ifc] — from memory buffer
    auto temp_ifc_manager = std::make_unique<WEBIFCManager>(geometric_settings.ptr());
    try {
        if (!temp_ifc_manager->read_ifc_file(buf_ptr, buf_size)) {
            thread_failed_ = true;
            ERR_FAIL_MSG("[web-ifc] Failed to read IFC file: the file is malformed and could only be partially parsed.");
        }
    } catch (const std::exception& e) {
        thread_failed_ = true;
        ERR_FAIL_MSG(String("[web-ifc] Failed to read IFC file: ") + e.what());
    } catch (...) {
        thread_failed_ = true;
        ERR_FAIL_MSG("[web-ifc] Failed to read IFC file: unknown error.");
    }
    progress_done_.store(1);

    const webifc::parsing::IfcLoader& loader = *temp_ifc_manager->loader;
    const webifc::schema::IfcSchemaManager& schema_manager = temp_ifc_manager->schemaManager;

    bool is_ifc4x3 = false;
    try {
        is_ifc4x3 = loader.GetSchema() == IFC_SCHEMA::IFC4X3;
    } catch (...) {}

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
    progress_done_.store(2);

    auto& geometry_loader = *temp_ifc_manager->geometry_loader;

    // Which IfcMaterial coloured each surface, read only with IFC materials on
    std::unique_ptr<MaterialResolver> material_resolver;
    if (ifc_materials_enabled_) {
        try {
            material_resolver = std::make_unique<MaterialResolver>(loader, geometry_loader.GetLoader());
        } catch (const std::exception& e) {
            ERR_PRINT(String("Failed to read IFC material associations, using one material per colour: ") + e.what());
        } catch (...) {
            ERR_PRINT("Failed to read IFC material associations, using one material per colour: unknown error.");
        }
    }
    // A material that can't be read leaves the element with one material per colour
    auto element_materials_for = [&](uint32_t id) -> std::optional<ElementMaterials> {
        if (!material_resolver) return {};
        return element_materials(*material_resolver, id);
    };

    Vector<PrecalculatedIFCItem> temp_queue;

    // Build hierarchy parent map (EXPRESSID, EXPRESSID), of elements based on its parent if they exist
    std::unordered_map<int, int> parent_map = build_parent_map(loader, schema_manager, is_ifc4x3);

    // Identity, GlobalId and parent of one item. Attributes, property sets and quantities
    // come later from IfcParse, after the model is shown.
    auto init_item = [&](PrecalculatedIFCItem& item, uint32_t id, const String& ifc_class) {
        item.valid = true;
        item.express_id = id;
        item.node_name = vformat("%s_%d", ifc_class, (int64_t)id); // Godot demands an unique name per node at the same tree level
        item.ifc_class = ifc_class;
        item.global_id = read_global_id(loader, id);
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
    const std::vector<std::pair<const char*, uint32_t>> spatial_types = {
        {"IfcProject", webifc::schema::IFCPROJECT},
        {"IfcSite", webifc::schema::IFCSITE},
        {"IfcBuilding", webifc::schema::IFCBUILDING},
        {"IfcBuildingStorey", webifc::schema::IFCBUILDINGSTOREY},
    };

    // The geometry stage counts every entity the three passes below visit
    uint32_t geometry_total = static_cast<uint32_t>(active_parents.size());
    for (const auto& spatial_type : spatial_types) {
        geometry_total += static_cast<uint32_t>(loader.GetExpressIDsWithType(spatial_type.second).size());
    }
    for (auto type : schema_manager.GetIfcElementList()) {
        geometry_total += static_cast<uint32_t>(loader.GetExpressIDsWithType(type).size());
    }
    _set_worker_progress(STAGE_GEOMETRY, geometry_total);

    for (const auto& spatial_type : spatial_types) {
        std::vector<uint32_t> ids = loader.GetExpressIDsWithType(spatial_type.second);
        std::sort(ids.begin(), ids.end());

        for (uint32_t id : ids) {
            progress_done_.fetch_add(1, std::memory_order_relaxed);
            try {

            if (processed_ids.find(id) != processed_ids.end()) continue;

            PrecalculatedIFCItem item;
            init_item(item, id, spatial_type.first);

            // Spatial elements can carry geometry (e.g. IfcSite terrain). IfcProject is not a product and
            // has no Representation, and web-ifc logs an "unexpected mesh type" error when asked for one
            if (spatial_type.second != webifc::schema::IFCPROJECT) {
                const std::optional<ElementMaterials> item_materials = element_materials_for(id);
                fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), false, item_materials ? &*item_materials : nullptr, item);
            }

            temp_queue.push_back(item);
            processed_ids.insert(id);

            } catch (const std::exception& e) {
                ERR_PRINT(String("Failed to process spatial entity ") + spatial_type.first + " #" + String::num_int64(id) + ": " + e.what());
            } catch (...) {
                ERR_PRINT(String("Failed to process spatial entity ") + spatial_type.first + " #" + String::num_int64(id) + ": unknown error.");
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

    // Parents already handled as spatial structure were counted in the total too
    progress_done_.fetch_add(static_cast<uint32_t>(active_parents.size() - other_parents.size()), std::memory_order_relaxed);

    for (int id : other_parents) {
        progress_done_.fetch_add(1, std::memory_order_relaxed);
        try {
        const uint32_t type_code = loader.GetLineType(id);
        if (type_code == 0) continue;

        std::string s_type = schema_manager.IfcTypeCodeToType(type_code);

        PrecalculatedIFCItem item;
        init_item(item, id, s_type.c_str());

        if (s_type != "IfcOpeningElement") {
            const std::optional<ElementMaterials> item_materials = element_materials_for(id);
            fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), s_type == "IfcSpace", item_materials ? &*item_materials : nullptr, item);
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
    for (auto type : schema_manager.GetIfcElementList()) {
        auto expressIDs = loader.GetExpressIDsWithType(type);
        for (uint32_t expressID : expressIDs) {
            progress_done_.fetch_add(1, std::memory_order_relaxed);
            try {

            if (processed_ids.find(expressID) != processed_ids.end()) continue;

            std::string type_str = schema_manager.IfcTypeCodeToType(loader.GetLineType(expressID));
            if (type_str == "IfcOpeningElement") continue;

            auto flat_mesh = geometry_loader.GetFlatMesh(expressID);
            if (flat_mesh.geometries.empty()) continue;

            PrecalculatedIFCItem item;
            const std::optional<ElementMaterials> item_materials = element_materials_for(expressID);
            fill_item_geometry(geometry_loader, flat_mesh, type_str == "IfcSpace", item_materials ? &*item_materials : nullptr, item);
            if (item.geometry.empty()) continue;

            init_item(item, expressID, type_str.c_str());
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

    // Georeferencing data comes from IfcParse later. The offset web-ifc subtracted from every
    // vertex (coordinate_to_origin) is needed to place this model relative to another one
    // even without georef data.
    this->pending_georreference = GeorreferenceData();
    this->pending_georreference.coordination_offset = glm::dvec3(geometry_loader.GetCoordinationMatrix()[3]);

    // Hand results to the main thread
    if (material_resolver) {
        this->ifc_materials_ = std::move(material_resolver->materials);
    }
    this->web_ifc_manager = std::move(temp_ifc_manager);
    this->generation_queue = temp_queue;

    // web-ifc has consumed the bytes; the metadata task holds its own reference while it parses
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

// Creates one item's node, mesh and collision under its parent, or under the staging root.
// With collect_editor_caches, the mesh and shape are queued for their gizmo caches.
IFCNode* GDIFCManager::_create_item_node(const PrecalculatedIFCItem& item, bool collect_editor_caches) {
    // 1. Create Node
    IFCNode* element_node = memnew(IFCNode);
    element_node->set_name(item.node_name);
    element_node->set_transform(item.node_transform);
    element_node->set_ifc_class(item.ifc_class);
    element_node->set_global_id(item.global_id);

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

        // Surfaces are already merged by material; only more distinct colors than the limit get here
        if (i == RenderingServer::MAX_MESH_SURFACES) {
            WARN_PRINT(vformat("IFC element #%d (%s) has %d materials; only the first %d are shown.",
                item.express_id, item.ifc_class, (int64_t)item.geometry.size(), RenderingServer::MAX_MESH_SURFACES));
            break;
        }

        Array arrays;
        arrays.resize(Mesh::ARRAY_MAX);
        arrays[Mesh::ARRAY_VERTEX] = item.geometry[i].vertices;
        arrays[Mesh::ARRAY_NORMAL] = item.geometry[i].normals;
        arrays[Mesh::ARRAY_TEX_UV] = item.geometry[i].uvs;
        arrays[Mesh::ARRAY_INDEX] = item.geometry[i].indices;

        mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), item.geometry[i].lods);

        // 3. Material
        Ref<StandardMaterial3D> mat = _get_material(item.geometry[i].color, item.geometry[i].is_transparent, item.ifc_class == "IfcSpace", item.geometry[i].material_id);
        mesh->surface_set_material(i,mat);
    }

    if (mesh->get_surface_count() > 0) {

        element_node->set_mesh(mesh);
        if (collect_editor_caches) {
            editor_cache_queue.push_back(mesh);
        }

        if (this->should_create_collisions &&
            (this->collision_classes.is_empty() || this->collision_classes.has(item.ifc_class))) {
            Ref<Shape3D> shape = create_trimesh_collision_from_geometry(element_node, item.geometry);
            if (collect_editor_caches && shape.is_valid()) {
                editor_cache_queue.push_back(shape);
            }
        }

    }

    element_node->add_to_group(item.ifc_class,true);
    return element_node;
}

// ---------------------------------------------------------
// DUMB & FAST GENERATOR
// ---------------------------------------------------------
void GDIFCManager::_process_generation_queue() {


    uint64_t start_time = Time::get_singleton()->get_ticks_usec();
    const uint64_t time_budget = get_frame_time_budget();
    const bool prebuild_editor_caches = Engine::get_singleton()->is_editor_hint();

    while (current_generation_index < generation_queue.size()) {

        _create_item_node(generation_queue[current_generation_index], prebuild_editor_caches);

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
    if (!editor_cache_queue.is_empty()) {
        load_stage_.store(STAGE_EDITOR_CACHES);
    }
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

    // Report the reveal one frame early: the editor makes the reveal frame long, and a progress
    // display updated in that same frame would only be drawn after it
    if (load_stage_.load() != STAGE_REVEAL) {
        load_stage_.store(STAGE_REVEAL);
        return;
    }

    if (current_generation_index >= generation_queue.size()) {
        invisible_staging_root->set_name("IFCModel");

        // The Georeference node, placement relative to a reference georeference and typed
        // objects are added by _finish_metadata() once IfcParse is done
        add_child(invisible_staging_root, true);

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
        generation_queue.clear();
        current_state = DONE;
        load_stage_.store(STAGE_DONE);
        _update_load_progress();
        emit_signal("ifc_read");
        UtilityFunctions::print(vformat("IFC model shown after %.2f s",
            (Time::get_singleton()->get_ticks_usec() - start_loading_time) / 1000000.0));
    }
}

// ---------------------------------------------------------
// MATERIAL CACHING (Optimization)
// ---------------------------------------------------------
Ref<StandardMaterial3D> GDIFCManager::_get_material(Color color, bool transparent, bool is_space, uint32_t material_id) {
    // Surfaces coloured by the same IfcMaterial share one material, named after it, so editing it
    // changes every element made of that material. The rest share one material per colour.
    String key = material_id != 0
        ? String("M") + String::num_uint64(material_id)
        : String(color.to_html()) + (transparent ? "_T" : "_O") + (is_space ? "_S" : "");

    if (material_cache.has(key)) {
        return material_cache[key];
    }

    Ref<StandardMaterial3D> mat;
    mat.instantiate();
    mat->set_albedo(color);
    if (material_id != 0) {
        auto material = ifc_materials_.find(material_id);
        if (material != ifc_materials_.end()) {
            mat->set_name(material->second.name);
        }
    }
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

// IfcProperty values -> {property name -> value}. Shared by element property sets and IFC material
// properties. Properties is an aggregate of schema::IfcProperty.
template <typename schema, typename Properties>
static godot::Dictionary convert_ifc_properties(const Properties& props) {
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
    return props_dict;
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

                psets[godot::String::utf8(pset_key.c_str())] = convert_ifc_properties<schema>(props);
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

