include(GNUInstallDirs)

set(_patch_step "")
if (APPLE)
    set(_patch_step PATCH_COMMAND ${PATCH_CMD} ${CMAKE_CURRENT_LIST_DIR}/Qhull.patch)
endif ()


prusaslicer_add_cmake_project(Qhull
    URL "https://github.com/qhull/qhull/archive/v8.0.1.zip"
    URL_HASH SHA256=5287f5edd6a0372588f5d6640799086a4033d89d19711023ef8229dd9301d69b
    "${_patch_step}"
    CMAKE_ARGS 
        -DINCLUDE_INSTALL_DIR=${CMAKE_INSTALL_INCLUDEDIR}
)


if (MSVC)
    add_debug_dep(dep_Qhull)
endif ()