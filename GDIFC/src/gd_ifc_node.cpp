// gdifcreader.cpp
#include "gd_ifc_node.h"

#include "godot_cpp/core/class_db.hpp"
#include "godot_cpp/variant/utility_functions.hpp"


using namespace godot;

// Binds the new method for use in Godot scripts
void IFCNode::_bind_methods() {
    ClassDB::bind_method(D_METHOD("get_attributes"), &IFCNode::get_attributes);
    ClassDB::bind_method(D_METHOD("set_attributes","attrs"), &IFCNode::set_attributes);
    ClassDB::bind_method(D_METHOD("get_properties"), &IFCNode::get_properties);
    ClassDB::bind_method(D_METHOD("set_properties","props"), &IFCNode::set_properties);
    ClassDB::bind_method(D_METHOD("get_quantities"), &IFCNode::get_quantities);
    ClassDB::bind_method(D_METHOD("set_quantities","quants"), &IFCNode::set_quantities);
    ClassDB::bind_method(D_METHOD("get_ifc_class"), &IFCNode::get_ifc_class);
    ClassDB::bind_method(D_METHOD("set_ifc_class", "class"), &IFCNode::set_ifc_class);

    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY,"attributes"),"set_attributes","get_attributes");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY,"properties"),"set_properties","get_properties");
    ADD_PROPERTY(PropertyInfo(Variant::DICTIONARY,"quantities"),"set_quantities","get_quantities");
    ADD_PROPERTY(PropertyInfo(Variant::STRING,"ifc_class"),"set_ifc_class","get_ifc_class");
};



IFCNode::IFCNode() {};
IFCNode::~IFCNode() {}

godot::Dictionary IFCNode::get_attributes()
{
    return attributes;
}

void IFCNode::set_attributes(godot::Dictionary dict)
{
    attributes = std::move(dict);
}
godot::Dictionary IFCNode::get_properties()
{
    return properties;
}
void IFCNode::set_properties(godot::Dictionary props)
{
    properties = std::move(props);

}

godot::Dictionary IFCNode::get_quantities()
{
    return quantities;
}
void IFCNode::set_quantities(godot::Dictionary quants)
{
    quantities = std::move(quants);

}
