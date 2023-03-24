#prusaslicer_add_cmake_project(EXPAT
#  # GIT_REPOSITORY https://github.com/nigels-com/glew.git
#  # GIT_TAG 3a8eff7 # 2.1.0
#  SOURCE_DIR          ${CMAKE_CURRENT_LIST_DIR}/expat
#)

prusaslicer_add_cmake_project(EXPAT
  URL https://github.com/libexpat/libexpat/archive/refs/tags/R_2_4_3.zip
  URL_HASH SHA256=8851e199d763dc785277d6d414ed3e70ff683915158b51b8d8781df0e3af950a
  SOURCE_SUBDIR expat
  CMAKE_ARGS
    -DEXPAT_BUILD_TOOLS:BOOL=OFF
    -DEXPAT_BUILD_EXAMPLES:BOOL=OFF
    -DEXPAT_BUILD_TESTS:BOOL=OFF
    -DEXPAT_BUILD_DOCS=OFF
    -DEXPAT_BUILD_PKGCONFIG=OFF
)

if (MSVC)
    add_debug_dep(dep_EXPAT)
endif ()
