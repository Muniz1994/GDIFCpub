# Checks GDIFCLoaderSettings.ifc_objects:
#   godot --headless --path ifc-godot-project --script res://tests/ifc_objects_test.gd -- --file <ifc>
# With it off, nodes and material definitions must still get their attributes, property sets and
# material properties, but no ifc_object, and the IFCModel's queries must find nothing. A saved copy
# of the model must emit ifc_objects_ready without reading the file again. With it on, the saved
# copy must relink its ifc_objects. Exits with 1 when a check fails.
extends SceneTree

var _failures := 0
var _checks := 0


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	if not args.has("file"):
		printerr("Usage: --script res://tests/ifc_objects_test.gd -- --file <ifc>")
		quit(2)
		return
	_run.call_deferred(args["file"])


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


func _run(path: String) -> void:
	_check(GDIFCLoaderSettings.new().ifc_objects, "ifc_objects is on by default")

	# Off: the data without the objects
	var manager := await _load(path, false)
	var nodes := _nodes(manager)
	_check(nodes.any(func(n: IFCNode): return not n.properties.is_empty()), "nodes get their property sets")
	_check(nodes.any(func(n: IFCNode): return not n.attributes.is_empty()), "nodes get their attributes")
	_check(nodes.all(func(n: IFCNode): return n.ifc_object == null), "nodes get no ifc_object")
	var model := manager.get_ifc_model()
	_check(model.instances_by_type("IfcProduct").is_empty() and model.get_units().is_empty(), "the IFCModel's queries find nothing")
	_check(model.get_project() == null, "the IFCModel has no project")
	_check(manager.get_ifc_object_by_global_id(nodes[0].global_id) == null, "get_ifc_object_by_global_id finds nothing")
	if manager.material_library:
		var definitions: Array = manager.material_library.definitions + manager.material_library.materials
		_check(not definitions.is_empty() and definitions.all(func(d: GDIFCMaterialDefinition): return d.ifc_object == null),
			"material definitions get no ifc_object")

	# A saved copy has everything already
	var reloaded := await _reload(manager)
	_check(reloaded[1] <= 2, "a saved copy without IFC objects is ready within two frames (took %d)" % reloaded[1])
	_check(_nodes(reloaded[0]).all(func(n: IFCNode): return n.ifc_object == null), "a saved copy without IFC objects doesn't relink them")
	_check(_nodes(reloaded[0]).any(func(n: IFCNode): return not n.properties.is_empty()), "a saved copy keeps its property sets")

	# On: the saved copy relinks its objects from the file
	manager = await _load(path, true)
	_check(_nodes(manager).any(func(n: IFCNode): return n.ifc_object != null), "with IFC objects on, nodes get their ifc_object")
	_check(not manager.get_ifc_model().instances_by_type("IfcProduct").is_empty(), "with IFC objects on, the IFCModel's queries work")
	reloaded = await _reload(manager)
	_check(_nodes(reloaded[0]).any(func(n: IFCNode): return n.ifc_object != null), "a saved copy with IFC objects relinks them")
	_finish()


func _nodes(manager: GDIFCManager) -> Array:
	return manager.find_children("*", "IFCNode", true, false)


func _load(path: String, ifc_objects: bool) -> GDIFCManager:
	var manager := GDIFCManager.new()
	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = false
	settings.ifc_materials = true
	settings.ifc_objects = ifc_objects
	manager.geometric_settings = settings
	root.add_child(manager)
	await _wait_for_ready(manager, path)
	return manager


# The manager saved as a scene and instanced again, and how many frames it took to be ready
func _reload(manager: GDIFCManager) -> Array:
	var scene := PackedScene.new()
	_check(scene.pack(manager) == OK, "the model packs into a scene")
	manager.queue_free()
	var copy := scene.instantiate() as GDIFCManager
	var frames := [0]
	var done := [false]
	copy.ifc_objects_ready.connect(func(): done[0] = true)
	root.add_child(copy)
	while not done[0] and frames[0] < 600:
		await process_frame
		frames[0] += 1
	_check(done[0], "the saved copy emits ifc_objects_ready")
	return [copy, frames[0]]


func _wait_for_ready(manager: GDIFCManager, path: String) -> void:
	var done := [false]
	manager.ifc_objects_ready.connect(func(): done[0] = true)
	manager.ifc_load_failed.connect(func(): done[0] = true; _check(false, "%s loads" % path))
	if manager.read_ifc(path) != OK:
		_check(false, "read_ifc returns OK")
		return
	while not done[0]:
		await process_frame


func _check(ok: bool, what: String) -> void:
	_checks += 1
	if not ok:
		_failures += 1
		printerr("FAIL: ", what)


func _finish() -> void:
	print("%d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
