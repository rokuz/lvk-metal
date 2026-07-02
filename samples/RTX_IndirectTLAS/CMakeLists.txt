set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

add_executable(RTX_IndirectTLAS main.cpp ../common/platform_macos.mm)

target_compile_features(RTX_IndirectTLAS PRIVATE cxx_std_20)

target_include_directories(RTX_IndirectTLAS PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils")

target_link_libraries(RTX_IndirectTLAS PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(RTX_IndirectTLAS)
