#/|/ Copyright (c) SuperSlicer 2026 Durand Remi @supermerill
#/|/
#/|/ SuperSlicer is released under the terms of the AGPLv3 or higher
#/|/

# Build one external plugin into a self-contained directory, then archive its
# direct contents for resources/plugins. Both package and slicer versions are
# embedded in the filename because a plugin may evolve independently from the
# application that can load it.
function(slic3r_package_plugin target package_name)
    cmake_parse_arguments(PACKAGE "" "VERSION;UPDATE_REST;NAME;FULL_NAME;DESCRIPTION" "FILES" ${ARGN})
    get_target_property(_plugin_source_directory ${target} SOURCE_DIR)
    get_target_property(_plugin_binary_directory ${target} BINARY_DIR)
    set(_package_directory "${CMAKE_BINARY_DIR}/src/$<CONFIG>/plugin_packages/${package_name}")
    set(_description "${_plugin_binary_directory}/${package_name}-$<CONFIG>-description.ini")
    set(_package_version "${SLIC3R_RC_VERSION_DOTS}")
    if (PACKAGE_VERSION)
        set(_package_version "${PACKAGE_VERSION}")
    endif()
    set(_package_display_name "${package_name}")
    if (PACKAGE_NAME)
        set(_package_display_name "${PACKAGE_NAME}")
    endif()
    set(_package_full_name "${_package_display_name}")
    if (PACKAGE_FULL_NAME)
        set(_package_full_name "${PACKAGE_FULL_NAME}")
    endif()
    set(_archive "${SLIC3R_RESOURCES_DIR}/plugins/${package_name}_${_package_version}_${SLIC3R_RC_VERSION_DOTS}.zip")
    set(_package_contents "$<TARGET_FILE_NAME:${target}>" "description.ini" ${PACKAGE_FILES} ${PACKAGE_UNPARSED_ARGUMENTS})
    # Keep the generated default profile aligned with the packages built by
    # this CMake configuration. Package and slicer versions are independent.
    set_property(GLOBAL APPEND PROPERTY SLIC3R_DEFAULT_PLUGIN_PACKAGES
        "${package_name}|${_package_version}|${SLIC3R_RC_VERSION_DOTS}")
    set(_locale_commands)
    if (EXISTS "${_plugin_source_directory}/locale")
        list(APPEND _locale_commands
            COMMAND "${CMAKE_COMMAND}" -E copy_directory
                "${_plugin_source_directory}/locale"
                "$<TARGET_FILE_DIR:${target}>/locale")
    endif()

    file(GENERATE OUTPUT "${_description}" CONTENT
        "[plugin]\nid = ${package_name}\nname = ${_package_display_name}\nfull_name = ${_package_full_name}\ndescription = ${PACKAGE_DESCRIPTION}\nconfig_update_rest = ${PACKAGE_UPDATE_REST}\nslicer = SuperSlicer\npackage_version = ${_package_version}\nslicer_version = ${SLIC3R_RC_VERSION_DOTS}\n")

    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "plugin"
        PREFIX ""
        LIBRARY_OUTPUT_DIRECTORY "${_package_directory}"
        RUNTIME_OUTPUT_DIRECTORY "${_package_directory}"
    )

    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_RESOURCES_DIR}/plugins"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${SLIC3R_RESOURCES_DIR}/plugins/descriptions"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_description}"
            "$<TARGET_FILE_DIR:${target}>/description.ini"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${_description}"
            "${SLIC3R_RESOURCES_DIR}/plugins/descriptions/${package_name}.ini"
        ${_locale_commands}
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            "${SLIC3R_RESOURCES_DIR}/plugins/${package_name}_${SLIC3R_RC_VERSION_DOTS}.zip"
        COMMAND "${CMAKE_COMMAND}" -E tar cf "${_archive}" --format=zip --
            ${_package_contents}
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Packaging plugin ${package_name}"
        VERBATIM
    )
endfunction()
