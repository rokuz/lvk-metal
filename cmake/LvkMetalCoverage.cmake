option(LVK_METAL_COVERAGE "Enable code coverage" OFF)

if(NOT LVK_METAL_COVERAGE)
  return()
endif()

if(NOT LVK_METAL_SANITIZER STREQUAL "")
  message(FATAL_ERROR "LVK_METAL_COVERAGE and LVK_METAL_SANITIZER are mutually exclusive. Disable one of them.")
endif()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "LVK_METAL_COVERAGE requires Clang/AppleClang (LLVM source-based coverage)")
endif()

set(LVK_METAL_COVERAGE_COMPILE_FLAGS -fprofile-instr-generate -fcoverage-mapping)
set(LVK_METAL_COVERAGE_LINK_FLAGS -fprofile-instr-generate -fcoverage-mapping)

message(STATUS "lvk-metal coverage: ON (LLVM source-based)")
