@tool
extends EditorScript



# Called every frame. 'delta' is the elapsed time since the previous frame.
func _run() -> void:
	var managers = EditorInterface.get_edited_scene_root().find_children("","GDIFCManager")
	
	var manager: GDIFCManager
	
	if managers.size()> 0:
		manager = managers[0]
		
	for wall in manager.get_elements_by_class("IfcWall"):
		if wall.ifc_object is GDIfcWall:
			print(wall.ifc_object.global_id)
