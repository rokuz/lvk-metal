add_executable(YUV main.cpp ../common/platform_macos.mm)

target_compile_features(YUV PRIVATE cxx_std_20)

target_compile_definitions(YUV PRIVATE "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content/src/igl-samples\"")

target_link_libraries(YUV PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(YUV)
lvk_metal_target_use_slang(YUV "${CMAKE_CURRENT_SOURCE_DIR}")
