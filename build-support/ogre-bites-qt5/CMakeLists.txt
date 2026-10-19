cmake_minimum_required(VERSION 3.16)
project(OgreBitesQt5Bridge LANGUAGES CXX)

if(NOT DEFINED OGRE_SDK OR NOT DEFINED OGRE_SOURCE)
    message(FATAL_ERROR "OGRE_SDK and OGRE_SOURCE must be specified")
endif()

file(TO_CMAKE_PATH "${OGRE_SDK}" OGRE_SDK)
file(TO_CMAKE_PATH "${OGRE_SOURCE}" OGRE_SOURCE)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt5 REQUIRED COMPONENTS Gui)

add_library(OgreBites SHARED IMPORTED)
set_target_properties(OgreBites PROPERTIES
    IMPORTED_IMPLIB "${OGRE_SDK}/lib/OgreBites.lib"
    IMPORTED_LOCATION "${OGRE_SDK}/bin/OgreBites.dll")

add_library(OgreMain SHARED IMPORTED)
set_target_properties(OgreMain PROPERTIES
    IMPORTED_IMPLIB "${OGRE_SDK}/lib/OgreMain.lib"
    IMPORTED_LOCATION "${OGRE_SDK}/bin/OgreMain.dll")

add_library(OgreBitesQt SHARED
    "${OGRE_SOURCE}/Components/Bites/include/OgreApplicationContextQt.h"
    "${OGRE_SOURCE}/Components/Bites/src/OgreApplicationContextQt.cpp")

target_compile_definitions(OgreBitesQt PRIVATE OgreBitesQt_EXPORTS)
target_include_directories(OgreBitesQt PRIVATE
    "${OGRE_SDK}/include/OGRE"
    "${OGRE_SDK}/include/OGRE/Bites"
    "${OGRE_SDK}/include/OGRE/Overlay"
    "${OGRE_SDK}/include/OGRE/RTShaderSystem")
target_link_libraries(OgreBitesQt PRIVATE Qt5::Gui OgreBites OgreMain)

set_target_properties(OgreBitesQt PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
