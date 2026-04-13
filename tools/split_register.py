#!/usr/bin/env python3
"""Split register_all_gd_ifc_entities() into batch sub-functions.

Browser WASM engines enforce a per-function bytecode size limit (~7.5 MiB).
The monolithic register function with 876 class registrations can exceed this
limit, especially with LTO inlining.  This script splits it into batches of
~100 registrations each.
"""

import sys
import os

BATCH_SIZE = 100
REGISTER_FILE = os.path.join(
    os.path.dirname(__file__), "..",
    "GDIFC", "src", "generated", "gd_ifc_entities_register.cpp"
)

def main():
    filepath = os.path.abspath(REGISTER_FILE)
    print(f"Processing: {filepath}")

    with open(filepath, "r") as f:
        lines = f.read().split("\n")

    # Find the function signature
    func_line_idx = None
    for i, line in enumerate(lines):
        if "void register_all_gd_ifc_entities()" in line and "{" in line:
            func_line_idx = i
            break

    if func_line_idx is None:
        print("ERROR: Could not find register_all_gd_ifc_entities() function")
        sys.exit(1)

    print(f"Found function at line {func_line_idx + 1}")

    # Preamble: everything before the function
    preamble = lines[:func_line_idx]

    # Body: everything between the opening { and the final }
    # The function opens with "void register_all_gd_ifc_entities() {"
    # and closes with "}" on the last non-empty line
    body_lines = lines[func_line_idx + 1:]  # after opening line

    # Find the closing brace
    close_idx = None
    for i in range(len(body_lines) - 1, -1, -1):
        if body_lines[i].strip() == "}":
            close_idx = i
            break

    if close_idx is None:
        print("ERROR: Could not find closing brace")
        sys.exit(1)

    # Anything after the closing brace (should be empty or trailing newline)
    after = body_lines[close_idx + 1:]
    body_lines = body_lines[:close_idx]

    # Split body into registration blocks.  Each block ends with "});"
    blocks = []
    current_block = []
    for line in body_lines:
        current_block.append(line)
        if line.strip() == "});":
            blocks.append(current_block)
            current_block = []
    if current_block:
        # Shouldn't happen, but handle gracefully
        blocks.append(current_block)

    print(f"Found {len(blocks)} registration blocks")

    # Group into batches
    batches = []
    for i in range(0, len(blocks), BATCH_SIZE):
        batches.append(blocks[i:i + BATCH_SIZE])

    print(f"Split into {len(batches)} batches of up to {BATCH_SIZE}")

    # Build output
    output = []
    output.extend(preamble)
    output.append("")

    # Batch functions
    for batch_idx, batch in enumerate(batches):
        output.append(f"static void register_gd_ifc_entities_batch_{batch_idx}() {{")
        for block in batch:
            output.extend(block)
        output.append("}")
        output.append("")

    # Main function calls all batches
    output.append("void register_all_gd_ifc_entities() {")
    for batch_idx in range(len(batches)):
        output.append(f"    register_gd_ifc_entities_batch_{batch_idx}();")
    output.append("}")
    output.append("")

    with open(filepath, "w") as f:
        f.write("\n".join(output))

    total_lines = len(output)
    print(f"Done! Wrote {total_lines} lines to {filepath}")

if __name__ == "__main__":
    main()
