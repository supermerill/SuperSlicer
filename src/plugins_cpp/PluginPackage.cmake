#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

# Build one external plugin into a self-contained directory, then archive its
# direct contents for resources/plugins. The runtime loader always expects the
# platform library name to be plugin.dll, plugin.so, or plugin.dylib.
function(slic3r_package_plugin target package_name)
    get_target_property(_plugin_source_directory ${target} SOURCE_DIR)
    get_target_property(_plugin_binary_directory ${target} BINARY_DIR)
    set(_package_directory "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugin_packages/${package_name}")
    set(_manifest "${_plugin_binary_directory}/${package_name}-$<CONFIG>.ini")
    set(_archive "${SLIC3R_RESOURCES_DIR}/plugins/${package_name}_${SLIC3R_RC_VERSION_DOTS}.zip")
    set(_package_contents "$<TARGET_FILE_NAME:${target}>" "${package_name}.ini" ${ARGN})
    set(_locale_commands)
    if (EXISTS "${_plugin_source_directory}/locale")
        list(APPEND _locale_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_plugin_source_directory}/locale"
                "$<TARGET_FILE_DIR:${target}>/locale")
    endif()

    file(GENERATE OUTPUT "${_manifest}" CONTENT
        "[plugin]\nid = ${package_name}\ntype = local\nversion = ${SLIC3R_RC_VERSION_DOTS}\n")

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_package_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${_package_directory}"
    )

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_RESOURCES_DIR}/plugins"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_manifest}"
            "$<TARGET_FILE_DIR:${target}>/${package_name}.ini"
        ${_locale_commands}
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip --
            ${_package_contents}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Packaging plugin ${package_name}"
        VERBATIM
    )
endfunction()
