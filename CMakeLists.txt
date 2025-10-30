include(FetchContent)
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

# download the external projects and save their paths
Message("Downloading godot-cpp 4.2")
FetchContent_Declare(godot-cpp GIT_REPOSITORY "https://github.com/godotengine/godot-cpp.git" GIT_TAG "4bc6e67d51df4c12e150fe4769911e239c627a47")
FetchContent_MakeAvailable(godot-cpp)
FetchContent_GetProperties(godot-cpp)

add_subdirectory(web-ifc)
add_subdirectory(GDIFC)