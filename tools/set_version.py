#!/usr/bin/env python3
"""
Set the GDIFC version in both addon plugin.cfg copies.

Usage:
    python tools/set_version.py 1.2.0-alpha   # set the version
    python tools/set_version.py               # show the current version

Writes `version` into addons/GDIFC/plugin.cfg and
ifc-godot-project/addons/GDIFC/plugin.cfg. The build number is left alone;
CI (or tools/bump_build.py) increments it when the next build is cut, under
this version. Commit the result, ideally with the Unreleased entries for it.
"""

import argparse
import re

from bump_build import (
    PLUGIN_CFGS,
    ROOT,
    VERSION_PATTERN,
    fail,
    read_cfg_value,
    read_text,
    set_cfg_value,
    write_text,
)


def main():
    parser = argparse.ArgumentParser(description="Set the GDIFC version in both plugin.cfg copies.")
    parser.add_argument("version", nargs="?", help="new version, e.g. 1.2.0-alpha; omit to show the current one")
    args = parser.parse_args()

    texts = {path: read_text(path) for path in PLUGIN_CFGS}
    current = {path: read_cfg_value(text, "version") for path, text in texts.items()}

    if args.version is None:
        for path, version in current.items():
            print(f"{path.relative_to(ROOT)}: {version}")
        if len(set(current.values())) != 1:
            print("warning: the copies disagree; run with a version to sync them")
        return

    if not re.fullmatch(VERSION_PATTERN, args.version):
        fail(f"'{args.version}' is not a version like 1.2.0 or 1.2.0-alpha")

    for path, text in texts.items():
        name = path.relative_to(ROOT)
        if current[path] == args.version:
            print(f"{name}: already {args.version}")
            continue
        write_text(path, set_cfg_value(text, "version", args.version, "name"))
        print(f"{name}: {current[path]} -> {args.version}")


if __name__ == "__main__":
    main()
