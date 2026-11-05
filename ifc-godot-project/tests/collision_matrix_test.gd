# Checks GDIFCCollisionMatrix rule matching, then the collision bodies created for a model:
#   godot --headless --path ifc-godot-project --script res://tests/collision_matrix_test.gd -- [--file <ifc>] [--mode matrix|classes|all] [--matrix <tres>]
# --mode matrix (default) loads with a matrix of every shape type, or the --matrix resource,
# classes with read_ifc()'s class list, and all with an empty class list. Exits with 1 when a check fails.
extends SceneTree

const SHAPE_NAMES := ["None", "Trimesh", "Simple Convex", "Box", "Decompose Convex"]

var _failures := 0
var _checks := 0
var _manager: GDIFCManager
var _matrix: GDIFCCollisionMatrix
var _mode := "matrix"
var _schema := "IFC4"


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	_test_matching()

	var path: String = args.get("file", "")
	if path.is_empty():
		_finish()
		return

	_mode = args.get("mode", "matrix")
	_schema = _read_schema(path)
	_manager = GDIFCManager.new()
	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = false
	_manager.geometric_settings = settings
	root.add_child(_manager)
	_manager.ifc_read.connect(_on_ifc_read)
	_manager.ifc_load_failed.connect(func(): _check(false, "the model loads"); _finish())

	var err := OK
	match _mode:
		"matrix":
			_matrix = load(args["matrix"]) if args.has("matrix") else _model_matrix()
			_manager.collision_matrix = _matrix
			# Ignored when a matrix is set
			err = _manager.read_ifc(path, false, ["IfcWall"])
		"classes":
			err = _manager.read_ifc(path, true, ["IfcWall", "IfcSlab"])
		"all":
			err = _manager.read_ifc(path, true, [])
	if err != OK:
		_check(false, "read_ifc returns OK")
		_finish()


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


func _read_schema(path: String) -> String:
	var file := FileAccess.open(path, FileAccess.READ)
	if not file:
		return "IFC4"
	var head := file.get_buffer(min(file.get_length(), 4096)).get_string_from_ascii()
	var regex := RegEx.create_from_string("FILE_SCHEMA\\s*\\(\\s*\\(\\s*'([^']+)'")
	var found := regex.search(head)
	return found.get_string(1) if found else "IFC4"


func _check(ok: bool, what: String) -> void:
	_checks += 1
	if not ok:
		_failures += 1
		printerr("FAIL: ", what)


func _rule(ifc_class: String, shape_type: GDIFCCollisionRule.ShapeType, layer := 1, include_subtypes := true) -> GDIFCCollisionRule:
	var rule := GDIFCCollisionRule.new()
	rule.ifc_class = ifc_class
	rule.shape_type = shape_type
	rule.collision_layer = layer
	rule.include_subtypes = include_subtypes
	return rule


func _test_matching() -> void:
	var wall := _rule("IfcWall", GDIFCCollisionRule.SHAPE_TRIMESH)
	var building_element := _rule("IfcBuildingElement", GDIFCCollisionRule.SHAPE_BOX)
	var door := _rule("IfcDoor", GDIFCCollisionRule.SHAPE_NONE)
	var exact_slab := _rule("IfcSlab", GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX, 1, false)
	var matrix := GDIFCCollisionMatrix.new()
	matrix.rules = [building_element, wall, door, exact_slab]

	_check(wall.resource_name == "IfcWall", "a rule is named after its class")
	_check(matrix.get_rule_for_class("IfcWall") == wall, "exact class")
	_check(matrix.get_rule_for_class("ifcwall") == wall, "class names ignore case")
	_check(matrix.get_rule_for_class("IfcWallStandardCase") == wall, "IFC4 subtype gets the closest rule, not an earlier more general one")
	_check(matrix.get_rule_for_class("IfcWallStandardCase", "IFC2X3") == wall, "IFC2X3 subtype")
	_check(matrix.get_rule_for_class("IfcColumn") == building_element, "subtype of an abstract class")
	_check(matrix.get_rule_for_class("IfcColumn", "IFC4X3_ADD2") == building_element, "IfcBuildingElement also matches IFC4X3's IfcBuiltElement")
	_check(matrix.get_rule_for_class("IfcColumn", "IFC4X3") == building_element, "IFC4X3 is read as IFC4X3_ADD2")
	_check(matrix.get_rule_for_class("IfcDoor") == null, "a None rule gives no collision")
	_check(matrix.get_rule_for_class("IfcDoorStandardCase") == null, "a None rule also covers subtypes")
	_check(matrix.get_rule_for_class("IfcSlab") == exact_slab, "exact rule matches its class")
	_check(matrix.get_rule_for_class("IfcSlabStandardCase") == building_element, "a rule without subtypes skips them")
	_check(matrix.get_rule_for_class("IfcFurnishingElement") == null, "no match and no default rule gives no collision")
	_check(matrix.get_rule_for_class("IfcNotAClass") == null, "unknown class")

	var default_rule := _rule("", GDIFCCollisionRule.SHAPE_BOX, 8)
	matrix.default_rule = default_rule
	_check(matrix.get_rule_for_class("IfcFurnishingElement") == default_rule, "the default rule catches the rest")
	_check(matrix.get_rule_for_class("IfcDoor") == null, "a None rule wins over the default rule")

	var builtin := GDIFCCollisionMatrix.new()
	builtin.rules = [_rule("IfcBuiltElement", GDIFCCollisionRule.SHAPE_BOX)]
	_check(builtin.get_rule_for_class("IfcWall", "IFC4") != null, "IfcBuiltElement also matches IFC4's IfcBuildingElement")

	var decompose := _rule("IfcMember", GDIFCCollisionRule.SHAPE_DECOMPOSE_CONVEX)
	var properties := decompose.get_property_list().filter(func(p): return p.name == "decomposition_settings")
	_check(not properties.is_empty() and properties[0].usage & PROPERTY_USAGE_EDITOR, "decomposition settings are shown for Decompose Convex")
	properties = wall.get_property_list().filter(func(p): return p.name == "decomposition_settings")
	_check(not properties.is_empty() and not (properties[0].usage & PROPERTY_USAGE_EDITOR), "decomposition settings are hidden for other shapes")

	# Saved matrices store shape types as numbers
	var shape_values := [GDIFCCollisionRule.SHAPE_NONE, GDIFCCollisionRule.SHAPE_TRIMESH, GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX,
		GDIFCCollisionRule.SHAPE_BOX, GDIFCCollisionRule.SHAPE_DECOMPOSE_CONVEX]
	_check(shape_values == [0, 1, 2, 3, 4], "shape types keep their saved values (new ones go at the end)")


