# This file will be configured to contain variables for CPack. These variables
# should be set in the CMake list file of the project before CPack module is
# included. The list of available CPACK_xxx variables and their associated
# documentation may be obtained using
#  cpack --help-variable-list
#
# Some variables are common to all generators (e.g. CPACK_PACKAGE_NAME)
# and some are specific to a generator
# (e.g. CPACK_NSIS_EXTRA_INSTALL_COMMANDS). The generator specific variables
# usually begin with CPACK_<GENNAME>_xxxx.


set(CPACK_BUILD_SOURCE_DIRS "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7;/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/build")
set(CPACK_CMAKE_GENERATOR "Unix Makefiles")
set(CPACK_COMPONENTS_ALL "Devel;Unspecified")
set(CPACK_COMPONENT_UNSPECIFIED_HIDDEN "TRUE")
set(CPACK_COMPONENT_UNSPECIFIED_REQUIRED "TRUE")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_FILE "/usr/share/cmake-3.25/Templates/CPack.GenericDescription.txt")
set(CPACK_DEFAULT_PACKAGE_DESCRIPTION_SUMMARY "Slic3r built using CMake")
set(CPACK_DMG_SLA_USE_RESOURCE_FILE_LICENSE "ON")
set(CPACK_GENERATOR "STGZ;TGZ;TZ")
set(CPACK_INSTALL_CMAKE_PROJECTS "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/build;Slic3r;ALL;/")
set(CPACK_INSTALL_PREFIX "/usr/local")
set(CPACK_MODULE_PATH "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/cmake/modules/")
set(CPACK_NSIS_DISPLAY_NAME "SuperSlicer 2.7-rc")
set(CPACK_NSIS_DISPLAY_NAME_SET "TRUE")
set(CPACK_NSIS_INSTALLER_ICON_CODE "")
set(CPACK_NSIS_INSTALLER_MUI_ICON_CODE "")
set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES")
set(CPACK_NSIS_PACKAGE_NAME "SuperSlicer 2.7-rc")
set(CPACK_NSIS_UNINSTALL_NAME "Uninstall")
set(CPACK_OBJCOPY_EXECUTABLE "/usr/bin/objcopy")
set(CPACK_OBJDUMP_EXECUTABLE "/usr/bin/objdump")
set(CPACK_OUTPUT_CONFIG_FILE "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/cmake/CPackConfig.cmake")
set(CPACK_PACKAGE_DEFAULT_LOCATION "/")
set(CPACK_PACKAGE_DESCRIPTION_FILE "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/README.md")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "CMake is a build tool")
set(CPACK_PACKAGE_EXECUTABLES "superSlicer")
set(CPACK_PACKAGE_FILE_NAME "SuperSlicer-2.7-rc-Linux-x86_64")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "SuperSlicer 2.7-rc")
set(CPACK_PACKAGE_INSTALL_REGISTRY_KEY "SuperSlicer_2.7.61.0_2024-07-19")
set(CPACK_PACKAGE_NAME "SuperSlicer")
set(CPACK_PACKAGE_RELOCATABLE "true")
set(CPACK_PACKAGE_VENDOR "SuperSlicer")
set(CPACK_PACKAGE_VERSION "2.7.61.0")
set(CPACK_PACKAGE_VERSION_MAJOR "2")
set(CPACK_PACKAGE_VERSION_MINOR "0")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_READELF_EXECUTABLE "/usr/bin/readelf")
set(CPACK_RESOURCE_FILE_LICENSE "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/LICENSE")
set(CPACK_RESOURCE_FILE_README "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/README.md")
set(CPACK_RESOURCE_FILE_WELCOME "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/README.md")
set(CPACK_SET_DESTDIR "OFF")
set(CPACK_SOURCE_GENERATOR "TGZ;TZ")
set(CPACK_SOURCE_OUTPUT_CONFIG_FILE "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/build/CPackSourceConfig.cmake")
set(CPACK_SOURCE_PACKAGE_FILE_NAME "SuperSlicer-2.7.61.0")
set(CPACK_SOURCE_STRIP_FILES "")
set(CPACK_STRIP_FILES "bin/superSlicer")
set(CPACK_SYSTEM_NAME "Linux-x86_64")
set(CPACK_THREADS "1")
set(CPACK_TOPLEVEL_TAG "Linux-x86_64")
set(CPACK_WIX_SIZEOF_VOID_P "8")

if(NOT CPACK_PROPERTIES_FILE)
  set(CPACK_PROPERTIES_FILE "/home/wschadow/github/Software/Slicer/SuperSlicer-2.7/build/CPackProperties.cmake")
endif()

if(EXISTS ${CPACK_PROPERTIES_FILE})
  include(${CPACK_PROPERTIES_FILE})
endif()
