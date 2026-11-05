# API Reference

This page lists the GDIFC runtime API.

## GDIFCManager (Node3D)

Main entry point for IFC loading and model access.

### Methods

- `read_ifc(path: String, create_collision := false, collision_classes := []) -> Error`
  - Loads an IFC from disk. `path` can be `res://` or absolute.
  - With `create_collision`, `collision_classes` lists the exact IFC class names to create collision for, or `[]` for every element. Ignored when `collision_matrix` is set. See [Collision Strategy](loading-models.md#collision-strategy).
- `read_ifc_base64(base64_data: String, create_collision := false, collision_classes := []) -> Error`
  - Loads an IFC from a base64 string. Same collision parameters as `read_ifc`.
- `get_ifc_model() -> IFCModel`
  - The `IFCModel` root node of the loaded model, or null before the first load. Its queries work after `ifc_objects_ready`.
- `get_node_by_global_id(global_id: String) -> IFCNode`
  - The node of the element with that GlobalId, or null. Available after `ifc_objects_ready`.
- `get_ifc_object_by_global_id(global_id: String) -> GDIFCEntityBase`
  - The typed entity with that GlobalId, whether or not it has a node. Available after `ifc_objects_ready`.
- `get_elements_by_class(ifc_class: String) -> Array`
  - Every `IFCNode` whose `ifc_class` is exactly `ifc_class`, such as `"IfcWall"`. Subtypes are not included. Nodes are found through a scene group named after their class, so the result includes the nodes of every model in the scene tree, not only this manager's.
- `get_load_progress() -> float`
  - Progress of the current or last load, from `0.0` to `1.0`. It never decreases during a load.
- `get_load_stage() -> String`
  - Current load stage: `"Parsing"`, `"Processing geometry"`, `"Generating LODs"` (when enabled), `"Creating nodes"`, `"Preparing editor"` (editor only), `"Adding to scene"`, `"Done"` or `"Failed"`. Empty before the first load. Progress reaches `1.0` at `ifc_read`; property sets load after that. The text is for display and may be reworded; compare `get_load_stage_id()` instead.
- `get_load_stage_id() -> LoadStage`
  - The same stage as a constant: `STAGE_IDLE`, `STAGE_PARSING`, `STAGE_GEOMETRY`, `STAGE_LODS`, `STAGE_NODES`, `STAGE_EDITOR_CACHES`, `STAGE_REVEAL`, `STAGE_DONE` or `STAGE_FAILED`, in load order. New stages may be added at the end.
- `get_load_error() -> String`
  - Why the last load failed: the file couldn't be opened, was empty, or the geometry engine couldn't read it. Empty while loading and after a load that succeeded. Read it after `ifc_load_failed`, or after a load method returns an error.
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
  - The file couldn't be read by the geometry engine. `get_load_error()` says why, and the Output panel has the details. The manager can load again afterwards.

```gdscript
ifc_manager.load_progress_changed.connect(func(progress: float, stage: String):
    $ProgressBar.value = progress
    $StatusLabel.text = stage
    $Spinner.visible = ifc_manager.get_load_stage_id() == GDIFCManager.STAGE_GEOMETRY)
ifc_manager.ifc_load_failed.connect(func(): $StatusLabel.text = ifc_manager.get_load_error())
ifc_manager.read_ifc("res://models/building.ifc")
```

### Properties

- `geometric_settings: GDIFCLoaderSettings`
  - Assign before calling load methods.
- `collision_matrix: GDIFCCollisionMatrix`
  - Shape and physics layers of the collision per IFC class. When set, `read_ifc`'s `create_collision` and `collision_classes` are ignored. See [Collision Matrix](loading-models.md#collision-matrix).
- `reference_georeference: NodePath`
  - An `IFCGeoreference` to place the model relative to, as a path from the manager. Set it before loading. See [Loading Relative To A Georeference](loading-models.md#loading-relative-to-a-georeference).
- `material_library: GDIFCMaterialLibrary`
  - The model's IFC material definitions, after `ifc_objects_ready`. Null unless `ifc_materials` was on.
- `ifc_file_path: String`
  - The file the model was loaded from. Set by `read_ifc` and cleared by `read_ifc_base64`. It is saved with the scene, and the file is parsed again from it to relink `ifc_object` when the scene is reopened.

## IFCModel (Node3D)

The root node of a loaded model, under the manager, and the parent of its `IFCNode` tree and its `Georeference` node. Get it with `GDIFCManager.get_ifc_model()`. Its methods work after `ifc_objects_ready`.

### Header

From the file's `FILE_DESCRIPTION`, `FILE_NAME` and `FILE_SCHEMA`. A field written as `$` returns an empty value.

- `get_schema_name() -> String`
- `get_file_description() -> PackedStringArray`
- `get_file_name() -> String`
- `get_timestamp() -> String`
- `get_author() -> PackedStringArray`
- `get_organization() -> PackedStringArray`
- `get_preprocessor() -> String`
- `get_originating_system() -> String`
- `get_authorization() -> String`

### Units

- `get_units() -> Dictionary`
  - The project's units, from its `IfcUnitAssignment`: `{unit type: symbol}`, such as `{"LENGTHUNIT": "mm", "AREAUNIT": "m²", "MONETARYUNIT": "EUR"}`.
- `get_site_location() -> Dictionary`
  - The first `IfcSite`'s `latitude` and `longitude`, in decimal degrees, and `elevation`, in the file's length unit, each when the site gives it. Only informational: models are placed by their `IFCGeoreference`.

### Queries

- `get_by_id(express_id: int) -> GDIFCEntityBase`
- `get_by_global_id(global_id: String) -> GDIFCEntityBase`
- `instances_by_type(ifc_class: String) -> Array`
  - Every entity of that class, subtypes included.
- `instances_by_type_exact(ifc_class: String) -> Array`
  - Every entity of exactly that class.
- `get_project() -> GDIFCEntityBase`
  - The `IfcProject`.

### Editing

- `create(ifc_class: String) -> GDIFCEntityBase`
  - Adds a new entity of that class to the file and returns its typed wrapper. Its attributes start unset.
- `save(path: String) -> Error`
  - Writes the file, including entities created or changed from scripts, as STEP.
- `to_step_string() -> String`
  - The same STEP text, as a string.

```gdscript
await ifc_manager.ifc_objects_ready
var model := ifc_manager.get_ifc_model()
print(model.get_schema_name(), " from ", model.get_originating_system())
print(model.instances_by_type("IfcWall").size(), " walls")
```

## IFCNode (MeshInstance3D)

Node generated for each IFC object.

### Properties

- `ifc_class: String`
- `global_id: String`
- `attributes: Dictionary`: the entity's attribute values, such as `Name` or `Tag`. Attributes that refer to other entities, such as `ObjectPlacement`, are left out: read them from `ifc_object` (after `ifc_objects_ready`)
- `properties: Dictionary`: property sets, `{set name: {property: value}}`, including the sets of the element's type (after `ifc_objects_ready`)
- `quantities: Dictionary`: quantity sets, shaped like `properties` (after `ifc_objects_ready`)
- `property_details: Dictionary`, `quantity_details: Dictionary`: shaped like `properties` and `quantities`, with a read-only `Dictionary` per value holding its `value_type`, `unit` and `from_type` (after `ifc_objects_ready`). See [Property And Quantity Sets](loading-models.md#property-and-quantity-sets).
- `ifc_object: GDIFCEntityBase`: the IFC entity, to [walk the model](walking-the-model.md) from (after `ifc_objects_ready`; relinked on load, not saved)
- `material_definition: GDIFCMaterialDefinition` (after `ifc_objects_ready`, with `ifc_materials` on)

## IFCGeoreference (Node3D)

Created as `IFCModel/Georeference` at `ifc_objects_ready`, for IFC4 and later files that have an `IfcMapConversion` and `IfcProjectedCRS`, and IFC2X3 files that have the `ePSet_MapConversion` property set. Point another manager's `reference_georeference` at it to load a model in place.

### Properties

- `eastings: float`, `northings: float`, `orthogonal_height: float`: the map coordinates of the project origin
- `x_axis_abscissa: float`, `x_axis_ordinate: float`: the rotation of the project's X axis on the map
- `map_scale: float`
- `map_scale_x: float`, `map_scale_y: float`, `map_scale_z: float`: the per-axis factors of an IFC4X3 `IfcMapConversionScaled`, along the IFC x, y and z axes, applied on top of `map_scale`; 1 otherwise
- `crs_name: String`, `crs_description: String`, `geodetic_datum: String`, `vertical_datum: String`: from `IfcProjectedCRS`, or IFC2X3's `ePSet_ProjectedCRS`
- `coordination_offset_x: float`, `coordination_offset_y: float`, `coordination_offset_z: float`: the translation applied to the model by `coordinate_to_origin`, in Godot axes; zero without it

## GDIFCCollisionMatrix (Resource)

- `rules: Array[GDIFCCollisionRule]`
- `default_rule: GDIFCCollisionRule`
  - For classes no rule matches. Null gives them no collision.
- `get_rule_for_class(ifc_class: String, schema := "IFC4") -> GDIFCCollisionRule`
  - The rule an element of that class gets in a file of that schema (`IFC2X3`, `IFC4` or `IFC4X3_ADD2`, also accepted as `IFC4X3`), or null for no collision.

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

## IFC Material Definitions (Resource)

Read with `ifc_materials` on. One resource per IFC material definition, shared by every element and set that uses it. See [IFC Materials](loading-models.md#ifc-materials).

`GDIFCMaterialDefinition` is the base class:

- `resource_name`: the IFC Name (`LayerSetName` for a layer set; a usage takes its set's name)
- `description: String`, `category: String`
- `express_id: int`: IFC materials have no GlobalId
- `properties: Dictionary`: material property sets, `{set name: {property: value}}`
- `property_details: Dictionary`: shaped like `properties`, with the `value_type` and `unit` of each value, like `IFCNode.property_details`
- `classifications: Array`: classification and library references, one Dictionary each (`type`, and whichever of `identification`, `item_reference`, `name`, `location`, `description`, `notation`, `source` the reference has)
- `ifc_object: GDIFCEntityBase`: relinked on load, not saved

| Class | IFC entity | Properties |
| --- | --- | --- |
| `GDIFCMaterialSingle` | `IfcMaterial` | `visual: Material` (assigning it replaces it on every surface showing the old one), `relationships: Array` (IFC4 `IfcMaterialRelationship`: `name`, `description`, `expression`, `materials`) |
| `GDIFCMaterialLayer` | `IfcMaterialLayer` | `material` (null for an air gap), `thickness`, `is_ventilated`, `priority` (-1 when not given) |
| `GDIFCMaterialLayerSet` | `IfcMaterialLayerSet` | `layers` (in IFC order), `total_thickness` (read-only) |
| `GDIFCMaterialLayerSetUsage` | `IfcMaterialLayerSetUsage` | `layer_set`, `direction` (`AXIS1`..`AXIS3`), `sense` (`POSITIVE`, `NEGATIVE`), `offset`, `reference_extent` (0 when not given) |
| `GDIFCMaterialProfile` | `IfcMaterialProfile` | `material`, `priority` (-1 when not given) |
| `GDIFCMaterialProfileSet` | `IfcMaterialProfileSet` | `profiles` |
| `GDIFCMaterialProfileSetUsage` | `IfcMaterialProfileSetUsage` | `profile_set`, `cardinal_point` (0 when not given), `reference_extent` |
| `GDIFCMaterialConstituent` | `IfcMaterialConstituent` | `material`, `fraction` (-1 when not given) |
| `GDIFCMaterialConstituentSet` | `IfcMaterialConstituentSet` | `constituents` |
| `GDIFCMaterialList` | `IfcMaterialList` | `materials` |

Lengths are in the IFC file's length unit, like quantities. Subtypes such as `IfcMaterialLayerWithOffsets` use their supertype's class; their extra attributes are on `ifc_object`.

`GDIFCMaterialLibrary` holds `definitions` (those elements are associated with) and `materials` (every `GDIFCMaterialSingle`, including those inside sets), both by express id.

## GDIFCEntityBase (RefCounted)

Base wrapper for all IFC entities. Every IFC class, such as `IfcWall`, is a subclass with its attributes as properties: references to entities are `GDIFCEntityBase`, and every other attribute is a `Variant` that is null when unset. See [Typed Entity Access](typed-entities.md), and [Walking The Model](walking-the-model.md) to follow relationships with `get_attribute` and `get_inverse`.

### Common Methods

- `get_type() -> String`
  - The entity's IFC class, such as `"IfcWall"`.
- `get_express_id() -> int`
  - Its `#id` in the file.
- `is_a(ifc_class: String) -> bool`
  - Whether it is of that class or a subtype of it. The class name is case-sensitive: `"IfcWall"`, not `"IFCWALL"`.
- `get_attribute(attr_name: String) -> Variant`
  - The attribute's value, null when unset. A property value such as an `IfcLabel` or `IfcLengthMeasure` reads as its plain value.
- `get_attribute_names() -> PackedStringArray`
- `get_all_attributes() -> Dictionary`
- `get_inverse(inverse_attr_name: String) -> Array`
  - The entities that refer to this one through that inverse attribute, such as `"IsDefinedBy"` or `"ContainedInStructure"`.

Use these when your code must work across many IFC classes dynamically.

## GDIFCLoaderSettings (Resource)

Controls geometry conversion behavior and processing limits.

### Properties

- `coordinate_to_origin: bool` (default `false`)
- `circle_segments: int` (default `12`)
- `generate_lods: bool` (default `true`)
- `lod_normal_merge_angle: float` (default `60.0`, degrees)
- `ifc_materials: bool` (default `false`): read IFC material definitions, and use one material per IFC material instead of per colour. See [IFC Materials](loading-models.md#ifc-materials).
- `ifc_textures: bool` (default `false`): load the textures of IFC surface styles onto the generated materials. Used only with `ifc_materials`. See [Surface Styles And Textures](loading-models.md#surface-styles-and-textures).

### Advanced (web-ifc)

Passed unchanged to the web-ifc geometry engine, so they follow its releases: a web-ifc update may change what they do, or remove them. They are grouped under **Advanced (web-ifc)** in the Inspector.

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

## Practical Pattern

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.circle_segments = 16
settings.coordinate_to_origin = true
ifc_manager.geometric_settings = settings

var err := ifc_manager.read_ifc("res://models/project.ifc")
if err != OK:
    push_error("IFC load failed")
```
