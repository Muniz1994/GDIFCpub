# Checks the property and quantity sets of a model against its IFC file:
#   godot --headless --path ifc-godot-project --script res://tests/properties_test.gd -- --file <ifc> [--expect <kinds>]
# Every value in properties and quantities must have its details, every property of an element's type
# must be on the element, and only those may be marked from_type. The type's sets are found by walking
# ifc_object, as the documentation shows. --expect is a comma-separated list of what the file must
# contain: from_type, complex, table, complex_quantity, units. Exits with 1 when a check fails.
extends SceneTree

var _failures := 0
var _checks := 0
var _manager: GDIFCManager
var _expect := PackedStringArray()
var _found := {"from_type": 0, "complex": 0, "table": 0, "complex_quantity": 0, "units": 0}


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	var path: String = args.get("file", "")
	if path.is_empty():
		printerr("Usage: --script res://tests/properties_test.gd -- --file <ifc> [--expect from_type,complex,table,complex_quantity,units]")
		quit(2)
		return
	if args.has("expect"):
		_expect = String(args["expect"]).split(",", false)

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


func _check(condition: bool, message: String) -> void:
	_checks += 1
	if not condition:
		_failures += 1
		printerr("FAIL: ", message)


func _on_ifc_objects_ready() -> void:
	var units := _manager.get_ifc_model().get_units()
	print("units: ", units)
	if not units.is_empty():
		_found["units"] += 1

	var nodes := _manager.find_children("*", "IFCNode", true, false)
	for node: IFCNode in nodes:
		_check_details(node.name, node.properties, node.property_details, "properties")
		_check_details(node.name, node.quantities, node.quantity_details, "quantities")
		_check_type_sets(node)

	print("nodes: ", nodes.size(), " found: ", _found)
	for kind in _expect:
		_check(_found.get(kind, 0) > 0, "the file has %s values" % kind)
	_finish.call_deferred()


# Every set and value has its details, nested values included
func _check_details(owner: String, values: Dictionary, details: Dictionary, nested_key: String) -> void:
	_check(values.keys() == details.keys(), "%s: the details have the same keys as the values (%s vs %s)" % [owner, values.keys(), details.keys()])
	for set_name in values:
		var set_values: Dictionary = values[set_name]
		var set_details: Dictionary = details.get(set_name, {})
		for name in set_values:
			_check(set_details.has(name), "%s: %s.%s has details" % [owner, set_name, name])
			var detail: Dictionary = set_details.get(name, {})
			if detail.get("from_type", false):
				_found["from_type"] += 1
			if detail.has("unit"):
				_found["units"] += 1
			if detail.has(nested_key):
				_found["complex" if nested_key == "properties" else "complex_quantity"] += 1
				_check(set_values[name] is Dictionary, "%s: complex %s.%s holds a Dictionary" % [owner, set_name, name])
				_check_details(owner, {name: set_values[name]}, {name: detail[nested_key]}, nested_key)
			if set_values[name] is Dictionary and set_values[name].has("defining_values"):
				_found["table"] += 1


# The sets of the element's type, walked from ifc_object
func _type_sets(entity: GDIFCEntityBase) -> Dictionary:
	var sets := {}
	var types := []
	for rel in entity.get_inverse("IsTypedBy"): # IFC4 and later
		types.append(rel.get_attribute("RelatingType"))
	for rel in entity.get_inverse("IsDefinedBy"): # IFC2X3
		if rel.is_a("IfcRelDefinesByType"):
			types.append(rel.get_attribute("RelatingType"))
	for type in types:
		var definitions = type.get_attribute("HasPropertySets")
		if not definitions is Array:
			continue
		for definition: GDIFCEntityBase in definitions:
			var names := PackedStringArray()
			if definition.is_a("IfcPropertySet"):
				for property: GDIFCEntityBase in definition.get_attribute("HasProperties"):
					names.append(property.get_attribute("Name"))
			elif definition.is_a("IfcElementQuantity"):
				for quantity: GDIFCEntityBase in definition.get_attribute("Quantities"):
					names.append(quantity.get_attribute("Name"))
			else:
				continue
			# Sets without a name are keyed by their class
			var set_name = definition.get_attribute("Name")
			sets[set_name if set_name else definition.get_type()] = names
	return sets


func _check_type_sets(node: IFCNode) -> void:
	if node.ifc_object == null:
		return
	var type_sets := _type_sets(node.ifc_object)
	var all_values := node.properties.merged(node.quantities)
	var all_details := node.property_details.merged(node.quantity_details)
	for set_name in type_sets:
		for name in type_sets[set_name]:
			_check(all_values.has(set_name) and all_values[set_name].has(name),
				"%s: the type's %s.%s is on the element" % [node.name, set_name, name])
	for set_name in all_details:
		for name in all_details[set_name]:
			if all_details[set_name][name].get("from_type", false):
				_check(type_sets.has(set_name) and type_sets[set_name].has(name),
					"%s: %s.%s marked from_type is in the type's sets" % [node.name, set_name, name])


func _finish() -> void:
	print("%d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
