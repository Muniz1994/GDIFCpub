# Checks georeferencing read from files of every schema:
#   godot --headless --path ifc-godot-project --script res://tests/georeference_test.gd -- --ifc2x3 <ifc> --ifc4x3 <ifc>
# --ifc2x3 is any IFC2X3 file with a project and a site. A copy with the ePSet_MapConversion and
# ePSet_ProjectedCRS property sets on its project must get a Georeference node holding their values,
# and IFCModel.get_site_location() must read the site's latitude, longitude and elevation.
# --ifc4x3 is an IFC4X3 file with an IfcMapConversion. A copy using IfcMapConversionScaled must
# get its per-axis factors, and loaded relative to the original it must be scaled by them; the
# original loaded relative to itself must not move. Exits with 1 when a check fails.
extends SceneTree

const FACTORS := Vector3(2.0, 3.0, 4.0) # IFC x, y, z

var _failures := 0
var _checks := 0


func _initialize() -> void:
	var args := _parse_args(OS.get_cmdline_user_args())
	if not args.has("ifc2x3") or not args.has("ifc4x3"):
		printerr("Usage: --script res://tests/georeference_test.gd -- --ifc2x3 <ifc> --ifc4x3 <ifc>")
		quit(2)
		return
	_run.call_deferred(args["ifc2x3"], args["ifc4x3"])


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


func _run(ifc2x3_path: String, ifc4x3_path: String) -> void:
	await _check_property_sets(ifc2x3_path)
	await _check_scaled(ifc4x3_path)
	_finish()


func _check_property_sets(path: String) -> void:
	var text := FileAccess.get_file_as_string(path)
	var project := RegEx.create_from_string("#(\\d+)=IFCPROJECT\\('[^']*',(#\\d+)").search(text)
	if project == null:
		_check(false, "%s has a project" % path)
		return
	var next_id := _max_id(text) + 1
	var lines := PackedStringArray()
	var add := func(line: String) -> int:
		lines.append("#%d=%s;" % [next_id + lines.size(), line])
		return next_id + lines.size() - 1
	var owner_history := project.get_string(2)
	var guid := func(n: int) -> String: return "3GDIFCgeoreferenceTst%d" % n
	var values := [add.call("IFCPROPERTYSINGLEVALUE('Eastings',$,IFCLENGTHMEASURE(500000.),$)"),
		add.call("IFCPROPERTYSINGLEVALUE('Northings',$,IFCLENGTHMEASURE(4000000.),$)"),
		add.call("IFCPROPERTYSINGLEVALUE('OrthogonalHeight',$,IFCLENGTHMEASURE(100.),$)"),
		add.call("IFCPROPERTYSINGLEVALUE('XAxisAbscissa',$,IFCREAL(0.8),$)"),
		add.call("IFCPROPERTYSINGLEVALUE('XAxisOrdinate',$,IFCREAL(0.6),$)")]
	var conversion: int = add.call("IFCPROPERTYSET('%s',%s,'ePSet_MapConversion',$,(%s))" % [guid.call(1), owner_history, _refs(values)])
	values = [add.call("IFCPROPERTYSINGLEVALUE('Name',$,IFCLABEL('EPSG:32616'),$)"),
		add.call("IFCPROPERTYSINGLEVALUE('GeodeticDatum',$,IFCLABEL('WGS84'),$)")]
	var crs: int = add.call("IFCPROPERTYSET('%s',%s,'EPset_ProjectedCRS',$,(%s))" % [guid.call(2), owner_history, _refs(values)])
	for i in 2:
		add.call("IFCRELDEFINESBYPROPERTIES('%s',%s,$,$,(#%s),#%d)" % [guid.call(3 + i), owner_history, project.get_string(1), [conversion, crs][i]])
	var data_end := text.find("ENDSEC;", text.find("DATA;"))
	var copy := "user://gdifc_georeference_ifc2x3.ifc"
	_write(copy, text.substr(0, data_end) + "\n".join(lines) + "\n" + text.substr(data_end))

	var manager := await _load(copy)
	var georef := manager.get_node_or_null("IFCModel/Georeference") as IFCGeoreference
	_check(georef != null, "the IFC2X3 copy gets a Georeference node")
	if georef:
		_check(georef.eastings == 500000.0 and georef.northings == 4000000.0 and georef.orthogonal_height == 100.0,
			"the map conversion is read from ePSet_MapConversion")
		_check(is_equal_approx(georef.x_axis_abscissa, 0.8) and is_equal_approx(georef.x_axis_ordinate, 0.6) and georef.map_scale == 1.0,
			"the rotation is read, and the scale defaults to 1")
		_check(georef.crs_name == "EPSG:32616" and georef.geodetic_datum == "WGS84", "the CRS is read from ePSet_ProjectedCRS, whatever its case")

	# The site's location, from its IFCSITE line
	var site := RegEx.create_from_string("=IFCSITE\\([^;]*").search(text)
	var location := manager.get_ifc_model().get_site_location()
	var angles := RegEx.create_from_string("\\((-?\\d+),(-?\\d+),(-?\\d+)(?:,(-?\\d+))?\\)").search_all(site.get_string() if site else "")
	if angles.size() >= 2:
		for i in 2:
			var key: String = ["latitude", "longitude"][i]
			var a: RegExMatch = angles[i]
			var expected := float(a.get_string(1)) + float(a.get_string(2)) / 60.0 + float(a.get_string(3)) / 3600.0 + float(a.get_string(4)) / 3.6e9
			_check(location.has(key) and is_equal_approx(location[key], expected), "the site's %s is %f (got %s)" % [key, expected, location.get(key)])
	else:
		_check(location.is_empty() or not location.has("latitude"), "a site without a latitude has none")
	manager.queue_free()


