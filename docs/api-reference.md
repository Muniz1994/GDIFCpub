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
- `get_load_progress() -> float`
  - Progress of the current or last load, from `0.0` to `1.0`. It never decreases during a load.
- `get_load_stage() -> String`
  - Current load stage: `"Parsing"`, `"Processing geometry"`, `"Creating nodes"`, `"Preparing editor"` (editor only), `"Adding to scene"`, `"Done"` or `"Failed"`. Empty before the first load.

### Signals

- `load_progress_changed(progress: float, stage: String)`
  - Emitted while loading, when the stage changes or progress moves by at least 0.5%. `progress` and `stage` match the two getters above.
  - The stages are weighted by their typical share of the load time, so the bar moves roughly in step with time. The parsing stage is estimated from the file size.
- `ifc_read`
  - The model's nodes are in the scene.
- `ifc_objects_ready`
  - Typed IFC objects are available on the nodes. Also emitted after a saved scene is reopened and relinked.
- `ifc_load_failed`
  - The file couldn't be parsed or its schema isn't supported. Details are in the Output panel.

```gdscript
ifc_manager.load_progress_changed.connect(func(progress: float, stage: String):
    $ProgressBar.value = progress
    $StatusLabel.text = stage)
ifc_manager.read_ifc("res://models/building.ifc", false, [])
```

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
- `generate_lods: bool` (default `true`)
- `lod_normal_merge_angle: float` (default `60.0`, degrees)

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
