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
- `create_collision`: Set `true` to create collision shapes. Optional, `false` by default.
- `collision_classes`: exact IFC class names to create collision for. Subtypes are not included, so list `IfcWallStandardCase` along with `IfcWall` if you want both.

If `collision_classes` is empty and `create_collision` is true, every element with geometry gets a collision shape.

Both collision parameters are optional, so `read_ifc("res://models/bridge.ifc")` loads without collision.

## Importing `.ifc` Files As Scenes

`.ifc` files inside the project are imported like `.glb` files: drag one from the FileSystem dock into the 3D view, or instance it in a scene. The model is built once per import and stored in binary under `.godot/imported/`, so the scene that uses it only stores a reference to the `.ifc` file and stays small and quick to save. To change individual nodes, enable **Editable Children** on the instance.

From the GDIFC dock, turn on **Scene → Import as scene** before clicking **Load IFC...**: the file is imported with the dock's options and instanced in the edited scene. Files from outside the project are first copied into the **Import folder** (`res://ifc/` by default); files already in the project are imported where they are. Of the settings preset, only coordinate to origin and circle segments apply to imports.

Select the `.ifc` file and open the **Import** dock to set its options (coordinate to origin, circle segments, LODs, IFC materials and textures, IFC objects, collision, and the classes or collision matrix to create it with), then click **Reimport**. The model is reimported automatically when the `.ifc` file changes.

