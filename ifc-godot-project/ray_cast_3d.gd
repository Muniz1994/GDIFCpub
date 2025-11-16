extends RayCast3D


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(delta: float) -> void:
	
	if is_colliding():
		var collider: Node3D = get_collider()
		
		print(collider.name)
		var parent = collider.get_parent().get_parent()
		if parent.has_method("get_properties"):
			var props = parent.get_properties()
			if props:
				print(props)
	
	
