# Changelog

All notable changes to GDIFC are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Changes are grouped by version, then by build. The build number is global: it never resets when the version changes, so each build number identifies exactly one build. The current version and build are in the addon's `plugin.cfg` and are shown at the bottom of the GDIFC dock.

New changes go under **Unreleased**. To cut a build, run `python tools/bump_build.py`, or add `--version <new version>` to start a new version. The script moves the Unreleased entries into a new build section and updates both `plugin.cfg` copies.

## [Unreleased]

### Added

- A progress bar in the editor dock while a file loads. It shows the current stage (Parsing, Processing geometry, Creating nodes, Preparing editor, Adding to scene) and ends with "Loaded <file> in X s". A failed load shows the failure in the editor's error color, and no longer leaves an empty `GDIFCManager` in the scene. The **Load IFC...** button is disabled while a load runs.
- `GDIFCManager.load_progress_changed(progress, stage)` signal, with `get_load_progress()` and `get_load_stage()`, so your own UI can follow a load. Stages are weighted by their measured share of the load time, and the long parsing step is estimated from the file size, so the progress moves roughly in step with time.

## [1.1.0-alpha]

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
