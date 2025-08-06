cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(GodotExtension)

# Define this necessary flag
add_definitions(-DIFCQUERY_STATIC_LIB)

# Set environment variables
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_subdirectory(web-ifc)
add_subdirectory(godot-cpp)

add_subdirectory(GDIFC)