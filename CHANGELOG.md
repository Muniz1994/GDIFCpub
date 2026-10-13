# Changelog

All notable changes to GDIFC are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Changes are grouped by version, then by build. The build number is global: it never resets when the version changes, so each build number identifies exactly one build. The current version and build are in the addon's `plugin.cfg` and are shown at the bottom of the GDIFC dock.

New changes go under **Unreleased**. To cut a build, run `python tools/bump_build.py`, or add `--version <new version>` to start a new version. The script moves the Unreleased entries into a new build section and updates both `plugin.cfg` copies.

## [Unreleased]

## [1.1.0-alpha]

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
