#!/usr/bin/env python
"""
Loads every IFC file of a folder in Godot with GDIFC and writes HTML, JUnit XML and JSON reports.

Each file is loaded by its own headless Godot process (ifc-godot-project/tests/ifc_load_test.gd),
so a crash or a hang fails that file only. Uses the template_debug library in
ifc-godot-project/addons/GDIFC/, so build it first:
    scons platform=windows target=template_debug local=1

Usage:
    python tests/ifc_corpus/run_ifc_corpus.py D:/IFC/TestFiles --godot "C:/Godot/Godot_v4.8_win64.exe"
    python tests/ifc_corpus/run_ifc_corpus.py D:/IFC/TestFiles --filter "segfault" -j 4
    python tests/ifc_corpus/run_ifc_corpus.py D:/IFC/TestFiles --max-size-mb 20 --no-baseline

The Godot binary comes from --godot, else the GODOT environment variable, else `godot` on PATH.
On Windows, pass the plain executable rather than the _console one: the console one launches it as
a child process, which hides its peak memory.

Reports go to tests/reports/<timestamp>/ (report.html, report.json, junit.xml, logs/). Each run is
compared with the latest earlier run of the same folder, to list files whose result got worse.
Exit code: 0 when no file failed, 1 otherwise.
"""

import argparse
import concurrent.futures
import ctypes
import datetime
import html
import json
import os
import platform
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import xml.etree.ElementTree as ET

REPO_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PROJECT_DIR = os.path.join(REPO_DIR, "ifc-godot-project")
TEST_SCRIPT = "res://tests/ifc_load_test.gd"
DEFAULT_REPORTS_DIR = os.path.join(REPO_DIR, "tests", "reports")

STATUS_ORDER = ["pass", "warn", "fail", "crash", "timeout"]
FAILING = {"fail", "crash", "timeout"}

# Printed for every file, so they would hide the messages that tell files apart
IGNORED_MESSAGES = [
    re.compile(r"Invalid tag in doc file"),
]

ANSI_ESCAPE = re.compile(r"\x1b\[[0-9;]*m")
GODOT_ERROR = re.compile(r"^(?:SCRIPT )?ERROR: (.*)")
GODOT_WARNING = re.compile(r"^WARNING: (.*)")
# spdlog lines from web-ifc: [2026-11-02 10:19:57.733] [error] [GetMesh()] unexpected mesh type 54
WEBIFC_LOG = re.compile(r"^\[[\d\- :.]+\] \[(error|warning|critical)\] (.*)")
TRAILING_ID = re.compile(r"(?<![#\w])(\d+)$")
STEP_ENTITY = re.compile(rb"^\s*#(\d+)\s*=\s*([A-Za-z0-9_]+)")
# Skips quoted strings whole, so only comments outside them are removed
STEP_COMMENT = re.compile(r"('(?:[^']|'')*')|/\*.*?\*/", re.S)
# Printed by ifc_load_test.gd, the only progress left when a file times out or crashes
STAGE_LINE = re.compile(r"^\[ifc_load_test\] stage: (.*)$", re.M)

WINDOWS_EXIT_CODES = {
    0xC0000005: "access violation",
    0xC00000FD: "stack overflow",
    0xC0000409: "stack buffer overrun / fast fail",
    0xC0000374: "heap corruption",
    0xC0000017: "out of memory",
    0xC000001D: "illegal instruction",
    0xC0000094: "integer divide by zero",
    0x80000003: "breakpoint",
    0xE06D7363: "unhandled C++ exception",
    3: "abort()",
}


# ── Discovery ───────────────────────────────────────────────────────────────

def read_header(path):
    """Schema and originating system from the STEP header, or the reason the file isn't STEP."""
    with open(path, "rb") as f:
        head = f.read(64 * 1024).decode("latin-1", errors="replace")
    info = {"schema": "", "originating_system": "", "step": head.lstrip().startswith("ISO-10303-21")}
    match = re.search(r"FILE_SCHEMA\s*\(\s*\(\s*'([^']*)'", head)
    if match:
        info["schema"] = match.group(1).upper()
    match = re.search(r"FILE_NAME\s*\(", head)
    if match:
        # FILE_NAME(name, time_stamp, (author), (organization), preprocessor_version, originating_system, authorization)
        fields = step_arguments(head, match.end() - 1)
        if len(fields) >= 6 and fields[5].startswith("'"):
            info["originating_system"] = fields[5][1:-1].replace("''", "'")
    return info


