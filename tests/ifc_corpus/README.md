# IFC corpus tests

Loads every `.ifc` file in a folder with `GDIFCManager` and reports what happened to each one, so changes to GDIFC or web-ifc can be checked against a large set of real files.

Each file is loaded by its own headless Godot process running [`ifc-godot-project/tests/ifc_load_test.gd`](../../ifc-godot-project/tests/ifc_load_test.gd). A crash or a hang only fails that file.

## Running

Build the debug library into the test project first. Headless Godot loads that one:

```bash
scons platform=windows target=template_debug local=1
```

Then point the runner at a folder of IFC files:

```bash
python tests/ifc_corpus/run_ifc_corpus.py D:/IFC/TestFiles --godot "C:/Program Files/Godot/Godot_v4.8_win64.exe"
```

The Godot binary can also come from the `GODOT` environment variable, and the folder from `IFC_TEST_FILES`. On Windows, pass the plain executable rather than the `_console` one, so the peak memory measured is Godot's own.

Useful options:

| Option | Effect |
| --- | --- |
| `--filter REGEX` / `--exclude REGEX` | Only, or skip, files whose path relative to the folder matches |
| `--max-size-mb N` | Skip files larger than N MB, for a quick run |
| `-j N` | Files loaded at the same time (default: a third of the CPU threads, up to 4) |
| `--big-file-mb N` | Files above N MB (default 50) load one at a time, after the others, to limit memory use |
| `--timeout S` / `--timeout-per-mb S` | Time allowed per file: S seconds plus S per MB (defaults 120 and 6) |
| `--collision` | Also create collision shapes for every element |
| `--no-lods` | Don't generate mesh LODs |
| `--materials` / `--textures` | Read IFC materials, and also their textures |
| `--list` | Print the files that would be loaded, then exit |

Ctrl+C stops the run and still writes the report for the files done so far. The exit code is 1 when a file failed, crashed or timed out, so the runner can gate CI.

## Reports

Each run writes to `tests/reports/<date>_<time>/`, which git ignores:

- `report.html`: a summary, the changes since the last run, the most common error messages, and a table of every file. Click a row for its checks, load stages, model statistics, IFC classes and the end of its log.
- `report.json`: everything in the HTML report, for scripts.
- `junit.xml`: one test case per file, for CI.
- `logs/`: the full Godot output for each file.

Each run is compared with the latest earlier run of the same folder (or the `report.json` given with `--baseline`). The comparison lists files whose status got worse or better, files whose node or triangle count changed, and files that became much slower.

## Statuses

| Status | Meaning |
| --- | --- |
| pass | Loaded, with geometry and metadata, and nothing logged as an error |
| warn | Loaded, but web-ifc or Godot logged errors (usually geometry it couldn't build), no geometry was created, or some nodes lack metadata |
| fail | `ifc_load_failed` was emitted, `read_ifc` returned an error, or a mesh has NaN or infinite bounds |
| crash | Godot exited abnormally, for example with an access violation |
| timeout | Godot was killed after the time allowed. The report gives the last load stage reached |

File names follow the [IfcOpenShell test files](https://github.com/IfcOpenShell/files) conventions. Files tagged `--no-geometry` pass without geometry, and files tagged `--accept-errors` pass with errors logged.

web-ifc ends its geometry messages with an express ID. The runner looks up the entity, so `unexpected mesh type 16` becomes `unexpected mesh type #16 IFCFACETEDBREP`. Messages printed for every file are ignored; they are listed in `IGNORED_MESSAGES` in `run_ifc_corpus.py`.
