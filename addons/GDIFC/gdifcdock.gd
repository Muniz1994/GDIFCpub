@tool
extends Control

const FOLDING_METADATA_SECTION := "gdifc_dock"

# Concrete IfcBuildingElement (IFC2X3, IFC4) and IfcBuiltElement (IFC4X3) subtypes, matched by exact class name
const COLLISION_ELEMENT_TYPES: PackedStringArray = [
	"IfcBeam",
	"IfcBearing",
	"IfcBuildingElementPart",
	"IfcBuildingElementProxy",
	"IfcCaissonFoundation",
	"IfcChimney",
	"IfcColumn",
	"IfcCourse",
	"IfcCovering",
	"IfcCurtainWall",
	"IfcDeepFoundation",
	"IfcDoor",
	"IfcEarthworksElement",
	"IfcEarthworksFill",
	"IfcFooting",
	"IfcKerb",
	"IfcMember",
	"IfcMooringDevice",
	"IfcNavigationElement",
	"IfcPavement",
	"IfcPile",
	"IfcPlate",
	"IfcRail",
	"IfcRailing",
	"IfcRamp",
	"IfcRampFlight",
	"IfcReinforcedSoil",
	"IfcReinforcingBar",
	"IfcReinforcingMesh",
	"IfcRoof",
	"IfcShadingDevice",
	"IfcSlab",
	"IfcStair",
	"IfcStairFlight",
	"IfcTendon",
	"IfcTendonAnchor",
	"IfcTrackElement",
	"IfcWall",
	"IfcWindow",
]
# IFC4 subtypes that are added along with their selected parent type
const COLLISION_ELEMENT_VARIANTS := {
	"IfcBeam": ["IfcBeamStandardCase"],
	"IfcColumn": ["IfcColumnStandardCase"],
	"IfcDoor": ["IfcDoorStandardCase"],
	"IfcMember": ["IfcMemberStandardCase"],
	"IfcPlate": ["IfcPlateStandardCase"],
	"IfcSlab": ["IfcSlabStandardCase", "IfcSlabElementedCase"],
	"IfcWall": ["IfcWallStandardCase", "IfcWallElementedCase"],
	"IfcWindow": ["IfcWindowStandardCase"],
}
# Approximate visible rows of the collision element list before it scrolls
const COLLISION_LIST_VISIBLE_ROWS := 8

@export var settings: GDIFCLoaderSettings = load("res://addons/GDIFC/settings/default_gdifc_geometric_settings.tres")

@onready var button: Button = %LoadIFCButton
@onready var create_collisions_check: CheckBox = %CreateCollisionsCheckBox
@onready var elements_list: ItemList = %ElementsList
@onready var load_status: VBoxContainer = %LoadStatus
@onready var load_progress_bar: ProgressBar = %LoadProgressBar
@onready var load_status_label: Label = %LoadStatusLabel
@onready var geom_settings: HBoxContainer = %GeomSettings
@onready var georef_option: OptionButton = %GeorefOption
@onready var georef_refresh: Button = %GeorefRefresh
@onready var generate_lods_check: CheckBox = %GenerateLodsCheckBox
@onready var lod_merge_angle: SpinBox = %LodMergeAngleSpinBox
@onready var build_label: Label = %BuildLabel
@onready var logo: TextureRect = %Logo
@onready var margin_container: MarginContainer = %MarginContainer
@onready var groups: VBoxContainer = %Groups

var file_dialog: EditorFileDialog
var ifc_manager: GDIFCManager
var current_scene_root: Node
var geom_settings_picker = EditorResourcePicker.new()
# File name of the current or last load, when it started, and whether it failed
var load_file_name := ""
var load_start_msec := 0
var model_shown_seconds := 0.0
var load_failed := false

func _ready():
	button.pressed.connect(_on_open_button_pressed)
	georef_refresh.pressed.connect(_refresh_georef_options)
	# Rebuild the list every time it opens so it matches the edited scene
	georef_option.get_popup().about_to_popup.connect(_refresh_georef_options)
	
	_start_interface()

func _notification(what: int):
	# Fires on enter tree too, before @onready vars exist; _ready applies the theme then
	if what == NOTIFICATION_THEME_CHANGED and is_node_ready():
		_apply_editor_theme()

