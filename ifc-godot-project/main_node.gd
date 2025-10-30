extends Node3D

@onready var node_3d: Node3D = $Node3D

var my_list:Array = []

func _ready() -> void:
	node_3d.my_props = my_list
