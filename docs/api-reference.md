# API Reference

This page centralizes the most-used GDIFC runtime API.

## GDIFCManager (Node3D)

Main entry point for IFC loading and model access.

### Methods

- `read_ifc(path, create_collision, collision_classes) -> Error`
  - Loads an IFC from disk.
  - `path` can be `res://` or absolute.
- `read_ifc_base64(data, create_collision, collision_classes) -> Error`
  - Loads an IFC from a base64 string.

### Properties

- `geometric_settings: GDIFCLoaderSettings`
  - Assign before calling load methods.

## IFCNode (MeshInstance3D)

Node generated for each IFC object.

### Properties

- `ifc_class: String`
- `attributes: Dictionary`
- `properties: Dictionary`
- `quantities: Dictionary`
- `ifc_object: GDIFCEntityBase`

## GDIFCEntityBase (RefCounted)

Base wrapper for all IFC entities.

### Common Methods

- `get_type() -> String`
- `get_attribute(name: String) -> Variant`
- `get_all_attributes() -> Dictionary`

Use this when your code must work across many IFC classes dynamically.

## GDIFCLoaderSettings (Resource)

Controls geometry conversion behavior and processing limits.

### Selected Properties

- `coordinate_to_origin: bool` (default `false`)
- `circle_segments: int` (default `12`)
- `tape_size: int` (default `67108864`)
- `memory_limit: int` (default `2147483648`)
- `line_writer_buffer: int` (default `10000`)
- `tolerance_plane_intersection: float` (default `1.0e-1`)
- `tolerance_plane_deviation: float` (default `3.0e-4`)
- `tolerance_back_deviation_distance: float` (default `3.0e-4`)
- `tolerance_inside_outside_perimeter: float` (default `1.0e-10`)
- `tolerance_scalar_equality: float` (default `1.0e-4`)
- `plane_refit_iterations: int` (default `10`)
- `boolean_union_threshold: int` (default `150`)

## Practical Pattern

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.circle_segments = 16
settings.coordinate_to_origin = true
ifc_manager.geometric_settings = settings

var err := ifc_manager.read_ifc("res://models/project.ifc", false, [])
if err != OK:
    push_error("IFC load failed")
```
