#!/usr/bin/env python3
"""
Cut a new GDIFC build: bump the build number and release the changelog entry.

Usage:
    python tools/bump_build.py                        # next build of the current version
    python tools/bump_build.py --version 1.2.0-alpha  # next build, under a new version
    python tools/bump_build.py --dry-run              # show what would change, write nothing
    python tools/bump_build.py --skip-if-empty --notes notes.md   # what CI runs

CI runs this on every push to master (see .github/workflows/build.yml), so you
normally only write entries under Unreleased and push. CI never changes the
version: to start a new one, run `python tools/set_version.py <version>` and
commit the result. Run this script by hand only to cut a build locally.

What it does:
    1. Reads `version` and `build` from both addon plugin.cfg copies
       (addons/GDIFC and ifc-godot-project/addons/GDIFC) and checks they agree.
       A missing `build` key counts as 0.
    2. Increments the build number. It is global and never resets, so a build
       number always identifies exactly one build, whatever the version.
    3. Moves everything under `## [Unreleased]` in CHANGELOG.md into a new
       `### Build N — YYYY-MM-DD` section under `## [<version>]` (creating the
       version heading if needed), demoting its `###` headings to `####`.
    4. Writes the new `version` and `build` into both plugin.cfg files.

Nothing is committed; review the result and commit it yourself.

When GITHUB_OUTPUT is set (GitHub Actions), it also writes `released`
(true/false), `version` and `build` there for later workflow steps.
"""

import argparse
import datetime
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PLUGIN_CFGS = [
    ROOT / "addons" / "GDIFC" / "plugin.cfg",
    ROOT / "ifc-godot-project" / "addons" / "GDIFC" / "plugin.cfg",
]
CHANGELOG = ROOT / "CHANGELOG.md"

UNRELEASED_HEADING = "## [Unreleased]"

# MAJOR.MINOR.PATCH with an optional pre-release (1.2.0, 1.2.0-alpha, 1.2.0-rc.1).
# No "+metadata": the build number is kept separately.
VERSION_PATTERN = r"\d+\.\d+\.\d+(-[0-9A-Za-z]+(\.[0-9A-Za-z]+)*)?"


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(1)


def read_text(path):
    # newline="" keeps the file's own line endings intact
    with open(path, encoding="utf-8", newline="") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def read_cfg_value(text, key):
    match = re.search(rf'^{key}="([^"]*)"', text, re.MULTILINE)
    return match.group(1) if match else None


def set_cfg_value(text, key, value, insert_after):
    """Replace `key="..."`, or add it on the line after `insert_after` when missing."""
    line = f'{key}="{value}"'
    pattern = rf'^{key}="[^"]*"'
    if re.search(pattern, text, re.MULTILINE):
        return re.sub(pattern, line, text, count=1, flags=re.MULTILINE)

    newline = "\r\n" if "\r\n" in text else "\n"
    anchor = re.search(rf'^{insert_after}="[^"]*"', text, re.MULTILINE)
    if not anchor:
        fail(f"plugin.cfg has no {insert_after} key to place {key} after")
    return text[:anchor.end()] + newline + line + text[anchor.end():]


def release_changelog(text, version, build, date):
    """Return (new changelog, released notes), or None when Unreleased is empty."""
    newline = "\r\n" if "\r\n" in text else "\n"
    lines = text.splitlines()

    try:
        start = lines.index(UNRELEASED_HEADING)
    except ValueError:
        fail(f"CHANGELOG.md has no '{UNRELEASED_HEADING}' heading")

    end = next((i for i in range(start + 1, len(lines)) if lines[i].startswith("## ")), len(lines))
    body = lines[start + 1:end]
    while body and not body[0].strip():
        body.pop(0)
    while body and not body[-1].strip():
        body.pop()
    if not body:
        return None
    notes = newline.join(body) + newline

    # Unreleased uses ### sections; under a build they sit one level deeper
    body = [("#" + line) if re.match(r"^#{3,} ", line) else line for line in body]

    lines = lines[:start + 1] + [""] + lines[end:]
    build_block = [f"### Build {build} — {date}", "", *body, ""]

    version_heading = f"## [{version}]"
    if version_heading in lines:
        at = lines.index(version_heading) + 1
        lines[at:at] = ["", *build_block]
    else:
        at = start + 2
        lines[at:at] = [version_heading, "", *build_block]

    result = newline.join(lines)
    result = re.sub(rf"({re.escape(newline)}){{3,}}", newline * 2, result)
    return result.rstrip() + newline, notes


def write_github_output(**values):
    path = os.environ.get("GITHUB_OUTPUT")
    if not path:
        return
    with open(path, "a", encoding="utf-8") as f:
        for key, value in values.items():
            f.write(f"{key}={value}\n")


def main():
    parser = argparse.ArgumentParser(description="Bump the GDIFC build number and release the changelog entry.")
    parser.add_argument("--version", help="switch to this version (the build number keeps counting)")
    parser.add_argument("--dry-run", action="store_true", help="show what would change without writing")
    parser.add_argument("--skip-if-empty", action="store_true",
                        help="exit successfully without changes when Unreleased is empty")
    parser.add_argument("--notes", metavar="PATH", help="also write the released entries to this file")
    args = parser.parse_args()

    cfg_texts = {path: read_text(path) for path in PLUGIN_CFGS}
    values = {
        path: (read_cfg_value(text, "version"), read_cfg_value(text, "build") or "0")
        for path, text in cfg_texts.items()
    }
    if len(set(values.values())) != 1:
        details = "\n".join(f"  {p.relative_to(ROOT)}: version={v[0]} build={v[1]}" for p, v in values.items())
        fail(f"plugin.cfg copies disagree:\n{details}")

    old_version, old_build = next(iter(values.values()))
    if not old_version:
        fail("plugin.cfg has no version key")
    if not old_build.isdigit():
        fail(f"build '{old_build}' in plugin.cfg is not a number")

    version = args.version or old_version
    if not re.fullmatch(VERSION_PATTERN, version):
        fail(f"'{version}' is not a version like 1.2.0 or 1.2.0-alpha")
    build = int(old_build) + 1
    date = datetime.date.today().isoformat()

    released = release_changelog(read_text(CHANGELOG), version, build, date)
    if released is None:
        if not args.skip_if_empty:
            fail("the Unreleased section is empty, nothing to release")
        print("Unreleased is empty, nothing to release")
        write_github_output(released="false")
        return
    changelog, notes = released

    new_cfgs = {
        path: set_cfg_value(set_cfg_value(text, "version", version, "name"), "build", build, "version")
        for path, text in cfg_texts.items()
    }

    print(f"version: {old_version} -> {version}")
    print(f"build:   {old_build} -> {build}")
    print(f"CHANGELOG.md: Unreleased moved to [{version}] / Build {build} ({date})")

    if args.dry_run:
        print("dry run, nothing written")
        return

    write_text(CHANGELOG, changelog)
    for path, text in new_cfgs.items():
        write_text(path, text)
        print(f"updated {path.relative_to(ROOT)}")
    if args.notes:
        write_text(args.notes, notes)
        print(f"wrote release notes to {args.notes}")
    write_github_output(released="true", version=version, build=build)


if __name__ == "__main__":
    main()
