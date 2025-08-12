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

private:

    godot::Dictionary attributes;
    // Helper function to create Godot mesh from web-ifc data

};

#endif