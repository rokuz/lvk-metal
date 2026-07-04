add_library(lvk-metal STATIC metal/MetalClasses.cpp metal/MetalImmediateCommands.cpp metal/MetalStagingDevice.cpp)

target_compile_features(lvk-metal PUBLIC cxx_std_20)

target_include_directories(lvk-metal PUBLIC "${LVK_METAL_ROOT}" "${LVK_METAL_METAL_CPP_DIR}"
                                            "$<TARGET_PROPERTY:LVKLibrary,INTERFACE_INCLUDE_DIRECTORIES>")

target_compile_definitions(lvk-metal PRIVATE "$<TARGET_PROPERTY:LVKLibrary,INTERFACE_COMPILE_DEFINITIONS>")

if(LVK_METAL_DISABLE_GPU_COUNTERS)
  target_compile_definitions(lvk-metal PRIVATE LVK_METAL_DISABLE_GPU_COUNTERS)
endif()

if(APPLE)
  target_link_libraries(lvk-metal PUBLIC "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework Cocoa"
                                         "-framework MetalKit" "-framework CoreGraphics")
endif()

add_library(lvk-metal-imgui STATIC "${LVK_METAL_ROOT}/lvk/HelpersImGuiMetal.cpp")

target_compile_features(lvk-metal-imgui PUBLIC cxx_std_20)

target_include_directories(
  lvk-metal-imgui PUBLIC "${LVK_METAL_ROOT}" "${LVK_METAL_METAL_CPP_DIR}" "${LVK_METAL_LVK_DIR}/third-party/deps/src"
                         "${LVK_METAL_LVK_DIR}/third-party/deps/src/imgui" "$<TARGET_PROPERTY:LVKLibrary,INTERFACE_INCLUDE_DIRECTORIES>")

target_compile_definitions(lvk-metal-imgui PRIVATE "$<TARGET_PROPERTY:LVKLibrary,INTERFACE_COMPILE_DEFINITIONS>")

target_link_libraries(lvk-metal-imgui PRIVATE glfw)
