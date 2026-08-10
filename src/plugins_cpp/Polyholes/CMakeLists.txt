#/|/ Copyright (c) SuperSlicer 2026 Durand Rémi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

add_library(polyholes_plugin SHARED
    Polyholes.cpp
    Polyholes.hpp
)

target_compile_definitions(polyholes_plugin PRIVATE POLYHOLES_PLUGIN_DLL SLIC3R_PLUGIN_EXPORTS)
target_include_directories(polyholes_plugin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
if (WIN32)
    target_link_libraries(polyholes_plugin PRIVATE Slic3r)
else()
    target_link_libraries(polyholes_plugin PRIVATE libslic3r)
endif()

set_target_properties(polyholes_plugin PROPERTIES
    FOLDER "plugins"
)

slic3r_package_plugin(polyholes_plugin polyholes)
