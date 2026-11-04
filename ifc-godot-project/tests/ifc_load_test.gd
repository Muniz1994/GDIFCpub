# Loads one IFC file with GDIFCManager and writes what was built to a JSON file.
# Run by tests/ifc_corpus/run_ifc_corpus.py, one Godot process per file, so a crash only fails that file:
#   godot --headless --path ifc-godot-project --script res://tests/ifc_load_test.gd -- --file <ifc> --out <json>
# Options: --collision (create collision for every element), --no-lods, --materials (read IFC materials),
# --textures (also read IFC textures, needs --materials)
extends SceneTree

var _manager: GDIFCManager
var _out_path := ""
var _result := {}
var _start_usec := 0
var _read_emitted := false
var _objects_emitted := false
var _finished := false


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	_out_path = args.get("out", "")
	var path: String = args.get("file", "")
	if path.is_empty() or _out_path.is_empty():
		printerr("Usage: --script res://tests/ifc_load_test.gd -- --file <ifc> --out <json> [--collision] [--no-lods] [--materials] [--textures]")
		quit(2)
		return

	_result = {
		"file": path,
		"godot_version": Engine.get_version_info().string,
		"read_error": OK,
		"ifc_read": false,
		"ifc_objects_ready": false,
		"ifc_load_failed": false,
		"stages": {},
	}

	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = not args.has("no-lods")
	settings.ifc_materials = args.has("materials")
	settings.ifc_textures = args.has("textures")
	_manager = GDIFCManager.new()
	_manager.geometric_settings = settings
	root.add_child(_manager)
	_manager.ifc_read.connect(_on_ifc_read)
	_manager.ifc_objects_ready.connect(_on_ifc_objects_ready)
	_manager.ifc_load_failed.connect(_on_ifc_load_failed)
	_manager.load_progress_changed.connect(_on_load_progress_changed)

	# An empty class list with collision on would skip collision, so list nothing and let every element through
	var create_collision: bool = args.has("collision")
	_start_usec = Time.get_ticks_usec()
	var err := _manager.read_ifc(path, create_collision, [])
	if err != OK:
		_result["read_error"] = err
		_finish.call_deferred(2)


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


func _elapsed_ms() -> float:
	return (Time.get_ticks_usec() - _start_usec) / 1000.0


func _on_load_progress_changed(_progress: float, stage: String) -> void:
	var stages: Dictionary = _result["stages"]
	if not stages.has(stage):
		stages[stage] = _elapsed_ms()
		# Read back by the runner when the file times out and no results are written
		print("[ifc_load_test] stage: ", stage)


func _on_ifc_read() -> void:
	_read_emitted = true
	_result["ifc_read"] = true
	_result["ifc_read_ms"] = _elapsed_ms()
	if _objects_emitted:
		_finish.call_deferred(0)


func _on_ifc_objects_ready() -> void:
	_objects_emitted = true
	_result["ifc_objects_ready"] = true
	_result["ifc_objects_ready_ms"] = _elapsed_ms()
	if _read_emitted:
		_finish.call_deferred(0)


func _on_ifc_load_failed() -> void:
	_result["ifc_load_failed"] = true
	_result["ifc_load_failed_ms"] = _elapsed_ms()
	_finish.call_deferred(1)


# Deferred so the manager is never freed from inside its own signal emission
func _finish(exit_code: int) -> void:
	if _finished:
		return
	_finished = true
	if exit_code == 0:
		_result["metrics"] = _collect_metrics()
	_result["static_memory_peak"] = OS.get_static_memory_peak_usage()

	# Saved before each step that can crash, so the results survive and the runner can tell which step did
	_result["phase"] = "model_queries"
	if not _save():
		quit(2)
		return
	if exit_code == 0:
		_query_model(_result["metrics"])
	_result["phase"] = "teardown"
	_save()

	root.remove_child(_manager)
	_manager.free()
	quit(exit_code)


func _save() -> bool:
	var file := FileAccess.open(_out_path, FileAccess.WRITE)
	if file == null:
		printerr("Could not write results to ", _out_path)
		return false
	file.store_string(JSON.stringify(_result, "\t"))
	file.close()
	return true


