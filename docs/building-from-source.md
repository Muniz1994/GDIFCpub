# Building From Source

Use this guide when you need to compile GDIFC yourself.

## Requirements

- Python 3.8+
- SCons (`pip install scons`)
- C++17 compiler
  - Windows: MSVC 2022
  - Linux: GCC 12+ or Clang 14+
- Boost headers (downloaded by setup script)
- Godot 4.5 or later to run the result. The test project in `ifc-godot-project/` is saved with Godot 4.8.

Platform-specific:

- Android: `ANDROID_HOME` configured with NDK installed
- Web: Emscripten SDK installed and activated (`emcc` in `PATH`)

## Standard Build Flow

```bash
git clone --recurse-submodules https://github.com/Muniz1994/GDIFCpub.git
cd GDIFCpub
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

The library is written to one of two places:

- `addons/GDIFC/`, the distributed addon, by default
- `ifc-godot-project/addons/GDIFC/`, the test project, with `local=1`

```bash
scons platform=windows target=template_debug local=1
```

The dock scripts and scenes are edited in `ifc-godot-project/addons/GDIFC/`. CI copies them to `addons/GDIFC/` when it publishes the addon.

## Testing Against IFC Files

`tests/ifc_corpus/run_ifc_corpus.py` loads every `.ifc` file in a folder in headless Godot and writes HTML, JSON and JUnit reports. Build with `local=1` first. See [tests/ifc_corpus/README.md](../tests/ifc_corpus/README.md).

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
