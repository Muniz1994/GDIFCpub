cmake_minimum_required(VERSION 3.20 FATAL_ERROR)

project(GDIFC)

set(MY_OUTPUT_DIR "${CMAKE_SOURCE_DIR}/ifc-godot-project/extensions/${PROJECT_NAME}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${MY_OUTPUT_DIR})

if (NOT DEFINED BITS)
    math(EXPR BITS "8*${CMAKE_SIZEOF_VOID_P}")
endif()


set(TARGET ${PROJECT_NAME}.${CMAKE_SYSTEM_NAME}.${CMAKE_BUILD_TYPE}.${BITS})

string(TOLOWER ${TARGET} TARGET)

set(EXT_SOURCES
	src/gdifcreader.h
	src/gdifcreader.cpp
	src/register_types.h
	src/register_types.cpp)

add_library(${TARGET} SHARED ${EXT_SOURCES})

target_link_libraries(${TARGET} PUBLIC godot-cpp web-ifc)

target_include_directories(${TARGET} PUBLIC ${PROJECT_SOURCE_DIR}/godot-cpp/include)


if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")

    target_compile_options(${TARGET} PRIVATE /std:c++17)
    if(CMAKE_BUILD_TYPE MATCHES DEBUG)

        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    endif(CMAKE_BUILD_TYPE MATCHES DEBUG)

endif()

string(TOLOWER "${PROJECT_NAME}.gdextension" GDEXTENSION_CONFIGURATION_FILE)

string(TOLOWER ${PROJECT_NAME} GDEXTENSION_ENTRY_POINT)

# Construct the full path for the output file
set(GDEXTENSION_OUTPUT_FILE "${MY_OUTPUT_DIR}/${GDEXTENSION_CONFIGURATION_FILE}")

configure_file(gdifcreader.in ${GDEXTENSION_OUTPUT_FILE})

install(TARGETS ${TARGET}
		RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/ifc-godot-project/extensions/${PROJECT_NAME})

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${GDEXTENSION_CONFIGURATION_FILE}" CONFIGURATIONS Release Debug DESTINATION ${CMAKE_SOURCE_DIR}/ifc-godot-project/extensions/${PROJECT_NAME})
