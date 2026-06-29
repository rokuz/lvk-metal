add_executable(MeshShaders main.cpp ../common/platform_macos.mm)

target_compile_features(MeshShaders PRIVATE cxx_std_20)

target_link_libraries(MeshShaders PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(MeshShaders)
lvk_metal_target_use_slang(MeshShaders "${CMAKE_CURRENT_SOURCE_DIR}")
