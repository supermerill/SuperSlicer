cmake_minimum_required(VERSION 3.13)
project(OCCTWrapper)

if (APPLE)
    # TODO: we need to fix notarization with the separate shared library
    add_library(OCCTWrapper STATIC OCCTWrapper.cpp)
else ()
    add_library(OCCTWrapper MODULE OCCTWrapper.cpp)
endif ()

set_target_properties(OCCTWrapper
    PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/src"
    PREFIX ""
)

include(GenerateExportHeader)

generate_export_header(OCCTWrapper)

find_package(OpenCASCADE REQUIRED)
if (OpenCASCADE_VERSION VERSION_LESS 7.6.2)
    message(FATAL_ERROR "OpenCASCADE v7 >= v7.6.2 must be used")
elseif (OpenCASCADE_VERSION VERSION_GREATER_EQUAL 8.0.0)
    message(FATAL_ERROR "OpenCASCADE v7 >= v7.6.2 must be used")
endif ()

if (OpenCASCADE_VERSION VERSION_LESS 7.8.0)
    message(STATUS "OpenCASCADE_VERSION version < 7.8.0")
    set(OCCT_LIBS
        TKXDESTEP
        TKSTEP
        TKSTEP209
        TKSTEPAttr
        TKSTEPBase
        TKXCAF
        TKXSBase
        TKVCAF
        TKCAF
        TKLCAF
        TKCDF
        TKV3d
        TKService
        TKMesh
        TKBO
        TKPrim
        TKHLR
        TKShHealing
        TKTopAlgo
        TKGeomAlgo
        TKBRep
        TKGeomBase
        TKG3d
        TKG2d
        TKMath
        TKernel
    )
else()
    message(STATUS "OpenCASCADE_VERSION version >= 7.8.0")
    set(OCCT_LIBS
            TKXCAF
            TKXSBase
            TKVCAF
            TKCAF
            TKLCAF
            TKCDF
            TKV3d
            TKService
            TKMesh
            TKBO
            TKPrim
            TKHLR
            TKShHealing
            TKTopAlgo
            TKGeomAlgo
            TKBRep
            TKGeomBase
            TKG3d
            TKG2d
            TKMath
            TKernel
            TKDESTEP
            TKXSDRAWSTEP
    )
endif()

message(STATUS "OpenCASCADE_VERSION: '${OpenCASCADE_VERSION}'")

message(STATUS "OCCT_LIBS: '${OCCT_LIBS}'")
slic3r_remap_configs("${OCCT_LIBS}" RelWithDebInfo Release)

target_include_directories(OCCTWrapper PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_include_directories(OCCTWrapper PUBLIC ${OpenCASCADE_INCLUDE_DIR})
target_link_directories(OCCTWrapper PUBLIC ${OpenCASCADE_LIBRARY_DIR})
target_link_libraries(OCCTWrapper ${OCCT_LIBS})

include(GNUInstallDirs)

install(TARGETS OCCTWrapper DESTINATION "${CMAKE_INSTALL_LIBDIR}/superslicer")

