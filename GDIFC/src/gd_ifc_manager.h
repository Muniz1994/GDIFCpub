#ifndef GD_IFC_MANAGER_H
#define GD_IFC_MANAGER_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/classes/marshalls.hpp>

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>
#include <modelmanager/ModelManager.h>
#include "web_ifc_manager.h"
#include "value_conversion.h"


#include <Ifc4.h>
#include <Ifc2x3.h>
#include <Ifc4x3_add2.h>

#include <IfcFile.h>
#include <FileReader.h>

#include <godot_cpp/templates/vector.hpp> 
#include <string> // Standard string for the struct
#include <atomic>
#include <unordered_map>

#include "gd_ifc_node.h"
#include "gd_ifc_settings.h"
#include "gd_ifc_entity_base.h"
#include "gd_ifc_model.h"
#include "gd_ifc_georeference.h"
#include "gd_ifc_material.h"
#include "gd_ifc_collision.h"

namespace godot {

struct PrecalculatedIFCItemGeometry {

    // Mesh Data
    PackedVector3Array vertices;
    PackedVector3Array normals;
    PackedVector2Array uvs; // box-projected, 1 UV unit = 1 meter
    PackedInt32Array indices;
    Dictionary lods; // LOD size (float) -> PackedInt32Array of indices; empty when no LODs

    // Material Data
    Color color;
    bool is_transparent;
    // Express id of the IfcMaterial that coloured this surface, 0 for none. Only set with IFC materials on.
    uint32_t material_id = 0;
};

// Holds fully processed data.
// The Main Thread just reads this and assigns it. No math.
    struct PrecalculatedIFCItem {
        bool valid = false;
        String node_name;
        String ifc_class;
        String global_id;    // IFC GlobalId (22-char GUID) for scene persistence
        int express_id = -1; // NEW: Track the ID
        int parent_id = -1;  // NEW: Track the parent ID
        std::vector<PrecalculatedIFCItemGeometry> geometry;
        Transform3D node_transform; // IFC placement transform; vertices are in this node's local space
    };
    

// Attributes, property sets and quantities of one item, read by IfcParse after the model is shown
struct PrecalculatedIFCMetadata {
    int express_id = -1;
    Dictionary attributes;
    Dictionary properties;
    Dictionary quantities;
    Ref<GDIFCMaterialDefinition> material; // with IFC materials on
};

// The look of an IfcSurfaceStyle beyond its colour, read by the metadata task and applied to a
// generated material on the main thread
struct PrecalculatedIFCStyle {
    enum Slot { ALBEDO, NORMAL, HEIGHT, EMISSION };
    struct Texture {
        Slot slot = ALBEDO;
        String key;                // resolved path, or the embedding entity, to share textures
        Ref<Texture2D> texture;    // a texture imported in the project
        Ref<Image> image;          // an image decoded from a file outside it or from the IFC file
        bool repeat = true;
        Vector2 uv_scale = Vector2(1, 1);
        Vector2 uv_offset;
    };
    float roughness = -1.0f; // -1 keeps the material's own
    float metallic = -1.0f;
    std::vector<Texture> textures;

    bool is_empty() const { return roughness < 0.0f && metallic < 0.0f && textures.empty(); }
    bool same_look(const PrecalculatedIFCStyle &other) const;
};

// An IfcMaterial found by the web-ifc task, which may or may not colour a surface
struct PrecalculatedIFCMaterial {
    String name;
    Color color = Color(1, 1, 1, 1); // web-ifc's colour for it, white when it has none
    bool has_color = false;
};

class GDIFCManager : public Node3D {
    GDCLASS(GDIFCManager, Node3D)

  private:
    int64_t task_id = -1;
    // Which elements get collision and which shape, resolved per class. Built when a load starts,
    // from collision_matrix_ or from read_ifc()'s class list.
    GDIFCCollisionPlan collision_plan_;
    Ref<GDIFCCollisionMatrix> collision_matrix_;
    // Convex decomposition only runs at import: V-HACD is too slow for loads spread across frames
    bool allow_decomposition_ = false;
    bool warned_decomposition_ = false;
    // Express ID -> generated node. ObjectID so lookups survive nodes freed by the user.
    HashMap<int, ObjectID> node_registry;

    enum LoadState {
        IDLE,
        LOADING_THREAD,
        GENERATING_LODS,
        GENERATING_NODES,
        RELINKING,
        DONE,
        FAILED
    };
    LoadState current_state = IDLE;

    // Storage
    std::unique_ptr<WEBIFCManager> web_ifc_manager;
    std::shared_ptr<IfcParse::IfcFile> ifc_parse_file;

