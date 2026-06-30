add_executable(IndirectDraws main.cpp ../common/platform_macos.mm)

target_compile_features(IndirectDraws PRIVATE cxx_std_20)

target_link_libraries(IndirectDraws PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(IndirectDraws)
lvk_metal_target_use_slang(IndirectDraws "${CMAKE_CURRENT_SOURCE_DIR}")
