#ifndef GDIFCNODE_H
#define GDIFCNODE_H


#include "godot_cpp/classes/node3d.hpp"
#include "godot_cpp/classes/mesh_instance3d.hpp"



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

private:

    godot::String ifc_class;
    godot::Dictionary attributes;
    godot::Dictionary properties;
    godot::Dictionary quantities;
    // Helper function to create Godot mesh from web-ifc data

};

#endif