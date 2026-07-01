add_executable(QueryPool main.cpp ../common/platform_macos.mm)

target_compile_features(QueryPool PRIVATE cxx_std_20)

target_link_libraries(QueryPool PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(QueryPool)
lvk_metal_target_use_slang(QueryPool "${CMAKE_CURRENT_SOURCE_DIR}")
