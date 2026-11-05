#!/usr/bin/env python3
"""
Godot GDExtension C++ code generator for IFC Express schemas.

Generates per-entity C++ GDExtension classes that expose all IFC entity
attributes as typed Godot getters/setters, mirroring the IFC inheritance
hierarchy inside Godot's class system.

Several schemas can be given: one class is generated per entity name across all of them, so a
script using IfcWall works with files of every schema. The first schema is the primary one: its
hierarchy, attribute names and types win. Entities and attributes only found in the others are
added, and attributes the primary schema renamed are read under their old name in files of the
older schemas. Accessors look attributes up by name in the entity's own schema. References to
entities are typed GDIFCEntityBase properties; every other attribute is a Variant property, read
as the type the file's schema declares, and null when it's unset or missing from that schema.

Usage:
    python3 godot_codegen.py <primary.exp> [<other.exp> ...] <output_dir>
    e.g.:
    python3 godot_codegen.py schemas/IFC4X3_ADD2.exp schemas/IFC4.exp schemas/IFC2X3_TC1.exp ../GDIFC/src/generated/

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
    doc_classes/*.xml                  - Class reference, compiled into gd_ifc_doc_data.gen.cpp
"""

import csv
import html
import os
import re
import sys

# ── locate ifcopenshell-express package ──────────────────────────────────────
_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)

