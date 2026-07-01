add_executable(TextureView main.cpp ../common/platform_macos.mm)

target_compile_features(TextureView PRIVATE cxx_std_20)

target_link_libraries(TextureView PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(TextureView)
lvk_metal_target_use_slang(TextureView "${CMAKE_CURRENT_SOURCE_DIR}")
