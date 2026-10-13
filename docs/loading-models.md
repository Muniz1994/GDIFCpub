# Loading Models

This page covers practical loading patterns and runtime behavior.

## Loading From File Path

Use `read_ifc(path, create_collision, collision_classes)` when your IFC is a file.

```gdscript
var err := ifc_manager.read_ifc("res://models/bridge.ifc", false, [])
if err != OK:
    push_error("read_ifc failed with code %s" % err)
```

Parameter details:

- `path`: Godot resource path (`res://...`) or absolute path.
- `create_collision`: Set `true` to create collision shapes.
- `collision_classes`: IFC class names to include in collision generation.

If `collision_classes` is empty, collision generation is skipped even if `create_collision` is true.

## Loading From Base64 Data

Use `read_ifc_base64(data, create_collision, collision_classes)` when data comes from APIs, storage, or networking.

```gdscript
var payload: String = fetch_ifc_payload_somehow()
var err := ifc_manager.read_ifc_base64(payload, false, [])
if err != OK:
    push_error("read_ifc_base64 failed with code %s" % err)
```

## Collision Strategy

For large models, generate collision only for classes that matter to gameplay.

```gdscript
var classes := ["IfcWall", "IfcSlab", "IfcColumn"]
ifc_manager.read_ifc("res://models/site.ifc", true, classes)
```

This reduces memory pressure and load time compared to colliders for every object.

## Accessing Generated Nodes

After load completion, `IFCNode` children are created under the manager.

```gdscript
for child in ifc_manager.get_children():
    if child is IFCNode:
        print(child.name, " class=", child.ifc_class)
```

Each `IFCNode` provides:

- `ifc_class`: IFC class name
- `attributes`: base IFC attributes
- `properties`: property sets
- `quantities`: quantity sets

## Geometry Settings

Assign a `GDIFCLoaderSettings` resource to `ifc_manager.geometric_settings` before loading.

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.circle_segments = 24
settings.memory_limit = 3221225472
ifc_manager.geometric_settings = settings
```

Tune these values for model scale and target hardware.

## Loading Relative To A Georeference

When several models of one project are loaded into the same scene (architecture, structure, MEP...), each one is generated in its own local frame. To line a new model up with one that is already loaded, point `reference_georeference` at the existing model's `IFCGeoreference` node before calling `read_ifc`. The path is relative to the manager, so add the manager to the tree first.

```gdscript
var reference := $ArchManager/IFCModel/Georeference
var ifc_manager := GDIFCManager.new()
add_child(ifc_manager)
ifc_manager.reference_georeference = ifc_manager.get_path_to(reference)
ifc_manager.read_ifc("res://models/structure.ifc")
```

When loading finishes, the new `IFCModel` gets a transform that maps its map conversion (eastings, northings, height, rotation and scale) onto the reference's map conversion. It also accounts for the `coordinate_to_origin` offset of both models. The math is done in double precision, so large map coordinates don't lose accuracy. Moving the reference model before loading moves the result with it.

If the new model has no georeferencing data (IFC2x3, or IFC4 without `IfcMapConversion`), it is placed assuming it shares the reference model's engineering coordinate system, and a warning is printed.

In the editor dock, pick the reference in **Config → Reference georeference**. The list shows every `IFCGeoreference` in the edited scene and refreshes each time it opens.

The `coordinate_to_origin` offset is stored on each `IFCGeoreference` node (`coordination_offset_x/y/z`). Models loaded with an older GDIFC version don't have it. If such a model was loaded with `coordinate_to_origin` enabled, reload it before using it as a reference.