# ─────────────────────────────────────────────────────────────────────────────
# Type-mapping tables  (Express argument type → Godot C++ side)
# ─────────────────────────────────────────────────────────────────────────────
# Each entry: (cpp_type, variant_type_enum, getter_helper, default_val). Only entity references
# keep these types as properties (see value_info); the others name what a Variant property holds.
ARGTYPE_MAP = {
    "IfcUtil::Argument_INT":     ("int64_t",                   "Variant::INT",                "gd_attr_int",        "0"),
    "IfcUtil::Argument_DOUBLE":  ("double",                    "Variant::FLOAT",              "gd_attr_double",     "0.0"),
    "IfcUtil::Argument_STRING":  ("godot::String",             "Variant::STRING",             "gd_attr_string",     "godot::String()"),
    "IfcUtil::Argument_BOOL":    ("bool",                      "Variant::BOOL",               "gd_attr_bool",       "false"),
    "IfcUtil::Argument_LOGICAL": ("godot::String",             "Variant::STRING",             "gd_attr_string",     "godot::String()"),
    "IfcUtil::Argument_BINARY":  ("godot::String",             "Variant::STRING",             "gd_attr_string",     "godot::String()"),
    "IfcUtil::Argument_ENUMERATION": (
        "godot::String", "Variant::STRING", "gd_attr_string", "godot::String()"),
    "IfcUtil::Argument_ENTITY_INSTANCE": (
        "Ref<GDIFCEntityBase>", "Variant::OBJECT", "gd_attr_entity", "Ref<GDIFCEntityBase>()"),
    "IfcUtil::Argument_AGGREGATE_OF_INT": (
        "godot::PackedInt64Array",   "Variant::PACKED_INT64_ARRAY",   "gd_attr_agg_int",    "godot::PackedInt64Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_DOUBLE": (
        "godot::PackedFloat64Array", "Variant::PACKED_FLOAT64_ARRAY", "gd_attr_agg_double", "godot::PackedFloat64Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_STRING": (
        "godot::PackedStringArray",  "Variant::PACKED_STRING_ARRAY",  "gd_attr_agg_string", "godot::PackedStringArray()"),
    "IfcUtil::Argument_AGGREGATE_OF_BINARY": (
        "godot::PackedStringArray",  "Variant::PACKED_STRING_ARRAY",  "gd_attr_agg_string", "godot::PackedStringArray()"),
    "IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_agg_entity", "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_INT": (
        "godot::Array", "Variant::ARRAY", "gd_attr_array", "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_DOUBLE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_array", "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_STRING": (
        "godot::Array", "Variant::ARRAY", "gd_attr_array", "godot::Array()"),
    "IfcUtil::Argument_AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE": (
        "godot::Array", "Variant::ARRAY", "gd_attr_array", "godot::Array()"),
}

# Every attribute but an entity reference: null when unset, otherwise the type the file's schema
# declares, read by gd_get_attribute
VARIANT_TYPE = ("godot::Variant", "Variant::NIL", "gd_get_attribute", "godot::Variant()")

# Argument types holding a plain value, whose declared type may change when the attribute is
# renamed (see same_value). Enumerations, entities and selects must keep their declared type.
PLAIN_ARGTYPES = {
    "IfcUtil::Argument_INT", "IfcUtil::Argument_DOUBLE", "IfcUtil::Argument_STRING",
    "IfcUtil::Argument_BOOL", "IfcUtil::Argument_LOGICAL", "IfcUtil::Argument_BINARY",
    "IfcUtil::Argument_AGGREGATE_OF_INT", "IfcUtil::Argument_AGGREGATE_OF_DOUBLE",
    "IfcUtil::Argument_AGGREGATE_OF_STRING",
}

ENTITY_HINT = 'PROPERTY_HINT_RESOURCE_TYPE, "GDIFCEntityBase"'
VARIANT_PROPERTY = 'PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT'

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

def join_names(names: list) -> str:
    """["IFC4", "IFC2X3"] → "IFC2X3 and IFC4". Sorting puts IFC schemas oldest first."""
    names = sorted(names)
    return names[0] if len(names) == 1 else ", ".join(names[:-1]) + " and " + names[-1]

# ─────────────────────────────────────────────────────────────────────────────
# Documentation helpers  (CSV → Godot XML)
# ─────────────────────────────────────────────────────────────────────────────

def _strip_html(text: str) -> str:
    """Remove HTML tags, unescape entities, and normalise whitespace."""
    text = re.sub(r'<[^>]+>', ' ', text)
    text = html.unescape(text)
    return ' '.join(text.split())


def _xml(text: str) -> str:
    return html.escape(text, quote=False)


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
    """Returns {doc_line_int: (attr_name, plain_description)}."""
    result = {}
    with open(csv_path, newline='', encoding='cp1252', errors='replace') as f:
        for row in csv.reader(f, delimiter=';'):
            if len(row) >= 3:
                try:
                    result[int(row[0])] = (row[1].strip(), _strip_html(row[2]))
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


def load_docs() -> tuple:
    """Returns ({entity_lower: description}, {entity_lower: {attr_name: description}})."""
    entity_doc_csv = os.path.join(_here, "DocEntity.csv")
    attr_doc_csv = os.path.join(_here, "DocAttribute.csv")
    entity_attr_csv = os.path.join(_here, "DocEntityAttributes.csv")

    entity_docs = _load_entity_docs(entity_doc_csv) if os.path.exists(entity_doc_csv) else {}
    attr_docs = _load_attr_docs(attr_doc_csv) if os.path.exists(attr_doc_csv) else {}
    links = _load_entity_attr_docs(entity_attr_csv) if os.path.exists(entity_attr_csv) else {}

    entity_desc = {}
    entity_attr_desc = {}
    for line, (name, desc) in entity_docs.items():
        entity_desc[name.lower()] = desc
        by_name = entity_attr_desc.setdefault(name.lower(), {})
        for _, a_line in sorted(links.get(line, {}).items()):
            if a_line in attr_docs:
                a_name, a_desc = attr_docs[a_line]
                by_name.setdefault(a_name, a_desc)
    return entity_desc, entity_attr_desc


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
    "godot::Variant":            "Variant",
}


def holds_description(a, all_schemas: list) -> str:
    """What a Variant attribute holds, e.g. "A float, or null when unset."""
    kinds = a.kinds_by_schema()
    if len(kinds) == 1:
        held = kinds[0][0]
    else:
        held = ", ".join(f"{kind} in {join_names(schemas)} files" for kind, schemas in kinds)
    return f"{held[0].upper()}{held[1:]}, or null when unset."


def emit_xml_doc(cls, entity_desc: str, attr_desc: dict, all_schemas: list) -> str:
    """Emit a Godot XML doc file for one generated class."""
    parent = cls.parent if cls.parent else "GDIFCEntityBase"
    brief = entity_desc[:400] if entity_desc else ""
    description = ""
    if cls.schemas != all_schemas:
        description = f"Only in {join_names(cls.schemas)} files."
    lines = [
        '<?xml version="1.0" encoding="UTF-8" ?>',
        f'<class name="{cls.name}" inherits="{parent}"',
        '    xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"',
        '    xsi:noNamespaceSchemaLocation="https://raw.githubusercontent.com/godotengine/godot/master/doc/class.xsd">',
        '\t<brief_description>',
        f'\t\t{_xml(brief)}' if brief else '',
        '\t</brief_description>',
        f'\t<description>{_xml(description)}</description>',
        # Open and close tags: Godot's doc parser reads a self-closing one as still open
        '\t<tutorials>',
        '\t</tutorials>',
        '\t<methods>',
        '\t</methods>',
        '\t<members>',
    ]
    for a in cls.attrs:
        if a.type_info is None:
            continue
        xml_type = _CPP_TO_XML_TYPE.get(a.type_info[0], "Variant")
        notes = [attr_desc.get(a.name, "")]
        for old_name, schemas in a.renamed_in():
            notes.append(f"In {join_names(schemas)} files, this is the [code]{old_name}[/code] attribute.")
        if a.type_info is VARIANT_TYPE:
            notes.append(holds_description(a, all_schemas))
        if a.schemas != all_schemas:
            notes.append(f"Only in {join_names(a.schemas)} files: in others it reads as null and can't be set.")
        lines.append(f'\t\t<member name="{a.name}" type="{xml_type}" setter="set_{a.name}" getter="get_{a.name}">')
        text = " ".join(n for n in notes if n)
        if text:
            lines.append(f'\t\t\t{_xml(text)}')
        lines.append('\t\t</member>')
    for inv in cls.inverses:
        lines.append(f'\t\t<member name="{inv.name}" type="Array" setter="" getter="get_{inv.name}">')
        if inv.schemas != all_schemas:
            lines.append(f'\t\t\t{_xml(f"Only in {join_names(inv.schemas)} files: in others it is empty.")}')
        lines.append('\t\t</member>')
    lines += ['\t</members>', '\t<signals>', '\t</signals>', '\t<constants>', '\t</constants>', '</class>']
    return '\n'.join(lines) + '\n'


# ─────────────────────────────────────────────────────────────────────────────
# Schema model
# ─────────────────────────────────────────────────────────────────────────────

class SchemaAttr:
    """An explicit attribute as one schema declares it."""
    def __init__(self, name, arg_type, declared, holds_values=True, holds_entities=False):
        self.name = name
        self.arg_type = arg_type    # IfcUtil::Argument_*, or SKIP when it can't be mapped
        self.declared = declared    # declared type, e.g. IfcLabel or set[1:?] of IfcRepresentationItem
        # Whether it can hold plain values (anything but entities and selects of them) and entities
        self.holds_values = holds_values
        self.holds_entities = holds_entities

    @property
    def type_info(self):
        if self.arg_type == "SKIP" or "UNKNOWN" in self.arg_type:
            return None
        return ARGTYPE_MAP.get(self.arg_type)

    @property
    def is_reference(self):
        """A single entity, or a select of only entities: a typed GDIFCEntityBase property"""
        return self.arg_type == "IfcUtil::Argument_ENTITY_INSTANCE" and not self.holds_values

    @property
    def kind(self):
        """What its value reads as, for the class reference"""
        t = self.arg_type.split("_", 1)[1]
        if t == "ENTITY_INSTANCE":
            if not self.holds_values:
                return "a GDIFCEntityBase"
            return "a value or a GDIFCEntityBase" if self.holds_entities else "a value (String, float, int or bool)"
        if t in ("AGGREGATE_OF_ENTITY_INSTANCE", "AGGREGATE_OF_AGGREGATE_OF_ENTITY_INSTANCE"):
            if not self.holds_values:
                inner = "GDIFCEntityBase entities"
            elif self.holds_entities:
                inner = "values and GDIFCEntityBase entities"
            else:
                inner = "values (String, float, int or bool)"
            return ("an Array of Arrays of " if "AGGREGATE_OF_AGGREGATE" in t else "an Array of ") + inner
        if t == "LOGICAL":
            return 'a String ("TRUE", "FALSE" or "UNKNOWN")'
        if t == "BINARY":
            return "a String of 0s and 1s"
        if t == "ENUMERATION":
            return "a String, one of the enumeration's values"
        xml = _CPP_TO_XML_TYPE.get(self.type_info[0], "Variant")
        return ("an " if xml[0] in "AEIOU" or xml == "int" else "a ") + xml


class Schema:
    """One parsed EXPRESS schema."""
    def __init__(self, exp_path: str):
        import express_parser
        print(f"[godot_codegen] Parsing: {exp_path}")
        m = express_parser.parse(exp_path)
        self.mapping = m
        self.name = str(getattr(m.schema, 'name', os.path.basename(exp_path)))
        self.entities = {str(k): v for k, v in m.schema.entities.items()}
        self._full = {}
        self._full_inverses = {}
        print(f"[godot_codegen] Schema: {self.name}, {len(self.entities)} entities")

    def __contains__(self, name):
        return name in self.entities

    def parent(self, name):
        supertypes = list(self.entities[name].supertypes or [])
        return str(supertypes[0]) if supertypes else None

    def is_abstract(self, name):
        return bool(self.entities[name].abstract)

    def ancestors(self, name):
        """The entity and its supertypes, nearest first."""
        while name is not None:
            yield name
            name = self.parent(name)

    def own_attrs(self, name):
        attrs = []
        for attr in (self.entities[name].attributes or []):
            try:
                arg_type = self.mapping.make_argument_type(attr)
            except Exception:
                # Derived attributes, expressions, or unsupported types
                arg_type = "SKIP"
            values, entities = self.holds(attr.type)
            attrs.append(SchemaAttr(str(attr.name), arg_type, str(attr.type), values, entities))
        return attrs

    def holds(self, attr_type):
        """(plain values, entities) an attribute of this type can hold, through aggregates and selects"""
        import nodes
        schema = self.mapping.schema
        while isinstance(attr_type, nodes.AggregationType):
            attr_type = attr_type.type
        name = str(attr_type)
        if schema.is_entity(name):
            return False, True
        if schema.is_select(name):
            held = [self.holds(option) for option in schema.selects[name].values]
            return any(v for v, _ in held), any(e for _, e in held)
        return True, False

    def full_attrs(self, name):
        """Every explicit attribute of the entity, inherited first, in STEP order."""
        if name not in self._full:
            parent = self.parent(name)
            self._full[name] = (self.full_attrs(parent) if parent else []) + self.own_attrs(name)
        return self._full[name]

    def full_inverses(self, name):
        if name not in self._full_inverses:
            parent = self.parent(name)
            own = []
            for inv in (self.entities[name].inverse or []):
                try:
                    own.append(str(inv.name))
                except Exception:
                    pass
            self._full_inverses[name] = (self.full_inverses(parent) if parent else []) + own
        return self._full_inverses[name]


class GenAttr:
    """An attribute property of a generated class."""
    def __init__(self, name):
        self.name = name
        self.found = []     # (schema name, SchemaAttr) wherever the attribute is read
        self.aliases = []   # (schema name, entity name, old name)
        self.schemas = []
        self.type_info = None

    def renamed_in(self):
        """[(old name, [schema names])]"""
        by_name = {}
        for schema, _, old_name in self.aliases:
            schemas = by_name.setdefault(old_name, [])
            if schema not in schemas:
                schemas.append(schema)
        return list(by_name.items())

    def kinds_by_schema(self):
        """[(what it reads as, [schema names])]"""
        by_kind = {}
        for schema, attr in self.found:
            if attr.type_info is not None:
                schemas = by_kind.setdefault(attr.kind, [])
                if schema not in schemas:
                    schemas.append(schema)
        return list(by_kind.items())


class GenInverse:
    def __init__(self, name):
        self.name = name
        self.schemas = []


class GenClass:
    def __init__(self, name, parent, schemas):
        self.name = name
        self.parent = parent
        self.schemas = schemas
        self.attrs = []     # GenAttr declared by this class
        self.inverses = []  # GenInverse declared by this class


def related_names(a: str, b: str) -> bool:
    """NumberOfRiser / NumberOfRisers, DeltaT_Constant / DeltaTConstant."""
    a, b = a.lower().replace("_", ""), b.lower().replace("_", "")
    return a in b or b in a


def same_value(new, old) -> bool:
    """Whether a renamed attribute still holds the same value."""
    if new.arg_type != old.arg_type:
        return False
    if new.declared == old.declared:
        return True
    # Plain values whose declared type changed, such as INTEGER to IfcInteger. The names must be
    # related too, since reordered attributes often line up with one of the same kind.
    return new.arg_type in PLAIN_ARGTYPES and related_names(new.name, old.name)


def find_aliases(schemas: list) -> list:
    """Attributes the primary schema renamed: (schema, entity, new name, old name).

    An entity in both schemas has a renamed attribute where, at the same position, the primary
    schema has a name the other lacks and the other a name the primary lacks, and both hold the
    same value. Only the topmost entity of each rename is kept: its subtypes inherit it.
    """
    primary = schemas[0]
    found = []
    for older in schemas[1:]:
        for name in older.entities:
            if name not in primary:
                continue
            new_attrs, old_attrs = primary.full_attrs(name), older.full_attrs(name)
            new_names = {a.name for a in new_attrs}
            old_names = {a.name for a in old_attrs}
            for new, old in zip(new_attrs, old_attrs):
                if new.name == old.name or new.name in old_names or old.name in new_names:
                    continue
                if same_value(new, old):
                    found.append((older, name, new.name, old.name))
    keys = {(older.name, name, new, old) for older, name, new, old in found}
    result = []
    for older, name, new, old in found:
        parent = older.parent(name)
        if parent is not None and any((older.name, p, new, old) in keys for p in older.ancestors(parent)):
            continue
        result.append((older, name, new, old))
    return result


def build_classes(schemas: list) -> list:
    """One class per entity name of every schema, parents before children."""
    primary = schemas[0]
    all_schema_names = [s.name for s in schemas]

    # Hierarchy: the first schema with the entity decides its parent
    names, parent_of, schemas_of = [], {}, {}
    for schema in schemas:
        for name in schema.entities:
            if name not in parent_of:
                names.append(name)
                parent_of[name] = schema.parent(name)
            schemas_of.setdefault(name, []).append(schema.name)

    ordered, visited = [], set()
    def visit(name):
        if name in visited:
            return
        visited.add(name)
        if parent_of[name]:
            visit(parent_of[name])
        ordered.append(name)
    for name in names:
        visit(name)

    classes = {name: GenClass(name, parent_of[name], [s for s in all_schema_names if s in schemas_of[name]])
               for name in ordered}

    def godot_ancestors(name):
        while name is not None:
            yield name
            name = parent_of[name]

    # Renames, and the old names they cover for an entity of a schema
    aliases = find_aliases(schemas)
    renamed = {}  # (schema name, entity) -> {old name: new name}, including inherited renames
    for older, name, new, old in aliases:
        for entity in older.entities:
            if name in older.ancestors(entity):
                renamed.setdefault((older.name, entity), {})[old] = new

    # Names the primary schema declares on a class's descendants. An older schema declaring one
    # higher up leaves it there, so it doesn't appear on the primary's other subtypes.
    below_attrs = {name: set() for name in ordered}
    below_inverses = {name: set() for name in ordered}
    for name in reversed(ordered):
        parent = parent_of[name]
        if parent is None:
            continue
        below_attrs[parent] |= below_attrs[name]
        below_inverses[parent] |= below_inverses[name]
        if name in primary:
            below_attrs[parent] |= {a.name for a in primary.own_attrs(name)}
            parent_inverses = set(primary.full_inverses(primary.parent(name))) if primary.parent(name) else set()
            below_inverses[parent] |= set(primary.full_inverses(name)) - parent_inverses

    # Attributes: a class declares the names its entity has in any schema, unless an ancestor
    # declares them. Old names of renamed attributes are read through the new name.
    declared_by = {}  # class -> {attribute name: GenAttr}, including inherited ones
    inverse_names = {}  # class -> set of inverse names, including inherited ones
    for name in ordered:
        cls = classes[name]
        inherited = dict(declared_by.get(cls.parent, {}))
        inherited_inverses = set(inverse_names.get(cls.parent, set()))
        for schema in schemas:
            if name not in schema:
                continue
            covered = renamed.get((schema.name, name), {})
            for attr in schema.full_attrs(name):
                if attr.name in inherited or attr.name in inherited_inverses:
                    continue
                # A renamed attribute, read through its new name
                if covered.get(attr.name) in inherited:
                    continue
                if schema is not primary and attr.name in below_attrs[name] and schema.is_abstract(name):
                    continue
                gen = GenAttr(attr.name)
                cls.attrs.append(gen)
                inherited[attr.name] = gen
        for schema in schemas:
            if name not in schema:
                continue
            for inv in schema.full_inverses(name):
                if inv in inherited_inverses:
                    continue
                if schema is not primary and inv in below_inverses[name] and schema.is_abstract(name):
                    continue
                if inv in inherited:
                    print(f"[godot_codegen] WARNING: {name}.{inv} is both an attribute and an inverse; the inverse is left out")
                    continue
                cls.inverses.append(GenInverse(inv))
                inherited_inverses.add(inv)
        declared_by[name] = inherited
        inverse_names[name] = inherited_inverses

    # Where each attribute is read, through its own name or a rename
    for schema in schemas:
        for name in schema.entities:
            covered = renamed.get((schema.name, name), {})
            attrs = declared_by[name]
            for attr in schema.full_attrs(name):
                gen = attrs.get(attr.name)
                if gen is None and attr.name in covered:
                    gen = attrs.get(covered[attr.name])
                if gen is None:
                    # Fine for abstract entities: their subtypes declare the attribute
                    if not schema.is_abstract(name):
                        print(f"[godot_codegen] WARNING: {schema.name} {name}.{attr.name} has no property")
                    continue
                gen.found.append((schema.name, attr))
                if schema.name not in gen.schemas:
                    gen.schemas.append(schema.name)
            inverse_classes = {}
            for ancestor in godot_ancestors(name):
                for inv in classes[ancestor].inverses:
                    inverse_classes.setdefault(inv.name, inv)
            for inv in schema.full_inverses(name):
                gen = inverse_classes.get(inv)
                if gen is not None and schema.name not in gen.schemas:
                    gen.schemas.append(schema.name)

    for older, name, new, old in aliases:
        gen = declared_by[name].get(new)
        if gen is not None:
            gen.aliases.append((older.name, name, old))

    # Types: a typed reference when every schema declares an entity there, else Variant
    for cls in classes.values():
        for gen in cls.attrs:
            gen.schemas = [s for s in all_schema_names if s in gen.schemas]
            found = [attr for _, attr in gen.found if attr.type_info is not None]
            if not found:
                continue
            gen.type_info = found[0].type_info if all(attr.is_reference for attr in found) else VARIANT_TYPE
        for inv in cls.inverses:
            inv.schemas = [s for s in all_schema_names if s in inv.schemas]

    return [classes[name] for name in ordered]


# ─────────────────────────────────────────────────────────────────────────────
# Code emission helpers
# ─────────────────────────────────────────────────────────────────────────────

def alias_table_name(cls, attr) -> str:
    return f"{cls.name}_{attr.name}_aliases"


def emit_alias_table(cls, attr) -> str:
    entries = [f'    {{"{schema}", "{entity.upper()}", "{old}"}},' for schema, entity, old in attr.aliases]
    return "\n".join([f"static const GDIFCAttributeAlias {alias_table_name(cls, attr)}[] = {{"]
                     + entries + ["    {nullptr, nullptr, nullptr},", "};"])


def emit_class_decl(cls) -> str:
    """Emit a single class declaration block for the header."""
    gd_class = gd_class_name(cls.name)
    gd_parent = gd_class_name(cls.parent) if cls.parent else "GDIFCEntityBase"

    lines = [
        f"class {gd_class} : public {gd_parent} {{",
        f"    GDCLASS({gd_class}, {gd_parent})",
        "protected:",
        "    static void _bind_methods();",
    ]

    bindable = [a for a in cls.attrs if a.type_info is not None]
    if bindable or cls.inverses:
        lines.append("public:")
        for a in bindable:
            cpp_type = a.type_info[0]
            lines.append(f"    {cpp_type} get_{a.name}();")
            lines.append(f"    void set_{a.name}({cpp_type} v);")
        for inv in cls.inverses:
            lines.append(f"    godot::Array get_{inv.name}();")
    lines.append("};")
    return "\n".join(lines)


def emit_bind_methods(cls) -> str:
    """Emit the _bind_methods() body for one entity."""
    gd_class = gd_class_name(cls.name)
    lines = [f"void {gd_class}::_bind_methods() {{"]

    for a in cls.attrs:
        if a.type_info is None:
            continue
        variant_type = a.type_info[1]
        if a.type_info is VARIANT_TYPE:
            hint_part = f", {VARIANT_PROPERTY}"
        elif "OBJECT" in variant_type:
            hint_part = f", {ENTITY_HINT}"
        else:
            hint_part = ""
        lines.append(
            f'    ClassDB::bind_method(D_METHOD("get_{a.name}"), &{gd_class}::get_{a.name});')
        lines.append(
            f'    ClassDB::bind_method(D_METHOD("set_{a.name}","v"), &{gd_class}::set_{a.name});')
        lines.append(
            f'    ADD_PROPERTY(PropertyInfo({variant_type}, "{a.name}"{hint_part}), "set_{a.name}", "get_{a.name}");')

    for inv in cls.inverses:
        lines.append(
            f'    ClassDB::bind_method(D_METHOD("get_{inv.name}"), &{gd_class}::get_{inv.name});')
        lines.append(
            f'    ADD_PROPERTY(PropertyInfo(Variant::ARRAY, "{inv.name}"), "", "get_{inv.name}");')

    lines.append("}")
    return "\n".join(lines)


def emit_getter(cls, a) -> str:
    gd_class = gd_class_name(cls.name)
    cpp_type, _, getter_helper, default_val = a.type_info
    alias_arg = f", {alias_table_name(cls, a)}" if a.aliases else ""
    if a.type_info is VARIANT_TYPE:
        return (
            f"{cpp_type} {gd_class}::get_{a.name}() {{\n"
            f'    return gd_get_attribute(entity_, "{a.name}", file_{alias_arg});\n'
            f"}}"
        )
    return (
        f"{cpp_type} {gd_class}::get_{a.name}() {{\n"
        f'    const ptrdiff_t i = gd_attr_index(entity_, "{a.name}"{alias_arg});\n'
        f"    return i < 0 ? {default_val} : {getter_helper}(entity_->get_attribute_value(i), file_);\n"
        f"}}"
    )


def emit_setter(cls, a) -> str:
    gd_class = gd_class_name(cls.name)
    alias_arg = f", {alias_table_name(cls, a)}" if a.aliases else ""
    return (
        f"void {gd_class}::set_{a.name}({a.type_info[0]} v) {{\n"
        f'    gd_set_attribute(entity_, "{a.name}", v{alias_arg});\n'
        f"}}"
    )

def emit_inverse_getter(cls, inv) -> str:
    """Emit a getter that delegates to get_inverse() for one inverse attr."""
    gd_class = gd_class_name(cls.name)
    return (
        f"godot::Array {gd_class}::get_{inv.name}() {{\n"
        f'    return get_inverse(godot::String("{inv.name}"));\n'
        f"}}"
    )

# ─────────────────────────────────────────────────────────────────────────────
# Main generation
# ─────────────────────────────────────────────────────────────────────────────

_HEADER_PREAMBLE = """\
// ============================================================
// AUTO-GENERATED by godot_codegen.py — do not edit manually.
// Generated from: {schema_names}
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


def _write(path: str, text: str):
    # LF on every platform, like the rest of the repository
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def generate(exp_paths: list, output_dir: str):
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

    # ── Parse and merge ───────────────────────────────────────────────────────
    schemas = [Schema(path) for path in exp_paths]
    all_schema_names = [s.name for s in schemas]
    classes = build_classes(schemas)
    print(f"[godot_codegen] Classes: {len(classes)}")

    entity_desc, entity_attr_desc = load_docs()
    print(f"[godot_codegen] Loaded docs for {len(entity_desc)} entities.")

    # ── Open output files ─────────────────────────────────────────────────────
    header_lines = [_HEADER_PREAMBLE.format(schema_names=", ".join(all_schema_names))]
    impl_shards = {k: [_IMPL_PREAMBLE] for k in ('A', 'B', 'C', 'D')}
    reg_lines = [_REGISTER_PREAMBLE]

    # ── Forward declarations ──────────────────────────────────────────────────
    header_lines.append("// Forward declarations\n")
    for cls in classes:
        header_lines.append(f"class {gd_class_name(cls.name)};")
    header_lines.append("\n")

    # ── Class definitions + implementations ───────────────────────────────────
    for cls in classes:
        # Header declaration
        header_lines.append(emit_class_decl(cls))
        header_lines.append("")

        # Shard key
        impl = impl_shards[_shard_letter(cls.name)]

        # Renamed attributes
        for a in cls.attrs:
            if a.type_info is not None and a.aliases:
                impl.append(emit_alias_table(cls, a))
                impl.append("")

        # _bind_methods
        impl.append(emit_bind_methods(cls))
        impl.append("")

        # Getters and setters
        for a in cls.attrs:
            if a.type_info is not None:
                impl.append(emit_getter(cls, a))
                impl.append("")
                impl.append(emit_setter(cls, a))
                impl.append("")

        # Inverse getters
        for inv in cls.inverses:
            impl.append(emit_inverse_getter(cls, inv))
            impl.append("")

        # Registration: ClassDB + factory
        gd = gd_class_name(cls.name)
        reg_lines.append(f"    ClassDB::register_class<{gd}>();")
        reg_lines.append(
            f"    GDIFCEntityBase::register_factory(\"{cls.name}\",\n"
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
    _write(header_path, "\n".join(header_lines))
    print(f"[godot_codegen] Wrote {header_path}")

    for shard, lines in impl_shards.items():
        impl_path = os.path.join(output_dir, f"gd_ifc_entities_impl_{shard}.cpp")
        _write(impl_path, "\n".join(lines))
        print(f"[godot_codegen] Wrote {impl_path}")

    reg_path = os.path.join(output_dir, "gd_ifc_entities_register.cpp")
    _write(reg_path, "\n".join(reg_lines))
    print(f"[godot_codegen] Wrote {reg_path}")

    typed = [a for cls in classes for a in cls.attrs if a.type_info is not None]
    print(f"[godot_codegen] Inverse attribute getters: {sum(len(cls.inverses) for cls in classes)}")
    print(f"[godot_codegen] Generated {len(classes)} entity classes, {len(typed)} typed accessors "
          f"({sum(a.type_info is VARIANT_TYPE for a in typed)} Variant, "
          f"{sum(bool(a.aliases) for a in typed)} with renames, "
          f"{sum(a.schemas != all_schema_names for a in typed)} not in every schema).")

    # ── Write Godot XML doc files ─────────────────────────────────────────────
    doc_dir = os.path.join(output_dir, "doc_classes")
    os.makedirs(doc_dir, exist_ok=True)

    for cls in classes:
        xml_path = os.path.join(doc_dir, f"{cls.name}.xml")
        _write(xml_path, emit_xml_doc(cls, entity_desc.get(cls.name.lower(), ""),
                                      entity_attr_desc.get(cls.name.lower(), {}), all_schema_names))

    print(f"[godot_codegen] Wrote {len(classes)} XML doc files to {doc_dir}")

    # ── Generate compressed doc data .cpp ────────────────────────────────────
    sys.path.insert(0, os.path.join(_here, "..", "godot-cpp"))
    try:
        from doc_source_generator import generate_doc_source_from_directory
        gen_cpp_path = os.path.join(output_dir, "gd_ifc_doc_data.gen.cpp")
        generate_doc_source_from_directory(gen_cpp_path, doc_dir)
        with open(gen_cpp_path, encoding="utf-8") as f:
            _write(gen_cpp_path, f.read())
        print(f"[godot_codegen] Wrote {gen_cpp_path}")
    except ImportError as e:
        print(f"[godot_codegen] WARNING: could not import doc_source_generator: {e}")


# ─────────────────────────────────────────────────────────────────────────────
if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    generate(sys.argv[1:-1], sys.argv[-1])
