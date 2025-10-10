// gdifcreader.cpp
#include "gd_ifc_node.h"

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"


using namespace godot;

// Binds the new method for use in Godot scripts
void IFCNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_attributes"), &IFCNode::get_attributes);
};



IFCNode::IFCNode() {};
IFCNode::~IFCNode() {}

godot::Dictionary IFCNode::get_attributes()
{
    return attributes;
}
;

void IFCNode::set_attributes(godot::Dictionary dict)
{
    attributes = dict;
}
godot::Array IFCNode::get_properties()
{
    return properties;
}
void IFCNode::set_properties(godot::Array props)
{
    properties = props;

}
;