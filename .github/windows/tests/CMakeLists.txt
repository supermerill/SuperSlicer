cmake_minimum_required(VERSION 3.13)
project(VcRuntimePackagingFixture CXX)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/probe.cpp"
    "#include <thread>\n#include <chrono>\nint main() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); return 0; }\n")
add_executable(runtime-probe "${CMAKE_CURRENT_BINARY_DIR}/probe.cpp")
add_executable(delay-probe "${CMAKE_CURRENT_BINARY_DIR}/probe.cpp")
target_link_libraries(delay-probe PRIVATE delayimp)
target_link_options(delay-probe PRIVATE /DELAYLOAD:msvcp140.dll)