    // Written by worker tasks, committed to the members above by _process() once the task completes
    std::shared_ptr<IfcParse::IfcFile> pending_ifc_parse_file;
    GeorreferenceData pending_georreference;
    std::atomic<bool> thread_failed_{false};

    // The Optimized Queue
    Vector<PrecalculatedIFCItem> generation_queue;
    int current_generation_index = 0;
    // Editor only: meshes and collision shapes whose gizmo caches are built before the model is revealed
    Vector<Ref<Resource>> editor_cache_queue;
    int current_editor_cache_index = 0;
    ObjectID ifc_model_node_id_;

    // Material Cache to reduce draw calls and allocation time
    HashMap<String, Ref<StandardMaterial3D>> material_cache;
    // IfcMaterial express id -> name and colour, for the materials of every element with geometry.
    // Written by the web-ifc task, read by the main thread once it completes.
    std::unordered_map<uint32_t, PrecalculatedIFCMaterial> ifc_materials_;

    // Material definitions of the loaded model, with IFC materials on
    Ref<GDIFCMaterialLibrary> material_library_;

    // Replacing a GDIFCMaterialSingle.visual replaces it on every surface showing the old one.
    // Keyed by the GDIFCMaterialSingle's instance id: the visual its surfaces last got, and the
    // callable watching its changed signal.
    HashMap<uint64_t, Ref<Material>> applied_visuals_;
    HashMap<uint64_t, Callable> material_watchers_;
    // Surfaces of each material, found on the first replacement
    struct MaterialSurface {
        ObjectID node;
        int surface = 0;
    };
    HashMap<uint64_t, Vector<MaterialSurface>> material_surfaces_;
    bool material_surfaces_found_ = false;

    GeorreferenceData georreference;

    Ref<GDIFCLoaderSettings> geometric_settings;

    // Buffer shared between read_ifc / read_ifc_base64 and the worker thread
    PackedByteArray pending_buffer;

    uint64_t start_loading_time{};

    // Persistent IFC file path — serialized so cold-reload can re-parse
    String ifc_file_path_;

    // Optional IFCGeoreference the loaded model is placed relative to
    NodePath reference_georeference_;

    // Set by _metadata_thread_task if the IFC file is missing
    bool relink_failed_ = false;

  public:
    enum LoadStage {
        STAGE_IDLE,
        STAGE_PARSING,
        STAGE_GEOMETRY,
        STAGE_LODS,
        STAGE_NODES,
        STAGE_EDITOR_CACHES,
        STAGE_REVEAL,
        STAGE_DONE,
        STAGE_FAILED
    };

  private:
    // Load progress. The worker stages (parsing, geometry) report through done/total;
    // the main-thread stages read their queue indices directly.
    std::atomic<int> load_stage_{STAGE_IDLE};
    std::atomic<uint32_t> progress_done_{0};
    std::atomic<uint32_t> progress_total_{0};
    // Start of the opaque IfcParse step and the size it parses, to estimate its progress
    std::atomic<uint64_t> parse_step_start_usec_{0};
    uint64_t parse_bytes_ = 0;
    // Last reported values, main thread only. Progress never moves backwards within a load.
    float load_progress_ = 0.0f;
    int reported_stage_ = STAGE_IDLE;

    // LODs are generated by a group task over generation_queue once web-ifc is done
    int64_t lod_group_id = -1;
    PrecalculatedIFCItem* lod_items_ = nullptr;
    bool lod_enabled_ = false;
    float lod_merge_angle_ = 60.0f;

    // IFC material options, read once per load like the LOD options
    bool ifc_materials_enabled_ = false;
    bool ifc_textures_enabled_ = false;

    // Deferred metadata. IfcParse parses on its own task while web-ifc builds the geometry.
    // Attributes, property sets, quantities, typed objects and georeferencing are applied
    // after the model is shown, then ifc_objects_ready is emitted.
    enum MetadataState {
        META_IDLE,
        META_PARSING,
        META_EXTRACTING,
        META_APPLYING,
        META_DONE
    };
    MetadataState metadata_state = META_IDLE;
    int64_t metadata_task_id = -1;
    // Written by the metadata tasks, read by the main thread once each task completes
    PackedByteArray metadata_buffer_;
    std::shared_ptr<IfcParse::IfcFile> metadata_file_;
    int metadata_schema_index_ = -1;
    std::vector<int> metadata_ids_;
    std::vector<PrecalculatedIFCMetadata> metadata_results_;
    Ref<GDIFCMaterialLibrary> metadata_material_library_;
    // Surface style looks for generated materials: by IfcMaterial express id, and by the colour
    // key of the materials shared per colour
    std::unordered_map<uint32_t, PrecalculatedIFCStyle> metadata_material_styles_;
    std::vector<std::pair<String, PrecalculatedIFCStyle>> metadata_colour_styles_;
    GeorreferenceData metadata_georreference_;
    size_t metadata_apply_index_ = 0;

