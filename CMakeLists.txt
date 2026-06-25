cmake_minimum_required(VERSION 3.21)

project(lvk-metal CXX)

if(NOT APPLE)
  message(FATAL_ERROR "lvk-metal targets Apple platforms with Metal 4 only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LVK_METAL_WITH_SAMPLES "Build sample apps" ON)

set(LVK_METAL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(LVK_METAL_3PARTY_SRC "${LVK_METAL_ROOT}/3party/src")
set(LVK_METAL_METAL_CPP_DIR "${LVK_METAL_3PARTY_SRC}/metal-cpp")

set(LVK_METAL_BUNDLED_LVK_DIR "${LVK_METAL_3PARTY_SRC}/lightweightvk")
set(LVK_METAL_LVK_DIR "${LVK_METAL_BUNDLED_LVK_DIR}" CACHE PATH "Path to the lightweightvk source tree")

if(LVK_METAL_LVK_DIR STREQUAL LVK_METAL_BUNDLED_LVK_DIR)
  if(NOT EXISTS "${LVK_METAL_LVK_DIR}/CMakeLists.txt")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    message(STATUS "lvk-metal: fetching dependencies via bootstrap.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${LVK_METAL_ROOT}/bootstrap.py" WORKING_DIRECTORY "${LVK_METAL_ROOT}/3party"
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

if(LVK_METAL_WITH_SAMPLES AND EXISTS "${LVK_METAL_ROOT}/samples/HelloTriangle/CMakeLists.txt")
  add_subdirectory(samples/HelloTriangle)
endif()
