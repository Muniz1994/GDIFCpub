#ifndef GDIFCNODE_H
#define GDIFCNODE_H


#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"
#include "gd_ifc_entity_base.h"


// Your existing class definition
class IFCNode : public godot::MeshInstance3D {
    GDCLASS(IFCNode, MeshInstance3D);

protected:
    static void _bind_methods();

public:
    IFCNode();
    ~IFCNode();

    

    godot::Dictionary get_attributes();
    void set_attributes(godot::Dictionary dict);

    godot::Dictionary get_properties();
    void set_properties(godot::Dictionary props);

    godot::Dictionary get_quantities();
    void set_quantities(godot::Dictionary quants);

    godot::String get_class();
    void set_class(godot::String class_name);

    godot::String get_ifc_class() {return ifc_class;}
    void set_ifc_class(godot::String ifcclass) {ifc_class = ifcclass;};

    /// IFC GlobalId (22-char GUID) — serialized so it survives scene save/reload.
    godot::String get_global_id() const { return global_id_; }
    void set_global_id(godot::String id) { global_id_ = id; }

    /// Access the typed IFC object wrapping the underlying data.
    godot::Ref<godot::GDIFCEntityBase> get_ifc_object() const { return ifc_object_; }
    void set_ifc_object(godot::Ref<godot::GDIFCEntityBase> obj) { ifc_object_ = obj; }

private:

    godot::String ifc_class;
    godot::String global_id_;
    godot::Dictionary attributes;
    godot::Dictionary properties;
    godot::Dictionary quantities;

    /// Typed GD wrapper for the IFC entity (set after loading).
    godot::Ref<godot::GDIFCEntityBase> ifc_object_;

};

#endif