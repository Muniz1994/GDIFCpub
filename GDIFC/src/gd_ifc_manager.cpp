#include "gd_ifc_manager.h"
#include <algorithm> // Required for std::sort
#include <cctype>     // Required for std::toupper
#include <cstring>    // Required for strlen
#include <functional> // Required for lambdas
#include <stdexcept>  // Required for std::exception
#include <mutex>

#include "godot_cpp/classes/collision_shape3d.hpp"
#include "godot_cpp/classes/concave_polygon_shape3d.hpp"
#include "godot_cpp/classes/engine.hpp"
#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/classes/importer_mesh.hpp"
#include "godot_cpp/classes/os.hpp"
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
    this->node_registry.clear();
    this->current_generation_index = 0;
    this->thread_failed_ = false;
    this->relink_failed_ = false;
    this->current_state = LOADING_THREAD;

    // Snapshot LOD options once so edits to the settings resource mid-load aren't picked up
    this->lod_enabled_ = geometric_settings.is_valid() && geometric_settings->getGenerateLods();
    this->lod_merge_angle_ = geometric_settings.is_valid() ? geometric_settings->getLodNormalMergeAngle() : 60.0f;

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

// Tessellate an entity's flat mesh into one surface per material: placed geometries sharing a
// color are appended to the same surface, since an element can have hundreds of them and a mesh
// holds at most RenderingServer::MAX_MESH_SURFACES surfaces.
// Node transforms are currently identity, so vertices are baked in world space.
static void fill_item_geometry(webifc::geometry::IfcGeometryProcessor& geometry_loader,
                               const webifc::geometry::IfcFlatMesh& flat_mesh,
                               bool is_space, PrecalculatedIFCItem& item) {
    // Same key as _get_material (RGBA8 color + transparency) -> index in item.geometry
    std::unordered_map<uint64_t, size_t> surface_by_material;

    for (const auto& geom_data : flat_mesh.geometries) {
        auto& ifc_geometry = geometry_loader.GetGeometry(geom_data.geometryExpressID);
        if (ifc_geometry.numPoints == 0 || ifc_geometry.numFaces == 0) continue;

        Color color;
        bool is_transparent;
        if (is_space) {
            is_transparent = true;
            color = Color(0.025, 0.037, 0.034, 0.1);
        } else {
            color = Color(geom_data.color.r, geom_data.color.g, geom_data.color.b, geom_data.color.a);
            is_transparent = (geom_data.color.a < 0.7);
        }

        const uint64_t key = ((uint64_t)color.to_rgba32() << 1) | (is_transparent ? 1 : 0);
        auto [surface, inserted] = surface_by_material.try_emplace(key, item.geometry.size());
        if (inserted) {
            PrecalculatedIFCItemGeometry new_geometry;
            new_geometry.color = color;
            new_geometry.is_transparent = is_transparent;
            item.geometry.push_back(std::move(new_geometry));
        }
        PrecalculatedIFCItemGeometry& item_geometry = item.geometry[surface->second];

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

            // Project onto the plane of the dominant normal axis
            const glm::dvec3 a = glm::abs(n);
            if (a.y >= a.x && a.y >= a.z) {
                uvs[k] = Vector2((float)p.x, (float)(n.y >= 0.0 ? p.z : -p.z));
            } else if (a.x >= a.z) {
                uvs[k] = Vector2((float)(n.x >= 0.0 ? -p.z : p.z), (float)-p.y);
            } else {
                uvs[k] = Vector2((float)(n.z >= 0.0 ? p.x : -p.x), (float)-p.y);
            }
        }
        for (size_t k = 0; k < out_indices.size(); k++) {
            indices[k] = out_indices[k] + (int32_t)vertex_base;
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
    loader.MoveToArgumentOffset(express_id, 0);
    if (loader.GetTokenType() != webifc::parsing::IfcTokenType::STRING) return String();
    loader.StepBack();
    return String::utf8(loader.GetDecodedStringArgument().c_str());
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

            // Spatial elements can carry geometry (e.g. IfcSite terrain); IfcProject usually has none
            fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), false, item);

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
            fill_item_geometry(geometry_loader, geometry_loader.GetFlatMesh(id), s_type == "IfcSpace", item);
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
            fill_item_geometry(geometry_loader, flat_mesh, type_str == "IfcSpace", item);
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
        Ref<StandardMaterial3D> mat = _get_material(item.geometry[i].color, item.geometry[i].is_transparent, item.ifc_class == "IfcSpace");
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

