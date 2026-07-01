add_executable(SwapchainHDR main.cpp ../common/platform_macos.mm)

target_compile_features(SwapchainHDR PRIVATE cxx_std_20)

target_link_libraries(SwapchainHDR PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(SwapchainHDR)
lvk_metal_target_use_slang(SwapchainHDR "${CMAKE_CURRENT_SOURCE_DIR}")
