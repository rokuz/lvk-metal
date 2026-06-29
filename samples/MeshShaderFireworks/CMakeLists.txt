set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

add_executable(MeshShaderFireworks main.cpp ../common/platform_macos.mm)

target_compile_features(MeshShaderFireworks PRIVATE cxx_std_20)

target_include_directories(MeshShaderFireworks PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils")

target_link_libraries(MeshShaderFireworks PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw)

lvk_metal_target_add_common(MeshShaderFireworks)
lvk_metal_target_use_slang(MeshShaderFireworks "${CMAKE_CURRENT_SOURCE_DIR}")