# IFCModel reads IfcParse directly, so these run after the load results are saved
func _query_model(metrics: Dictionary) -> void:
	var model := _manager.get_ifc_model()
	if model == null:
		return
	metrics["schema"] = model.get_schema_name()
	metrics["originating_system"] = model.get_originating_system()
	metrics["products"] = model.instances_by_type("IfcProduct").size()


func _collect_metrics() -> Dictionary:
	var metrics := {
		"ifc_nodes": 0,
		"mesh_nodes": 0,
		"surfaces": 0,
		"materials": 0,
		"vertices": 0,
		"triangles": 0,
		"collision_bodies": 0,
		"non_finite_bounds": 0,
		"nodes_with_attributes": 0,
		"nodes_with_properties": 0,
		"nodes_with_quantities": 0,
		"nodes_with_ifc_object": 0,
		"nodes_with_material_definition": 0,
		"ifc_materials": 0,
		"nodes_without_global_id": 0,
		"duplicate_global_ids": 0,
		"classes": {},
	}
	var model := _manager.get_ifc_model()
	if model:
		var georef := model.get_node_or_null("Georeference")
		if georef:
			metrics["georeference"] = {
				"eastings": georef.eastings,
				"northings": georef.northings,
				"orthogonal_height": georef.orthogonal_height,
				"crs_name": georef.crs_name,
			}

	var classes: Dictionary = metrics["classes"]
	var global_ids := {}
	# Distinct surface materials, keyed by instance id
	var materials := {}
	var bounds := AABB()
	var has_bounds := false
	for node in _manager.find_children("*", "IFCNode", true, false):
		var ifc_node := node as IFCNode
		metrics["ifc_nodes"] += 1
		classes[ifc_node.ifc_class] = classes.get(ifc_node.ifc_class, 0) + 1
		if ifc_node.global_id.is_empty():
			metrics["nodes_without_global_id"] += 1
		elif global_ids.has(ifc_node.global_id):
			metrics["duplicate_global_ids"] += 1
		else:
			global_ids[ifc_node.global_id] = true
		if not ifc_node.attributes.is_empty():
			metrics["nodes_with_attributes"] += 1
		if not ifc_node.properties.is_empty():
			metrics["nodes_with_properties"] += 1
		if not ifc_node.quantities.is_empty():
			metrics["nodes_with_quantities"] += 1
		if ifc_node.ifc_object != null:
			metrics["nodes_with_ifc_object"] += 1
		if ifc_node.material_definition != null:
			metrics["nodes_with_material_definition"] += 1
		for child in ifc_node.get_children():
			if child is StaticBody3D:
				metrics["collision_bodies"] += 1

		var mesh := ifc_node.mesh
		if mesh == null or mesh.get_surface_count() == 0:
			continue
		metrics["mesh_nodes"] += 1
		for surface in mesh.get_surface_count():
			metrics["surfaces"] += 1
			var material := mesh.surface_get_material(surface)
			if material:
				materials[material.get_instance_id()] = true
			var vertex_count: int = mesh.surface_get_array_len(surface)
			var index_count: int = mesh.surface_get_array_index_len(surface)
			metrics["vertices"] += vertex_count
			metrics["triangles"] += (index_count if index_count > 0 else vertex_count) / 3
		var node_bounds := ifc_node.global_transform * mesh.get_aabb()
		if not (node_bounds.position.is_finite() and node_bounds.size.is_finite()):
			metrics["non_finite_bounds"] += 1
		elif has_bounds:
			bounds = bounds.merge(node_bounds)
		else:
			bounds = node_bounds
			has_bounds = true

	metrics["materials"] = materials.size()
	# IfcMaterials read into the material library, with ifc_materials on
	if _manager.material_library:
		metrics["ifc_materials"] = _manager.material_library.materials.size()
	if has_bounds:
		metrics["bounds_min"] = [bounds.position.x, bounds.position.y, bounds.position.z]
		metrics["bounds_size"] = [bounds.size.x, bounds.size.y, bounds.size.z]
	return metrics
