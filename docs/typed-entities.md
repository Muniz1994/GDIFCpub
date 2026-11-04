# Typed Entity Access

This guide explains how to use generated IFC classes in GDIFC.

## Why Typed Access Matters

Every IFC object is available through `GDIFCEntityBase`, but many workflows become cleaner with typed classes such as `IfcWall`, `IfcDoor`, or `IfcSlab`.

Typed access gives:

- Better script readability
- Explicit attribute names
- Easier class-based logic

## Getting the IFC Object From a Node

Each `IFCNode` stores a reference to the underlying IFC object in `ifc_object`. It is filled in at `ifc_objects_ready`, after the geometry appears, so wait for that signal first:

```gdscript
await ifc_manager.ifc_objects_ready
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

If the cast returns `null`, the object is a different IFC class. A cast also succeeds for subtypes, so an `IfcWallStandardCase` casts to `IfcWall`.

## Runtime Class Filtering

The `IFCNode` tree sits under the manager's `IFCModel` node, nested by spatial structure (project, site, building, storey), so search it recursively:

```gdscript
await ifc_manager.ifc_objects_ready
for node in ifc_manager.find_children("*", "IFCNode", true, false):
    var slab := node.ifc_object as IfcSlab
    if slab:
        print("Slab: ", slab.Name)
```

Use this to isolate classes for analytics, UI lists, or gameplay interactions. To query the IFC file itself, including entities without a node, use `IFCModel`: `ifc_manager.get_ifc_model().instances_by_type("IfcSlab")`. See the [API reference](api-reference.md#ifcmodel-node3d).

## Dynamic Fallback

When class names are not known at compile time, use `GDIFCEntityBase` methods.

```gdscript
var e: GDIFCEntityBase = ifc_node.ifc_object
var kind := e.get_type()
var all := e.get_all_attributes()
print(kind)
print(all)
if e.is_a("IfcElement"):
    print("a physical element")
```

Generic methods are ideal for debug inspectors and import diagnostics.

## Creating IFC Objects at Runtime

```gdscript
# Create entities through the loaded model so they belong to its IFC file.
# (IfcWall.new() gives an empty wrapper: its setters do nothing.)
var wall := ifc_manager.get_ifc_model().create("IfcWall") as IfcWall
wall.Name = "Generated Wall"
ifc_manager.get_ifc_model().save("user://edited.ifc")
```

This is useful for tool scripts or procedural data experiments.
