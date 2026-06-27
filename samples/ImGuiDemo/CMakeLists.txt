add_executable(ImGuiDemo main.cpp ../common/platform_macos.mm)

target_compile_features(ImGuiDemo PRIVATE cxx_std_20)

target_link_libraries(ImGuiDemo PRIVATE lvk-metal lvk-metal-imgui LVKLibrary glfw)

lvk_metal_target_add_common(ImGuiDemo)
