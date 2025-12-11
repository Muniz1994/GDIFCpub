@tool
extends Control


@onready var button: Button = %LoadIFCButton
@onready var create_collision_check: CheckButton = %CreateCollisionCheck
@onready var elements_list: ItemList = %ElementsList
@onready var loading_label: Label = %LoadingLabel

var file_dialog: EditorFileDialog
var ifc_manager: GDIFCManager
var current_scene_root: Node

func _ready():
	button.pressed.connect(_on_open_button_pressed)
	

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
	ifc_manager.connect("ifc_read",_on_file_read)
	current_scene_root = EditorInterface.get_edited_scene_root()
	
	if not current_scene_root:
		print("No scene is currently open!")
		return

	# 1. Add the manager to the scene
	current_scene_root.add_child(ifc_manager)
	
	# 2. Set the manager's owner (so the Manager itself appears)
	ifc_manager.owner = current_scene_root
	
	# 3. Generate the geometry (This creates hidden children)
	ifc_manager.read_ifc(path,create_collision,collision_elements)
	
	ifc_manager.ifc_read.connect(_set_owner)
	ifc_manager.set_display_folded(true)
	print("IFC Loaded and scene tree updated.")

func _set_owner():
	_set_owner_recursive(ifc_manager, current_scene_root)
# --- Helper Function ---
func _set_owner_recursive(node: Node, root: Node):
	# Loop through every child of the current node
	for child in node.get_children():
		# Set the owner to the scene root
		child.owner = root
		# Continue diving deeper (in case the IFC has nested nodes)
		_set_owner_recursive(child, root)

func _on_file_read():
	loading_label.visible = false
