// gdifcreader.cpp
#include "gd_ifc_node.h"

#include "godot_cpp/classes/file_access.hpp"
#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"


using namespace godot;

// Binds the new method for use in Godot scripts
void IFCNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_attributes"), &IFCNode::get_attributes);
    ClassDB::bind_method(D_METHOD("set_attributes","attrs"), &IFCNode::set_attributes);
    ClassDB::bind_method(D_METHOD("get_properties"), &IFCNode::get_properties);
    ClassDB::bind_method(D_METHOD("set_properties","props"), &IFCNode::set_properties);
    ClassDB::bind_method(D_METHOD("get_class"), &IFCNode::get_class);


    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY,"attributes"),"set_attributes","get_attributes");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY,"properties"),"set_properties","get_properties");
};



IFCNode::IFCNode() {};
IFCNode::~IFCNode() {}

godot::Dictionary IFCNode::get_attributes()
{
    return attributes;
}

void IFCNode::set_attributes(godot::Dictionary dict)
{
    attributes = dict;
}
godot::Dictionary IFCNode::get_properties()
{
    return properties;
}
void IFCNode::set_properties(godot::Dictionary props)
{
    properties = props;

}

godot::String IFCNode::get_class()
{return (this->ifc_class);}

void IFCNode::set_class(godot::String class_name) {
    this->ifc_class = class_name;
}