def step_arguments(text, start):
    """Top-level arguments, as written, of the STEP parameter list whose '(' is at text[start].

    Splits on commas outside strings and nested lists, so $ and () fields keep their place."""
    # Exporters such as Geometry Gym label each field with a /* comment */
    text = text[:start + 1] + STEP_COMMENT.sub(lambda m: m.group(1) or " ", text[start + 1:])
    fields, current, depth, in_string = [], [], 0, False
    for ch in text[start + 1:]:
        if in_string:
            # '' inside a string toggles out and straight back in, which keeps both quotes
            in_string = ch != "'"
        elif ch == "'":
            in_string = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            if depth == 0:
                break
            depth -= 1
        elif ch == "," and depth == 0:
            fields.append("".join(current).strip())
            current = []
            continue
        current.append(ch)
    fields.append("".join(current).strip())
    return fields


def discover(corpus, args):
    include = re.compile(args.filter, re.I) if args.filter else None
    exclude = re.compile(args.exclude, re.I) if args.exclude else None
    files = []
    for dirpath, dirnames, filenames in os.walk(corpus):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if not name.lower().endswith(".ifc"):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, corpus).replace("\\", "/")
            if include and not include.search(rel):
                continue
            if exclude and exclude.search(rel):
                continue
            size = os.path.getsize(path)
            if args.max_size_mb and size > args.max_size_mb * 1024 * 1024:
                continue
            files.append({"path": path, "rel": rel, "size": size})
    return files


# ── Running one file ────────────────────────────────────────────────────────

class PROCESS_MEMORY_COUNTERS(ctypes.Structure):
    _fields_ = [
        ("cb", ctypes.c_ulong),
        ("PageFaultCount", ctypes.c_ulong),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
    ]


def peak_memory(proc):
    """Peak working set of an exited process. Windows only: the process handle outlives the process."""
    if os.name != "nt":
        return None
    counters = PROCESS_MEMORY_COUNTERS()
    counters.cb = ctypes.sizeof(counters)
    get_info = ctypes.windll.psapi.GetProcessMemoryInfo
    get_info.argtypes = [ctypes.c_void_p, ctypes.POINTER(PROCESS_MEMORY_COUNTERS), ctypes.c_ulong]
    if get_info(int(proc._handle), ctypes.byref(counters), counters.cb):
        return counters.PeakWorkingSetSize
    return None


def kill_tree(proc):
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(proc.pid)], capture_output=True)
    else:
        os.killpg(proc.pid, signal.SIGKILL)


def describe_exit(code):
    if code is None:
        return ""
    unsigned = code & 0xFFFFFFFF
    if unsigned in WINDOWS_EXIT_CODES:
        return "0x%08X %s" % (unsigned, WINDOWS_EXIT_CODES[unsigned])
    if code < 0:
        try:
            return signal.Signals(-code).name
        except ValueError:
            pass
    return str(code)


def scan_log(text):
    found = {"errors": [], "warnings": [], "webifc_errors": [], "webifc_warnings": []}
    for raw in text.splitlines():
        line = ANSI_ESCAPE.sub("", raw).strip()
        match = GODOT_ERROR.match(line)
        if match:
            found["errors"].append(match.group(1))
            continue
        match = GODOT_WARNING.match(line)
        if match:
            found["warnings"].append(match.group(1))
            continue
        match = WEBIFC_LOG.match(line)
        if match:
            key = "webifc_warnings" if match.group(1) == "warning" else "webifc_errors"
            found[key].append(match.group(2))
    return found