    Error _start_loading(bool _create_collision, const Array &_collision_classes);
    void _reset_load_state(bool _create_collision, const Array &_collision_classes);
    IFCNode* _create_item_node(const PrecalculatedIFCItem& item, bool collect_editor_caches);
    void _create_item_collision(IFCNode* node, const PrecalculatedIFCItem& item, const GDIFCCollisionPlan::Rule& rule, bool collect_editor_caches);
    void _apply_metadata_item(PrecalculatedIFCMetadata& item);
    void _apply_reference_placement(const GeorreferenceData& model_georeference);
    void _apply_instance_reference_placement();
    void _wait_for_task();
    void _wait_for_metadata_task();
    bool _is_loading() const;
    void _lod_group_task(uint32_t index);
    void _metadata_parse_task();
    void _metadata_extract_task();
    void _process_metadata();
    void _finish_metadata();
    void _bind_material_visuals();
    void _apply_material_styles();
    void _relink_material_library();
    void _watch_material_library();
    void _unwatch_material_library();
    void _find_material_surfaces();
    void _on_ifc_material_changed(uint64_t material_id);
    void _set_worker_progress(LoadStage stage, uint32_t total);
    void _update_load_progress();

  protected:
    static void _bind_methods();

  public:
    GDIFCManager();
    ~GDIFCManager() override;

    Error read_ifc(const String &_path, bool _create_collision, const Array &_collision_classes);
    Error read_ifc_blocking(const String &_path, bool _create_collision, const Array &_collision_classes);
    float get_load_progress() const { return load_progress_; }
    String get_load_stage() const;
    Error read_ifc_base64(const String &_base64_data, bool _create_collision, const Array &_collision_classes);
    void _process(double delta) override;
    void _ready() override;

    // Thread Functions
    void _thread_task();
    void _metadata_thread_task();

    // Main Thread Functions
    void _process_generation_queue();
    void _relink_ifc_objects();
    Ref<StandardMaterial3D> _get_material(Color color, bool transparent, bool is_space, uint32_t material_id);

    Node3D* invisible_staging_root = nullptr;

    GeorreferenceData get_georreference_data();
    void set_georreference_data(godot::GeorreferenceData data);

    Ref<GDIFCLoaderSettings> get_gdifc_settings();
    void set_gdifc_settings(Ref<GDIFCLoaderSettings> gdifc_settings);

    String get_ifc_file_path() const { return ifc_file_path_; }
    void set_ifc_file_path(const String &path) { ifc_file_path_ = path; }

    /// Per-class collision. When set, it decides collision for every element, and read_ifc()'s
    /// create_collision and collision_classes are ignored.
    Ref<GDIFCCollisionMatrix> get_collision_matrix() const { return collision_matrix_; }
    void set_collision_matrix(const Ref<GDIFCCollisionMatrix> &matrix) { collision_matrix_ = matrix; }

    Ref<GDIFCMaterialLibrary> get_material_library() const { return material_library_; }
    void set_material_library(const Ref<GDIFCMaterialLibrary> &library);

    /// Every material definition in the library with this IFC Name
    godot::Array find_material_definitions(const String &name) const;

    /// Gives every IfcMaterial with this IFC Name the visual, on all its surfaces. Returns how many materials changed.
    int set_material_visual(const String &material_name, const Ref<Material> &visual);

    NodePath get_reference_georeference() const { return reference_georeference_; }
    void set_reference_georeference(const NodePath &path);

    /// Return the IFCModel root node (available after loading completes).
    IFCModel* get_ifc_model();

    /// Find a scene node by IFC GlobalId string.
    IFCNode* get_node_by_global_id(godot::String global_id);

    /// Get a typed IFC entity object by GlobalId (does not require node existence).
    Ref<GDIFCEntityBase> get_ifc_object_by_global_id(godot::String global_id);

    /// Return all scene nodes that belong to the given IFC class (including subtypes).
    godot::Array get_elements_by_class(godot::String ifc_class);
};



} // namespace godot



template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID);

template <typename schema>
godot::Dictionary get_ifc_quantity_sets(IfcParse::IfcFile& file, int expressID);

template <typename schema>
godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID);

godot::Variant to_godot_variant(const AttributeValue& attr_value);

template <typename schema>
godot::GeorreferenceData get_georreference(IfcParse::IfcFile& file);


#endif