func _model_matrix() -> GDIFCCollisionMatrix:
	var matrix := GDIFCCollisionMatrix.new()
	matrix.rules = [
		_rule("IfcWall", GDIFCCollisionRule.SHAPE_TRIMESH, 2),
		_rule("IfcSlab", GDIFCCollisionRule.SHAPE_BOX, 4),
		_rule("IfcStairFlight", GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX, 8),
		_rule("IfcMember", GDIFCCollisionRule.SHAPE_DECOMPOSE_CONVEX, 16),
		_rule("IfcRailing", GDIFCCollisionRule.SHAPE_DECOMPOSE_CONVEX, 16),
		_rule("IfcDoor", GDIFCCollisionRule.SHAPE_NONE),
		_rule("IfcSpace", GDIFCCollisionRule.SHAPE_NONE),
	]
	matrix.default_rule = _rule("", GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX, 32)
	return matrix


# What the class list of read_ifc() gives: trimesh on layer 1, for exact class names
func _expected_shape(ifc_class: String) -> Dictionary:
	match _mode:
		"matrix":
			var rule := _matrix.get_rule_for_class(ifc_class, _schema)
			if not rule:
				return {}
			var shape_type := rule.shape_type
			# Decomposition is only built at import; read_ifc() builds a hull instead
			if shape_type == GDIFCCollisionRule.SHAPE_DECOMPOSE_CONVEX:
				shape_type = GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX
			return {"shape_type": shape_type, "layer": rule.collision_layer}
		"classes":
			if ifc_class in ["IfcWall", "IfcSlab"]:
				return {"shape_type": GDIFCCollisionRule.SHAPE_TRIMESH, "layer": 1}
			return {}
		_:
			return {"shape_type": GDIFCCollisionRule.SHAPE_TRIMESH, "layer": 1}


func _on_ifc_read() -> void:
	var stats := {}
	for node in _manager.find_children("*", "IFCNode", true, false):
		var element := node as IFCNode
		var body := element.get_node_or_null(NodePath(element.name + "_col")) as StaticBody3D
		var expected := _expected_shape(element.ifc_class) if element.mesh else {}
		var key := "%s -> %s" % [element.ifc_class, "none" if expected.is_empty() else SHAPE_NAMES[expected.shape_type]]
		stats[key] = stats.get(key, 0) + 1

		if expected.is_empty():
			_check(body == null, "%s has no collision" % element.name)
			continue
		_check(body != null, "%s has a collision body" % element.name)
		if not body:
			continue
		_check(body.collision_layer == expected.layer, "%s is on layer %d, not %d" % [element.name, expected.layer, body.collision_layer])
		var shapes := body.get_children().filter(func(c): return c is CollisionShape3D)
		_check(shapes.size() == 1, "%s has one shape" % element.name)
		if shapes.is_empty():
			continue
		var collision_shape: CollisionShape3D = shapes[0]
		var aabb := element.mesh.get_aabb()
		match expected.shape_type:
			GDIFCCollisionRule.SHAPE_TRIMESH:
				_check(collision_shape.shape is ConcavePolygonShape3D, "%s has a trimesh" % element.name)
			GDIFCCollisionRule.SHAPE_SIMPLE_CONVEX:
				var convex := collision_shape.shape as ConvexPolygonShape3D
				_check(convex != null and convex.points.size() >= 4, "%s has a convex hull" % element.name)
				if convex:
					var points_aabb := AABB(convex.points[0], Vector3())
					for p in convex.points:
						points_aabb = points_aabb.expand(p)
					_check(points_aabb.grow(0.001).encloses(aabb) and aabb.grow(0.001).encloses(points_aabb),
						"%s hull spans its mesh" % element.name)
			GDIFCCollisionRule.SHAPE_BOX:
				var box := collision_shape.shape as BoxShape3D
				_check(box != null, "%s has a box" % element.name)
				if box:
					var box_aabb := AABB(collision_shape.position - box.size / 2, box.size)
					_check(box_aabb.grow(0.001).encloses(aabb) and aabb.grow(0.002).encloses(box_aabb),
						"%s box matches its mesh: %s vs %s" % [element.name, box_aabb, aabb])

	var keys := stats.keys()
	keys.sort()
	for key in keys:
		print("  %s: %d" % [key, stats[key]])
	_finish()


func _finish() -> void:
	print("collision_matrix_test: %d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
