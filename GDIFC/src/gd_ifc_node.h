#ifndef GDIFCNODE_H
#define GDIFCNODE_H


#include "godot_cpp/classes/node3d.hpp"



// Your existing class definition
class IFCNode : public godot::Node3D {
    GDCLASS(IFCNode, Node3D);

protected:
    static void _bind_methods();

public:
    IFCNode();
    ~IFCNode();

    

    godot::Dictionary get_attributes();
    void set_attributes(godot::Dictionary dict);

    godot::Array get_properties();
    void set_properties(godot::Array props);

private:

    godot::Dictionary attributes;
    godot::Array properties;
    // Helper function to create Godot mesh from web-ifc data

};

#endif