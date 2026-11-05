# Checks the load stage and error a script can read from GDIFCManager:
#   godot --headless --path ifc-godot-project --script res://tests/load_status_test.gd -- --file <ifc>
# The stage id must step through the STAGE_* constants in order and end at STAGE_DONE. A file
# that can't be opened, and one web-ifc can't read, must leave a load error, and the next load
# must clear it. Exits with 1 when a check fails.
extends SceneTree

var _failures := 0
var _checks := 0
var _manager: GDIFCManager
var _path := ""
var _stages := []


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	_path = args.get("file", "")
	if _path.is_empty():
		printerr("Usage: --script res://tests/load_status_test.gd -- --file <ifc>")
		quit(2)
		return

	_manager = GDIFCManager.new()
	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = false
	_manager.geometric_settings = settings
	root.add_child(_manager)
	_manager.load_progress_changed.connect(func(_progress: float, _stage: String): _stages.append(_manager.get_load_stage_id()))
	_run.call_deferred()


func _parse_args(user_args: PackedStringArray) -> Dictionary:
	var args := {}
	var i := 0
	while i < user_args.size():
		var key := user_args[i].trim_prefix("--")
		if i + 1 < user_args.size() and not user_args[i + 1].begins_with("--"):
			args[key] = user_args[i + 1]
			i += 2
		else:
			args[key] = true
			i += 1
	return args


func _run() -> void:
	_check(GDIFCManager.STAGE_IDLE == 0 and GDIFCManager.STAGE_FAILED == 8, "stage constants keep their values")

	# A file that can't be opened fails at once
	_check(_manager.read_ifc("user://gdifc_missing.ifc") != OK, "read_ifc fails for a missing file")
	_check(_manager.get_load_error().contains("gdifc_missing.ifc"), "the load error names the missing file (got '%s')" % _manager.get_load_error())

	# A file web-ifc can't read, here with an unterminated string, fails on the worker
	var broken := FileAccess.open("user://gdifc_broken.ifc", FileAccess.WRITE)
	broken.store_string("ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('IFC4'));\nENDSEC;\nDATA;\n#1=IFCPROJECT('0YvctVUKr0kugbFTf53O9L")
	broken.close()
	_stages.clear()
	_check(_manager.read_ifc("user://gdifc_broken.ifc") == OK, "read_ifc starts loading a broken file")
	_check(_manager.get_load_error().is_empty(), "starting a load clears the load error")
	var failed := await _wait_for_load()
	if failed:
		_check(not _manager.get_load_error().is_empty(), "a failed load leaves a load error")
		_check(_manager.get_load_stage_id() == GDIFCManager.STAGE_FAILED, "a failed load ends at STAGE_FAILED (got %d)" % _manager.get_load_stage_id())
	else:
		print("web-ifc read the broken file: worker failure not checked")

	# A good file goes through the stages in order
	_stages.clear()
	_check(_manager.read_ifc(_path) == OK, "read_ifc returns OK")
	_check(_manager.get_load_error().is_empty(), "a new load clears the load error")
	failed = await _wait_for_load()
	_check(not failed, "the model loads")
	var ordered := true
	for i in range(1, _stages.size()):
		ordered = ordered and _stages[i] >= _stages[i - 1]
	_check(ordered and _stages.has(GDIFCManager.STAGE_GEOMETRY), "stage ids step forward through the stages (got %s)" % [_stages])
	_check(_manager.get_load_stage_id() == GDIFCManager.STAGE_DONE, "the load ends at STAGE_DONE (got %d)" % _manager.get_load_stage_id())
	_check(_manager.get_load_error().is_empty(), "a good load leaves no load error")
	_finish()


# True when the load failed
func _wait_for_load() -> bool:
	var result := [null]
	var on_ready := func(): result[0] = false
	var on_failed := func(): result[0] = true
	_manager.ifc_objects_ready.connect(on_ready)
	_manager.ifc_load_failed.connect(on_failed)
	while result[0] == null:
		await process_frame
	_manager.ifc_objects_ready.disconnect(on_ready)
	_manager.ifc_load_failed.disconnect(on_failed)
	return result[0]


func _check(ok: bool, what: String) -> void:
	_checks += 1
	if not ok:
		_failures += 1
		printerr("FAIL: ", what)


func _finish() -> void:
	print("%d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
