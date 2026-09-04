#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

add_library(extra_perimeter_overhang_wave_plugin SHARED
    ExtraPerimeterOverhangWave.cpp
    ExtraPerimeterOverhangWave.hpp
)

target_compile_definitions(extra_perimeter_overhang_wave_plugin PRIVATE EXTRA_PERIMETER_OVERHANG_WAVE_PLUGIN_DLL SLIC3R_PLUGIN_EXPORTS)
target_include_directories(extra_perimeter_overhang_wave_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
if (WIN32)
    target_link_libraries(extra_perimeter_overhang_wave_plugin PRIVATE Slic3r)
else()
    target_link_libraries(extra_perimeter_overhang_wave_plugin PRIVATE libslic3r)
endif()

set_target_properties(extra_perimeter_overhang_wave_plugin PROPERTIES
    FOLDER "plugins"
)

slic3r_package_plugin(
    extra_perimeter_overhang_wave_plugin
    extra_perimeter_overhang_wave
    VERSION 1.0.1
    NAME "Extra perimeter overhang wave"
    FULL_NAME "Extra perimeter overhang wave"
    DESCRIPTION "Add overhang perimeter anchors by growing printable waves from lower-layer support."
)
