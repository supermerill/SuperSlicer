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
    OUTPUT_NAME "plugin"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins/flat_area_layer_height"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugins/flat_area_layer_height"
)
