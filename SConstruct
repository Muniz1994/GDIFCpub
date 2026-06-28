#!/usr/bin/env python
"""
SConstruct for GDIFC — GDExtension for IFC file loading in Godot.

Builds for: Linux (x86_64), Windows (x86_64), Android (arm64), Web (wasm32)

Usage:
    scons                                    # Build for host platform, template_debug
    scons platform=linux target=template_release
    scons platform=windows target=template_release
    scons platform=android target=template_release arch=arm64
    scons platform=web target=template_release

Prerequisites:
    - godot-cpp submodule initialized (git submodule update --init --recursive)
    - Boost headers in thirdparty/boost/ (run: python tools/setup_boost.py)
    - For Android: ANDROID_HOME set, NDK installed
    - For Web: Emscripten SDK installed and activated (emcc in PATH)
    - For Windows cross-compile from Linux: MinGW-w64 installed
"""

import os
import sys
from SCons.Variables import BoolVariable

# ── Custom build options ────────────────────────────────────────────────────
opts = Variables([], ARGUMENTS)
opts.Add(BoolVariable("local", "Install library into the test Godot project", False))

# ── Build godot-cpp and get the configured environment ──────────────────────
env = SConscript("godot-cpp/SConstruct")
opts.Update(env)

# ── Helper: platform-aware compile flags ────────────────────────────────────
is_msvc = env.get("is_msvc", False)
platform = env["platform"]

# ── Thirdparty include paths ────────────────────────────────────────────────
thirdparty_dir = Dir("thirdparty").srcnode().abspath

# Boost (header-only subset)
boost_dir = os.path.join(thirdparty_dir, "boost")
if os.path.isdir(os.path.join(boost_dir, "boost")):
    boost_include = boost_dir
elif os.path.isdir(boost_dir):
    # boost headers might be directly in thirdparty/boost/boost/
    boost_include = boost_dir
else:
    print("WARNING: Boost headers not found in thirdparty/boost/.")
    print("         Run: python tools/setup_boost.py")
    boost_include = boost_dir

# web-ifc dependencies
fastfloat_include = os.path.join(thirdparty_dir, "fastfloat", "include")
tinynurbs_include = os.path.join(thirdparty_dir, "tinynurbs", "include")
glm_include = thirdparty_dir + "/glm"
glm_glm_include = thirdparty_dir + "/glm/glm"
earcut_include = os.path.join(thirdparty_dir, "earcut", "include")
cdt_include = os.path.join(thirdparty_dir, "cdt", "CDT", "include")
spdlog_include = os.path.join(thirdparty_dir, "spdlog", "include")
stduuid_include = os.path.join(thirdparty_dir, "stduuid", "include")

# Source directories
ifcparse_dir = Dir("ifcparse").srcnode().abspath
webifc_dir = Dir("web-ifc").srcnode().abspath
webifc_src_dir = os.path.join(webifc_dir, "web-ifc")
gdifc_dir = Dir("GDIFC").srcnode().abspath

# ── Common include paths ────────────────────────────────────────────────────
common_includes = [
    boost_include,
    fastfloat_include,
    tinynurbs_include,
    glm_include,
    glm_glm_include,
    earcut_include,
    cdt_include,
    spdlog_include,
    stduuid_include,
]


def objects_in_build(target_env, lib_name, sources):
    """Compile sources into build/obj/<lib_name>/... and return object nodes."""
    object_nodes = []
    for src in sources:
        src_path = str(src).replace("\\", "/")
        src_root, _ = os.path.splitext(src_path)
        obj_target = os.path.join("build", "obj", lib_name, src_root).replace("\\", "/")
        object_nodes.append(target_env.Object(target=obj_target, source=src))
    return object_nodes

# ── Helper: enable exceptions (godot-cpp disables them by default) ──────────
def enable_exceptions(target_env):
    """Re-enable C++ exceptions which godot-cpp disables."""
    if is_msvc:
        # Replace /EHs-c- with /EHsc
        for flag_var in ["CXXFLAGS"]:
            flags = target_env.get(flag_var, [])
            new_flags = [f for f in flags if f not in ("/EHs-c-", "/EHs-")]
            if "/EHsc" not in new_flags:
                new_flags.append("/EHsc")
            target_env[flag_var] = type(flags)(new_flags) if hasattr(flags, 'data') else new_flags
    elif platform == "web":
        # Emscripten side modules cannot use -s linker settings for exceptions
        # (they have no JS runtime). Use -fignore-exceptions so throw/try/catch
        # syntax compiles but throws become abort() and catches are skipped.
        # This avoids both the invoke_* (JS EH) and __cpp_exception (Wasm EH)
        # linking issues with Godot's main module.
        for var in ["CXXFLAGS", "CCFLAGS"]:
            flags = target_env.get(var, [])
            new_flags = [f for f in flags if f != "-fno-exceptions"]
            target_env[var] = type(flags)(new_flags) if hasattr(flags, 'data') else new_flags
        target_env.Append(CXXFLAGS=["-fignore-exceptions"])
    else:
        # GCC/Clang: Remove -fno-exceptions, add -fexceptions
        for var in ["CXXFLAGS", "CCFLAGS"]:
            flags = target_env.get(var, [])
            new_flags = [f for f in flags if f != "-fno-exceptions"]
            target_env[var] = type(flags)(new_flags) if hasattr(flags, 'data') else new_flags
        target_env.Append(CXXFLAGS=["-fexceptions"])

