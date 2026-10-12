cmake_minimum_required(VERSION 3.24)

project(limitbook
  VERSION 0.1.0
  DESCRIPTION "Full-depth limit order book and matching engine in C++20, validated against NASDAQ ITCH 5.0"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(QC_BUILD_TESTS "Build unit and property tests" ON)
option(QC_BUILD_BENCH "Build google/benchmark suites" ON)
option(QC_BUILD_TOOLS "Build replay and generator tools" ON)
option(QC_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(ProjectOptions)

# ---- Library (header-only: everything on the hot path must be inlinable) ----
add_library(limitbook INTERFACE)
add_library(limitbook::limitbook ALIAS limitbook)
target_include_directories(limitbook INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_features(limitbook INTERFACE cxx_std_20)

include(Dependencies)

if(QC_BUILD_TOOLS)
  add_subdirectory(tools)
endif()

if(QC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(QC_BUILD_BENCH)
  add_subdirectory(bench)
endif()
