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

#include <parsing/IfcLoader.h>
#include <schema/IfcSchemaManager.h>
#include <geometry/IfcGeometryProcessor.h>
#include <schema/ifc-schema.h>
#include <modelmanager/ModelManager.h>
#include "utils.h"


#include <Ifc4.h>
#include <Ifc2x3.h>

#include <IfcFile.h>
#include <IfcLogger.h>
#include <FileReader.h>

#include <godot_cpp/templates/vector.hpp> 
#include <string> // Standard string for the struct


#include "gd_ifc_node.h"

namespace godot {
// Holds the geometric data to allow the creation of the Godot
// geometries of the IFC object
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
    String node_name;
    Dictionary properties; // Parsed in thread
    String ifc_class;

    // Holds the geometric information of all objects
    std::vector<PrecalculatedIFCItemGeometry> geometry;

     // Flag if geometry was found
    bool valid = false;
};

class GDIFCManager : public Node {
    GDCLASS(GDIFCManager, Node)

  private:
    int64_t task_id = -1;
    bool should_create_collisions = false;
    Array collision_classes = Array();

    enum LoadState {
        IDLE,
        LOADING_THREAD,
        GENERATING_NODES,
        DONE
    };
    LoadState current_state = IDLE;

    // Storage
    std::unique_ptr<IFCManager> web_ifc_manager;
    std::unique_ptr<IfcParse::IfcFile> ifc_parse_file;

    // The Optimized Queue
    Vector<PrecalculatedIFCItem> generation_queue;
    int current_generation_index = 0;
    Node3D* main_node_root = nullptr;

    // Material Cache to reduce draw calls and allocation time
    HashMap<String, Ref<StandardMaterial3D>> material_cache;

  protected:
    static void _bind_methods();

  public:
    GDIFCManager();
    ~GDIFCManager();

    void read_ifc(String path, bool create_collision, Array collision_classes);
    void _process(double delta) override;

    // Thread Functions
    void _thread_task(String path);

    // Main Thread Functions
    void _process_generation_queue();
    Ref<StandardMaterial3D> _get_material(Color color, bool transparent);

    Node3D* invisible_staging_root = nullptr;
};



} // namespace godot

template <typename schema>
godot::Dictionary get_ifc_property_sets(IfcParse::IfcFile& file, int expressID);

godot::Dictionary get_ifc_object_attributes(IfcParse::IfcFile& file, int expressID);

godot::Variant to_godot_variant(const AttributeValue& attr_value);

#endif