def resolve_entity_types(path, messages):
    """web-ifc ends its geometry messages with an express ID; name the entity it refers to."""
    ids = {match.group(1) for match in map(TRAILING_ID.search, messages) if match}
    if not ids:
        return messages
    types = {}
    with open(path, "rb") as f:
        for line in f:
            match = STEP_ENTITY.match(line)
            if match and match.group(1).decode() in ids:
                types[match.group(1).decode()] = match.group(2).decode().upper()
                if len(types) == len(ids):
                    break
    return [TRAILING_ID.sub(lambda m: "#%s %s" % (m.group(1), types.get(m.group(1), "")), message).rstrip()
            for message in messages]


def summarize_messages(messages, limit=8):
    """Distinct messages with their counts, most frequent first."""
    counts = {}
    for message in messages:
        counts[message] = counts.get(message, 0) + 1
    ordered = sorted(counts.items(), key=lambda item: -item[1])
    return [{"message": m, "count": c} for m, c in ordered[:limit]]


def run_file(entry, args, out_dir, active):
    safe_name = re.sub(r"[^\w.\-]+", "_", entry["rel"])
    json_path = os.path.join(out_dir, "results", safe_name + ".json")
    log_path = os.path.join(out_dir, "logs", safe_name + ".log")
    timeout = args.timeout + args.timeout_per_mb * entry["size"] / (1024 * 1024)

    cmd = [args.godot, "--headless", "--path", PROJECT_DIR, "--script", TEST_SCRIPT, "--",
           "--file", os.path.abspath(entry["path"]).replace("\\", "/"), "--out", json_path.replace("\\", "/")]
    if args.collision:
        cmd.append("--collision")
    if args.no_lods:
        cmd.append("--no-lods")

    popen_kwargs = {}
    if os.name != "nt":
        popen_kwargs["start_new_session"] = True
    started = time.monotonic()
    with open(log_path, "wb") as log_file:
        proc = subprocess.Popen(cmd, stdout=log_file, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **popen_kwargs)
        active.add(proc)
        timed_out = False
        try:
            proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            kill_tree(proc)
            proc.wait()
        finally:
            active.discard(proc)
    duration = time.monotonic() - started

    with open(log_path, "rb") as f:
        log_text = f.read().decode("utf-8", errors="replace")
    data = None
    if os.path.exists(json_path):
        try:
            with open(json_path, encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            data = None

    result = {
        "file": entry["rel"],
        "size": entry["size"],
        "header": read_header(entry["path"]),
        "duration_s": round(duration, 2),
        "timeout_s": round(timeout),
        "timed_out": timed_out,
        "exit_code": proc.returncode,
        "exit_description": describe_exit(proc.returncode),
        "peak_memory": peak_memory(proc),
        "log": os.path.relpath(log_path, out_dir).replace("\\", "/"),
        "log_tail": [ANSI_ESCAPE.sub("", l) for l in log_text.splitlines()[-40:]],
        "last_stage": ([""] + STAGE_LINE.findall(log_text))[-1].strip(),
        "data": data,
    }
    messages = scan_log(log_text)
    for key in ("webifc_errors", "webifc_warnings"):
        messages[key] = resolve_entity_types(entry["path"], messages[key])
    for key, values in messages.items():
        messages[key] = [m for m in values if not any(p.search(m) for p in IGNORED_MESSAGES)]
    result["messages"] = {key: summarize_messages(values) for key, values in messages.items()}
    result["message_counts"] = {key: len(values) for key, values in messages.items()}
    evaluate(result)
    return result


# ── Checks ──────────────────────────────────────────────────────────────────

def evaluate(result):
    """Turn the process outcome and the load data into checks and an overall status."""
    checks = []

    def check(name, status, message):
        checks.append({"name": name, "status": status, "message": message})

    name = result["file"].lower()
    data = result["data"] or {}
    metrics = data.get("metrics") or {}
    counts = result["message_counts"]

    if result["timed_out"]:
        check("process", "timeout", "Killed after %d s (last stage: %s)" % (result["timeout_s"], result["last_stage"] or "none"))
    elif result["data"] is None and result["exit_code"] in (0, 1, 2):
        check("process", "fail", "Exited with code %d without writing results" % result["exit_code"])
    elif result["data"] is None:
        check("process", "crash", "Exited with %s (last stage: %s)" % (result["exit_description"], result["last_stage"] or "none"))
    elif result["exit_code"] not in (0, 1, 2):
        step = {"model_queries": "querying IFCModel (header, instances_by_type)"}.get(data.get("phase"), "freeing the scene")
        check("process", "crash", "Loaded, then exited with %s while %s" % (result["exit_description"], step))
    else:
        check("process", "pass", "Exited normally")

    if data:
        if data.get("read_error", 0) != 0:
            check("load", "fail", "read_ifc returned error %s" % data["read_error"])
        elif data.get("ifc_load_failed"):
            check("load", "fail", "ifc_load_failed emitted after %.0f ms" % data.get("ifc_load_failed_ms", 0))
        elif data.get("ifc_read") and data.get("ifc_objects_ready"):
            check("load", "pass", "Model shown after %.0f ms, objects ready after %.0f ms"
                  % (data["ifc_read_ms"], data["ifc_objects_ready_ms"]))
        else:
            check("load", "fail", "Finished without ifc_read and ifc_objects_ready")

    if metrics:
        if metrics["triangles"] > 0:
            check("geometry", "pass", "%d triangles in %d meshes" % (metrics["triangles"], metrics["mesh_nodes"]))
        elif "no-geometry" in name:
            check("geometry", "pass", "No geometry, as the file name says")
        else:
            check("geometry", "warn", "No geometry created (%d IfcProduct in the file)" % metrics.get("products", 0))

        if metrics["non_finite_bounds"]:
            check("bounds", "fail", "%d meshes with NaN or infinite bounds" % metrics["non_finite_bounds"])
        elif metrics.get("bounds_size") and max(metrics["bounds_size"]) > 1e6:
            check("bounds", "warn", "Model is %.0f km across" % (max(metrics["bounds_size"]) / 1000))
        else:
            check("bounds", "pass", "Finite bounds")

        if metrics["ifc_nodes"] and metrics["nodes_with_attributes"] == 0:
            check("metadata", "warn", "No node got attributes")
        elif metrics["ifc_nodes"] and metrics["nodes_with_ifc_object"] < metrics["ifc_nodes"]:
            check("metadata", "warn", "%d of %d nodes have no ifc_object"
                  % (metrics["ifc_nodes"] - metrics["nodes_with_ifc_object"], metrics["ifc_nodes"]))
        else:
            check("metadata", "pass", "%d nodes with attributes, %d with property sets"
                  % (metrics["nodes_with_attributes"], metrics["nodes_with_properties"]))

        if metrics["duplicate_global_ids"]:
            check("global_ids", "warn", "%d duplicate GlobalIds" % metrics["duplicate_global_ids"])

    errors = counts["errors"] + counts["webifc_errors"]
    if errors:
        status = "pass" if "accept-errors" in name else "warn"
        parts = ["%d %s error%s" % (n, label, "" if n == 1 else "s")
                 for n, label in ((counts["errors"], "Godot"), (counts["webifc_errors"], "web-ifc")) if n]
        top = (result["messages"]["errors"] + result["messages"]["webifc_errors"])[0]["message"]
        check("log", status, "%s, e.g. %s" % (", ".join(parts), top))
    else:
        check("log", "pass", "No errors logged")

    result["checks"] = checks
    result["status"] = max((c["status"] for c in checks), key=STATUS_ORDER.index)


# ── Baseline comparison ─────────────────────────────────────────────────────

def find_baseline(reports_dir, corpus, current_dir):
    if not os.path.isdir(reports_dir):
        return None
    for name in sorted(os.listdir(reports_dir), reverse=True):
        path = os.path.join(reports_dir, name, "report.json")
        if os.path.join(reports_dir, name) == current_dir or not os.path.exists(path):
            continue
        try:
            with open(path, encoding="utf-8") as f:
                report = json.load(f)
        except (OSError, ValueError):
            continue
        if os.path.normcase(report["meta"]["corpus"]) == os.path.normcase(corpus):
            return path
    return None


def compare(results, baseline_path):
    with open(baseline_path, encoding="utf-8") as f:
        baseline = json.load(f)
    previous = {r["file"]: r for r in baseline["results"]}
    changes = {"baseline": baseline_path, "baseline_meta": baseline["meta"],
               "regressions": [], "fixes": [], "geometry_changes": [], "slower": []}
    for result in results:
        old = previous.get(result["file"])
        if not old:
            continue
        old_rank, new_rank = STATUS_ORDER.index(old["status"]), STATUS_ORDER.index(result["status"])
        if new_rank > old_rank:
            changes["regressions"].append({"file": result["file"], "from": old["status"], "to": result["status"]})
        elif new_rank < old_rank:
            changes["fixes"].append({"file": result["file"], "from": old["status"], "to": result["status"]})

        old_metrics = (old.get("data") or {}).get("metrics") or {}
        new_metrics = (result.get("data") or {}).get("metrics") or {}
        for key in ("triangles", "mesh_nodes", "ifc_nodes"):
            if key in old_metrics and key in new_metrics and old_metrics[key] != new_metrics[key]:
                changes["geometry_changes"].append({"file": result["file"], "metric": key,
                                                    "from": old_metrics[key], "to": new_metrics[key]})

        old_ms = (old.get("data") or {}).get("ifc_objects_ready_ms")
        new_ms = (result.get("data") or {}).get("ifc_objects_ready_ms")
        if old_ms and new_ms and new_ms > old_ms * 1.5 and new_ms - old_ms > 2000:
            changes["slower"].append({"file": result["file"], "from_ms": old_ms, "to_ms": new_ms})
    return changes


# ── Reports ─────────────────────────────────────────────────────────────────

def git_info():
    def git(*args):
        try:
            return subprocess.run(["git", *args], cwd=REPO_DIR, capture_output=True, text=True, check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            return ""
    return {"branch": git("rev-parse", "--abbrev-ref", "HEAD"), "commit": git("rev-parse", "--short", "HEAD"),
            "dirty": bool(git("status", "--porcelain", "--untracked-files=no"))}


def library_info():
    suffix = {"nt": ".dll", "posix": ".so"}.get(os.name, "")
    system = {"Windows": "windows", "Linux": "linux"}.get(platform.system(), platform.system().lower())
    path = os.path.join(PROJECT_DIR, "addons", "GDIFC", "libgdifc.%s.template_debug.x86_64%s" % (system, suffix))
    if not os.path.exists(path):
        return {"path": path, "exists": False}
    modified = datetime.datetime.fromtimestamp(os.path.getmtime(path))
    return {"path": path, "exists": True, "modified": modified.isoformat(timespec="seconds")}


def write_junit(report, path):
    results = report["results"]
    suite = ET.Element("testsuite", name="ifc-corpus", tests=str(len(results)),
                       failures=str(sum(r["status"] == "fail" for r in results)),
                       errors=str(sum(r["status"] in ("crash", "timeout") for r in results)),
                       time="%.2f" % sum(r["duration_s"] for r in results),
                       timestamp=report["meta"]["started"])
    for r in results:
        directory, _, name = r["file"].rpartition("/")
        case = ET.SubElement(suite, "testcase", classname=directory or ".", name=name, time="%.2f" % r["duration_s"])
        bad = [c for c in r["checks"] if c["status"] != "pass"]
        details = "\n".join("[%s] %s: %s" % (c["status"], c["name"], c["message"]) for c in bad)
        if r["status"] in ("crash", "timeout"):
            ET.SubElement(case, "error", type=r["status"], message=bad[0]["message"]).text = details + "\n\n" + "\n".join(r["log_tail"])
        elif r["status"] == "fail":
            ET.SubElement(case, "failure", type="fail", message=bad[0]["message"]).text = details
        elif bad:
            ET.SubElement(case, "system-out").text = details
    ET.ElementTree(suite).write(path, encoding="utf-8", xml_declaration=True)


def write_html(report, path):
    template_path = os.path.join(os.path.dirname(__file__), "report_template.html")
    with open(template_path, encoding="utf-8") as f:
        template = f.read()
    # Embedded as JSON in a <script> block; escape what could close it
    payload = json.dumps(report).replace("</", "<\\/")
    title = "IFC corpus report · %s" % report["meta"]["started"][:16].replace("T", " ")
    page = template.replace("__TITLE__", html.escape(title)).replace("__REPORT_JSON__", payload)
    with open(path, "w", encoding="utf-8") as f:
        f.write(page)


# ── Main ────────────────────────────────────────────────────────────────────

def find_godot(arg):
    candidate = arg or os.environ.get("GODOT") or shutil.which("godot")
    if not candidate or not os.path.exists(candidate):
        sys.exit("Godot not found. Pass --godot <path> or set the GODOT environment variable.")
    return candidate


def ensure_imported(godot):
    """GDExtensions only load outside the editor once the project has been imported once."""
    if os.path.exists(os.path.join(PROJECT_DIR, ".godot", "extension_list.cfg")):
        return
    print("Importing the test project once, so GDIFC is registered...")
    subprocess.run([godot, "--headless", "--path", PROJECT_DIR, "--import"], stdin=subprocess.DEVNULL,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=600)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("corpus", nargs="?", default=os.environ.get("IFC_TEST_FILES"),
                        help="Folder searched recursively for .ifc files (default: IFC_TEST_FILES environment variable)")
    parser.add_argument("--godot", help="Godot executable (default: GODOT environment variable, then godot on PATH)")
    parser.add_argument("--out", help="Report folder (default: tests/reports/<timestamp>)")
    parser.add_argument("-j", "--jobs", type=int, default=max(1, min(4, (os.cpu_count() or 2) // 3)),
                        help="Files loaded at the same time (default: %(default)s)")
    parser.add_argument("--big-file-mb", type=float, default=50,
                        help="Files above this size are loaded one at a time, after the others (default: %(default)s)")
    parser.add_argument("--filter", help="Only files whose path relative to the corpus matches this regex")
    parser.add_argument("--exclude", help="Skip files whose path relative to the corpus matches this regex")
    parser.add_argument("--max-size-mb", type=float, help="Skip files larger than this")
    parser.add_argument("--timeout", type=float, default=120, help="Seconds allowed per file (default: %(default)s)")
    parser.add_argument("--timeout-per-mb", type=float, default=6, help="Extra seconds per MB of file (default: %(default)s)")
    parser.add_argument("--collision", action="store_true", help="Create collision shapes for every element")
    parser.add_argument("--no-lods", action="store_true", help="Don't generate mesh LODs")
    parser.add_argument("--baseline", help="report.json to compare with (default: latest earlier run of the same corpus)")
    parser.add_argument("--no-baseline", action="store_true", help="Don't compare with an earlier run")
    parser.add_argument("--list", action="store_true", help="List the files that would be loaded, then exit")
    args = parser.parse_args()

    if not args.corpus:
        parser.error("pass the folder of IFC files, or set IFC_TEST_FILES")
    corpus = os.path.abspath(args.corpus)
    files = discover(corpus, args)
    if args.list:
        for entry in files:
            print("%10.1f MB  %s" % (entry["size"] / 1048576, entry["rel"]))
        print("%d files" % len(files))
        return 0
    if not files:
        sys.exit("No .ifc files found in %s" % corpus)

    args.godot = find_godot(args.godot)
    library = library_info()
    if not library["exists"]:
        sys.exit("%s is missing. Build it with: scons platform=<platform> target=template_debug local=1" % library["path"])
    ensure_imported(args.godot)

    started = datetime.datetime.now()
    out_dir = os.path.abspath(args.out or os.path.join(DEFAULT_REPORTS_DIR, started.strftime("%Y-%m-%d_%H-%M-%S")))
    os.makedirs(os.path.join(out_dir, "logs"), exist_ok=True)
    os.makedirs(os.path.join(out_dir, "results"), exist_ok=True)

    if os.name == "nt":
        # No Windows Error Reporting dialog when a Godot process crashes; the children inherit this
        SEM_FAILCRITICALERRORS, SEM_NOGPFAULTERRORBOX = 0x0001, 0x0002
        ctypes.windll.kernel32.SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX)

    godot_version = subprocess.run([args.godot, "--version", "--headless"], capture_output=True, text=True,
                                   stdin=subprocess.DEVNULL).stdout.strip().splitlines()
    big_limit = args.big_file_mb * 1024 * 1024
    small = sorted((f for f in files if f["size"] <= big_limit), key=lambda f: f["size"])
    big = sorted((f for f in files if f["size"] > big_limit), key=lambda f: f["size"])
    print("Loading %d files (%d at a time, then %d large ones one by one)" % (len(files), args.jobs, len(big)))
    print("Godot: %s\nReport: %s\n" % (args.godot, out_dir))

    results = []
    active = set()
    lock = threading.Lock()

    def record(result):
        with lock:
            results.append(result)
            bad = [c for c in result["checks"] if c["status"] != "pass"]
            note = (" - " + bad[0]["message"]) if bad else ""
            print("[%*d/%d] %-7s %7.1fs  %s%s" % (len(str(len(files))), len(results), len(files),
                                                  result["status"].upper(), result["duration_s"], result["file"], note),
                  flush=True)

    interrupted = False
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for future in concurrent.futures.as_completed([pool.submit(run_file, f, args, out_dir, active) for f in small]):
                record(future.result())
        for entry in big:
            record(run_file(entry, args, out_dir, active))
    except KeyboardInterrupt:
        interrupted = True
        print("\nInterrupted, writing the report for the files done so far...")
        for proc in list(active):
            kill_tree(proc)

    finished = datetime.datetime.now()
    order = {f["rel"]: i for i, f in enumerate(files)}
    results.sort(key=lambda r: order[r["file"]])
    summary = {status: sum(r["status"] == status for r in results) for status in STATUS_ORDER}
    summary["total"] = len(results)
    report = {
        "meta": {
            "corpus": corpus,
            "started": started.isoformat(timespec="seconds"),
            "finished": finished.isoformat(timespec="seconds"),
            "duration_s": round((finished - started).total_seconds(), 1),
            "interrupted": interrupted,
            "files_found": len(files),
            "godot": args.godot,
            "godot_version": godot_version[-1] if godot_version else "",
            "library": library,
            "git": git_info(),
            "machine": {"system": platform.platform(), "cpu_count": os.cpu_count()},
            "options": {"jobs": args.jobs, "collision": args.collision, "lods": not args.no_lods,
                        "timeout": args.timeout, "timeout_per_mb": args.timeout_per_mb,
                        "filter": args.filter, "exclude": args.exclude, "max_size_mb": args.max_size_mb},
        },
        "summary": summary,
        "results": results,
    }
    baseline = None if args.no_baseline else (args.baseline or find_baseline(DEFAULT_REPORTS_DIR, corpus, out_dir))
    if baseline:
        report["changes"] = compare(results, baseline)

    with open(os.path.join(out_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1)
    write_junit(report, os.path.join(out_dir, "junit.xml"))
    write_html(report, os.path.join(out_dir, "report.html"))

    print("\n" + "  ".join("%s %d" % (s, summary[s]) for s in STATUS_ORDER) + "  (of %d)" % summary["total"])
    if "changes" in report:
        changes = report["changes"]
        print("Compared with %s: %d regressions, %d fixes, %d geometry changes"
              % (changes["baseline"], len(changes["regressions"]), len(changes["fixes"]), len(changes["geometry_changes"])))
        for item in changes["regressions"]:
            print("  REGRESSION %s: %s -> %s" % (item["file"], item["from"], item["to"]))
    print("Report: %s" % os.path.join(out_dir, "report.html"))
    return 1 if interrupted or any(r["status"] in FAILING for r in results) else 0


if __name__ == "__main__":
    sys.exit(main())
