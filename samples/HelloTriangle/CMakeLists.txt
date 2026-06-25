add_executable(HelloTriangle main.cpp ../common/platform_macos.mm)

target_compile_features(HelloTriangle PRIVATE cxx_std_20)

target_link_libraries(HelloTriangle PRIVATE lvk-metal LVKLibrary glfw)
