# Changelog

All notable changes to GDIFC are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Changes are grouped by version, then by build. The build number is global: it never resets when the version changes, so each build number identifies exactly one build. The current version and build are in the addon's `plugin.cfg` and are shown at the bottom of the GDIFC dock.

New changes go under **Unreleased**. Builds are cut automatically: when a push to `master` builds successfully and Unreleased has entries, CI runs `tools/bump_build.py`, which moves those entries into a new build section and updates both `plugin.cfg` copies. CI then commits the result, tags it `v<version>-build.<build>`, and creates a draft GitHub release with the addon zip. To start a new version, run `python tools/set_version.py <new version>`, which updates both `plugin.cfg` copies, and commit it with the new entries; the next build is cut under that version. To cut a build locally instead, run `python tools/bump_build.py` (optionally with `--version <new version>`).

## [Unreleased]

## [1.1.0-alpha]

### Build 5 — 2026-11-02

##### Added

- `.ifc` files are imported as scenes, like `.glb` files. Drag one into the 3D view or instance it, and the scene only stores a reference to the file instead of every mesh, collision shape and property set: a scene holding a 2.5 MB model went from a 2.2 MB `.tscn` to 230 bytes. Import options cover coordinate to origin, circle segments, LODs and collision. Instances re-link `ifc_object` from the `.ifc` file, and follow their `reference_georeference`.
- **Import as scene** option in the dock's new Scene group. **Load IFC...** then imports the file with the dock's options and adds it to the edited scene as an instance, with undo. Files from outside the project are copied into a per-project import folder (`res://ifc/` by default). Picking a different file with the same name asks before replacing it.
- IFC meshes have UVs, box-projected at 1 UV unit per meter, so textured materials display on them.
- **IFC materials** option, off by default: `GDIFCLoaderSettings.ifc_materials`, the dock's new Materials group, and the importer's `materials/ifc_materials`. Each IFC material gets its own `StandardMaterial3D`, named after it and shared by every surface it colours, so editing it changes every element made of that material. Without the option, elements share materials by colour, so unrelated materials of the same colour are edited together. Layered walls and slabs show their layers: web-ifc gives them one solid in a single layer's colour, which is cut into one surface per layer of their `IfcMaterialLayerSetUsage`, each in its own material and colour. The parts of constituent and profile sets are linked to their materials through `IfcShapeAspect`, including aspects named after the parts of the element's type, so doors, windows and other multi-material elements get a material per part even when their styled colours differ from their materials'.
- IFC material definitions, read with the IFC materials option. Each `IFCNode.material_definition` holds the element's material, layer set, profile set, constituent set, usage or list as resources (`GDIFCMaterialSingle`, `GDIFCMaterialLayerSetUsage`, ...), shared by every element using them, with their material property sets, classification references and, in IFC4, material relationships. `GDIFCMaterialSingle.visual` is the Godot material its surfaces use. `GDIFCManager.material_library` lists every definition. Definitions are saved in the scene and their `ifc_object` is relinked when it is opened again.
- Replacing IFC materials: assign any `Material` to `GDIFCMaterialSingle.visual`, in code or in the inspector, and every surface showing the old visual gets it, so every element made of that material changes. `GDIFCManager.set_material_visual(name, material)` does it for every IFC material with that name, and `find_material_definitions(name)` finds definitions by name. Node surface overrides are left alone, and replaced materials are saved with the scene.
- Collision matrix: `GDIFCManager.collision_matrix` takes a `GDIFCCollisionMatrix` resource with one `GDIFCCollisionRule` per IFC class, each choosing the shape (None, Trimesh, Simple Convex, Box or Decompose Convex) and the physics layers of the element's `StaticBody3D`. Rules match subtypes in the file's schema, the rule closest to the element's class wins, and a default rule covers the rest. Hulls and decompositions are built from the geometry in memory, without reading meshes back from the GPU. Decompose Convex is only built at import. The dock's Collisions group has a **Matrix** picker, and the importer a `collision/matrix` option; saving a matrix reimports the `.ifc` files using it. Three presets ship in `addons/GDIFC/settings/collision/`: **Walkthrough**, **Structure only** and **Picking**.
- IFC surface styles on the generated materials, with the IFC materials option: roughness from `IfcSurfaceStyleRendering`'s specular highlight, and metallic from its reflectance method. With the new **Textures** option (`GDIFCLoaderSettings.ifc_textures`, importer `materials/textures`), the textures of `IfcSurfaceStyleWithTextures` too: image files, found at their path or next to the `.ifc` file, and textures embedded as `IfcBlobTexture` or `IfcPixelTexture`, as albedo, normal, height or emission maps. Textures use GDIFC's own UVs, not IFC texture coordinates.
- `IFCNode.properties` and `quantities` include the sets of the element's type (`IfcTypeObject.HasPropertySets`). When the element and its type both set a property, the element's value is kept. In a test, Snowdon Towers' elements gained 2,107 values that were only on their types.
- `IFCNode.property_details` and `quantity_details`: for each value, its IFC value type (`IfcLengthMeasure`, `IfcLabel`, ...), its unit as a symbol (`mm`, `m²`, `W/(m²·K)`), from the property or else the project's units, and whether it came from the type. They are shaped like `properties` and `quantities`, and read-only.
- Complex properties (`IfcComplexProperty`) as nested dictionaries, table properties (`IfcPropertyTableValue`) as their defining and defined values, complex quantities (`IfcPhysicalComplexQuantity`) as nested dictionaries, and IFC4X3's `IfcQuantityNumber`. They used to be left out.
- `IFCModel.get_units()`: the project's units, such as `{"LENGTHUNIT": "mm"}`.
- [Walking The Model](docs/walking-the-model.md): how to reach the rest of the IFC file from a node's `ifc_object`, with recipes for the element's type, classifications, groups and systems, openings and their fillings, space boundaries and entities without a node.

