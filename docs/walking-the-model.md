# Walking The Model

Each `IFCNode` carries a copy of the data most scripts need: attributes, property and quantity sets (with the sets of its type merged in), and the value type and unit of each value. The rest of the IFC file is not copied, but none of it is lost. After `ifc_objects_ready`, a node's `ifc_object` is its IFC entity, and from it you can follow relationships to any other entity in the file, including those without a node: types, classifications, systems, zones, tasks, documents and so on.

## What Is On The Nodes

| On every `IFCNode` | Reached by walking |
|---|---|
| `ifc_class`, `global_id`, `attributes` | The element's type entity and its attributes |
| `properties` and `quantities`, including the type's sets | Classification references |
| `property_details` and `quantity_details`: value types and units | Groups, systems and zones |
| The spatial structure, as the node tree | Openings, and the doors and windows filling them |
| `material_definition`, with `ifc_materials` on | Space boundaries, port connections and other relationships |
| | Entities without a node: types, tasks, cost items, actors, documents, products without geometry, openings |
| | IFC4 predefined property sets, such as `IfcDoorLiningProperties` |

## How Walking Works

Three calls cover everything:

- `entity.get_attribute(name)` follows an attribute of the entity. Entities come back as `GDIFCEntityBase` objects and lists as an `Array` of them, so the result can be walked further.
- `entity.get_inverse(name)` returns the entities that refer to this one through one of IFC's inverse attributes, such as `IsTypedBy` or `HasAssociations`. The [IFC documentation](https://ifc43-docs.standards.buildingsmart.org/) lists each class's inverse attributes.
- `ifc_manager.get_ifc_model()` searches the whole file: `instances_by_type`, `get_by_id`, `get_by_global_id`, `get_project` and `get_units`.

IFC relates entities through relationship entities. A wall doesn't point at its type: an `IfcRelDefinesByType` points at both. So most walks take two steps: `get_inverse` to reach the relationship, then `get_attribute` to reach the other side.

To get from an entity back to its node, use `ifc_manager.get_node_by_global_id(entity.get_attribute("GlobalId"))`.

## Recipes

These work in IFC2X3, IFC4 and IFC4X3 unless noted. Each takes a node's `ifc_object`.

### The Element's Type

```gdscript
func get_type(entity: GDIFCEntityBase) -> GDIFCEntityBase:
    for rel in entity.get_inverse("IsTypedBy"): # IFC4 and later
        return rel.get_attribute("RelatingType")
    for rel in entity.get_inverse("IsDefinedBy"): # IFC2X3
        if rel.is_a("IfcRelDefinesByType"):
            return rel.get_attribute("RelatingType")
    return null
```

The type's property sets are already merged into the node's `properties`. Walk to the type for its other attributes, such as its `Name` or `PredefinedType`, or to find every element sharing it through the type's own `get_inverse("Types")` (IFC4) or `get_inverse("ObjectTypeOf")` (IFC2X3).

### Classifications

```gdscript
func get_classifications(entity: GDIFCEntityBase) -> Array:
    var references := []
    for rel in entity.get_inverse("HasAssociations"):
        if rel.is_a("IfcRelAssociatesClassification"):
            references.append(rel.get_attribute("RelatingClassification"))
    return references
```

A reference's code is its `Identification` in IFC4 and later, and its `ItemReference` in IFC2X3.

### Groups, Systems And Zones

```gdscript
func get_groups(entity: GDIFCEntityBase) -> Array:
    var groups := []
    for rel in entity.get_inverse("HasAssignments"):
        if rel.is_a("IfcRelAssignsToGroup"):
            groups.append(rel.get_attribute("RelatingGroup"))
    return groups
```

Check each group with `is_a("IfcSystem")`, `is_a("IfcZone")` and so on. To list every system of the model, use `ifc_manager.get_ifc_model().instances_by_type("IfcSystem")`.

### The Containing Storey Or Space

```gdscript
func get_container(entity: GDIFCEntityBase) -> GDIFCEntityBase:
    for rel in entity.get_inverse("ContainedInStructure"):
        return rel.get_attribute("RelatingStructure")
    return null
```

The node tree already follows this, along with aggregation and nesting. An element referenced in several storeys (`IfcRelReferencedInSpatialStructure`) appears in the tree only under the storey that contains it.

### Doors And Windows In A Wall

```gdscript
func get_fillings(entity: GDIFCEntityBase) -> Array:
    var fillings := []
    for voids in entity.get_inverse("HasOpenings"):
        var opening: GDIFCEntityBase = voids.get_attribute("RelatedOpeningElement")
        for fills in opening.get_inverse("HasFillings"):
            fillings.append(fills.get_attribute("RelatedBuildingElement"))
    return fillings
```

Openings have no node, so this is the only way to reach them.

### The Elements Bounding A Space

```gdscript
func get_bounding_elements(space: GDIFCEntityBase) -> Array:
    var elements := []
    for boundary in space.get_inverse("BoundedBy"):
        var element = boundary.get_attribute("RelatedBuildingElement")
        if element and not elements.has(element):
            elements.append(element)
    return elements
```

### Entities Without A Node

```gdscript
var model := ifc_manager.get_ifc_model()
for task in model.instances_by_type("IfcTask"):
    print(task.get_attribute("Name"))
```

### References In `attributes`

In a node's `attributes` dictionary, an attribute that refers to another entity holds that entity's express id, such as `ObjectPlacement` or `OwnerHistory`. Resolve it with `get_by_id`, or read it from `ifc_object`, which returns the entity itself:

```gdscript
var placement := ifc_manager.get_ifc_model().get_by_id(node.attributes["ObjectPlacement"])
var same_placement: GDIFCEntityBase = node.ifc_object.get_attribute("ObjectPlacement")
```

### Units

`property_details` and `quantity_details` already give each value's unit. The project's units are also available on their own:

```gdscript
var units := ifc_manager.get_ifc_model().get_units() # {"LENGTHUNIT": "mm", "AREAUNIT": "m²", ...}
```

## When Walking Is Not Available

Walking needs the parsed IFC file, so it is not available:

- before `ifc_objects_ready`
- for files whose schema GDIFC can't read: they load with geometry only
- in a reopened scene whose `.ifc` file can't be found. `ifc_object` is relinked from `GDIFCManager.ifc_file_path` when the scene opens, so models loaded with `read_ifc_base64` can't be walked after a reload, and exported projects need `*.ifc` in the export preset's **Filters to export non-resource files/folders**.

The dictionaries on the nodes are saved with the scene, so they stay available in all of these cases.
