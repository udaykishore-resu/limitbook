# Warnings, sanitizers and hot-path flags applied per target (never globally,
# so third-party code fetched with FetchContent builds with its own flags).

function(qc_set_warnings target)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    message(FATAL_ERROR "Only GCC and Clang (incl. AppleClang) are supported")
  endif()
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wconversion -Wsign-conversion -Wold-style-cast
    -Wnon-virtual-dtor -Woverloaded-virtual -Wcast-align
    -Wdouble-promotion -Wimplicit-fallthrough -Wformat=2 -Wunused)
  # -Wuseless-cast is deliberately off: size_t and uint64_t are the same type
  # on Linux but not on macOS, so a cast that is "useless" here is required there.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC's -Wnull-dereference and -Wstringop-overflow run after inlining at
    # -O2/-O3 and report paths that cannot happen (e.g. inside
    # std::vector::insert). Clang's flow analysis is quieter, so it keeps them.
    target_compile_options(${target} PRIVATE -Wduplicated-cond -Wlogical-op -Wno-stringop-overflow)
  else()
    target_compile_options(${target} PRIVATE -Wnull-dereference)
  endif()
  if(QC_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

# Hot-path binaries: no exceptions, no RTTI. The headers they include must be
# exception-free, so building these targets is itself a compile-time check.
function(qc_hot_path target)
  target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
endfunction()

# QC_SANITIZER: "" | "address,undefined" | "thread"
set(QC_SANITIZER "" CACHE STRING "Sanitizers to enable (address,undefined | thread)")
if(QC_SANITIZER)
  add_compile_options(-fsanitize=${QC_SANITIZER} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${QC_SANITIZER})
endif()

option(QC_NATIVE "Tune for the build machine (-march/-mcpu=native)" OFF)
if(QC_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-march=native" QC_HAS_MARCH_NATIVE)
  if(QC_HAS_MARCH_NATIVE)
    add_compile_options(-march=native)
  else()
    check_cxx_compiler_flag("-mcpu=native" QC_HAS_MCPU_NATIVE)
    if(QC_HAS_MCPU_NATIVE)
      add_compile_options(-mcpu=native)
    endif()
  endif()
endif()
