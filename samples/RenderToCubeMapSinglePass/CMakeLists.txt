add_executable(RenderToCubeMapSinglePass main.cpp ../common/platform_macos.mm)

target_compile_features(RenderToCubeMapSinglePass PRIVATE cxx_std_20)

target_include_directories(RenderToCubeMapSinglePass PRIVATE "${LVK_METAL_LVK_DIR}/third-party/deps/src/glm")

target_link_libraries(RenderToCubeMapSinglePass PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(RenderToCubeMapSinglePass)
lvk_metal_target_use_slang(RenderToCubeMapSinglePass "${CMAKE_CURRENT_SOURCE_DIR}")
