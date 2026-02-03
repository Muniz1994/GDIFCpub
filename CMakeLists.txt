include(FetchContent)
cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(GodotExtension)

if (UNIX)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif (UNIX)


# Define this necessary flag
add_definitions(-DIFCQUERY_STATIC_LIB)

# Set environment variables
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)


if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

# download the external projects and save their paths
Message("Downloading godot-cpp 4.5")
FetchContent_Declare(godot-cpp GIT_REPOSITORY "https://github.com/godotengine/godot-cpp.git" GIT_TAG "e83fd0904c13356ed1d4c3d09f8bb9132bdc6b77")
FetchContent_MakeAvailable(godot-cpp)
FetchContent_GetProperties(godot-cpp)

add_subdirectory(web-ifc)
add_subdirectory(ifcparse)
add_subdirectory(GDIFC)