# ── Web WASM size optimizations ─────────────────────────────────────────────
# Enable section-level granularity so the linker (with LTO) can discard unused
# functions and data more aggressively, reducing the final .wasm size.
if platform == "web":
    env.Append(CCFLAGS=["-fdata-sections", "-ffunction-sections"])

# ── Build IfcParse static library ───────────────────────────────────────────
ifcparse_env = env.Clone()
enable_exceptions(ifcparse_env)

# C++17 for IfcParse
if is_msvc:
    ifcparse_env.Append(CXXFLAGS=["/std:c++17", "/bigobj"])
else:
    ifcparse_env.Append(CXXFLAGS=["-std=c++17"])
    if platform == "windows":
        # MinGW needs big-obj support for large IFC schema files (PE/COFF section limit)
        ifcparse_env.Append(CCFLAGS=["-Wa,-mbig-obj"])

ifcparse_env.Append(CPPPATH=[ifcparse_dir] + common_includes)
ifcparse_env.Append(CPPDEFINES=[
    "_CRT_SECURE_NO_WARNINGS",
    "_UNICODE",
    "IFC_PARSE_EXPORTS",
    "HAS_SCHEMA_4",
    "HAS_SCHEMA_2x3",
    "HAS_SCHEMA_4x3_add2",
    "IFCQUERY_STATIC_LIB",
])

# IfcParse source files (matching CMake logic: exclude files with digits, re-add selected schemas)
ifcparse_sources_no_digits = [
    "ifcparse/buildinfo.cpp",
    "ifcparse/FileReader.cpp",
    "ifcparse/Header_section_schema-schema.cpp",
    "ifcparse/Header_section_schema.cpp",
    "ifcparse/IfcAlignmentHelper.cpp",
    "ifcparse/IfcCharacterDecoder.cpp",
    "ifcparse/IfcEntityInstanceData.cpp",
    "ifcparse/IfcException.cpp",
    "ifcparse/IfcFile.cpp",
    "ifcparse/IfcGlobalId.cpp",
    "ifcparse/IfcHierarchyHelper.cpp",
    "ifcparse/IfcLogger.cpp",
    "ifcparse/IfcParse.cpp",
    "ifcparse/IfcSchema.cpp",
    "ifcparse/IfcSIPrefix.cpp",
    "ifcparse/IfcSpfHeader.cpp",
    "ifcparse/IfcUtil.cpp",
    "ifcparse/IfcWrite.cpp",
    "ifcparse/parse_ifcxml.cpp",
]

# Schema sources (selected versions only)
# On web, only compile schemas that are actually registered (HAS_SCHEMA_*)
# to reduce WASM binary size. 4x1 and 4x2 have no HAS_SCHEMA define and are dead weight.
if platform == "web":
    _schema_list = ["4", "4x3_add2", "2x3"]
else:
    _schema_list = ["4", "4x3_add2", "2x3"]

ifcparse_schema_sources = []
for schema in _schema_list:
    ifcparse_schema_sources.append(f"ifcparse/Ifc{schema}.cpp")
    ifcparse_schema_sources.append(f"ifcparse/Ifc{schema}-schema.cpp")

ifcparse_all_sources = ifcparse_sources_no_digits + ifcparse_schema_sources
ifcparse_objects = objects_in_build(ifcparse_env, "ifcparse", ifcparse_all_sources)
ifcparse_lib = ifcparse_env.StaticLibrary(
    target="ifcparse/IfcParse",
    source=ifcparse_objects,
)

# ── Build web-ifc static library ───────────────────────────────────────────
webifc_env = env.Clone()
enable_exceptions(webifc_env)

# C++20 for web-ifc
if is_msvc:
    webifc_env.Append(CXXFLAGS=["/std:c++latest", "/utf-8", "/bigobj"])
else:
    webifc_env.Append(CXXFLAGS=["-std=c++20"])
    if platform == "web":
        webifc_env.Append(CXXFLAGS=["-fexperimental-library"])
    if platform == "android":
        webifc_env.Append(CXXFLAGS=["-Wno-narrowing"])
    if platform == "windows":
        webifc_env.Append(CCFLAGS=["-Wa,-mbig-obj"])

webifc_env.Append(CPPPATH=[webifc_src_dir] + common_includes)
webifc_env.Append(CPPDEFINES=["IFCQUERY_STATIC_LIB"])

webifc_sources = Glob("web-ifc/web-ifc/**/*.cpp") + Glob("web-ifc/web-ifc/**/**/*.cpp") + Glob("web-ifc/web-ifc/**/**/**/*.cpp")

