add_executable(RenderToCubeMap main.cpp ../common/platform_macos.mm)

target_compile_features(RenderToCubeMap PRIVATE cxx_std_20)

target_include_directories(RenderToCubeMap PRIVATE "${LVK_METAL_LVK_DIR}/third-party/deps/src/glm")

target_link_libraries(RenderToCubeMap PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(RenderToCubeMap)
lvk_metal_target_use_slang(RenderToCubeMap "${CMAKE_CURRENT_SOURCE_DIR}")
