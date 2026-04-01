@tool
extends EditorScript



# Called every frame. 'delta' is the elapsed time since the previous frame.
func _run() -> void:
	var managers = EditorInterface.get_edited_scene_root().find_children("","GDIFCManager")
	
	var manager: GDIFCManager
	
	if managers.size()> 0:
		manager = managers[0]
		
		for alignemnt:IfcAlignment in manager.get_elements_by_class("IfcAlignment"):
			alignemnt.PredefinedType = "EXTERNAL"

		var model := manager.find_children("","IFCModel")[0]
		
		if model is IFCModel:
			print("saving model")
			model.save("res://new_model.ifc")
	
