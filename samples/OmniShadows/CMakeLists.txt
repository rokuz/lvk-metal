set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

add_executable(OmniShadows main.cpp ../common/platform_macos.mm "${LVK_DEPS}/ldrutils/lmath/GeometryShapes.cpp")

target_compile_features(OmniShadows PRIVATE cxx_std_20)

target_include_directories(OmniShadows PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils")

target_link_libraries(OmniShadows PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw)

lvk_metal_target_add_common(OmniShadows)
lvk_metal_target_use_slang(OmniShadows "${CMAKE_CURRENT_SOURCE_DIR}")
