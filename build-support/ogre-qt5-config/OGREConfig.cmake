if(NOT DEFINED ENV{RUN3OGITOR_OGRE_SDK} OR
   NOT DEFINED ENV{RUN3OGITOR_QT5_BRIDGE_DIR})
    message(FATAL_ERROR "The Run3Ogitor build environment is not initialized")
endif()

file(TO_CMAKE_PATH "$ENV{RUN3OGITOR_OGRE_SDK}" RUN3OGITOR_OGRE_SDK)
file(TO_CMAKE_PATH "$ENV{RUN3OGITOR_QT5_BRIDGE_DIR}" RUN3OGITOR_QT5_BRIDGE_DIR)

include("${RUN3OGITOR_OGRE_SDK}/CMake/OGREConfig.cmake")

# The Run3 Ogre SDK uses Qt 6. Ogitor 0.5.5 uses Qt 5, so use the matching
# bridge built by build.bat instead of mixing Qt major versions in one process.
set_target_properties(OgreBitesQt PROPERTIES
    IMPORTED_IMPLIB_RELEASE "${RUN3OGITOR_QT5_BRIDGE_DIR}/lib/OgreBitesQt.lib"
    IMPORTED_LOCATION_RELEASE "${RUN3OGITOR_QT5_BRIDGE_DIR}/bin/OgreBitesQt.dll"
    INTERFACE_LINK_LIBRARIES "Qt5::Gui;OgreBites")
