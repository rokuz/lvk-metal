set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

if(NOT TARGET meshoptimizer)
  add_subdirectory("${LVK_DEPS}/meshoptimizer" "${CMAKE_BINARY_DIR}/meshoptimizer")
endif()

add_executable(RTX_AO main.cpp ../common/platform_macos.mm "${LVK_DEPS}/fast_obj/fast_obj.c")

target_compile_features(RTX_AO PRIVATE cxx_std_20)

target_include_directories(RTX_AO PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils" "${LVK_DEPS}/fast_obj" "${LVK_DEPS}/meshoptimizer/src"
                                          "${LVK_DEPS}/3D-Graphics-Rendering-Cookbook")

target_compile_definitions(RTX_AO PRIVATE "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content\"")

target_link_libraries(RTX_AO PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw meshoptimizer)

lvk_metal_target_add_common(RTX_AO)
lvk_metal_target_use_slang(RTX_AO "${CMAKE_CURRENT_SOURCE_DIR}")