##### Changed

- Updated web-ifc to 0.0.79 (`a26fae51`). It brings many geometry fixes (booleans, swept disks, sectioned and revolved solids, profile sweeps along IFC4X3 curves), support for `IfcSegmentedReferenceCurve`, the element type's material colour when an occurrence has none, and caching of mapped representations. Its bundled dependencies were moved to the versions web-ifc now pins: fast_float 8.3.2, earcut 3.2.4, CDT 2.0.1 and unordered_dense 5.3.3.
- Malformed IFC files that web-ifc can only partially parse now fail to load with an error, instead of loading an incomplete model.
- Loading a file whose `FILE_SCHEMA` isn't IFC2X3, IFC4 or IFC4X3_ADD2 now prints a highlighted message and a warning that name the declared schema and the supported ones. The geometry still loads, but properties, quantities, typed objects and georeferencing are unavailable. Before, the only sign was a generic "Failed to parse IFC file" error.
- `GDIFCLoaderSettings` geometry tolerances now default to web-ifc's own defaults: `tolerance_plane_intersection` 1.0e-4 (was 1.0e-1), `tolerance_plane_deviation` and `tolerance_back_deviation_distance` 1.0e-4 (were 3.0e-4), and `plane_refit_iterations` 1 (was 10). The old values had been tuned in web-ifc's test program for one model. With them, some files never finished processing geometry (in a test, `417--buildingElementProxy` ran for over 120 s, against 0.4 s with the new defaults). Set the old values on a `GDIFCLoaderSettings` to keep the previous results.
- Property and quantity sets without a name are keyed by their class, such as `"IfcElementQuantity"`, instead of being left out.
- Scenes that store their nodes are larger, since they also save the type's sets and the details. On a 13,000-element Revit model, the property data grew from 5.5 MB to 16.4 MB and memory use by about 11%. Scenes that instance an imported `.ifc` file are not affected.

##### Fixed

- Vertices shared by faces facing different directions are split, so each face keeps its own flat normal.
- The dock's collision element list no longer shows every element type twice.
- Tasks, work plans and schedules, cost items and type objects no longer become nodes when they nest or aggregate other entities. Only products and the project are part of the node hierarchy. Before, a 4D file showed every task that had subtasks, without geometry, at the top of the model.
- `IFCModel` header getters (`get_originating_system`, `get_preprocessor`, `get_authorization`, ...) return an empty value when the field is `$` in `FILE_NAME`, instead of crashing Godot with an unhandled exception. Several exporters, such as Simplebim, write `$` for these fields.
- Loading a file no longer logs a web-ifc `[GetMesh()] unexpected mesh type` error for the `IfcProject`, which has no geometry.
- Quantities whose value is 0 are 0 in `IFCNode.quantities`, instead of null.

### Build 4 — 2026-10-15

#### Changed

- Changed ration of loading File Dialog to 0.8. 

##### Fixed

- Fixed dock logo sizing, that was preventing the correct collapse of the dock in small windows.

### Build 3 — 2026-10-15

#### Added

- A progress bar in the editor dock while a file loads. It shows the current stage (Parsing, Processing geometry, Creating nodes, Preparing editor, Adding to scene) and ends with "Loaded <file> in X s". A failed load shows the failure in the editor's error color, and no longer leaves an empty `GDIFCManager` in the scene. The **Load IFC...** button is disabled while a load runs.
- `GDIFCManager.load_progress_changed(progress, stage)` signal, with `get_load_progress()` and `get_load_stage()`, so your own UI can follow a load. Stages are weighted by their measured share of the load time, and the long parsing step is estimated from the file size, so the progress moves roughly in step with time.

