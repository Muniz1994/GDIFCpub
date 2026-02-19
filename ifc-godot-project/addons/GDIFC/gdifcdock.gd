@tool
extends Control

@export var settings: GDIFCLoaderSettings = load("res://addons/GDIFC/settings/default_gdifc_geometric_settings.tres")

@onready var button: Button = %LoadIFCButton
@onready var create_collision_check: CheckButton = %CreateCollisionCheck
@onready var elements_list: ItemList = %ElementsList
@onready var loading_label: Label = %LoadingLabel
@onready var geom_settings: HBoxContainer = %GeomSettings

var file_dialog: EditorFileDialog
var ifc_manager: GDIFCManager
var current_scene_root: Node
var geom_settings_picker = EditorResourcePicker.new()

func _ready():
	button.pressed.connect(_on_open_button_pressed)
	
	_start_interface()
	
func _start_interface():
	geom_settings_picker.base_type = "GDIFCLoaderSettings"
	geom_settings_picker.edited_resource = settings
	geom_settings.add_child(geom_settings_picker)

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
	var create_collision = create_collision_check.button_pressed
	
	var collision_elements_index = elements_list.get_selected_items()
	
	var collision_elements = []
	for i in collision_elements_index:
		collision_elements.append(elements_list.get_item_text(i))
	
	ifc_manager = GDIFCManager.new()
	ifc_manager.set_gdifc_settings(geom_settings_picker.edited_resource)
	
	print(ifc_manager.get_gdifc_settings().coordinate_to_origin)
	
	current_scene_root = EditorInterface.get_edited_scene_root()
	
	if not current_scene_root:
		print("No scene is currently open!")
		return

	# 1. Add the manager to the scene
	current_scene_root.add_child(ifc_manager,true)
	
	# 2. Set the manager's owner (so the Manager itself appears)
	ifc_manager.owner = current_scene_root
	
	# 3. Generate the geometry (This creates hidden children)
	ifc_manager.read_ifc(path,create_collision,collision_elements)
	
	ifc_manager.ifc_read.connect(_set_owner)
	ifc_manager.set_display_folded(true)

const BATCH_SIZE = 100 # The number of nodes to process per frame. 
var _nodes_processed = 0

func _set_owner():
	_nodes_processed = 0

	# Wait for the recursive function to finish completely
	await _set_owner_recursive_async(ifc_manager, current_scene_root)

	# Hide the loading label ONLY after all owners are set
	loading_label.visible = false
	print("IFC Loading and ownership assignment complete!")

# --- Helper Function ---
func _set_owner_recursive_async(node: Node, root: Node):
	for child in node.get_children():
		child.owner = root
		_nodes_processed += 1

		# Every BATCH_SIZE nodes, pause this function and yield to the main thread
		if _nodes_processed % BATCH_SIZE == 0:
			await get_tree().process_frame
			
		# If the child has children, recursively call and await
		if child.get_child_count() > 0:
			await _set_owner_recursive_async(child, root)
