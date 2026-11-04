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
  - Current load stage: `"Parsing"`, `"Processing geometry"`, `"Generating LODs"` (when enabled), `"Creating nodes"`, `"Preparing editor"` (editor only), `"Adding to scene"`, `"Done"` or `"Failed"`. Empty before the first load. Progress reaches `1.0` at `ifc_read`; property sets load after that.
- `find_material_definitions(name: String) -> Array`
  - Every material definition in `material_library` with that IFC Name, whatever its class.
- `set_material_visual(material_name: String, visual: Material) -> int`
  - Sets `visual` on every `GDIFCMaterialSingle` with that IFC Name, and returns how many there were.

### Signals

- `load_progress_changed(progress: float, stage: String)`
  - Emitted while loading, when the stage changes or progress moves by at least 0.5%. `progress` and `stage` match the two getters above.
  - The stages are weighted by their typical share of the load time, so the bar moves roughly in step with time. The parsing stage is estimated from the file size.
- `ifc_read`
  - The model's nodes are in the scene, with their geometry, `ifc_class` and `global_id`. Attributes, property sets, quantities and `ifc_object` are not filled yet.
- `ifc_objects_ready`
  - Attributes, property sets, quantities and typed `ifc_object` references are on the nodes, and the `Georeference` node and placement relative to `reference_georeference` are applied. Emitted after `ifc_read`, since this data is read by a second parser in the background. Also emitted after a saved scene is reopened and relinked.
  - If that parser can't read the file (for example an unsupported schema such as IFC4X3_RC2), it is still emitted, with an error in the Output panel; the model keeps its geometry without properties.
- `ifc_load_failed`
  - The file couldn't be read by the geometry engine. Details are in the Output panel.

```gdscript
ifc_manager.load_progress_changed.connect(func(progress: float, stage: String):
    $ProgressBar.value = progress
    $StatusLabel.text = stage)
ifc_manager.read_ifc("res://models/building.ifc", false, [])
```

### Properties

- `geometric_settings: GDIFCLoaderSettings`
  - Assign before calling load methods.
- `material_library: GDIFCMaterialLibrary`
  - The model's IFC material definitions, after `ifc_objects_ready`. Null unless `ifc_materials` was on.
- `collision_matrix: GDIFCCollisionMatrix`
  - Shape and physics layers of the collision per IFC class. When set, `read_ifc`'s `create_collision` and `collision_classes` are ignored. See [Collision Matrix](loading-models.md#collision-matrix).

## GDIFCCollisionMatrix (Resource)

- `rules: Array[GDIFCCollisionRule]`
- `default_rule: GDIFCCollisionRule`
  - For classes no rule matches. Null gives them no collision.
- `get_rule_for_class(ifc_class: String, schema := "IFC4") -> GDIFCCollisionRule`
  - The rule an element of that class gets in a file of that schema (`IFC2X3`, `IFC4` or `IFC4X3_ADD2`), or null for no collision.

## GDIFCCollisionRule (Resource)

- `ifc_class: String`
  - Also set as the resource name, so the Inspector's rule list shows it.
- `include_subtypes: bool` (default `true`)
- `shape_type: ShapeType` (default `SHAPE_TRIMESH`)
  - `SHAPE_NONE`, `SHAPE_TRIMESH`, `SHAPE_SIMPLE_CONVEX`, `SHAPE_BOX` or `SHAPE_DECOMPOSE_CONVEX`.
- `collision_layer: int` (default `1`)
  - Layers of the `StaticBody3D`.
- `decomposition_settings: MeshConvexDecompositionSettings`
  - Decompose Convex only.

## IFCNode (MeshInstance3D)

Node generated for each IFC object.

### Properties

- `ifc_class: String`
- `attributes: Dictionary`
- `properties: Dictionary`
- `quantities: Dictionary`
- `ifc_object: GDIFCEntityBase`
- `material_definition: GDIFCMaterialDefinition` (after `ifc_objects_ready`, with `ifc_materials` on)

## IFC Material Definitions (Resource)

Read with `ifc_materials` on. One resource per IFC material definition, shared by every element and set that uses it. See [IFC Materials](loading-models.md#ifc-materials).

`GDIFCMaterialDefinition` is the base class:

- `resource_name`: the IFC Name (`LayerSetName` for a layer set; a usage takes its set's name)
- `description: String`, `category: String`
- `express_id: int`: IFC materials have no GlobalId
- `properties: Dictionary`: material property sets, `{set name: {property: value}}`
- `classifications: Array`: classification and library references, one Dictionary each (`type`, and whichever of `identification`, `item_reference`, `name`, `location`, `description`, `notation`, `source` the reference has)
- `ifc_object: GDIFCEntityBase`: relinked on load, not saved

| Class | IFC entity | Properties |
| --- | --- | --- |
| `GDIFCMaterialSingle` | `IfcMaterial` | `visual: Material` (assigning it replaces it on every surface showing the old one), `relationships: Array` (IFC4 `IfcMaterialRelationship`: `name`, `description`, `expression`, `materials`) |
| `GDIFCMaterialLayer` | `IfcMaterialLayer` | `material` (null for an air gap), `thickness`, `is_ventilated`, `priority` (-1 when not given) |
| `GDIFCMaterialLayerSet` | `IfcMaterialLayerSet` | `layers` (in IFC order), `total_thickness` (read-only) |
| `GDIFCMaterialLayerSetUsage` | `IfcMaterialLayerSetUsage` | `layer_set`, `direction` (`AXIS1`..`AXIS3`), `sense` (`POSITIVE`, `NEGATIVE`), `offset`, `reference_extent` (0 when not given) |
| `GDIFCMaterialProfile` | `IfcMaterialProfile` | `material`, `priority` |
| `GDIFCMaterialProfileSet` | `IfcMaterialProfileSet` | `profiles` |
| `GDIFCMaterialProfileSetUsage` | `IfcMaterialProfileSetUsage` | `profile_set`, `cardinal_point` (0 when not given), `reference_extent` |
| `GDIFCMaterialConstituent` | `IfcMaterialConstituent` | `material`, `fraction` (-1 when not given) |
| `GDIFCMaterialConstituentSet` | `IfcMaterialConstituentSet` | `constituents` |
| `GDIFCMaterialList` | `IfcMaterialList` | `materials` |

Lengths are in the IFC file's length unit, like quantities. Subtypes such as `IfcMaterialLayerWithOffsets` use their supertype's class; their extra attributes are on `ifc_object`.

`GDIFCMaterialLibrary` holds `definitions` (those elements are associated with) and `materials` (every `GDIFCMaterialSingle`, including those inside sets), both by express id.

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
- `tolerance_plane_intersection: float` (default `1.0e-4`)
- `tolerance_plane_deviation: float` (default `1.0e-4`)
- `tolerance_back_deviation_distance: float` (default `1.0e-4`)
- `tolerance_inside_outside_perimeter: float` (default `1.0e-10`)
- `tolerance_scalar_equality: float` (default `1.0e-4`)
- `plane_refit_iterations: int` (default `1`)
- `boolean_union_threshold: int` (default `150`)
- `generate_lods: bool` (default `true`)
- `lod_normal_merge_angle: float` (default `60.0`, degrees)
- `ifc_materials: bool` (default `false`): read IFC material definitions, and use one material per IFC material instead of per colour. See [IFC Materials](loading-models.md#ifc-materials).
- `ifc_textures: bool` (default `false`): load the textures of IFC surface styles onto the generated materials. Used only with `ifc_materials`. See [Surface Styles And Textures](loading-models.md#surface-styles-and-textures).

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
