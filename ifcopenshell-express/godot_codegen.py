#!/usr/bin/env python3
"""
Godot GDExtension C++ code generator for IFC Express schemas.

Generates per-entity C++ GDExtension classes that expose all IFC entity
attributes as typed Godot getters/setters, mirroring the full IFC inheritance
hierarchy inside Godot's class system.

Usage:
    python3 godot_codegen.py <schema.exp> <output_dir>
    e.g.:
    python3 godot_codegen.py schemas/IFC4X3_ADD2.exp ../GDIFC/src/generated/

Requirements:
    - pyparsing  (pip install pyparsing)
    - express_parser.py must be present (run bootstrap.py if missing)

Outputs in <output_dir>:
    gd_ifc_entities.h                  - All class declarations
    gd_ifc_entities_impl_A.cpp         - Implementations for A-E
    gd_ifc_entities_impl_B.cpp         - Implementations for F-L
    gd_ifc_entities_impl_C.cpp         - Implementations for M-R
    gd_ifc_entities_impl_D.cpp         - Implementations for S-Z
    gd_ifc_entities_register.cpp       - ClassDB + factory registrations
"""

import csv
import html
import os
import re
import sys

# ── locate ifcopenshell-express package ──────────────────────────────────────
_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)

import express_parser
from schema import Schema
from mapping import Mapping
import nodes as _nodes

