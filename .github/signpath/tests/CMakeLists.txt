cmake_minimum_required(VERSION 3.13)
project(SignPathFixtures C)

get_filename_component(REPOSITORY "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
include("${REPOSITORY}/version.inc")
set(SLIC3R_RESOURCES_DIR "${REPOSITORY}/resources")
configure_file("${REPOSITORY}/src/platform/msw/PrusaSlicer.manifest.in" "${SLIC3R_APP_KEY}.manifest" @ONLY)
configure_file("${REPOSITORY}/src/platform/msw/Slic3r.rc.in" "superslicer.rc" @ONLY)
configure_file("${REPOSITORY}/src/platform/msw/gcodeviewer.rc.in" "gcodeviewer.rc" @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/stub.c" "int main(void) { return 0; }\n")

foreach(name superslicer superslicer_console superslicer-gcodeviewer)
    if(name STREQUAL "superslicer-gcodeviewer")
        set(resource "${CMAKE_CURRENT_BINARY_DIR}/gcodeviewer.rc")
    else()
        set(resource "${CMAKE_CURRENT_BINARY_DIR}/superslicer.rc")
    endif()
    add_executable(${name} "${CMAKE_CURRENT_BINARY_DIR}/stub.c" "${resource}")
    target_include_directories(${name} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_options(${name} PRIVATE /MANIFEST:NO)
endforeach()

foreach(name Slic3r OCCTWrapper)
    set(SLIC3R_DLL_NAME "${name}")
    configure_file("${REPOSITORY}/src/platform/msw/Dll.rc.in" "${name}.rc" @ONLY)
    add_library(${name} SHARED "${CMAKE_CURRENT_BINARY_DIR}/stub.c" "${CMAKE_CURRENT_BINARY_DIR}/${name}.rc")
endforeach()

# These deliberately have no SuperSlicer metadata and must not be submitted.
foreach(name libgmp-10 libmpfr-4)
    add_library(${name} SHARED "${CMAKE_CURRENT_BINARY_DIR}/stub.c")
endforeach()

# Use the real MSI template, but never the real product's upgrade code or registry key.
set(SLIC3R_GUID "3fa87a13-05cb-4fee-8e09-cdc6d131cd86")
set(SLIC3R_APP_KEY "SuperSlicerSignatureFixture")
configure_file("${REPOSITORY}/src/platform/msw/winInstaller.wxs.in" "winInstaller.wxs" @ONLY)
