#!/usr/bin/env python3
"""
Download and extract the Boost header-only subset needed by IfcParse.

Usage:
    python tools/setup_boost.py

This downloads a Boost release and copies only the headers required by
IfcParse into thirdparty/boost/. No compiled Boost libraries are needed;
all remaining Boost usage in IfcParse is header-only.

Required Boost headers:
    - boost/algorithm/string (string manipulation)
    - boost/circular_buffer (token stream)
    - boost/dynamic_bitset (IFC binary attributes)
    - boost/logic/tribool (IFC logical values)
    - boost/math (quadrature, fpclassify, constants)
    - boost/multi_index (indexed containers in IfcFile)
    - boost/optional (used widely in IfcParse API)
    - boost/property_tree (JSON in IfcLogger)
    - boost/range (adaptors in parse_ifcxml)
    - boost/scope_exit (IfcLogger)
    - boost/shared_ptr (legacy shared_ptr usage)
    - boost/unordered_map (storage.h)
    - boost/uuid (IfcGlobalId)
    - boost/variant (IfcParse serialization)
    - boost/lexical_cast (string conversion)
    - boost/iostreams (only for USE_MMAP, optional)
"""

import io
import os
import shutil
import sys
import tarfile
import urllib.request
import subprocess

BOOST_VERSION = "1.86.0"
BOOST_VERSION_UNDERSCORE = BOOST_VERSION.replace(".", "_")
BOOST_URL = f"https://archives.boost.io/release/{BOOST_VERSION}/source/boost_{BOOST_VERSION_UNDERSCORE}.tar.gz"

# Only extract the boost/ include directory (all headers)
# A full Boost source is ~200MB compressed, but the headers alone are ~70MB.
# For a truly minimal extraction, we'd use bcp, but that requires building bcp first.
# Instead, we download the full source and copy only boost/ (headers).
BOOST_HEADER_PREFIX = f"boost_{BOOST_VERSION_UNDERSCORE}/boost/"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
OUTPUT_DIR = os.path.join(PROJECT_ROOT, "thirdparty", "boost")


def download_boost(dest_path):
    """Download Boost source archive."""
    print(f"Downloading Boost {BOOST_VERSION} from {BOOST_URL}...")
    urllib.request.urlretrieve(BOOST_URL, dest_path)
    print(f"Downloaded to {dest_path}")


def extract_headers(archive_path, output_dir):
    """Extract only the boost/ header directory from the archive."""
    print(f"Extracting Boost headers to {output_dir}...")
    if os.path.exists(output_dir):
        shutil.rmtree(output_dir)
    os.makedirs(output_dir, exist_ok=True)

    with tarfile.open(archive_path, "r:gz") as tar:
        members = []
        for member in tar.getmembers():
            if member.name.startswith(BOOST_HEADER_PREFIX):
                # Rebase path: boost_1_86_0/boost/foo.hpp -> boost/foo.hpp
                member.name = member.name[len(BOOST_HEADER_PREFIX) - len("boost/"):]
                members.append(member)

        print(f"  Extracting {len(members)} files...")
        tar.extractall(path=output_dir, members=members)

    print(f"Boost headers installed to {output_dir}/boost/")


def try_bcp_minimal(archive_path, output_dir):
    """
    If bcp is available, extract only the minimal subset.
    Falls back to full header extraction if bcp is not found.
    Returns True if bcp was used successfully.
    """
    try:
        subprocess.run(["bcp", "--version"], capture_output=True, check=True)
    except (FileNotFoundError, subprocess.CalledProcessError):
        return False

    print("bcp found, extracting minimal Boost subset...")
    modules = [
        "boost/algorithm/string.hpp",
        "boost/algorithm/string/replace.hpp",
        "boost/any.hpp",
        "boost/circular_buffer.hpp",
        "boost/dynamic_bitset.hpp",
        "boost/lexical_cast.hpp",
        "boost/logic/tribool.hpp",
        "boost/math/constants/constants.hpp",
        "boost/math/quadrature/trapezoidal.hpp",
        "boost/math/special_functions/fpclassify.hpp",
        "boost/multi_index/ordered_index.hpp",
        "boost/multi_index/random_access_index.hpp",
        "boost/multi_index/sequenced_index.hpp",
        "boost/multi_index_container.hpp",
        "boost/optional.hpp",
        "boost/property_tree/json_parser.hpp",
        "boost/property_tree/ptree.hpp",
        "boost/range/adaptor/transformed.hpp",
        "boost/range/algorithm/copy.hpp",
        "boost/scope_exit.hpp",
        "boost/shared_ptr.hpp",
        "boost/unordered_map.hpp",
        "boost/uuid/uuid.hpp",
        "boost/uuid/uuid_generators.hpp",
        "boost/uuid/uuid_io.hpp",
        "boost/variant.hpp",
        "boost/version.hpp",
    ]

    # Extract full source to temp dir for bcp
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        print("  Extracting full source for bcp...")
        with tarfile.open(archive_path, "r:gz") as tar:
            tar.extractall(path=tmpdir)

        boost_src = os.path.join(tmpdir, f"boost_{BOOST_VERSION_UNDERSCORE}")
        if os.path.exists(output_dir):
            shutil.rmtree(output_dir)
        os.makedirs(output_dir, exist_ok=True)

        cmd = ["bcp", "--boost=" + boost_src] + modules + [output_dir]
        subprocess.run(cmd, check=True)

    print(f"Minimal Boost subset installed to {output_dir}/boost/")
    return True


def main():
    import tempfile
    archive_path = os.path.join(tempfile.gettempdir(), f"boost_{BOOST_VERSION_UNDERSCORE}.tar.gz")

    if not os.path.exists(archive_path):
        download_boost(archive_path)
    else:
        print(f"Using cached archive: {archive_path}")

    # Try bcp for minimal extraction, fall back to full headers
    if not try_bcp_minimal(archive_path, OUTPUT_DIR):
        extract_headers(archive_path, OUTPUT_DIR)

    print("\nDone! Boost headers are ready in thirdparty/boost/")
    print("You can now build with: scons platform=<platform> target=<target>")


if __name__ == "__main__":
    main()
