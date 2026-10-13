@tool
extends Control

@export var settings: GDIFCLoaderSettings = load("res://addons/GDIFC/settings/default_gdifc_geometric_settings.tres")

@onready var button: Button = %LoadIFCButton
@onready var check_box: CheckBox = %CheckBox
@onready var elements_list: ItemList = %ElementsList
@onready var loading_label: Label = %LoadingLabel
@onready var geom_settings: HBoxContainer = %GeomSettings
@onready var georef_option: OptionButton = %GeorefOption
@onready var georef_refresh: Button = %GeorefRefresh

var file_dialog: EditorFileDialog
var ifc_manager: GDIFCManager
var current_scene_root: Node
var geom_settings_picker = EditorResourcePicker.new()

func _ready():
	button.pressed.connect(_on_open_button_pressed)
	georef_refresh.pressed.connect(_refresh_georef_options)
	# Rebuild the list every time it opens so it matches the edited scene
	georef_option.get_popup().about_to_popup.connect(_refresh_georef_options)
	
	_start_interface()
	
func _start_interface():
	geom_settings_picker.size_flags_horizontal = EditorResourcePicker.SIZE_EXPAND
	geom_settings_picker.base_type = "GDIFCLoaderSettings"
	geom_settings_picker.edited_resource = settings
	geom_settings.add_child(geom_settings_picker)
	
	georef_refresh.icon = EditorInterface.get_editor_theme().get_icon("Reload", "EditorIcons")
	_refresh_georef_options()

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
	
	file_dialog.popup_centered_ratio()

func _on_file_selected(path: String):
	
	loading_label.visible = true
	# set options
	var create_collision = check_box.button_pressed
	
	var collision_elements_index = elements_list.get_selected_items()
	
	var collision_elements = []
	for i in collision_elements_index:
		collision_elements.append(elements_list.get_item_text(i))
	
	current_scene_root = EditorInterface.get_edited_scene_root()
	
	if not current_scene_root:
		print("No scene is currently open!")
		loading_label.hide()
		return
	
	ifc_manager = GDIFCManager.new()
	ifc_manager.set_gdifc_settings(geom_settings_picker.edited_resource)

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
	
	# 3. Generate the geometry (This creates hidden children)
	ifc_manager.read_ifc(path,create_collision,collision_elements)
	
	ifc_manager.set_display_folded(true)
	ifc_manager.ifc_read.connect(_on_readed_file)
	ifc_manager.ifc_load_failed.connect(_on_readed_file)

func _on_readed_file()->void:
	loading_label.hide()
