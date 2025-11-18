extends RayCast3D

# Track the currently highlighted object to reset it later
var last_mesh: MeshInstance3D
var original_material: Material
var highlight_material: StandardMaterial3D

func _ready() -> void:
	# Setup the highlight style (e.g., glowing yellow)
	highlight_material = StandardMaterial3D.new()
	highlight_material.albedo_color = Color(1, 1, 0) 
	highlight_material.emission_enabled = true
	highlight_material.emission = Color(1, 1, 0)
	highlight_material.emission_energy_multiplier = 2.0
	# Optional: Draw on top of everything
	# highlight_material.no_depth_test = true 

func _process(delta: float) -> void:
	
	
	if is_colliding():
		var collider = get_collider()
		if Input.is_action_just_pressed("mouse_click"):
			# Attempt to find the visual mesh associated with the physics collider
			#var mesh_instance = _find_mesh(collider)
			#
			#if mesh_instance:
				#if mesh_instance != last_mesh:
					#_reset_highlight() # Un-highlight previous object
					#_apply_highlight(mesh_instance) # Highlight new object
			#else:
				## Colliding, but no visual mesh found
				#_reset_highlight()
			#
			# --- Your Original Logic ---
			# Note: Added checks to ensure parents exist to prevent crashes
			var p1 = collider.get_parent()
			if p1:
				var parent = p1.get_parent()
				if parent and parent.has_method("get_properties"):
					var props = parent.get_properties()
					if props:
						print(props)
			# ---------------------------

		#else:
			## Not colliding with anything
			#_reset_highlight()

# Helper to apply the material
func _apply_highlight(mesh: MeshInstance3D) -> void:
	last_mesh = mesh
	# Save the current material (returns null if using default mesh material)
	original_material = mesh.get_surface_override_material(0)
	# Apply the highlight
	mesh.set_surface_override_material(0, highlight_material)

# Helper to restore the original material
func _reset_highlight() -> void:
	if last_mesh and is_instance_valid(last_mesh):
		# Reset to whatever it was before (null means default mesh material)
		last_mesh.set_surface_override_material(0, original_material)
	
	last_mesh = null
	original_material = null

# Helper to find a MeshInstance3D relative to the collider
func _find_mesh(node: Node) -> MeshInstance3D:
	# 1. Check if the collider has a mesh child (Common structure)
	for child in node.get_children():
		if child is MeshInstance3D:
			return child
	
	# 2. Check if the collider is a child of a mesh (Common structure)
	var parent = node.get_parent()
	if parent is MeshInstance3D:
		return parent
		
	return null
	