# SCons Glob with ** doesn't recurse properly. List explicitly.
webifc_sources = [
    "web-ifc/web-ifc/schema/schema-functions.cpp",
    "web-ifc/web-ifc/schema/IfcSchemaManager.cpp",
    "web-ifc/web-ifc/parsing/IfcLoader.cpp",
    "web-ifc/web-ifc/parsing/IfcFileStream.cpp",
    "web-ifc/web-ifc/parsing/IfcTokenStream.cpp",
    "web-ifc/web-ifc/parsing/IfcTokenChunk.cpp",
    "web-ifc/web-ifc/parsing/uuid_utils.cpp",
    "web-ifc/web-ifc/parsing/string_parsing.cpp",
    "web-ifc/web-ifc/modelmanager/ModelManager.cpp",
    "web-ifc/web-ifc/geometry/IfcGeometryProcessor.cpp",
    "web-ifc/web-ifc/geometry/IfcGeometryLoader.cpp",
    "web-ifc/web-ifc/geometry/nurbs.cpp",
    "web-ifc/web-ifc/geometry/representation/IfcGeometry.cpp",
    "web-ifc/web-ifc/geometry/representation/IfcCurve.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/aabb.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/arc.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/alignment.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/face.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/extrusion.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/plane.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/circularSweep.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/buffers.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/clothoid.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/boolean.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/parabola.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/profile.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/geometry.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/curve.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/sweep.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/revolution.cpp",
    "web-ifc/web-ifc/geometry/operations/bim-geometry/cylindricalRevolution.cpp",
]

webifc_objects = objects_in_build(webifc_env, "webifc", webifc_sources)
webifc_lib = webifc_env.StaticLibrary(
    target="web-ifc/web-ifc",
    source=webifc_objects,
)

# ── Build GDIFC shared library (GDExtension) ───────────────────────────────
gdifc_env = env.Clone()
enable_exceptions(gdifc_env)

# C++17 for GDIFC
if is_msvc:
    gdifc_env.Append(CXXFLAGS=["/std:c++17", "/bigobj"])
    gdifc_env.Append(LINKFLAGS=["/MAP:my_program.map"])
else:
    gdifc_env.Append(CXXFLAGS=["-std=c++17"])

gdifc_env.Append(CPPPATH=[
    os.path.join(gdifc_dir, "src"),
    ifcparse_dir,
    webifc_src_dir,
] + common_includes)

gdifc_env.Append(CPPDEFINES=[
    "IFCQUERY_STATIC_LIB",
    "HAS_SCHEMA_4",
    "HAS_SCHEMA_2x3",
    "HAS_SCHEMA_4x3_add2",
])

gdifc_sources = [
    "GDIFC/src/gd_ifc_manager.cpp",
    "GDIFC/src/register_types.cpp",
    "GDIFC/src/web_ifc_manager.cpp",
    "GDIFC/src/value_conversion.cpp",
    "GDIFC/src/gd_ifc_settings.cpp",
    "GDIFC/src/gd_ifc_node.cpp",
    "GDIFC/src/gd_ifc_entity_base.cpp",
    "GDIFC/src/gd_ifc_model.cpp",
    "GDIFC/src/gd_ifc_georeference.cpp",
]

import glob as _glob
_generated_sources = _glob.glob("GDIFC/src/generated/*.cpp")
if platform == "web":
    # Exclude documentation data from web builds — tooltips aren't useful in
    # browser exports and the 155K-line embedded blob adds ~0.5 MiB to WASM.
    _generated_sources = [s for s in _generated_sources if "doc_data" not in s]
gdifc_sources += _generated_sources
gdifc_objects = objects_in_build(gdifc_env, "gdifc", gdifc_sources)

# Link against static libraries
gdifc_env.Append(LIBS=[ifcparse_lib, webifc_lib])

# Output naming follows godot-cpp convention:
#   libgdifc.{platform}.{target}.{arch}.{ext}
# env["suffix"] = ".{platform}.{target}.{arch}"
# env["SHLIBSUFFIX"] = platform-appropriate extension (.so, .dll, .wasm, .dylib)
# Root addon dir (distributed via Asset Library / git)
root_addon_dir = "addons/GDIFC"

if env["local"]:
# Test Godot project addon dir (for local development only)
    root_addon_dir = "ifc-godot-project/addons/GDIFC"

if platform == "macos":
    library = gdifc_env.SharedLibrary(
        "{}/libgdifc.{}.{}.framework/libgdifc.{}.{}".format(
            root_addon_dir,
            platform, env["target"],
            platform, env["target"],
        ),
        source=gdifc_objects,
    )
else:
    library = gdifc_env.SharedLibrary(
        "{}/libgdifc{}{}".format(
            root_addon_dir,
            env["suffix"],
            env["SHLIBSUFFIX"],
        ),
        source=gdifc_objects,
    )



env.NoCache(library)
Default(library)
