# Checks the typed entity classes against the IFC file, whatever its schema:
#   godot --headless --path ifc-godot-project --script res://tests/entities_test.gd -- --file <ifc> [--limit <n>]
# Every entity must get a class of its own type. Every attribute property of that class must read
# what get_attribute() reads: under its own name, under the old name of an attribute IFC4X3
# renamed, or as null when the file's schema doesn't have it. Unset attributes read as null.
# Setters must write through the same lookup, convert enumerations and plain property values, and
# refuse attributes the schema lacks and values that don't fit, without changing the entity. Exits
# with 1 when a check fails.
extends SceneTree

var _failures := 0
var _checks := 0
var _manager: GDIFCManager
var _limit := 100000
# Attribute properties per class, without inherited ones
var _class_properties := {}


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	var path: String = args.get("file", "")
	if path.is_empty():
		printerr("Usage: --script res://tests/entities_test.gd -- --file <ifc> [--limit <n>]")
		quit(2)
		return
	if args.has("limit"):
		_limit = int(args["limit"])

	_manager = GDIFCManager.new()
	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = false
	_manager.geometric_settings = settings
	root.add_child(_manager)
	_manager.ifc_objects_ready.connect(_on_ifc_objects_ready)
	_manager.ifc_load_failed.connect(func(): _check(false, "the model loads"); _finish())
	if _manager.read_ifc(path) != OK:
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


func _on_ifc_objects_ready() -> void:
	var model := _manager.get_ifc_model()
	var schema := model.get_schema_name()
	var entities := 0
	var untyped := {}
	var stats := {"own name": 0, "old name": 0, "not in schema": 0}

	# Express ids can have gaps, so stop after a long run of missing ones
	var id := 1
	var missing := 0
	while missing < 5000 and entities < _limit:
		var e: GDIFCEntityBase = model.get_by_id(id)
		id += 1
		if e == null:
			missing += 1
			continue
		missing = 0
		entities += 1
		if e.get_class() != e.get_type():
			untyped[e.get_type()] = true
		_check_entity(e, stats)

	_check(untyped.is_empty(), "every entity gets a class of its own type (missing: %s)" % ", ".join(untyped.keys()))
	print("%s: %d entities; properties read by their own name %d, by an old name %d, null as not in the schema %d"
		% [schema, entities, stats["own name"], stats["old name"], stats["not in schema"]])

	_check_renames(model, schema)
	_check_setters(model, schema)
	_finish()


# Attribute properties of the entity's class and its ancestors. Inverse properties have no setter.
func _attribute_properties(cls: String) -> PackedStringArray:
	if _class_properties.has(cls):
		return _class_properties[cls]
	var names := PackedStringArray()
	if cls != "GDIFCEntityBase" and ClassDB.class_exists(cls):
		names = _attribute_properties(ClassDB.get_parent_class(cls)).duplicate()
		for p in ClassDB.class_get_property_list(cls, true):
			if not ClassDB.class_get_property_setter(cls, p.name).is_empty():
				names.append(p.name)
	_class_properties[cls] = names
	return names


func _check_entity(e: GDIFCEntityBase, stats: Dictionary) -> void:
	var names := e.get_attribute_names()
	var properties := _attribute_properties(e.get_class())
	for p in properties:
		var typed = e.get(p)
		_checks += 1
		if p in names:
			stats["own name"] += 1
			var expected = e.get_attribute(p)
			if not _same(typed, expected):
				_fail("%s #%d %s reads %s, as get_attribute() does (got %s)"
					% [e.get_type(), e.get_express_id(), p, _show(expected), _show(typed)])
		elif typed == null:
			stats["not in schema"] += 1
		else:
			# A renamed attribute: it must be one the class has no property for
			var source := ""
			for n in names:
				if not n in properties and _same(typed, e.get_attribute(n)):
					source = n
					break
			stats["old name"] += 1
			if source.is_empty():
				_fail("%s #%d %s isn't in %s, so it reads as null or as a renamed attribute (got %s)"
					% [e.get_type(), e.get_express_id(), p, names, _show(typed)])


