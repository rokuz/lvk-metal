set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

lvk_metal_build_bistro_deps()

add_executable(Bistro
  main.cpp
  ../common/platform_macos.mm
  "${LVK_DEPS}/3D-Graphics-Rendering-Cookbook/shared/UtilsCubemap.cpp"
  "${LVK_DEPS}/fast_obj/fast_obj.c")

target_compile_features(Bistro PRIVATE cxx_std_20)

target_include_directories(Bistro PRIVATE
  "${LVK_DEPS}/glm"
  "${LVK_DEPS}/3D-Graphics-Rendering-Cookbook"
  "${LVK_DEPS}/fast_obj"
  "${LVK_DEPS}/meshoptimizer/src"
  "${LVK_DEPS}/ktx-software/include"
  "${LVK_DEPS}/taskflow")

target_compile_definitions(Bistro PRIVATE
  "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content\"")

target_link_libraries(Bistro PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw meshoptimizer ktx)

lvk_metal_target_add_common(Bistro)
lvk_metal_target_use_slang(Bistro "${CMAKE_CURRENT_SOURCE_DIR}")
