cmake_minimum_required(VERSION 3.10)

# Defines the project name. This sets the internal variable ${PROJECT_NAME}
project(
    IfcParse
    VERSION 1.0.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

file(GLOB IFCPARSE_H_FILES_ALL *.h)
file(GLOB IFCPARSE_CPP_FILES_ALL *.cpp)

find_package(Boost REQUIRED COMPONENTS system filesystem)

foreach(file ${IFCPARSE_H_FILES_ALL})
    get_filename_component(filename "${file}" NAME)

    if(NOT "${filename}" MATCHES "[0-9]")
        list(APPEND IFCPARSE_H_FILES "${file}")
    endif()
endforeach()

foreach(file ${IFCPARSE_CPP_FILES_ALL})
    get_filename_component(filename "${file}" NAME)

    if(NOT "${filename}" MATCHES "[0-9]")
        list(APPEND IFCPARSE_CPP_FILES "${file}")
    endif()
endforeach()

set(SCHEMA_VERSIONS
    4
    4x3
    4x3_add2
    4x2
    4x1
    2x3)

message(STATUS "SCHEMA_VERSIONS from ifcparse: ${SCHEMA_VERSIONS}")
foreach(schema ${SCHEMA_VERSIONS})
    list(APPEND IFCPARSE_H_FILES
        Ifc${schema}.h
        Ifc${schema}-definitions.h
    )
    list(APPEND IFCPARSE_CPP_FILES
        Ifc${schema}.cpp
        Ifc${schema}-schema.cpp
    )
endforeach()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
add_compile_definitions(_UNICODE)

set(IFCPARSE_FILES ${IFCPARSE_CPP_FILES} ${IFCPARSE_H_FILES})

add_library(IfcParse ${IFCPARSE_FILES} )

target_include_directories(IfcParse 
	PUBLIC
	 ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(IfcParse PRIVATE
    $<$<CXX_COMPILER_ID:MSVC>:/bigobj>
)

target_compile_definitions(IfcParse PRIVATE HAS_SCHEMA_4)
target_compile_definitions(IfcParse PRIVATE HAS_SCHEMA_2x3)
target_compile_definitions(IfcParse PRIVATE HAS_SCHEMA_4x3)
target_compile_definitions(IfcParse PRIVATE HAS_SCHEMA_4x3_add2)

set_target_properties(IfcParse PROPERTIES COMPILE_FLAGS -DIFC_PARSE_EXPORTS VERSION "${PROJECT_VERSION}" SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")

target_link_libraries(IfcParse PUBLIC 
    Boost::system 
    Boost::filesystem
)