# Renames that must be read through the IFC4X3 name
func _check_renames(model: IFCModel, schema: String) -> void:
	if schema == "IFC4X3_ADD2":
		return
	var cases := [["IfcProperty", "Specification", "Description"]]
	if schema == "IFC2X3":
		cases.append_array([
			["IfcExternalReference", "Identification", "ItemReference"],
			["IfcOrganization", "Identification", "Id"],
			["IfcPerson", "Identification", "Id"],
		])
	for c in cases:
		var checked := 0
		for e in model.instances_by_type(c[0]):
			if e.get_attribute(c[2]) == null:
				continue
			_check(e.get(c[1]) == e.get_attribute(c[2]), "%s #%d %s reads %s" % [e.get_type(), e.get_express_id(), c[1], c[2]])
			checked += 1
			if checked >= 20:
				break


func _check_setters(model: IFCModel, schema: String) -> void:
	# Fragments used to test geometry have no project
	var project := model.get_project()
	if project == null:
		print("No IfcProject: setters not checked")
		return

	project.Name = "GDIFC test"
	_check(project.get_attribute("Name") == "GDIFC test", "Name is set")

	# Enumerations take any case, and keep their value when given one the schema lacks
	var element: GDIFCEntityBase = null
	for e in model.instances_by_type("IfcElement"):
		if "PredefinedType" in e.get_attribute_names() and "PredefinedType" in _attribute_properties(e.get_class()):
			element = e
			break
	if element != null:
		element.set("PredefinedType", "notdefined")
		_check(element.get_attribute("PredefinedType") == "NOTDEFINED", "%s.PredefinedType is set from lower case" % element.get_type())
		element.set("PredefinedType", "NOT_A_VALUE")
		_check(element.get_attribute("PredefinedType") == "NOTDEFINED", "%s.PredefinedType keeps its value when set to one its enumeration lacks" % element.get_type())

	# Entity references must fit the declared type
	var owner_history = project.get_attribute("OwnerHistory")
	project.set("OwnerHistory", project)
	_check(_same(project.get_attribute("OwnerHistory"), owner_history), "OwnerHistory refuses an IfcProject")

	# Attributes the schema lacks are refused instead of writing into another one
	if schema == "IFC2X3":
		for e in model.instances_by_type("IfcSpace"):
			var before = e.get_attribute("InteriorOrExteriorSpace")
			e.PredefinedType = "INTERNAL"
			_check(_same(e.get_attribute("InteriorOrExteriorSpace"), before), "IFC2X3 IfcSpace.PredefinedType is refused")
			_check(e.PredefinedType == null, "IFC2X3 IfcSpace.PredefinedType reads as null")
			break
		for e in model.instances_by_type("IfcDoor"):
			e.PredefinedType = "DOOR"
			_check(e.PredefinedType == null, "IFC2X3 IfcDoor.PredefinedType is refused without crashing")
			break

	project.Description = null
	_check(project.Description == null and project.get_attribute("Description") == null, "an unset attribute reads as null")

	_check_property_values(model)

	# Attributes whose type differs between schemas take the type of the file's schema
	for q in model.instances_by_type("IfcQuantityCount"):
		q.CountValue = 3
		var count = q.get_attribute("CountValue")
		_check(count == 3 and (typeof(count) == TYPE_INT) == (schema == "IFC4X3_ADD2"),
			"IfcQuantityCount.CountValue is set as %s" % ("an int" if schema == "IFC4X3_ADD2" else "a float"))
		break


