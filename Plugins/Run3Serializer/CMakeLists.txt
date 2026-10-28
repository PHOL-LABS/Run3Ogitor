cmake_minimum_required(VERSION 3.16)
project(Run3Serializer)
include_directories(${DEPENDENCIES_INCLUDES} ${OGITOR_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt5 REQUIRED COMPONENTS Network)
ogitor_add_library(Run3Serializer SHARED Run3Document.cpp Run3Entity.cpp Run3Serializer.cpp Run3Mcp.cpp)
target_link_libraries(Run3Serializer PRIVATE Ogitor TinyXML Qt5::Widgets Qt5::Network ${OGRE_LIBRARIES})
if(WIN32)
    install(FILES "$<TARGET_FILE:Qt5::Network>" DESTINATION bin)
endif()
# Canonicalize old exporter output with the port's official Ogre 14.5 tool.
# Conversion is isolated in a subprocess and never rewrites game assets.
find_program(RUN3_LEGACY_MESH_UPGRADER OgreMeshUpgrader
    PATHS "${CMAKE_SOURCE_DIR}/../build/windows-msvc-x64-release/vcpkg_installed/x64-windows/tools/ogre"
    NO_DEFAULT_PATH)
if(RUN3_LEGACY_MESH_UPGRADER)
    target_compile_definitions(Run3Serializer PRIVATE RUN3_LEGACY_MESH_UPGRADER="${RUN3_LEGACY_MESH_UPGRADER}")
endif()
set_target_properties(Run3Serializer PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
install(TARGETS Run3Serializer RUNTIME DESTINATION ${OGITOR_PLUGIN_INSTALL_PATH} LIBRARY DESTINATION ${OGITOR_PLUGIN_INSTALL_PATH})

add_executable(run3_document_tests Run3DocumentTests.cpp Run3Document.cpp)
target_link_libraries(run3_document_tests PRIVATE TinyXML Qt5::Core)
