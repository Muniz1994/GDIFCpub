extends GDIFCManager

# A simple example script to measure function execution time

func _ready():
	# Call the function to test
	test_function_speed()

func test_function_speed():
	# Record the start time in milliseconds
	var start_time = Time.get_ticks_msec()

	# Call the function to be timed
	read_ifc("C:\\Users\\engbr\\Documents\\GitHub\\IFcFiles\\MainBuilding.ifc",false)
	
	for i in get_children():
		for y in i.get_children():
			if y.get_properties():
				print(y.get_properties())

	# Record the end time
	var end_time = Time.get_ticks_msec()

	# Calculate the elapsed time
	var elapsed_time = end_time - start_time

	# Print the result
	print("Elapsed time: " + str(elapsed_time) + " ms")