# Property values are instances of defined types (IfcLabel, IfcReal...) in an IfcValue select, read
# and set as plain values
func _check_property_values(model: IFCModel) -> void:
	var label: GDIFCEntityBase = null
	for prop in model.instances_by_type("IfcPropertySingleValue"):
		var value = prop.NominalValue
		if value == null:
			continue
		_check(typeof(value) != TYPE_OBJECT and _same(value, prop.get_attribute("NominalValue")),
			"IfcPropertySingleValue #%d NominalValue reads as a plain value (got %s)" % [prop.get_express_id(), _show(value)])
		# An unknown IfcLogical reads as "UNKNOWN" too
		if label == null and typeof(value) == TYPE_STRING and value != "UNKNOWN":
			label = prop
	if label == null:
		return

	var wrapper := _step_value(model, label)
	label.NominalValue = "GDIFC"
	_check(label.NominalValue == "GDIFC", "NominalValue takes a plain value")
	_check(_step_value(model, label) == "%s('GDIFC')" % wrapper.get_slice("(", 0),
		"NominalValue keeps its type, %s (written %s)" % [wrapper, _step_value(model, label)])
	label.NominalValue = null
	_check(label.NominalValue == null, "NominalValue is unset by null")
	label.NominalValue = 2.5
	_check(label.NominalValue == 2.5 and _step_value(model, label).begins_with("IFCREAL(2.5"),
		"an unset NominalValue takes a float as an IfcReal (written %s)" % _step_value(model, label))
	label.NominalValue = [1]
	_check(label.NominalValue == 2.5, "NominalValue refuses an array")

	for list in model.instances_by_type("IfcPropertyListValue"):
		list.ListValues = ["a", 1.5]
		_check(_same(list.ListValues, ["a", 1.5]), "ListValues takes plain values (got %s)" % _show(list.ListValues))
		break


# The NominalValue of a property as written to the file, such as IFCLABEL('Wall')
func _step_value(model: IFCModel, prop: GDIFCEntityBase) -> String:
	var step := model.to_step_string()
	var start := step.find("#%d=" % prop.get_express_id())
	var line := step.substr(start, step.find(";", start) - start)
	# Name, Description, NominalValue, Unit
	var regex := RegEx.create_from_string("^#\\d+=IFCPROPERTYSINGLEVALUE\\((?:'(?:[^']|'')*'|\\$),(?:'(?:[^']|'')*'|\\$),(\\$|[A-Z0-9]+\\((?:'(?:[^']|'')*'|[^)]*)\\))")
	var found := regex.search(line)
	return found.get_string(1) if found else line


func _same(a, b) -> bool:
	if a == null or b == null:
		return a == null and b == null
	if typeof(a) == TYPE_OBJECT and typeof(b) == TYPE_OBJECT:
		return a.get_type() == b.get_type() and a.get_express_id() == b.get_express_id()
	if typeof(a) == TYPE_STRING and typeof(b) == TYPE_BOOL:
		return a == ("TRUE" if b else "FALSE")
	# Integer attributes written as reals read truncated
	if typeof(a) == TYPE_INT and typeof(b) == TYPE_FLOAT:
		return a == int(b)
	if typeof(a) == TYPE_FLOAT or typeof(b) == TYPE_FLOAT:
		return (typeof(a) == TYPE_FLOAT or typeof(a) == TYPE_INT) and (typeof(b) == TYPE_FLOAT or typeof(b) == TYPE_INT) and is_equal_approx(a, b)
	if _is_array(a) and _is_array(b):
		if a.size() != b.size():
			return false
		for i in a.size():
			if not _same(a[i], b[i]):
				return false
		return true
	return typeof(a) == typeof(b) and a == b


func _is_array(v) -> bool:
	return typeof(v) >= TYPE_ARRAY and typeof(v) <= TYPE_PACKED_VECTOR4_ARRAY



func _show(v) -> String:
	if typeof(v) == TYPE_OBJECT and v != null:
		return "%s #%d" % [v.get_type(), v.get_express_id()]
	return str(v).left(80)


func _check(ok: bool, what: String) -> void:
	_checks += 1
	if not ok:
		_fail(what)


func _fail(what: String) -> void:
	_failures += 1
	if _failures <= 30:
		printerr("FAIL: ", what)


func _finish() -> void:
	print("%d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
