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

## Attribute Types

Attributes that refer to another entity, such as `OwnerHistory` or `ObjectPlacement`, are `GDIFCEntityBase` properties. Every other attribute is a `Variant` property: the class reference says what it holds, such as a `float` for `IfcDoor.OverallHeight`, and it is `null` when the file leaves it unset.

```gdscript
var door := ifc_node.ifc_object as IfcDoor
if door.OverallHeight != null:
    print("Height: ", door.OverallHeight)
```

Since the property is a `Variant`, a typed variable fails at runtime when the attribute is unset: `var height: float = door.OverallHeight` errors on a door without a height. Check for `null` first.

- Numbers are `int` or `float`, as the schema declares, even when the file writes them the other way.
- Enumerations are `String`s, such as `"NOTDEFINED"`. Logicals are `"TRUE"`, `"FALSE"` or `"UNKNOWN"`.
- Lists are arrays: packed arrays of numbers or strings, or `Array`s of entities.
- Property values, such as an `IfcPropertySingleValue`'s `NominalValue`, read as their plain value (`"Reference"`, `3.5`), not as an `IfcLabel` or `IfcLengthMeasure`. Setting one keeps its IFC type when the new value fits.

## Files of Every Schema

The same classes serve IFC2X3, IFC4 and IFC4X3 files. Each class has the attributes its entity has in any of them, and reads them from the file's own schema:

- An attribute the file's schema doesn't have reads as `null`, and setting it reports an error. `IfcSpace.PredefinedType` is `null` in an IFC2X3 file, which has `InteriorOrExteriorSpace` instead.
- Attributes IFC4X3 renamed are read under their new name. `IfcProperty.Specification` is the `Description` of an IFC2X3 or IFC4 property, and `IfcOrganization.Identification` the `Id` of an IFC2X3 organization.
- An attribute whose type changed between schemas reads as the type of the file's schema, such as `IfcQuantityCount.CountValue` (an `int` in IFC4X3, a `float` before) and the dates that IFC2X3 holds as entities.
- Entities only found in older schemas, such as `IfcDoorStyle` or `IfcBuildingElement`, have classes too.

The class reference in the editor says which schemas a class or attribute is in, and the old name of a renamed attribute. The hierarchy is IFC4X3's: an IFC4 wall is an `IfcBuiltElement` for a cast, although IFC4 calls that class `IfcBuildingElement`. `is_a()` follows the file's own schema, so `is_a("IfcBuildingElement")` is true for it.

Setters convert the value to the type the file's schema declares: an enumeration takes its value in any case, and an entity reference must be an entity of the same file of the right type. A value that doesn't fit is refused with an error, and the entity is left unchanged.

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

Generic methods are ideal for debug inspectors and import diagnostics. With `get_attribute` and `get_inverse`, they also follow relationships to other entities: see [Walking The Model](walking-the-model.md) for the element's type, classifications, systems, openings and more.

## Creating IFC Objects at Runtime

```gdscript
# Create entities through the loaded model so they belong to its IFC file.
# (IfcWall.new() gives an empty wrapper: its setters do nothing.)
var wall := ifc_manager.get_ifc_model().create("IfcWall") as IfcWall
wall.Name = "Generated Wall"
ifc_manager.get_ifc_model().save("user://edited.ifc")
```

This is useful for tool scripts or procedural data experiments.
