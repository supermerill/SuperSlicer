project(test-utils)

# convert stl to c++ code to inclusion in test
if (!APPLE) 
    add_executable(stl_to_cpp stl_to_cpp.cpp)
    target_link_libraries(stl_to_cpp libslic3r)
endif()

if (WIN32)
    prusaslicer_copy_dlls(stl_to_cpp)
endif()
