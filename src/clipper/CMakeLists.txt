cmake_minimum_required(VERSION 3.5)
project(clipper)

add_library(clipper STATIC
# We are using ClipperLib compiled as part of the libslic3r project using Slic3r::Point as its base type.
#    clipper.cpp
#    clipper.hpp
    clipper_z.cpp
    clipper_z.hpp
)

target_link_libraries(clipper TBB::tbb TBB::tbbmalloc libslic3r)
