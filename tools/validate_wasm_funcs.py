#!/usr/bin/env python3
"""Validate that no WASM function body exceeds the browser per-function limit.

V8 (Chrome/Edge) enforces a maximum of 7,654,321 bytes per function body.
SpiderMonkey (Firefox) has a similar limit.  If any function exceeds this,
the browser will reject the entire WASM module at instantiation time with:
  CompileError: wasm validation error: function body too big

Usage:
    python3 tools/validate_wasm_funcs.py path/to/file.wasm
"""

import struct
import sys

# V8's hard limit (see v8/src/wasm/wasm-limits.h kV8MaxWasmFunctionSize)
MAX_FUNC_BODY_SIZE = 7_654_321


def read_leb128(data, offset):
    """Read an unsigned LEB128 value, return (value, new_offset)."""
    result = 0
    shift = 0
    while True:
        byte = data[offset]
        offset += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not (byte & 0x80):
            break
    return result, offset


def validate_wasm(path):
    with open(path, "rb") as f:
        data = f.read()

    # Validate magic number
    if data[:4] != b'\x00asm':
        print(f"ERROR: {path} is not a valid WASM file")
        return False

    i = 8  # Skip magic (4 bytes) + version (4 bytes)

    while i < len(data):
        sec_id = data[i]
        i += 1
        sec_size, i = read_leb128(data, i)
        sec_end = i + sec_size

        if sec_id == 10:  # Code section
            func_count, i = read_leb128(data, i)
            oversized = []
            largest = 0
            for idx in range(func_count):
                body_size, i = read_leb128(data, i)
                if body_size > largest:
                    largest = body_size
                if body_size > MAX_FUNC_BODY_SIZE:
                    oversized.append((idx, body_size))
                i += body_size

            print(f"Total functions:     {func_count:,}")
            print(f"Largest function:    {largest:,} bytes")
            print(f"V8 limit:            {MAX_FUNC_BODY_SIZE:,} bytes")
            print(f"Headroom:            {MAX_FUNC_BODY_SIZE - largest:,} bytes")

            if oversized:
                print()
                for idx, size in oversized:
                    overshoot = size - MAX_FUNC_BODY_SIZE
                    print(f"FAIL: Function #{idx} = {size:,} bytes "
                          f"(exceeds limit by {overshoot:,} bytes)")
                return False
            else:
                print("PASS: All functions within V8 size limit")
                return True
        else:
            i = sec_end

    print("WARNING: No Code section found in WASM file")
    return True


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file.wasm>")
        sys.exit(2)

    ok = validate_wasm(sys.argv[1])
    sys.exit(0 if ok else 1)
