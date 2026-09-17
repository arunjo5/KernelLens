cmake_minimum_required(VERSION 3.22)
project(latency_tracer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(LATENCY_BUILD_BPF "Build Linux CO-RE tracer" ON)
option(LATENCY_SANITIZERS "Enable ASan and UBSan for userspace and tests" OFF)
include(CTest)

add_library(latency_shared INTERFACE)
target_include_directories(latency_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

find_package(Threads REQUIRED)
add_library(latency_core src/histogram.cpp src/metrics.cpp)
target_link_libraries(latency_core PUBLIC latency_shared Threads::Threads)
target_compile_options(latency_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
if(LATENCY_SANITIZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "ASan/UBSan require Clang or GCC")
  endif()
  target_compile_options(latency_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(latency_core PUBLIC -fsanitize=address,undefined)
endif()

if(LATENCY_BUILD_BPF)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Tracer requires Linux; use -DLATENCY_BUILD_BPF=OFF for the userspace library and tests")
  endif()
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64")
    set(BPF_ARCH x86)
  elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
    set(BPF_ARCH arm64)
  else()
    message(FATAL_ERROR "Supported targets: x86_64 and arm64")
  endif()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(LIBBPF REQUIRED IMPORTED_TARGET libbpf>=1.0)
  find_program(BPF_CLANG NAMES clang-19 clang-18 clang-17 clang-16 clang REQUIRED)
  find_program(BPFTOOL bpftool REQUIRED)
  set(VMLINUX_BTF /sys/kernel/btf/vmlinux CACHE FILEPATH "Kernel BTF used to generate vmlinux.h")
  if(NOT EXISTS "${VMLINUX_BTF}")
    message(FATAL_ERROR "No kernel BTF at ${VMLINUX_BTF}; enable CONFIG_DEBUG_INFO_BTF")
  endif()
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
  set(GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
  add_custom_command(OUTPUT "${GENERATED}/vmlinux.h"
    COMMAND "${CMAKE_COMMAND}" -DKIND=btf -DBPFTOOL=${BPFTOOL} -DINPUT=${VMLINUX_BTF}
      -DOUTPUT=${GENERATED}/vmlinux.h -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Generate.cmake"
    DEPENDS "${VMLINUX_BTF}" cmake/Generate.cmake VERBATIM)
  add_custom_command(OUTPUT "${GENERATED}/latency.bpf.o"
    COMMAND "${BPF_CLANG}" -g -O2 -target bpf -D__TARGET_ARCH_${BPF_ARCH} -Wall -Werror
      -I "${GENERATED}" -I "${CMAKE_CURRENT_SOURCE_DIR}/include" -I /usr/include
      -c "${CMAKE_CURRENT_SOURCE_DIR}/bpf/latency.bpf.c" -o "${GENERATED}/latency.bpf.o"
    DEPENDS bpf/latency.bpf.c include/latency_shared.h "${GENERATED}/vmlinux.h" VERBATIM)
  add_custom_command(OUTPUT "${GENERATED}/latency.skel.h"
    COMMAND "${CMAKE_COMMAND}" -DKIND=skeleton -DBPFTOOL=${BPFTOOL}
      -DINPUT=${GENERATED}/latency.bpf.o -DOUTPUT=${GENERATED}/latency.skel.h
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Generate.cmake"
    DEPENDS "${GENERATED}/latency.bpf.o" cmake/Generate.cmake VERBATIM)
  add_custom_target(latency-bpf ALL DEPENDS "${GENERATED}/latency.skel.h")
  add_executable(latency-tracer src/main.cpp "${GENERATED}/latency.skel.h")
  add_dependencies(latency-tracer latency-bpf)
  target_include_directories(latency-tracer PRIVATE "${GENERATED}")
  target_link_libraries(latency-tracer PRIVATE latency_core PkgConfig::LIBBPF)
  target_compile_options(latency-tracer PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  install(TARGETS latency-tracer RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
  find_package(Catch2 3 QUIET)
  if(NOT Catch2_FOUND)
    include(FetchContent)
    if(POLICY CMP0135)
      cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(Catch2
      URL https://codeload.github.com/catchorg/Catch2/tar.gz/refs/tags/v3.7.1
      URL_HASH SHA256=c991b247a1a0d7bb9c39aa35faf0fe9e19764213f28ffba3109388e62ee0269c)
    FetchContent_MakeAvailable(Catch2)
  endif()
  add_executable(latency-tests tests/histogram_test.cpp tests/metrics_test.cpp)
  target_link_libraries(latency-tests PRIVATE latency_core Catch2::Catch2WithMain)
  add_test(NAME latency-tests COMMAND latency-tests)
  set_tests_properties(latency-tests PROPERTIES TIMEOUT 30)
endif()