func _check_scaled(path: String) -> void:
	var text := FileAccess.get_file_as_string(path)
	var conversion := RegEx.create_from_string("=IFCMAPCONVERSION\\(([^;]*)\\);").search(text)
	if conversion == null:
		_check(false, "%s has an IfcMapConversion" % path)
		return
	var copy := "user://gdifc_georeference_scaled.ifc"
	_write(copy, text.replace(conversion.get_string(), "=IFCMAPCONVERSIONSCALED(%s,%.1f,%.1f,%.1f);" % [conversion.get_string(1), FACTORS.x, FACTORS.y, FACTORS.z]))

	var reference := await _load(path)
	var reference_georef := reference.get_node_or_null("IFCModel/Georeference") as IFCGeoreference
	_check(reference_georef != null, "the IFC4X3 file gets a Georeference node")
	if reference_georef == null:
		return
	_check(reference_georef.map_scale_x == 1.0 and reference_georef.map_scale_y == 1.0 and reference_georef.map_scale_z == 1.0,
		"an IfcMapConversion has no per-axis factors")

	var scaled := await _load(copy, reference_georef)
	var georef := scaled.get_node_or_null("IFCModel/Georeference") as IFCGeoreference
	_check(georef != null and Vector3(georef.map_scale_x, georef.map_scale_y, georef.map_scale_z) == FACTORS,
		"IfcMapConversionScaled's factors are read")
	# IFC z is Godot's Y, IFC y its Z
	var expected := Basis.from_scale(Vector3(FACTORS.x, FACTORS.z, FACTORS.y))
	var placed := scaled.get_ifc_model().transform
	_check(placed.basis.is_equal_approx(expected) and placed.origin.is_zero_approx(),
		"relative to the unscaled original, the model is scaled by the factors (got %s)" % placed)

	var same := await _load(path, reference_georef)
	_check(same.get_ifc_model().transform.is_equal_approx(Transform3D()), "relative to itself, a model doesn't move (got %s)" % same.get_ifc_model().transform)


# A manager with the model loaded, placed relative to `reference` when given
func _load(path: String, reference: IFCGeoreference = null) -> GDIFCManager:
	var manager := GDIFCManager.new()
	var settings := GDIFCLoaderSettings.new()
	settings.generate_lods = false
	settings.coordinate_to_origin = false
	manager.geometric_settings = settings
	root.add_child(manager)
	if reference:
		manager.reference_georeference = manager.get_path_to(reference)
	var done := [false]
	manager.ifc_objects_ready.connect(func(): done[0] = true)
	manager.ifc_load_failed.connect(func(): done[0] = true; _check(false, "%s loads" % path))
	if manager.read_ifc(path) != OK:
		_check(false, "read_ifc returns OK for %s" % path)
		return manager
	while not done[0]:
		await process_frame
	return manager


func _max_id(text: String) -> int:
	var highest := 0
	for found in RegEx.create_from_string("(?m)^#(\\d+)=").search_all(text):
		highest = maxi(highest, int(found.get_string(1)))
	return highest


func _refs(ids: Array) -> String:
	return ",".join(ids.map(func(id): return "#%d" % id))


func _write(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	file.store_string(text)
	file.close()


func _check(ok: bool, what: String) -> void:
	_checks += 1
	if not ok:
		_failures += 1
		printerr("FAIL: ", what)


func _finish() -> void:
	print("%d checks, %d failed" % [_checks, _failures])
	quit(1 if _failures > 0 else 0)