The root of an imported scene is a `GDIFCManager`. When an instance enters the tree, it re-parses the `.ifc` file to restore `ifc_object` and the `IFCModel` queries, then emits `ifc_objects_ready`. With the `data/ifc_objects` import option off, it skips that and emits `ifc_objects_ready` on the next frame (see [Without IFC Objects](#without-ifc-objects)). Set its `reference_georeference` to place the model relative to another model's `IFCGeoreference`. In exported projects, add `*.ifc` to the export preset's **Filters to export non-resource files/folders** so the file is available at runtime.

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

Each element gets a `StaticBody3D` child named `<element>_col` with a trimesh (`ConcavePolygonShape3D`) on collision layer 1.

### Collision Matrix

For a different shape or physics layer per class, assign a `GDIFCCollisionMatrix` to `collision_matrix` before loading. It holds one `GDIFCCollisionRule` per class, and replaces `create_collision` and `collision_classes`:

```gdscript
func rule(ifc_class: String, shape: GDIFCCollisionRule.ShapeType, layer: int) -> GDIFCCollisionRule:
    var r := GDIFCCollisionRule.new()
    r.ifc_class = ifc_class
    r.shape_type = shape
    r.collision_layer = layer
    return r

func load_building() -> void:
    var matrix := GDIFCCollisionMatrix.new()
    matrix.rules = [
        rule("IfcWall", GDIFCCollisionRule.SHAPE_TRIMESH, 1), # openings stay open
        rule("IfcSlab", GDIFCCollisionRule.SHAPE_BOX, 1),
        rule("IfcStairFlight", GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX, 1), # a ramp for character controllers
        rule("IfcDoor", GDIFCCollisionRule.SHAPE_NONE, 1),
        rule("IfcSpace", GDIFCCollisionRule.SHAPE_NONE, 1),
    ]
    matrix.default_rule = rule("", GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX, 2) # everything else
    ifc_manager.collision_matrix = matrix
    ifc_manager.read_ifc("res://models/building.ifc")
```

A matrix is a resource, so it is usually made in the Inspector and saved as a `.tres` file, which can be shared between models and projects. `collision_layer` uses the layer names of **Project Settings → Layer Names → 3D Physics**.

Shape types:

| Shape | Godot shape | Notes |
|---|---|---|
| `SHAPE_NONE` | — | No collision. Overrides a more general rule or the default rule. |
| `SHAPE_TRIMESH` | `ConcavePolygonShape3D` | Exact, the most expensive. Static bodies only. |
| `SHAPE_SIMPLE_CONVEX` | `ConvexPolygonShape3D` | The convex hull of the element. Fills openings and concave parts. |
| `SHAPE_BOX` | `BoxShape3D` | The bounding box in the element's own axes, the cheapest. Fills door and window openings, so not for walls you walk through. |
| `SHAPE_DECOMPOSE_CONVEX` | Several `ConvexPolygonShape3D` | Convex decomposition (V-HACD). Only built when the `.ifc` file is imported as a scene: other loads build a Simple Convex shape and print a warning. Tune it with `decomposition_settings`; empty uses up to 16 hulls and a maximum concavity of 0.01. |

Matching:

- `ifc_class` is case-insensitive. With `include_subtypes` (the default), a rule also matches subtypes in the file's schema, so `IfcWall` covers `IfcWallStandardCase`, and `IfcBuildingElement` covers every building element. `IfcBuildingElement` and its IFC4X3 name `IfcBuiltElement` are interchangeable.
- When several rules match, the one closest to the element's class wins, so an `IfcWall` rule beats an `IfcBuildingElement` rule for walls, whatever their order. On a tie, the first rule wins.
- Classes no rule matches get `default_rule`, whose `ifc_class` is ignored, or no collision without one.
- `matrix.get_rule_for_class("IfcWallStandardCase", "IFC2X3")` returns the rule an element gets, or null, to check a matrix from a script.

#### Presets

The addon ships three matrices in `res://addons/GDIFC/settings/collision/`. Use them as they are, or duplicate one (**Make Unique** in the picker, then **Save As**) as a starting point; edits to the originals are lost when the addon is updated.

| Preset | For | Rules |
|---|---|---|
| `walkthrough.tres` | First-person navigation | Trimesh for walls, slabs, roofs, coverings, ramps, railings, proxies and terrain, so door, stair and shaft openings stay open. Simple Convex for stairs, which become ramps. Box for columns, beams, footings and windows. No collision for doors, spaces, virtual elements and annotations. Everything else (furniture, equipment, MEP, members, plates) gets Simple Convex. All on layer 1. |
| `structure_only.tres` | Large models, where only the building has to be solid | The walkthrough rules for the building itself, and nothing else: no windows, doors, furniture, equipment or MEP. |
| `picking.tres` | Selecting elements with a raycast | Every element gets a trimesh on layer 2 only, except spaces, virtual elements and annotations. Cast rays with a mask of layer 2: the collider is the `<element>_col` body, and its parent is the `IFCNode`. Bodies on layer 1 don't collide with them. |

In the editor dock, pick a matrix in **Collisions → Matrix**. The element list is greyed out while one is set. To import a model as a scene with a matrix, save the matrix to a `.tres` file first. The importer's `collision/matrix` option points to it, and saving the matrix reimports every `.ifc` file using it.

## Accessing Generated Nodes

Loading happens in two steps, so the model appears as soon as its geometry is ready:

1. `ifc_read`: the `IFCNode` tree is in the scene under `ifc_manager/IFCModel`, with geometry, `ifc_class` and `global_id`.
2. `ifc_objects_ready`: `attributes`, `properties`, `quantities` and `ifc_object` are filled in, and georeferencing is applied.

Wait for `ifc_objects_ready` before reading property data:

```gdscript
ifc_manager.ifc_objects_ready.connect(func():
    for node in ifc_manager.find_children("*", "IFCNode", true, false):
        print(node.name, " class=", node.ifc_class, " psets=", node.properties.keys()))
ifc_manager.read_ifc("res://models/building.ifc", false, [])
```

Each `IFCNode` provides:

- `ifc_class`: IFC class name
- `global_id`: IFC GlobalId
- `attributes`: the IFC attribute values, without references to other entities, which are on `ifc_object` (after `ifc_objects_ready`)
- `properties`: property sets, including its type's (after `ifc_objects_ready`)
- `quantities`: quantity sets, including its type's (after `ifc_objects_ready`)
- `property_details` and `quantity_details`: the value type, unit and origin of each value (after `ifc_objects_ready`)
- `ifc_object`: the IFC entity, to [walk the rest of the model](walking-the-model.md) from (after `ifc_objects_ready`, unless `ifc_objects` is off)

### Without IFC Objects

After the data above is copied onto the nodes, GDIFC keeps the parsed IFC file in memory, so that `ifc_object` and the `IFCModel` queries can reach the rest of the model. Set `ifc_objects = false` when the dictionaries on the nodes are enough:

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.ifc_objects = false
ifc_manager.geometric_settings = settings
```

Attributes, property and quantity sets, IFC materials and their properties, and georeferencing are still read. Then the file is released, which saves memory. In exchange:

- `ifc_object` is null on nodes and material definitions, and `get_ifc_object_by_global_id()` returns null.
- The `IFCModel` queries find nothing (`get_by_id`, `instances_by_type`, `get_project`, `get_units`, `get_site_location`), and the model can't be edited or saved.
- A saved scene, or an instance of an imported `.ifc` scene, doesn't read the `.ifc` file again when it enters the tree. It emits `ifc_objects_ready` on the next frame, so instancing a model costs no parsing at all.

Loading takes as long as before: the file is still parsed for its data. In the editor dock, the option is **Data → IFC objects**, which starts from the selected settings preset; the importer's option is `data/ifc_objects`.

### Property And Quantity Sets

`properties` holds `{set name: {property name: value}}` and `quantities` `{set name: {quantity name: value}}`. Both include the sets of the element's type (`IfcTypeObject.HasPropertySets`). When the element and its type both set a property, the element's value is kept. A set without a name is keyed by its class, such as `"IfcElementQuantity"`.

The value depends on the kind of property:

| IFC property | Value |
|---|---|
| `IfcPropertySingleValue` | The value |
| `IfcPropertyBoundedValue` | `[lower, upper]` |
| `IfcPropertyEnumeratedValue`, `IfcPropertyListValue` | An `Array` of values |
| `IfcPropertyReferenceValue` | Its `UsageName` |
| `IfcComplexProperty` | Its properties, nested: `{property name: value}` |
| `IfcPropertyTableValue` | `{"defining_values": Array, "defined_values": Array}` |
| Simple quantities (length, area, volume, count, weight, time, number) | The value, in the unit given by its details |
| `IfcPhysicalComplexQuantity` | Its quantities, nested |

`property_details` and `quantity_details` have the same shape, with a `Dictionary` of details for each value. Only the keys that apply are present:

| Key | Meaning |
|---|---|
| `value_type` | The IFC type of the value, such as `"IfcLengthMeasure"`, `"IfcLabel"` or `"IfcBoolean"`; for a reference, the class of the referenced entity |
| `unit` | Its unit, such as `"mm"`, `"m²"` or `"W/(m²·K)"`: the property's own unit, or else the project's unit for the value type |
| `from_type` | `true` when the value comes from the element's type |
| `reference` | For a reference, the express id of the referenced entity (see `IFCModel.get_by_id`) |
| `usage_name`, `properties` | For a complex property: its `UsageName`, and the details of its properties |
| `defining_value_type`, `defining_unit`, `defined_value_type`, `defined_unit`, `expression`, `curve_interpolation` | For a table |
| `discrimination`, `usage`, `quantities` | For a complex quantity: its `Discrimination`, its `Usage`, and the details of its quantities |

```gdscript
var area = node.quantities["Qto_WallBaseQuantities"]["NetSideArea"]
var detail: Dictionary = node.quantity_details["Qto_WallBaseQuantities"]["NetSideArea"]
print(area, " ", detail.get("unit", "")) # 12.5 m²

for set_name in node.property_details:
    for name in node.property_details[set_name]:
        if node.property_details[set_name][name].get("from_type", false):
            print(set_name, ".", name, " comes from the type")
```

The details are read-only and shared between the values that have the same ones. Units are given as symbols for the common SI and imperial units, and by their IFC name otherwise. Values are never converted: they are in the units the details give.

The type's sets and the details are saved with the scene, so they make scenes that store their nodes larger. A scene that instances an [imported `.ifc` file](#importing-ifc-files-as-scenes) is not affected.

## Geometry Settings

Assign a `GDIFCLoaderSettings` resource to `ifc_manager.geometric_settings` before loading.

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.circle_segments = 24
settings.memory_limit = 3221225472
ifc_manager.geometric_settings = settings
```

Tune these values for model scale and target hardware. The settings under **Advanced (web-ifc)**, such as `memory_limit` and the tolerances, are passed unchanged to the web-ifc geometry engine and may change or go away with a web-ifc update.

### Mesh LODs

With `generate_lods` enabled (the default), every element with enough triangles gets automatic levels of detail, generated by Godot's `ImporterMesh` the same way the glTF importer does it. The work runs on the loader thread, and the renderer switches LODs by screen size. Adjust per node with `lod_bias`, or project-wide with `rendering/mesh_lod/lod_change/threshold_pixels`.

LOD data is saved inside the scene, so `.tscn` files grow. Set `generate_lods = false` for faster loads and smaller scenes. `lod_normal_merge_angle` controls how aggressively vertices with different normals are merged while simplifying.

In the editor dock, **Geometry → Generate LODs** and **Geometry → LOD merge angle** start from the selected settings preset and override it for that load only. The preset itself is not modified.

### IFC Materials

By default, elements of the same colour share one `StandardMaterial3D`, whatever their IFC material. Set `ifc_materials = true` to read the IFC material definitions, and give each IFC material its own `StandardMaterial3D` instead, named after the `IfcMaterial`. Every surface coloured by that material shares it, so editing it changes every element made of that material:

```gdscript
var settings := GDIFCLoaderSettings.new()
settings.ifc_materials = true
ifc_manager.geometric_settings = settings
```

Once `ifc_objects_ready` is emitted, each `IFCNode.material_definition` holds the element's IFC material: a `GDIFCMaterialSingle`, or a layer, profile or constituent set, usage or list made of them (see the [API reference](api-reference.md#ifc-material-definitions-resource)). Every definition is one resource shared by every element and set using it, so a material used by two hundred walls is one `GDIFCMaterialSingle`. Its `visual` is the material its surfaces use; a material none of whose surfaces are shown, such as a hidden layer, gets one of its own in its colour. `GDIFCManager.material_library` lists every definition of the model.

```gdscript
var definition: GDIFCMaterialDefinition = node.material_definition
if definition is GDIFCMaterialLayerSetUsage:
    for layer in definition.layer_set.layers:
        print(layer.material.resource_name if layer.material else "air gap", " ", layer.thickness)

for material in ifc_manager.material_library.materials:
    if material.resource_name == "Concrete":
        material.visual.albedo_color = Color.DIM_GRAY # every concrete surface changes
```

To give an IFC material a different look, such as a downloaded `StandardMaterial3D`, `ShaderMaterial` or `ORMMaterial3D`, assign it to `visual`. Every surface showing the old visual gets the new one, so the change reaches every element made of that material:

```gdscript
var concrete := load("res://materials/concrete_pbr.tres") as Material

# One material
for definition in ifc_manager.find_material_definitions("Concrete C30/37"):
    if definition is GDIFCMaterialSingle:
        definition.visual = concrete

# Or every IfcMaterial with that name, since exporters often write one material several times
ifc_manager.set_material_visual("Concrete C30/37", concrete)
```

Assigning it in the inspector goes through the same setter. `find_material_definitions` returns every definition with that name, and layers and constituents are often named after their material, so check the class. The new visual replaces the material of each mesh surface; a node's surface material override is never changed, so one set by hand keeps showing. Assign the previous material again to go back. Materials replaced this way are saved with the scene. In a scene instanced from an imported `.ifc` file, replacements made in the editor are lost when the scene reloads, since the meshes belong to the imported file; replacements made at runtime work.

Definitions also hold their material property sets (`properties`, shaped like `IFCNode.properties`, with the value type and unit of each value in `property_details`) and classification references (`classifications`). In IFC2X3, typed material properties such as `IfcThermalMaterialProperties` become a set named after the entity, with its attributes as properties. Material definitions are saved in the scene, and their `ifc_object` is relinked from the `.ifc` file when the scene is opened again.

An element uses its own material association, or its type's when it has none. For a layer, profile or constituent set, a surface is matched to the set's material that has its colour. Geometry with no colour at all is matched to the first material without one. Apart from layered elements, below, every surface keeps its colour; the surface styles further down only add roughness, metallic and textures.

A layered wall or slab is one solid in its IFC file, which web-ifc gives the colour of a single layer. When the element has an `IfcMaterialLayerSetUsage` with two layers or more, and its geometry is coloured by its layer set, GDIFC cuts that geometry into the layers: one surface per layer material, in the layer's own colour, stacked along the usage's direction (`AXIS2` for walls, `AXIS3` for slabs) in the order its sense gives. Each layer takes its share of the element's extent in that direction, in proportion to its thickness, so the cut follows the body even when its `OffsetFromReferenceLine` doesn't. An air gap keeps the element's colour. Total surface area is unchanged.

Some surfaces keep a material per colour even with `ifc_materials` on:

- Surfaces coloured by a style on the geometry itself (`IfcStyledItem`) that doesn't match any of the element's materials. They aren't cut into layers either, since the style overrides the material's look. Revit, for example, often gives a layered wall's whole body one grey style.
- `IfcSpace` surfaces, which always use the transparent space material.

Constituent and profile sets need no cutting: their parts are separate representation items. An `IfcShapeAspect` names the items that make one part, after the `IfcMaterialConstituent` or `IfcMaterialProfile` it is made of, and GDIFC links those items to that part's material, whatever their colour. An occurrence's aspects usually sit on its type's geometry and follow the type's part names, which an occurrence with a material set of its own may name differently, so the type's parts are matched too, and its materials are in `material_library`. Without aspects, a part is only found by its colour.

A material's surfaces share one Godot material, so they have one colour. A material without a colour of its own takes the first colour it is shown in; a surface showing it in another colour keeps a material per colour, so no surface changes colour. A linked material without a style of its own takes the look of the style its colour comes from (see below).

`IfcMaterialList` has no names to match, so a list's materials are only found by colour. An element whose parts are one item, such as a stair flight modelled as a single solid with two constituents, keeps one surface.

In the editor dock, **Materials → IFC materials** and **Materials → Textures** start from the selected settings preset and override it for that load only. The **.ifc** importer has the same options as `materials/ifc_materials` and `materials/textures`.

#### Surface Styles And Textures

With `ifc_materials` on, the generated materials also take the rest of their IFC surface style once `ifc_objects_ready` is emitted. The model first appears in flat colours, then changes. Each material shared by an IFC material uses that material's style. A material shared per colour uses the style with its colour, when every style of that colour looks the same; opaque white is skipped, since it is also the colour of geometry without any style. Albedo colours stay as they are.

From `IfcSurfaceStyleRendering`:

- `SpecularHighlight`: an `IfcSpecularRoughness` sets `roughness`; an `IfcSpecularExponent` n sets it to (2 / (n + 2))^¼, its Blinn-Phong equivalent, so Revit's usual 64 gives 0.42.
- `ReflectanceMethod`: `METAL` sets `metallic` to 1, `MIRROR` sets it to 1 with roughness 0, and `GLASS` sets roughness 0.05 when no highlight is given.

Other fields, such as `SpecularColour`, are not used.

Set `ifc_textures = true` to also load the textures of `IfcSurfaceStyleWithTextures`:

- `IfcImageTexture` refers to a file. It is looked for at its path, then at that path and at its file name next to the `.ifc` file, so a texture shipped beside the model is found even when the exporter wrote the author's own absolute path. A texture inside the project is used as imported; one outside it is read and embedded in the scene when it is saved. Web addresses are not downloaded. Textures that can't be found are listed in one warning.
- `IfcBlobTexture` (PNG, JPG, BMP, TGA or WebP) and `IfcPixelTexture` are embedded in the IFC file.

The texture's purpose picks the slot: IFC2X3 `TextureType` or IFC4 `Mode`. Normal maps go to `normal`, bump and height maps to `heightmap`, self-illumination to `emission`, and anything else that is a colour (`TEXTURE`, `MODULATE`, no purpose...) to `albedo`, which then turns white so the texture shows its own colours. Purposes `StandardMaterial3D` has no texture for, such as opacity, reflection or specular, are skipped. `RepeatS` and `RepeatT` set texture repeat, and the scale and origin of `TextureTransform` set `uv1_scale` and `uv1_offset`; its rotation is not applied. IFC texture coordinates (`IfcIndexedTriangleTextureMap` and the like) are not used: textures follow GDIFC's own UVs, 1 UV unit per meter, so they tile by world size.

## Loading Relative To A Georeference

When several models of one project are loaded into the same scene (architecture, structure, MEP...), each one is generated in its own local frame. To line a new model up with one that is already loaded, point `reference_georeference` at the existing model's `IFCGeoreference` node before calling `read_ifc`. The path is relative to the manager, so add the manager to the tree first.

```gdscript
var reference := $ArchManager/IFCModel/Georeference
var ifc_manager := GDIFCManager.new()
add_child(ifc_manager)
ifc_manager.reference_georeference = ifc_manager.get_path_to(reference)
ifc_manager.read_ifc("res://models/structure.ifc")
```

When loading finishes, the new `IFCModel` gets a transform that maps its map conversion (eastings, northings, height, rotation and scale, including the per-axis scale of an IFC4X3 `IfcMapConversionScaled`) onto the reference's map conversion. It also accounts for the `coordinate_to_origin` offset of both models. The math is done in double precision, so large map coordinates don't lose accuracy. Moving the reference model before loading moves the result with it.

IFC4 and later files hold georeferencing in `IfcMapConversion` and `IfcProjectedCRS`. IFC2X3 has no such entities, but some exporters write the same values in the `ePSet_MapConversion` and `ePSet_ProjectedCRS` property sets of the project or site, as the buildingSMART georeferencing guide describes, and GDIFC reads those. If the new model has no georeferencing data, it is placed assuming it shares the reference model's engineering coordinate system, and a warning is printed.

A site's latitude, longitude and elevation are not used for placement, since they would need a map projection. `ifc_manager.get_ifc_model().get_site_location()` returns them.

In the editor dock, pick the reference in **Placement → Reference georeference**. The list shows every `IFCGeoreference` in the edited scene and refreshes each time it opens.

The `coordinate_to_origin` offset is stored on each `IFCGeoreference` node (`coordination_offset_x/y/z`). Models loaded with an older GDIFC version don't have it. If such a model was loaded with `coordinate_to_origin` enabled, reload it before using it as a reference.
