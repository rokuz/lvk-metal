set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

add_executable(SolarSystem
  main.cpp
  ../common/platform_macos.mm
  "${LVK_DEPS}/ldrutils/lmath/GeometryShapes.cpp"
  "${LVK_DEPS}/3D-Graphics-Rendering-Cookbook/shared/UtilsCubemap.cpp"
  "${LVK_DEPS}/fast_obj/fast_obj.c")

target_compile_features(SolarSystem PRIVATE cxx_std_20)

target_include_directories(SolarSystem PRIVATE
  "${LVK_DEPS}/glm"
  "${LVK_DEPS}/ldrutils"
  "${LVK_DEPS}/3D-Graphics-Rendering-Cookbook"
  "${LVK_DEPS}/fast_obj")

target_compile_definitions(SolarSystem PRIVATE
  "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content/src/solarsystem\"")

target_link_libraries(SolarSystem PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw)

lvk_metal_target_add_common(SolarSystem)
lvk_metal_target_use_slang(SolarSystem "${CMAKE_CURRENT_SOURCE_DIR}")
