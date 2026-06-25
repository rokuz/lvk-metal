cmake_minimum_required(VERSION 3.21)

project(lvk-metal CXX)

if(NOT APPLE)
  message(FATAL_ERROR "lvk-metal targets Apple platforms with Metal 4 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LVK_METAL_WITH_SAMPLES "Build sample apps" ON)
option(LVK_METAL_WITH_SAMPLES_SLANG "Use Slang (compiled to MSL at runtime) for sample shaders" OFF)

set(LVK_METAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(LVK_METAL_3PARTY_SRC "${LVK_METAL_ROOT}/3party/src")
set(LVK_METAL_METAL_CPP_DIR "${LVK_METAL_3PARTY_SRC}/metal-cpp")

set(LVK_METAL_BUNDLED_LVK_DIR "${LVK_METAL_3PARTY_SRC}/lightweightvk")
set(LVK_METAL_LVK_DIR "${LVK_METAL_BUNDLED_LVK_DIR}" CACHE PATH "Path to the lightweightvk source tree")

if(LVK_METAL_LVK_DIR STREQUAL LVK_METAL_BUNDLED_LVK_DIR)
  if(NOT EXISTS "${LVK_METAL_LVK_DIR}/CMakeLists.txt")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "lvk-metal: fetching dependencies via bootstrap.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${LVK_METAL_ROOT}/bootstrap.py" --use-unzip WORKING_DIRECTORY "${LVK_METAL_ROOT}/3party"
                                                                                                       COMMAND_ERROR_IS_FATAL ANY)
  endif()
  set(LVK_WITH_SAMPLES OFF CACHE BOOL "" FORCE)
  set(LVK_WITH_TRACY OFF CACHE BOOL "" FORCE)
  set(LVK_WITH_IMPLOT OFF CACHE BOOL "" FORCE)
  set(LVK_WITH_SLANG OFF CACHE BOOL "" FORCE)
  set(LVK_DEPLOY_SCREENSHOT_TESTS OFF CACHE BOOL "" FORCE)
  set(LVK_WITH_GLFW ON CACHE BOOL "" FORCE)
  add_subdirectory("${LVK_METAL_LVK_DIR}" lightweightvk)
endif()

if(NOT TARGET LVKLibrary)
  message(FATAL_ERROR "lvk-metal requires the lightweightvk target 'LVKLibrary'. "
                      "When LVK_METAL_LVK_DIR points to an external tree, that project must define " "LVKLibrary before adding lvk-metal.")
endif()

add_subdirectory(lvk)

function(lvk_metal_target_use_slang target shaders_dir)
  if(NOT LVK_METAL_WITH_SAMPLES_SLANG)
    return()
  endif()
  set(_slang_dir "${LVK_METAL_3PARTY_SRC}/slang")
  if(NOT EXISTS "${_slang_dir}/include/slang.h")
    message(FATAL_ERROR "Slang not found at ${_slang_dir}. Add it to 3party/bootstrap.json and re-run bootstrap.")
  endif()
  find_library(LVK_METAL_SLANG_LIB NAMES slang libslang PATHS "${_slang_dir}/lib" NO_DEFAULT_PATH REQUIRED)
  target_sources(${target} PRIVATE "${LVK_METAL_ROOT}/samples/common/slang_runtime.cpp")
  target_include_directories(${target} PRIVATE "${_slang_dir}/include" "${LVK_METAL_ROOT}/samples/common")
  target_link_libraries(${target} PRIVATE "${LVK_METAL_SLANG_LIB}")
  target_compile_definitions(${target} PRIVATE
    LVK_METAL_SAMPLE_USE_SLANG=1
    "LVK_METAL_SAMPLE_SHADERS_DIR=\"${shaders_dir}\"")
  set_target_properties(${target} PROPERTIES BUILD_RPATH "${_slang_dir}/lib")
endfunction()

if(LVK_METAL_WITH_SAMPLES AND EXISTS "${LVK_METAL_ROOT}/samples/HelloTriangle/CMakeLists.txt")
  add_subdirectory(samples/HelloTriangle)
endif()
