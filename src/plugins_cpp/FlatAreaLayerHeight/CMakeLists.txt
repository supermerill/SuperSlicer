#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

add_library(flat_area_layer_height_plugin SHARED
    FlatAreaLayerHeight.cpp
    FlatAreaLayerHeight.hpp
)

target_compile_definitions(flat_area_layer_height_plugin PRIVATE FLAT_AREA_LAYER_HEIGHT_PLUGIN_DLL SLIC3R_PLUGIN_EXPORTS)
target_include_directories(flat_area_layer_height_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
if (WIN32)
    target_link_libraries(flat_area_layer_height_plugin PRIVATE Slic3r)
else()
    target_link_libraries(flat_area_layer_height_plugin PRIVATE libslic3r)
endif()

set_target_properties(flat_area_layer_height_plugin PROPERTIES
    FOLDER "plugins"
)

slic3r_package_plugin(
    flat_area_layer_height_plugin
    flat_area_layer_height
    VERSION 2.7.63.1
    CHANGELOG changelog.json
    NAME "Flat area layer height"
    FULL_NAME "Flat area layer height"
    DESCRIPTION "Place layer boundaries on large horizontal mesh areas when possible."
)
