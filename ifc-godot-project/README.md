# Freefly Camera Script for Godot

This is a versatile free-fly camera script for Godot, designed to handle 3D character movement with smooth camera controls. It features adjustable movement settings, mouse sensitivity, sprinting, head rotation clamping, and more. This script is perfect for exploring 3D environments or any other project that requires a free-form camera perspective.

## Features

- **Movement Controls**: Move in 3D space with configurable movement speed and sprinting.
- **Mouse Look**: Control the camera's rotation with mouse input, with optional acceleration and clamping.
- **Collision**: Optionally enable/disable collision detection.
- **Customizable Key Bindings**: Configure movement keys, sprinting, and escape for toggling mouse capture.
- **Head Rotation Clamping**: Option to limit vertical camera rotation for more control.
- **Camera Node**: Easily attach the camera to a character node.
- **Advanced Settings**: Customize settings like whether updates should occur on physics ticks.

## Getting Started

### Prerequisites
- Godot 4.x (tested with Godot 4.3)
- A 3D scene with a `CharacterBody3D` node

### Installation

1. Clone or download this repository to your Godot project folder.
2. In your Godot project, create a new **CharacterBody3D** node (or use an existing one).
3. Attach this script to the **CharacterBody3D** node.
4. Assign a **Camera3D** node to the `camera` variable in the Inspector.
5. Adjust the settings in the Inspector to fit your needs.

### Key Bindings

You can modify the default key bindings in the Inspector for the following actions:

- **Movement**: 
  - Forward: `W`
  - Backward: `S`
  - Left: `A`
  - Right: `D`
  - Up: `Space`
  - Down: `Ctrl`
  
- **Sprint**: `Shift`
- **Escape**: Toggle mouse capture

### Customization

- `movement_speed`: Controls the speed of movement.
- `sprint_multiplier`: Adjusts how much faster you move when sprinting.
- `MOUSE_SENS`: Adjusts mouse sensitivity.
- `MOUSE_ACCEL`: Defines the acceleration factor for the mouse.
- `CLAMP_HEAD_ROTATION_MIN` and `CLAMP_HEAD_ROTATION_MAX`: Set the min/max angles for vertical camera rotation.
- `UPDATE_ON_PHYSICS`: Determines whether the input is processed during the physics step.

### Controls

- **W/A/S/D**: Move the camera around.
- **Mouse**: Look around (with optional head rotation clamping).
- **Shift**: Sprint.
- **Escape**: Toggle mouse capture.

## License

This project is licensed under the **GPL v3.0** License. See the [LICENSE](LICENSE) file for details.

## Attribution

Part of the camera code in this project is derived from the **Basic FPS Player Godot 4.0** project by [sammburr](https://github.com/sammburr/). You can find the original project [here](https://github.com/sammburr/Basic-FPS-Player-GODOT-4.0). Thank you!

## Contributions

Contributions are welcome! If you find bugs or have suggestions, feel free to open an issue or submit a pull request.
