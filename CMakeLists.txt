cmake_minimum_required(VERSION 3.22)
project(latency_tracer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
option(LATENCY_BUILD_BPF "Build Linux CO-RE probes" ON)

add_library(latency_shared INTERFACE)
target_include_directories(latency_shared INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")

if(LATENCY_BUILD_BPF)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Probes require Linux; use -DLATENCY_BUILD_BPF=OFF for the shared headers only")
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
endif()
