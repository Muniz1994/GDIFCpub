# Getting Started

This guide explains the full setup flow for using GDIFC inside a Godot 4 project.

## What GDIFC Adds to Your Project

GDIFC is a Godot GDExtension plugin that imports IFC models and creates scene nodes with:

- Renderable mesh geometry
- IFC attributes
- Property sets (Psets)
- Quantity sets (Qto)

The central runtime node is `GDIFCManager`.

## Requirements

Godot 4.5 or later, on Windows, Linux, Android or Web.

## Installation Paths

### Option A: Godot Asset Library (recommended)

1. Open your Godot 4 project.
2. Go to **AssetLib** and search for **GDIFC**.
3. Click **Download** and then **Install**.
4. Enable the plugin in **Project Settings -> Plugins**.

### Option B: Manual Installation

1. Copy `addons/GDIFC/` into your project at `res://addons/GDIFC/`.
2. Enable the plugin in **Project Settings -> Plugins**.

## First Scene Setup

1. Add a `GDIFCManager` node to your scene.
2. Add a script to any active node (for example your root `Node3D`).
3. Call `read_ifc` during startup.

Example:

```gdscript
extends Node3D

@onready var ifc_manager: GDIFCManager = $GDIFCManager

func _ready() -> void:
    var err := ifc_manager.read_ifc("res://models/building.ifc")
    if err != OK:
        push_error("Failed to start IFC load. Error code: %s" % err)
```

In the editor, you can also load a file from the GDIFC dock with **Load IFC...**, or drag an `.ifc` file from the FileSystem dock into the 3D view to [import it as a scene](loading-models.md#importing-ifc-files-as-scenes).

## How Loading Behaves

The load pipeline is asynchronous and threaded. This means your game or editor stays responsive while parsing and mesh generation are running.

Loading finishes in two steps:

1. `ifc_read`: the model is visible. GDIFC has added an `IFCModel` node under the `GDIFCManager`, holding the `IFCNode` tree nested by spatial structure (project, site, building, storey).
2. `ifc_objects_ready`: each node's attributes, property sets, quantities and typed `ifc_object` are filled in. Wait for this signal before reading IFC data.

## Next Steps

- Read [Loading Models](loading-models.md) to choose collision and data-source strategies.
- Read [Typed Entity Access](typed-entities.md) to work with IFC classes like `IfcWall` and `IfcSlab`.