func _start_interface():
	geom_settings_picker.size_flags_horizontal = EditorResourcePicker.SIZE_EXPAND_FILL
	geom_settings_picker.base_type = "GDIFCLoaderSettings"
	geom_settings_picker.edited_resource = settings
	geom_settings.add_child(geom_settings_picker)

	# LOD controls mirror the picked preset, and override it per load without editing it
	geom_settings_picker.resource_changed.connect(_sync_lod_controls)
	generate_lods_check.toggled.connect(func(on: bool): lod_merge_angle.editable = on)
	_sync_lod_controls(settings)

	for type_name in COLLISION_ELEMENT_TYPES:
		elements_list.add_item(type_name)
	create_collisions_check.toggled.connect(_set_collision_elements_enabled)
	_set_collision_elements_enabled(create_collisions_check.button_pressed)

	_restore_group_folding()
	_apply_editor_theme()
	_refresh_georef_options()
	_show_build_info()

# Everything that depends on the editor theme or scale, so the dock follows theme and scale changes
func _apply_editor_theme():
	var scale := EditorInterface.get_editor_scale()
	for side in ["margin_left", "margin_top", "margin_right", "margin_bottom"]:
		margin_container.add_theme_constant_override(side, int(4 * scale))
	groups.add_theme_constant_override("separation", int(4 * scale))

	# Fixed height so the long element list scrolls on its own instead of stretching the dock
	var row_height := get_theme_font("font", "ItemList").get_height(get_theme_font_size("font_size", "ItemList")) \
		+ get_theme_constant("v_separation", "ItemList")
	elements_list.custom_minimum_size.y = row_height * COLLISION_LIST_VISIBLE_ROWS \
		+ get_theme_stylebox("panel", "ItemList").get_minimum_size().y

	button.icon = get_theme_icon("Load", "EditorIcons")
	georef_refresh.icon = get_theme_icon("Reload", "EditorIcons")

	# The logo is white, so tinting it with the font color keeps it readable on light themes
	var footer_color := get_theme_color("font_color", "Editor")
	footer_color.a = 0.6
	build_label.add_theme_color_override("font_color", footer_color)
	build_label.add_theme_font_size_override("font_size", int(get_theme_default_font_size() * 0.85))
	logo.self_modulate = footer_color
	logo.custom_minimum_size = Vector2(70, 9) * scale

	if load_failed:
		load_status_label.add_theme_color_override("font_color", get_theme_color("error_color", "Editor"))
	else:
		load_status_label.remove_theme_color_override("font_color")

# Each group remembers whether it was folded, per project
func _restore_group_folding():
	var editor_settings := EditorInterface.get_editor_settings()
	for child in groups.get_children():
		var group := child as FoldableContainer
		if not group:
			continue
		var key := "%s_folded" % group.name
		group.folded = editor_settings.get_project_metadata(FOLDING_METADATA_SECTION, key, false)
		group.folding_changed.connect(func(is_folded: bool):
			editor_settings.set_project_metadata(FOLDING_METADATA_SECTION, key, is_folded))

func _set_collision_elements_enabled(on: bool):
	for i in elements_list.item_count:
		elements_list.set_item_disabled(i, not on)

# Version and build come from plugin.cfg next to this script, wherever the addon is installed
func _show_build_info():
	var config := ConfigFile.new()
	if config.load(get_script().resource_path.get_base_dir().path_join("plugin.cfg")) != OK:
		build_label.text = ""
		return
	var version := str(config.get_value("plugin", "version", ""))
	var build := str(config.get_value("plugin", "build", ""))
	if build.is_empty():
		build_label.text = "v%s" % version
		build_label.tooltip_text = "GDIFC %s" % version
	else:
		build_label.text = "v%s · build %s" % [version, build]
		build_label.tooltip_text = "GDIFC %s (build %s)" % [version, build]

func _sync_lod_controls(resource: Resource):
	var preset := resource as GDIFCLoaderSettings
	if preset:
		generate_lods_check.button_pressed = preset.generate_lods
		lod_merge_angle.value = preset.lod_normal_merge_angle
	lod_merge_angle.editable = generate_lods_check.button_pressed

# Copy of the picked preset with the dock's LOD options applied, so the shared .tres is never modified
func _build_load_settings() -> GDIFCLoaderSettings:
	var picked = geom_settings_picker.edited_resource
	var load_settings: GDIFCLoaderSettings = picked.duplicate() if picked else GDIFCLoaderSettings.new()
	load_settings.generate_lods = generate_lods_check.button_pressed
	load_settings.lod_normal_merge_angle = lod_merge_angle.value
	return load_settings

