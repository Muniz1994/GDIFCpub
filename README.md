# GDIFC

![GDIFC](images/GDIFC.png)

A GDExtension for loading [buildingSMART IFC](https://www.buildingsmart.org/standards/bsi-standards/industry-foundation-classes/) files directly in Godot 4.  
It creates a node tree of `IFCNode` meshes with the full geometry, properties, and quantity sets of the model.

## Features

- Asynchronous, threaded loading — the editor and game remain responsive while the model is parsed
- IFC schema support: IFC 2.3.0.1 (IFC 2x3 TC1), IFC 4.0.2.1 (IFC 4 ADD2 TC1), 4.3.2.0 (IFC 4.3 ADD2)    
- Per-object property sets, quantity sets, and attributes exposed as `Dictionary`
- Optional collision shape generation per object
- Georeferencing data (`MapConversion` / `ProjectedCRS`) available after load
- Platforms: Windows, Linux, Android, Web (WASM)

![Bridge](images/Bridge4.3.jpg)

IFC 4.3 example on godot 4.7 dev outline branch

## Installation

### Godot Asset Library (recommended)

1. Open your Godot 4 project.
2. Go to **AssetLib** tab → search for **GDIFC**.
3. Click **Download** → **Install**.
4. Enable the plugin under **Project → Project Settings → Plugins**.

### Manual

1. Copy the `addons/GDIFC/` folder into your project's `res://addons/` directory.
2. Enable the plugin under **Project → Project Settings → Plugins**.

## Quick Start

Add a **GDIFCManager** node to your scene, then call `read_ifc` from a script:

```gdscript
extends Node3D

@onready var ifc_manager: GDIFCManager = $GDIFCManager

func _ready() -> void:
    # Load an IFC file.  Pass an empty Array for collision_classes to skip
    # collision generation, or list IFC class names to include (e.g. ["IfcWall"]).
    ifc_manager.read_ifc("res://models/building.ifc", false, [])
```

After loading finishes, child `IFCNode` nodes are automatically added under the `GDIFCManager`.

To load from a base-64 encoded string (e.g. data received over the network):

```gdscript
ifc_manager.read_ifc_base64(base64_string, false, [])
```

## Documentation

Extended documentation is available in the [`docs/`](docs/) folder:

- [Documentation Index](docs/index.md)
- [Getting Started](docs/getting-started.md)
- [Loading Models](docs/loading-models.md)
- [Typed Entity Access](docs/typed-entities.md)
- [API Reference](docs/api-reference.md)
- [Building From Source](docs/building-from-source.md)

## API Reference

### `GDIFCManager` — `Node3D`

The entry point for all IFC loading.

| Member | Type | Description |
|---|---|---|
| `read_ifc(path, create_collision, collision_classes)` | `Error` | Load an IFC file from disk. `path` is a Godot resource path or absolute path. `collision_classes` is an `Array[String]` of IFC class names to generate collision shapes for (pass `[]` to disable). |
| `read_ifc_base64(data, create_collision, collision_classes)` | `Error` | Load an IFC file from a base-64 encoded string. Same parameters as `read_ifc`. |
| `geometric_settings` | `GDIFCLoaderSettings` | Resource controlling geometry tessellation, memory limits, and tolerances. Assign before calling `read_ifc`. |

---

### `IFCNode` — `MeshInstance3D`

Represents a single IFC object in the scene tree.

| Property | Type | Description |
|---|---|---|
| `ifc_class` | `String` | IFC class name, e.g. `"IfcWall"`, `"IfcSlab"`. |
| `attributes` | `Dictionary` | Core IFC attributes (GlobalId, Name, Description, …). |
| `properties` | `Dictionary` | All property sets keyed by set name. |
| `quantities` | `Dictionary` | All quantity sets keyed by set name. |

---

### `GDIFCLoaderSettings` — `Resource`

Attach to `GDIFCManager.geometric_settings` to control the loader.

| Property | Default | Description |
|---|---|---|
| `coordinate_to_origin` | `false` | Translate the model so the project origin aligns with the IFC site origin. |
| `circle_segments` | `12` | Number of segments used to approximate circular profiles. |
| `tape_size` | `67108864` (64 MB) | Internal parser tape buffer size in bytes. |
| `memory_limit` | `2147483648` (2 GB) | Maximum memory the geometry processor may use in bytes. |
| `line_writer_buffer` | `10000` | Internal line-writer buffer size. |
| `tolerance_plane_intersection` | `1.0e-1` | Tolerance for plane-intersection tests. |
| `tolerance_plane_deviation` | `3.0e-4` | Tolerance for plane-deviation tests. |
| `tolerance_back_deviation_distance` | `3.0e-4` | Tolerance for back-deviation distance tests. |
| `tolerance_inside_outside_perimeter` | `1.0e-10` | Tolerance for inside/outside perimeter tests. |
| `tolerance_scalar_equality` | `1.0e-4` | Tolerance for scalar equality comparisons. |
| `plane_refit_iterations` | `10` | Maximum iterations for the plane-refit solver. |
| `boolean_union_threshold` | `150` | Maximum number of boolean union operations per object before the operation is skipped. |

---

## Typed IFC Access

Every IFC entity class is exposed as a native Godot class following the IFC naming convention (`IfcWall`, `IfcSlab`, `IfcDoor`, …). Each class inherits from `GDIFCEntityBase` and exposes its own IFC attributes as typed Godot properties with the exact IFC attribute names (e.g. `PredefinedType`, `Name`, `Description`).

### Cast to a specific IFC type

```gdscript
# ifc_node.ifc_object is a GDIFCEntityBase reference.
# Cast it to the typed class to access typed properties.
var wall := ifc_node.ifc_object as IfcWall
if wall:
    print(wall.PredefinedType)   # e.g. "SOLIDWALL"
    print(wall.Name)             # e.g. "External Wall"
```

### Traverse the model tree

```gdscript
# Iterate all children of the GDIFCManager and access typed data.
for child in ifc_manager.get_children():
    if child is IFCNode:
        var slab := child.ifc_object as IfcSlab
        if slab:
            print("Slab name: ", slab.Name)
            print("Slab type: ", slab.PredefinedType)
```

### Create and modify an IFC object at runtime

```gdscript
# All generated classes exist in the ClassDB — you can instantiate them.
var wall: IfcWall = IfcWall.new()
wall.Name = "New Wall"
wall.PredefinedType = "SOLIDWALL"
```

### Dynamic fallback via GDIFCEntityBase

When the exact IFC class is not known at compile time, use the generic base-class API:

```gdscript
var entity: GDIFCEntityBase = ifc_node.ifc_object
print(entity.get_type())                      # e.g. "IfcWall"
print(entity.get_attribute("Name"))           # generic attribute read
print(entity.get_all_attributes())            # Dictionary of all attributes
```



### Requirements

- Python 3.8+ and [SCons](https://scons.org) (`pip install scons`)
- A C++17 compiler (MSVC 2022, GCC 12+, or Clang 14+)
- Boost headers — fetched automatically by the setup script (see below)
- **Android**: `ANDROID_HOME` set with the NDK installed
- **Web**: Emscripten SDK installed and activated (`emcc` in `PATH`)

### Steps

```bash
# 1. Clone with submodules
git clone --recurse-submodules https://github.com/Muniz1994/GDIFC.git
cd GDIFC

# 2. Download Boost headers into thirdparty/boost/
python tools/setup_boost.py

# 3. Build for the host platform (debug by default)
scons

# Or specify platform and target explicitly:
scons platform=windows target=template_release
scons platform=linux   target=template_release
scons platform=android target=template_release arch=arm64
scons platform=web     target=template_release
```

The compiled library is placed in both `addons/GDIFC/` (for distribution) and `ifc-godot-project/addons/GDIFC/` (for the test project) automatically.

## Dependencies

| Library | Purpose |
|---|---|
| [web-ifc](https://github.com/ThatOpen/engine_web-ifc) | Geometry processing — converts IFC solid geometry into triangle meshes |
| [IfcOpenShell / IfcParse](https://ifcopenshell.org) | IFC file parsing and schema support |
| [godot-cpp](https://github.com/godotengine/godot-cpp) | Godot 4 GDExtension C++ bindings |

## Licence

See [LICENCE.md](LICENCE.md).