#### Changed

- **Models appear much sooner.** The model is now built from web-ifc alone and shown first. Attributes, property sets, quantities, typed `ifc_object` references and georeferencing come from IfcOpenShell's parser, which used to run first and took most of the load time. It now runs in the background alongside the geometry, and its results are applied afterwards. Time until the model is shown, outside the editor: dental_clinic 1.4 → 0.45 s, schependomlaan 3.8 → 0.7 s, ARK NordicLCA 11.4 → 4.4 s. In the editor: dental_clinic 2.5 → 1.6 s, schependomlaan 5.3 → 2.4 s.
  - **Behaviour change:** `ifc_read` now fires before that data is available. Wait for `ifc_objects_ready` before reading `attributes`, `properties`, `quantities` or `ifc_object`. A model placed relative to `reference_georeference` moves into place at `ifc_objects_ready`, and the `Georeference` node is added then too.
  - Files whose schema IfcOpenShell can't read (for example IFC4X3_RC2) used to fail to load. They now load with geometry and without properties, with an error in the Output panel.
  - `IfcProject` nodes now get their GlobalId.
- LOD generation runs on all CPU cores, as its own stage after the geometry. On ARK NordicLCA it went from 4.6 s to about 1 s.
- The editor dock shows "reading properties…" until the properties arrive, and keeps **Load IFC...** disabled until then.
- The generated geometry is released from memory once the nodes are created.

### Build 2 — 2026-10-13

#### Fixed

- The editor no longer freezes for a second or more just before a loaded model appears. When the model was revealed, the editor built a selection gizmo for every mesh and collision shape in that single frame, and each gizmo read its mesh back from the GPU. Those gizmo caches are now built ahead of time, spread across frames within the existing time budget. On the test models, the freeze dropped from 1.3–1.8 s to about 0.5–0.6 s, and the model is visible at about the same time as before or sooner. Most of the remaining pause is the Scene dock adding storeys with around 1,000 direct children.

#### Changed

- Collision shapes are built from the geometry already in memory instead of being read back from the GPU, which makes loads with collisions faster, including at runtime. The node layout is unchanged: an `<element>_col` `StaticBody3D` with a `CollisionShape3D`.
- Loading uses more of each frame while the editor window is unfocused. The editor already waits about 100 ms per frame then, so loading in the background finishes sooner without making the editor slower.
- Reorganized the editor dock. **Load IFC...** is at the top, and the options are in foldable **Geometry**, **Collisions** and **Placement** groups inside a scroll area. Each group remembers whether it is folded, per project.
- The dock now follows the editor theme (light and dark presets, custom colors and editor scale). Hardcoded panel, selection and font colors were removed. The button icons come from the editor icon set, and the footer logo and build label are tinted with the theme's font color.
- **Collision elements** are greyed out while collisions are disabled.
- The **Elements** list in the **Collisions** group now offers every concrete `IfcBuildingElement` (IFC2X3, IFC4) and `IfcBuiltElement` (IFC4X3) subtype, 39 in all, in a fixed-height scrolling list. Selecting a type also matches its IFC4 `StandardCase` and `ElementedCase` variants, so selecting `IfcWall` also includes `IfcWallStandardCase` and `IfcWallElementedCase`.
- The class icons (`GDIFCManager`, `IFCNode`, `IFCGeoreference`, `IFCModel`) use Godot's standard icon gray and are converted with the editor theme, so they stay visible on light themes.

### Build 1 — 2026-10-13

#### Added

- Automatic mesh LOD generation. Each element's mesh gets levels of detail from Godot's `ImporterMesh.generate_lods()`, the same path the glTF importer uses. The renderer switches LODs by screen size, which reduces the triangle count for distant geometry in large models.
  - LODs are generated on the loader thread, so the main-thread node generation budget is unchanged.
  - Elements with fewer than 64 triangles are skipped, since they can't be simplified meaningfully.
  - Collision shapes still use the full-detail mesh.
- `GDIFCLoaderSettings.generate_lods` (default `true`) turns LOD generation on or off.
- `GDIFCLoaderSettings.lod_normal_merge_angle` (default `60.0` degrees) sets how aggressively vertices with different normals are merged while simplifying.
- Two new controls in the **Config** section of the editor dock: **Generate LODs** and **LOD merge angle**. They start from the selected settings preset and override it for that load only. The preset `.tres` is not modified.

#### Changed

- When LOD generation is enabled, saved scenes are larger, because the LOD index data is stored with each mesh. Set `generate_lods = false` to restore the previous load time and scene size.
- The editor dock now passes a copy of the selected settings preset to `GDIFCManager` instead of the preset itself.
