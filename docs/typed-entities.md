# Typed Entity Access

This guide explains how to use generated IFC classes in GDIFC.

## Why Typed Access Matters

Every IFC object is available through `GDIFCEntityBase`, but many workflows become cleaner with typed classes such as `IfcWall`, `IfcDoor`, or `IfcSlab`.

Typed access gives:

- Better script readability
- Explicit attribute names
- Easier class-based logic

## Getting the IFC Object From a Node

Each `IFCNode` stores a reference to the underlying IFC object.

```gdscript
var base_entity: GDIFCEntityBase = ifc_node.ifc_object
print(base_entity.get_type())
```

## Casting to a Specific IFC Class

```gdscript
var wall := ifc_node.ifc_object as IfcWall
if wall:
    print("Wall name: ", wall.Name)
    print("Wall predefined type: ", wall.PredefinedType)
```

If the cast returns `null`, the object is a different IFC class.

## Runtime Class Filtering

```gdscript
for child in ifc_manager.get_children():
    if child is IFCNode:
        var slab := child.ifc_object as IfcSlab
        if slab:
            print("Slab: ", slab.Name)
```

Use this to isolate classes for analytics, UI lists, or gameplay interactions.

## Dynamic Fallback

When class names are not known at compile time, use `GDIFCEntityBase` methods.

```gdscript
var e: GDIFCEntityBase = ifc_node.ifc_object
var kind := e.get_type()
var all := e.get_all_attributes()
print(kind)
print(all)
```

Generic methods are ideal for debug inspectors and import diagnostics.

## Creating IFC Objects at Runtime

```gdscript
var wall := IfcWall.new()
wall.Name = "Generated Wall"
wall.PredefinedType = "SOLIDWALL"
```

This is useful for tool scripts or procedural data experiments.