# ─────────────────────────────────────────────────────────────────────────────
# Type-mapping tables  (Express argument type → Godot C++ side)
# ─────────────────────────────────────────────────────────────────────────────
# Each entry: (cpp_type, variant_type_enum, getter_helper, setter_helper, default_val)
ARGTYPE_MAP = {
    "IfcUtil::Argument_INT":     ("int64_t",                   "Variant::INT",                "gd_attr_int",        "gd_set_int",        "0"),
    "IfcUtil::Argument_DOUBLE":  ("double",                    "Variant::FLOAT",              "gd_attr_double",     "gd_set_double",     "0.0"),
    "IfcUtil::Argument_STRING":  ("godot::String",             "Variant::STRING",             "gd_attr_string",     "gd_set_string",     "godot::String()"),
    "IfcUtil::Argument_BOOL":    ("bool",                      "Variant::BOOL",               "gd_attr_bool",       "gd_set_bool",       "false"),
    "IfcUtil::Argument_LOGICAL": ("godot::String",             "Variant::STRING",             "gd_attr_string",     "gd_set_string",     "godot::String()"),
    "IfcUtil::Argument_BINARY":  ("godot::String",             "Variant::STRING",             "gd_attr_string",     "gd_set_string",     "godot::String()"),
    "IfcUtil::Argument_ENUMERATION": (
        "godot::String", "Variant::STRING", "gd_attr_string", "gd_set_string", "godot::String()"),
    "IfcUtil::Argument_ENTITY_INSTANCE": (
        "Ref<GDIFCEntityBase>", "Variant::OBJECT", "gd_attr_entity", "gd_set_entity", "Ref<GDIFCEntityBase>()"),
    "IfcUtil::Argument_AGGREGATE_OF_INT": (
        "godot::PackedInt64Array",   "Variant::PACKED_INT64_ARRAY",   "gd_attr_agg_int",    "gd_set_agg_int",    "godot::PackedInt64Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_DOUBLE": (
        "godot::PackedFloat64Array", "Variant::PACKED_FLOAT64_ARRAY", "gd_attr_agg_double", "gd_set_agg_double", "godot::PackedFloat64Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_STRING": (
        "godot::PackedStringArray",  "Variant::PACKED_STRING_ARRAY",  "gd_attr_agg_string", "gd_set_agg_string", "godot::PackedStringArray()"),
    "IfcUtil::Argument_AGGREGATE_OF_BINARY": (
        "godot::PackedStringArray",  "Variant::PACKED_STRING_ARRAY",  "gd_attr_agg_string", "gd_set_agg_string", "godot::PackedStringArray()"),
    "IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_agg_entity", "gd_set_agg_entity", "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_INT": (
        "godot::Array", "Variant::ARRAY", "gd_attr_variant", None, "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_DOUBLE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_variant", None, "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_STRING": (
        "godot::Array", "Variant::ARRAY", "gd_attr_variant", None, "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_variant", None, "godot::Array()"),
}

ENTITY_HINT = 'PROPERTY_HINT_RESOURCE_TYPE, "GDIFCEntityBase"'

# ─────────────────────────────────────────────────────────────────────────────
# Helpers
# ─────────────────────────────────────────────────────────────────────────────

def to_snake(name: str) -> str:
    """Convert CamelCase attribute name to snake_case property name."""
    s = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    s = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', s)
    return s.lower()

def gd_class_name(ifc_name: str) -> str:
    """IFC entity name → Godot GDClass name (e.g. IfcWall → IfcWall)."""
    return ifc_name

# ─────────────────────────────────────────────────────────────────────────────
# Documentation helpers  (CSV → Godot XML)
# ─────────────────────────────────────────────────────────────────────────────

def _strip_html(text: str) -> str:
    """Remove HTML tags, unescape entities, and normalise whitespace."""
    text = re.sub(r'<[^>]+>', ' ', text)
    text = html.unescape(text)
    return ' '.join(text.split())


def _load_entity_docs(csv_path: str) -> dict:
    """Returns {doc_line_int: (entity_name, plain_description)}."""
    result = {}
    with open(csv_path, newline='', encoding='cp1252', errors='replace') as f:
        for row in csv.reader(f, delimiter=';'):
            if len(row) >= 3:
                try:
                    result[int(row[0])] = (row[1].strip(), _strip_html(row[2]))
                except ValueError:
                    pass
    return result


def _load_attr_docs(csv_path: str) -> dict:
    """Returns {doc_line_int: plain_description}."""
    result = {}
    with open(csv_path, newline='', encoding='cp1252', errors='replace') as f:
        for row in csv.reader(f, delimiter=';'):
            if len(row) >= 3:
                try:
                    result[int(row[0])] = _strip_html(row[2])
                except ValueError:
                    pass
    return result


def _load_entity_attr_docs(csv_path: str) -> dict:
    """Returns {entity_line_int: {attr_position_int: attr_doc_line_int}}."""
    result = {}
    with open(csv_path, newline='', encoding='cp1252', errors='replace') as f:
        for row in csv.reader(f, delimiter=';'):
            if len(row) >= 3:
                try:
                    e_line = int(row[0])
                    pos = int(row[1])
                    a_line = int(row[2])
                    result.setdefault(e_line, {})[pos] = a_line
                except ValueError:
                    pass
    return result


# Map C++ type → Godot XML type name for doc generation
_CPP_TO_XML_TYPE = {
    "godot::String":             "String",
    "int64_t":                   "int",
    "double":                    "float",
    "bool":                      "bool",
    "godot::Array":              "Array",
    "godot::PackedStringArray":  "PackedStringArray",
    "godot::PackedInt64Array":   "PackedInt64Array",
    "godot::PackedFloat64Array": "PackedFloat64Array",
    "Ref<GDIFCEntityBase>":      "GDIFCEntityBase",
}


def emit_xml_doc(entity_name: str, parent_name: str | None,
                 attrs: list, entity_desc: str, attr_descs: dict,
                 inverse_attrs: list) -> str:
    """
    Emit a Godot XML doc file for one IFC entity class.
    attr_descs: {attr_position_in_own_attrs (0-based) -> plain_description}
    """
    parent = parent_name if parent_name else "GDIFCEntityBase"
    brief = entity_desc[:400] if entity_desc else ""
    lines = [
        '<?xml version="1.0" encoding="UTF-8" ?>',
        f'<class name="{entity_name}" inherits="{parent}"',
        '    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"',
        '    xsi:noNamespaceSchemaLocation="https://raw.githubusercontent.com/godotengine/godot/master/doc/class.xsd">',
        '\t<brief_description>',
        f'\t\t{brief}' if brief else '',
        '\t</brief_description>',
        '\t<description></description>',
        '\t<tutorials/>',
        '\t<methods/>',
        '\t<members>',
    ]
    for i, a in enumerate(attrs):
        if a["type_info"] is not None:
            cpp_type, variant_type, _, setter_helper, _ = a["type_info"]
            xml_type = _CPP_TO_XML_TYPE.get(cpp_type, "Variant")
            setter_attr = f'setter="set_{a["name"]}"' if setter_helper else 'setter=""'
            desc = attr_descs.get(i, "")
            lines.append(
                f'\t\t<member name="{a["name"]}" type="{xml_type}"'
                f' {setter_attr} getter="get_{a["name"]}">'
            )
            if desc:
                lines.append(f'\t\t\t{desc}')
            lines.append('\t\t</member>')
    for inv_name in inverse_attrs:
        lines.append(f'\t\t<member name="{inv_name}" type="Array" setter="" getter="get_{inv_name}">')
        lines.append('\t\t</member>')
    lines += ['\t</members>', '\t<signals/>', '\t<constants/>', '</class>']
    return '\n'.join(lines) + '\n'


def topological_sort(entities: dict) -> list:
    """Return entity names in parent-before-child order."""
    result = []
    visited = set()

    def visit(name: str):
        key = name.lower()
        if key in visited:
            return
        visited.add(key)
        ent = entities.get(name)
        if ent is None:
            return
        supertypes = list(ent.supertypes or [])
        if supertypes:
            visit(str(supertypes[0]))
        result.append(name)

    for name in entities:
        visit(str(name))
    return result


def build_attr_index_cache(sorted_names: list, entities: dict) -> dict:
    """
    For each entity, compute how many attributes appear in the full
    all_attributes() result BEFORE its own attributes begin.
    Returns dict: entity_name → start_index_of_own_attrs
    """
    cache = {}  # entity_name → total attribute count (own + inherited)

    def total_count(name: str) -> int:
        if name in cache:
            return cache[name]
        ent = entities.get(name)
        if ent is None:
            cache[name] = 0
            return 0
        supertypes = list(ent.supertypes or [])
        parent_count = total_count(str(supertypes[0])) if supertypes else 0
        own_count = len(list(ent.attributes or []))
        result = parent_count + own_count
        cache[name] = result
        return result

    for name in sorted_names:
        total_count(name)

    # Now compute start index for each entity
    start_index = {}
    for name in sorted_names:
        ent = entities.get(name)
        supertypes = list(ent.supertypes or []) if ent else []
        start_index[name] = total_count(str(supertypes[0])) if supertypes else 0

    return start_index, cache


# ─────────────────────────────────────────────────────────────────────────────
# Per-entity attribute info extraction
# ─────────────────────────────────────────────────────────────────────────────

def get_entity_attrs(entity_name: str, entity, mapping: Mapping) -> list:
    """
    Returns list of dicts:
      { 'name': str, 'snake': str, 'arg_type': str, 'type_info': tuple|None }
    Only EXPLICIT (non-derived) attributes are included; derived ones produce
    None type_info so they are counted in the index but skipped for code gen.
    """
    attrs = []
    for attr in (entity.attributes or []):
        attr_name = str(attr.name)
        try:
            arg_type = mapping.make_argument_type(attr)
        except Exception:
            # Derived attributes, expressions, or unsupported types
            arg_type = "SKIP"

        if arg_type == "SKIP" or "UNKNOWN" in arg_type:
            type_info = None  # count in index but generate nothing
        else:
            type_info = ARGTYPE_MAP.get(arg_type)
            if type_info is None:
                type_info = None  # unrecognised – skip codegen

        attrs.append({
            "name": attr_name,
            "arg_type": arg_type,
            "type_info": type_info,
        })
    return attrs


def get_entity_inverse_attrs(entity) -> list:
    """Return list of own inverse attribute names for this entity."""
    result = []
    if not entity.inverse:
        return result
    for ia in entity.inverse:
        try:
            result.append(str(ia.name))
        except Exception:
            pass
    return result


# ─────────────────────────────────────────────────────────────────────────────
# Code emission helpers
# ─────────────────────────────────────────────────────────────────────────────

def emit_class_decl(entity_name: str, parent_name: str | None, attrs: list,
                    inverse_attrs: list) -> str:
    """Emit a single class declaration block for the header."""
    gd_class = gd_class_name(entity_name)
    gd_parent = gd_class_name(parent_name) if parent_name else "GDIFCEntityBase"

    lines = [
        f"class {gd_class} : public {gd_parent} {{",
        f"    GDCLASS({gd_class}, {gd_parent})",
        "protected:",
        "    static void _bind_methods();",
    ]

    bindable = [a for a in attrs if a["type_info"] is not None]
    if bindable or inverse_attrs:
        lines.append("public:")
        for a in bindable:
            cpp_type, _, _, setter_helper, _ = a["type_info"]
            lines.append(f"    {cpp_type} get_{a['name']}();")
            if setter_helper is not None:
                lines.append(f"    void set_{a['name']}({cpp_type} v);")
        for inv_name in inverse_attrs:
            lines.append(f"    godot::Array get_{inv_name}();")
    lines.append("};")
    return "\n".join(lines)


def emit_bind_methods(entity_name: str, attrs: list, start_idx: int,
                      inverse_attrs: list) -> str:
    """Emit the _bind_methods() body for one entity."""
    gd_class = gd_class_name(entity_name)
    lines = [f"void {gd_class}::_bind_methods() {{"]

    idx = start_idx
    for a in attrs:
        if a["type_info"] is not None:
            cpp_type, variant_type, _, setter_helper, _ = a["type_info"]
            attr_name = a["name"]
            hint_part = f", {ENTITY_HINT}" if "OBJECT" in variant_type else ""
            lines.append(
                f'    ClassDB::bind_method(D_METHOD("get_{attr_name}"), &{gd_class}::get_{attr_name});')
            if setter_helper is not None:
                lines.append(
                    f'    ClassDB::bind_method(D_METHOD("set_{attr_name}","v"), &{gd_class}::set_{attr_name});')
                lines.append(
                    f'    ADD_PROPERTY(PropertyInfo({variant_type}, "{attr_name}"{hint_part}), "set_{attr_name}", "get_{attr_name}");')
            else:
                lines.append(
                    f'    ADD_PROPERTY(PropertyInfo({variant_type}, "{attr_name}"{hint_part}), "", "get_{attr_name}");')
        idx += 1

    for inv_name in inverse_attrs:
        lines.append(
            f'    ClassDB::bind_method(D_METHOD("get_{inv_name}"), &{gd_class}::get_{inv_name});')
        lines.append(
            f'    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "{inv_name}"), "", "get_{inv_name}");')

    lines.append("}")
    return "\n".join(lines)


def emit_getter(entity_name: str, attr_name: str, cpp_type: str, getter_helper: str,
                default_val: str, idx: int, needs_file: bool) -> str:
    gd_class = gd_class_name(entity_name)
    file_arg = ", file_" if needs_file else ""
    return (
        f"{cpp_type} {gd_class}::get_{attr_name}() {{\n"
        f"    if (!entity_) return {default_val};\n"
        f"    return {getter_helper}(entity_->get_attribute_value({idx}){file_arg});\n"
        f"}}"
    )


def emit_setter(entity_name: str, attr_name: str, cpp_type: str,
                setter_helper: str, idx: int) -> str:
    gd_class = gd_class_name(entity_name)
    return (
        f"void {gd_class}::set_{attr_name}({cpp_type} v) {{\n"
        f"    if (!entity_) return;\n"
        f"    {setter_helper}(entity_, {idx}, v);\n"
        f"}}"
    )

def emit_inverse_getter(entity_name: str, inv_name: str) -> str:
    """Emit a getter that delegates to get_inverse() for one inverse attr."""
    gd_class = gd_class_name(entity_name)
    return (
        f"godot::Array {gd_class}::get_{inv_name}() {{\n"
        f'    return get_inverse(godot::String("{inv_name}"));\n'
        f"}}"
    )

# ─────────────────────────────────────────────────────────────────────────────
# Main generation
# ─────────────────────────────────────────────────────────────────────────────

_HEADER_PREAMBLE = """\
// ============================================================
// AUTO-GENERATED by godot_codegen.py — do not edit manually.
// Generated from: {schema_name}
// ============================================================
#pragma once
#include "../gd_ifc_entity_base.h"
using namespace godot;

"""

_IMPL_PREAMBLE = """\
// ============================================================
// AUTO-GENERATED by godot_codegen.py — do not edit manually.
// ============================================================
#include "gd_ifc_entities.h"
using namespace godot;

"""

_REGISTER_PREAMBLE = """\
// ============================================================
// AUTO-GENERATED by godot_codegen.py — do not edit manually.
// ============================================================
#include "gd_ifc_entities.h"
#include <godot_cpp/core/class_db.hpp>

void register_all_gd_ifc_entities() {
"""

_REGISTER_EPILOGUE = "}\n"


def _shard_letter(entity_name: str) -> str:
    """Map entity name to shard A/B/C/D for impl splitting.
    All IFC entity names start with 'Ifc', so shard by character index 3
    (the first letter of the actual type name, e.g. 'W' in 'IfcWall').
    """
    # Skip the 'Ifc' prefix; use char at index 3 if available
    c = entity_name[3].upper() if len(entity_name) > 3 else 'A'
    if c <= 'E':
        return 'A'
    elif c <= 'L':
        return 'B'
    elif c <= 'R':
        return 'C'
    else:
        return 'D'


def generate(exp_path: str, output_dir: str):
    print(f"[godot_codegen] Parsing: {exp_path}")
    os.makedirs(output_dir, exist_ok=True)

    # ── Bootstrap parser if express_parser.py is missing ─────────────────────
    ep_path = os.path.join(_here, "express_parser.py")
    if not os.path.exists(ep_path):
        import subprocess
        bnf_path = os.path.join(_here, "express.bnf")
        print("[godot_codegen] Bootstrapping express_parser.py …")
        result = subprocess.run(
            [sys.executable, os.path.join(_here, "bootstrap.py"), bnf_path],
            capture_output=True, text=True)
        with open(ep_path, "w", encoding="utf-8") as f:
            f.write(result.stdout)
        print("[godot_codegen] Done bootstrapping.")

    # ── Parse ─────────────────────────────────────────────────────────────────
    # express_parser.parse() returns a Mapping; Schema lives at m.schema
    m = express_parser.parse(exp_path)
    s = m.schema

    schema_name = getattr(s, 'name', os.path.basename(exp_path))
    print(f"[godot_codegen] Schema: {schema_name}")

    entities = s.entities  # OrderedCaseInsensitiveDict

    # ── Topological sort ──────────────────────────────────────────────────────
    sorted_names = topological_sort({str(k): v for k, v in entities.items()})
    print(f"[godot_codegen] Entities: {len(sorted_names)}")

    ent_dict = {str(k): v for k, v in entities.items()}

    # ── Build attribute-index cache ───────────────────────────────────────────
    start_index_map, _ = build_attr_index_cache(sorted_names, ent_dict)

    # ── Collect per-entity information ───────────────────────────────────────
    entity_info = []  # list of (name, parent_name, attrs, start_idx, inv_attrs)
    for name in sorted_names:
        ent = ent_dict.get(name)
        if ent is None:
            continue
        supertypes = list(ent.supertypes or [])
        parent_name = str(supertypes[0]) if supertypes else None
        attrs = get_entity_attrs(name, ent, m)
        start_idx = start_index_map.get(name, 0)
        inv_attrs = get_entity_inverse_attrs(ent)
        entity_info.append((name, parent_name, attrs, start_idx, inv_attrs))

    # ── Load documentation from CSVs ─────────────────────────────────────────
    entity_doc_csv = os.path.join(_here, "DocEntity.csv")
    attr_doc_csv   = os.path.join(_here, "DocAttribute.csv")
    entity_attr_csv = os.path.join(_here, "DocEntityAttributes.csv")

    entity_docs_by_line  = _load_entity_docs(entity_doc_csv)  if os.path.exists(entity_doc_csv)  else {}
    attr_docs_by_line    = _load_attr_docs(attr_doc_csv)      if os.path.exists(attr_doc_csv)    else {}
    entity_attr_links    = _load_entity_attr_docs(entity_attr_csv) if os.path.exists(entity_attr_csv) else {}

    # Build reverse lookup: entity_name (lowercased) → (doc_line, description)
    entity_name_to_doc = {}
    for doc_line, (ename, edesc) in entity_docs_by_line.items():
        entity_name_to_doc[ename.lower()] = (doc_line, edesc)

    print(f"[godot_codegen] Loaded docs for {len(entity_name_to_doc)} entities.")

    # ── Open output files ─────────────────────────────────────────────────────
    header_lines = [_HEADER_PREAMBLE.format(schema_name=schema_name)]
    impl_shards = {k: [_IMPL_PREAMBLE] for k in ('A', 'B', 'C', 'D')}
    reg_lines = [_REGISTER_PREAMBLE]

    # ── Forward declarations ──────────────────────────────────────────────────
    header_lines.append("// Forward declarations\n")
    for name, _, _, _, _ in entity_info:
        header_lines.append(f"class {gd_class_name(name)};")
    header_lines.append("\n")

    # ── Class definitions + implementations ───────────────────────────────────
    for name, parent_name, attrs, start_idx, inv_attrs in entity_info:
        # Header declaration
        header_lines.append(emit_class_decl(name, parent_name, attrs, inv_attrs))
        header_lines.append("")

        # Shard key
        shard = _shard_letter(name)
        impl = impl_shards[shard]

        # _bind_methods
        impl.append(emit_bind_methods(name, attrs, start_idx, inv_attrs))
        impl.append("")

        # Getters and setters
        idx = start_idx
        for a in attrs:
            if a["type_info"] is not None:
                cpp_type, _, getter_helper, setter_helper, default_val = a["type_info"]
                needs_file = getter_helper in ("gd_attr_entity", "gd_attr_agg_entity")
                impl.append(emit_getter(name, a["name"], cpp_type, getter_helper,
                                        default_val, idx, needs_file))
                impl.append("")
                if setter_helper is not None:
                    impl.append(emit_setter(name, a["name"], cpp_type, setter_helper, idx))
                    impl.append("")
            idx += 1

        # Inverse getters
        for inv_name in inv_attrs:
            impl.append(emit_inverse_getter(name, inv_name))
            impl.append("")

        # Registration: ClassDB + factory
        gd = gd_class_name(name)
        reg_lines.append(f"    ClassDB::register_class<{gd}>();")
        reg_lines.append(
            f"    GDIFCEntityBase::register_factory(\"{name}\",\n"
            f"        [](IfcUtil::IfcBaseClass* e, std::shared_ptr<IfcParse::IfcFile> f)"
            f" -> Ref<GDIFCEntityBase> {{\n"
            f"            Ref<{gd}> obj;\n"
            f"            obj.instantiate();\n"
            f"            obj->init(e, f);\n"
            f"            return Ref<GDIFCEntityBase>(obj);\n"
            f"        }});"
        )

    reg_lines.append(_REGISTER_EPILOGUE)

    # ── Write files ───────────────────────────────────────────────────────────
    header_path = os.path.join(output_dir, "gd_ifc_entities.h")
    with open(header_path, "w", encoding="utf-8") as f:
        f.write("\n".join(header_lines))
    print(f"[godot_codegen] Wrote {header_path}")

    for shard, lines in impl_shards.items():
        impl_path = os.path.join(output_dir, f"gd_ifc_entities_impl_{shard}.cpp")
        with open(impl_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        print(f"[godot_codegen] Wrote {impl_path}")

    reg_path = os.path.join(output_dir, "gd_ifc_entities_register.cpp")
    with open(reg_path, "w", encoding="utf-8") as f:
        f.write("\n".join(reg_lines))
    print(f"[godot_codegen] Wrote {reg_path}")

    total_attrs = sum(len([a for a in attrs if a["type_info"]]) for _, _, attrs, _, _ in entity_info)
    total_inv = sum(len(inv) for _, _, _, _, inv in entity_info)
    print(f"[godot_codegen] Inverse attribute getters: {total_inv}")
    print(f"[godot_codegen] Generated {len(entity_info)} entity classes, "
          f"{total_attrs} typed accessors.")

    # ── Write Godot XML doc files ─────────────────────────────────────────────
    doc_dir = os.path.join(output_dir, "doc_classes")
    os.makedirs(doc_dir, exist_ok=True)

    for name, parent_name, attrs, _, inv_attrs in entity_info:
        e_line, e_desc = entity_name_to_doc.get(name.lower(), (None, ""))
        attr_descs = {}
        if e_line is not None and e_line in entity_attr_links:
            for pos, a_line in entity_attr_links[e_line].items():
                attr_descs[pos] = attr_docs_by_line.get(a_line, "")
        xml_path = os.path.join(doc_dir, f"{name}.xml")
        with open(xml_path, "w", encoding="utf-8") as fxml:
            fxml.write(emit_xml_doc(name, parent_name, attrs, e_desc, attr_descs, inv_attrs))

    print(f"[godot_codegen] Wrote {len(entity_info)} XML doc files to {doc_dir}")

    # ── Generate compressed doc data .cpp ────────────────────────────────────
    sys.path.insert(0, os.path.join(_here, "..", "godot-cpp"))
    try:
        from doc_source_generator import generate_doc_source_from_directory
        gen_cpp_path = os.path.join(output_dir, "gd_ifc_doc_data.gen.cpp")
        generate_doc_source_from_directory(gen_cpp_path, doc_dir)
        print(f"[godot_codegen] Wrote {gen_cpp_path}")
    except ImportError as e:
        print(f"[godot_codegen] WARNING: could not import doc_source_generator: {e}")


# ─────────────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    generate(sys.argv[1], sys.argv[2])
