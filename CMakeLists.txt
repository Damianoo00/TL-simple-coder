cmake_minimum_required(VERSION 3.20)
project(TL-simple-coder LANGUAGES CXX)

# Keep executable paths stable for launch scripts and existing installations.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
include(CTest)
add_subdirectory(backend)
