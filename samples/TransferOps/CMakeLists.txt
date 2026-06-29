add_executable(TransferOps main.cpp ../common/platform_macos.mm)

target_compile_features(TransferOps PRIVATE cxx_std_20)

target_link_libraries(TransferOps PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(TransferOps)
lvk_metal_target_use_slang(TransferOps "${CMAKE_CURRENT_SOURCE_DIR}")
