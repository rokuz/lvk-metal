set(LVK_DEPS "${LVK_METAL_LVK_DIR}/third-party/deps/src")

if(NOT TARGET meshoptimizer)
  add_subdirectory("${LVK_DEPS}/meshoptimizer" "${CMAKE_BINARY_DIR}/meshoptimizer")
endif()

add_executable(RTX_Bistro main.cpp ../common/platform_macos.mm "${LVK_DEPS}/fast_obj/fast_obj.c")

target_compile_features(RTX_Bistro PRIVATE cxx_std_20)

target_include_directories(RTX_Bistro PRIVATE "${LVK_DEPS}/glm" "${LVK_DEPS}/ldrutils" "${LVK_DEPS}/fast_obj"
                                              "${LVK_DEPS}/meshoptimizer/src")

target_compile_definitions(RTX_Bistro PRIVATE "LVK_METAL_SAMPLE_CONTENT_DIR=\"${LVK_METAL_LVK_DIR}/third-party/content\"")

target_link_libraries(RTX_Bistro PRIVATE lvk-metal LVKLibrary glfw meshoptimizer)

lvk_metal_target_add_common(RTX_Bistro)
