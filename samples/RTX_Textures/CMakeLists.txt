set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

add_executable(RTX_Textures main.cpp ../common/platform_macos.mm)

target_compile_features(RTX_Textures PRIVATE cxx_std_20)

target_include_directories(RTX_Textures PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils")

target_compile_definitions(RTX_Textures PRIVATE "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content\"")

target_link_libraries(RTX_Textures PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(RTX_Textures)
