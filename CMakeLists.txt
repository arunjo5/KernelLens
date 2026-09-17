cmake_minimum_required(VERSION 3.22)
project(latency_tracer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(latency_shared INTERFACE)
target_include_directories(latency_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