# Lists every IFCGeoreference in the edited scene, keyed by its path from the scene root
func _refresh_georef_options():
	var previous: NodePath = _get_selected_georef_path()
	
	georef_option.clear()
	georef_option.add_item("None")
	georef_option.set_item_metadata(0, NodePath())
	georef_option.select(0)
	
	var scene_root = EditorInterface.get_edited_scene_root()
	if not scene_root:
		return
	
	for georef in scene_root.find_children("*", "IFCGeoreference", true, false):
		var path: NodePath = scene_root.get_path_to(georef)
		var index = georef_option.item_count
		georef_option.add_item(str(path))
		georef_option.set_item_metadata(index, path)
		if path == previous:
			georef_option.select(index)

func _get_selected_georef_path() -> NodePath:
	if georef_option.selected <= 0:
		return NodePath()
	return georef_option.get_item_metadata(georef_option.selected)

func _on_open_button_pressed():
	if not file_dialog:
		file_dialog = EditorFileDialog.new()
		file_dialog.file_mode = EditorFileDialog.FILE_MODE_OPEN_FILE
		file_dialog.access = EditorFileDialog.ACCESS_FILESYSTEM
		file_dialog.add_filter("*.ifc ; IFC models")
		file_dialog.file_selected.connect(_on_file_selected)
		add_child(file_dialog)
	
	file_dialog.popup_centered_ratio(0.5)

func _on_file_selected(path: String):
	load_file_name = path.get_file()
	load_status_label.tooltip_text = path

	current_scene_root = EditorInterface.get_edited_scene_root()
	if not current_scene_root:
		_show_load_status("No scene open", true)
		load_status_label.tooltip_text = "Open a scene to load %s into." % path
		return

	# set options
	var create_collision = create_collisions_check.button_pressed

	var collision_elements_index = elements_list.get_selected_items()

	var collision_elements = []
	for i in collision_elements_index:
		var type_name := elements_list.get_item_text(i)
		collision_elements.append(type_name)
		collision_elements.append_array(COLLISION_ELEMENT_VARIANTS.get(type_name, []))

	button.disabled = true
	load_start_msec = Time.get_ticks_msec()
	load_progress_bar.value = 0.0
	load_progress_bar.show()
	_show_load_status("Reading %s" % load_file_name, false)
	# read_ifc() reads the whole file before returning, so let the status draw first
	await get_tree().process_frame

	ifc_manager = GDIFCManager.new()
	ifc_manager.set_gdifc_settings(_build_load_settings())

	# 1. Add the manager to the scene
	current_scene_root.add_child(ifc_manager,true)
	
	# 2. Set the manager's owner (so the Manager itself appears)
	ifc_manager.owner = current_scene_root
	
	# Place the model relative to the chosen georeference, if any
	var georef_path = _get_selected_georef_path()
	if not georef_path.is_empty():
		var georef = current_scene_root.get_node_or_null(georef_path)
		if georef:
			ifc_manager.reference_georeference = ifc_manager.get_path_to(georef)
		else:
			push_warning("GDIFC: reference georeference '%s' no longer exists. Loading at the origin." % georef_path)
	
	ifc_manager.load_progress_changed.connect(_on_load_progress)
	ifc_manager.ifc_read.connect(_on_model_shown)
	ifc_manager.ifc_objects_ready.connect(_on_load_finished)
	ifc_manager.ifc_load_failed.connect(_on_load_failed)

	# 3. Generate the geometry (This creates hidden children)
	if ifc_manager.read_ifc(path, create_collision, collision_elements) != OK:
		_on_load_failed()
		return

	ifc_manager.set_display_folded(true)

func _on_load_progress(progress: float, stage: String):
	load_progress_bar.value = progress
	_show_load_status("%s — %s" % [stage, load_file_name], false)

# The model is in the scene; properties and typed objects follow with ifc_objects_ready
func _on_model_shown():
	model_shown_seconds = _load_seconds()
	load_progress_bar.hide()
	_show_load_status("Loaded %s in %.1f s · reading properties…" % [load_file_name, model_shown_seconds], false)

func _on_load_finished():
	button.disabled = false
	load_progress_bar.hide()
	_show_load_status("Loaded %s in %.1f s · properties %.1f s" % [load_file_name, model_shown_seconds, _load_seconds()], false)

func _load_seconds() -> float:
	return (Time.get_ticks_msec() - load_start_msec) / 1000.0

func _on_load_failed():
	button.disabled = false
	load_progress_bar.hide()
	_show_load_status("Failed to load %s" % load_file_name, true)
	load_status_label.tooltip_text += "\nSee the Output panel for details."
	# Nothing was generated, so don't leave an empty manager in the scene
	if is_instance_valid(ifc_manager):
		ifc_manager.queue_free()
	ifc_manager = null

func _show_load_status(text: String, is_error: bool):
	load_status.show()
	load_status_label.text = text
	load_failed = is_error
	_apply_editor_theme()
