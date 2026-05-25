# Building From Source

Use this guide when you need to compile GDIFC yourself.

## Requirements

- Python 3.8+
- SCons (`pip install scons`)
- C++17 compiler
  - Windows: MSVC 2022
  - Linux: GCC 12+ or Clang 14+
- Boost headers (downloaded by setup script)

Platform-specific:

- Android: `ANDROID_HOME` configured with NDK installed
- Web: Emscripten SDK installed and activated (`emcc` in `PATH`)

## Standard Build Flow

```bash
git clone --recurse-submodules https://github.com/Muniz1994/GDIFC.git
cd GDIFC
python tools/setup_boost.py
scons
```

## Explicit Platform Builds

```bash
scons platform=windows target=template_release
scons platform=linux   target=template_release
scons platform=android target=template_release arch=arm64
scons platform=web     target=template_release
```

## Output Location

Build artifacts are copied to:

- `addons/GDIFC/` for distribution
- `ifc-godot-project/addons/GDIFC/` for the sample project

## Regenerating IFC Typed Bindings

If schema or generated class sources change, regenerate code from Express schema:

```bash
cd ifcopenshell-express
python godot_codegen.py schemas/IFC4X3_ADD2.exp ../GDIFC/src/generated/
```

After regeneration, run your usual build command again.

## Common Build Notes

- If submodules are missing, run `git submodule update --init --recursive`.
- Keep compiler and Godot ABI settings aligned with your target runtime.
- On CI, cache third-party dependencies and generated files for faster rebuilds.
