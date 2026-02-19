#ifndef GD_IFC_MANAGER_H
#define GD_IFC_MANAGER_H

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/worker_thread_pool.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>
#include <modelmanager/ModelManager.h>
#include "web_ifc_manager.h"
#include "value_conversion.h"


#include <Ifc4.h>
#include <Ifc2x3.h>
#include <Ifc4x3.h>
#include <Ifc4x3_add2.h>

#include <IfcFile.h>
#include <FileReader.h>

#include <godot_cpp/templates/vector.hpp> 
#include <string> // Standard string for the struct

#include "gd_ifc_node.h"
#include "gd_ifc_settings.h"

namespace godot {
// Holds the geometric data to allow the creation of the Godot
// geometries of the IFC object

    struct MapConversion{

        int32_t Eastings;
        int32_t Northings;
        int32_t OrthogonalHeight;
        int32_t XAxisAbscissa;
        int32_t XAxisOrdinate;
        int16_t Scale;

    };

    struct ProjectedCRS {

        String Name;
        String Description;
        String GeodeticDatum;
        String VerticalDatum;
    };

struct GeorreferenceData {
    MapConversion map_conversion;
    ProjectedCRS projected_crs;

    GeorreferenceData():map_conversion{0,0,0,0,0,0},projected_crs{"NotDefined","NotDefined","NotDefined","NotDefined"}
    {}

    GeorreferenceData(MapConversion map_conversion, ProjectedCRS projected_crs): map_conversion{map_conversion},projected_crs{projected_crs}
    {}

};

struct PrecalculatedIFCItemGeometry {

    // Mesh Data
    PackedVector3Array vertices;
    PackedVector3Array normals;
    PackedInt32Array indices;

    // Material Data
    Color color;
    bool is_transparent;
};

// Holds fully processed data.
// The Main Thread just reads this and assigns it. No math.
    struct PrecalculatedIFCItem {
        bool valid = false;
        String node_name;
        String ifc_class;
        int express_id = -1; // NEW: Track the ID
        int parent_id = -1;  // NEW: Track the parent ID
        Dictionary properties;
        Dictionary attributes;
        std::vector<PrecalculatedIFCItemGeometry> geometry;
    };
    

class GDIFCManager : public Node3D {
    GDCLASS(GDIFCManager, Node3D)

  private:
    int64_t task_id = -1;
    bool should_create_collisions = false;
    Array collision_classes = Array();
    HashMap<int, Node*> node_registry;

    enum LoadState {
        IDLE,
        LOADING_THREAD,
        GENERATING_NODES,
        DONE,
        FAILED
    };
    LoadState current_state = IDLE;

    // Storage
    std::unique_ptr<WEBIFCManager> web_ifc_manager;
    std::unique_ptr<IfcParse::IfcFile> ifc_parse_file;

    // The Optimized Queue
    Vector<PrecalculatedIFCItem> generation_queue;
    int current_generation_index = 0;
    Node3D* main_node_root = nullptr;

    // Material Cache to reduce draw calls and allocation time
    HashMap<String, Ref<StandardMaterial3D>> material_cache;

    GeorreferenceData georreference;

    Ref<GDIFCLoaderSettings> geometric_settings;

    uint64_t start_loading_time{};

  protected:
    static void _bind_methods();

  public:
    GDIFCManager();
    ~GDIFCManager() override;

    Error read_ifc(const String &_path, bool _create_collision, const Array &_collision_classes);
    void _process(double delta) override;

    // Thread Functions
    void _thread_task(const String& _path);

    // Main Thread Functions
    void _process_generation_queue();
    Ref<StandardMaterial3D> _get_material(Color color, bool transparent);

    Node3D* invisible_staging_root = nullptr;

    GeorreferenceData get_georreference_data();
    void set_georreference_data(godot::GeorreferenceData data);

    Ref<GDIFCLoaderSettings> get_gdifc_settings();
    void set_gdifc_settings(Ref<GDIFCLoaderSettings> gdifc_settings);
};



} // namespace godot



template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID);

template <typename schema>
godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID);

godot::Variant to_godot_variant(const AttributeValue& attr_value);

template <typename schema>
godot::GeorreferenceData get_georreference(IfcParse::IfcFile& file);

template <typename schema>
std::unordered_map<int, int> build_spatial_hierarchy(IfcParse::IfcFile &file);

#endif
