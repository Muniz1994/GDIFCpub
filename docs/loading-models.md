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
