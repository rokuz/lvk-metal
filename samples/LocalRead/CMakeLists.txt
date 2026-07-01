add_executable(LocalRead main.cpp ../common/platform_macos.mm)

target_compile_features(LocalRead PRIVATE cxx_std_20)

target_include_directories(LocalRead PRIVATE "${LVK_METAL_LVK_DIR}/third-party/deps/src/glm")

target_compile_definitions(
  LocalRead PRIVATE "LVK_METAL_LOCALREAD_TEXTURES_DIR=\"${LVK_METAL_LVK_DIR}/third-party/deps/src/ktx-software/tests/srcimages/Iron_Bars\"")

target_link_libraries(LocalRead PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(LocalRead)
lvk_metal_target_use_slang(LocalRead "${CMAKE_CURRENT_SOURCE_DIR}")
