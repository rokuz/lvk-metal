add_executable(MachineLearning main.cpp ../common/platform_macos.mm)

target_compile_features(MachineLearning PRIVATE cxx_std_20)

target_include_directories(MachineLearning PRIVATE "${LVK_METAL_LVK_DIR}/third-party/deps/src/glm")

target_compile_definitions(MachineLearning PRIVATE "LVK_METAL_ML_ASSETS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/assets\"")

target_link_libraries(MachineLearning PRIVATE lvk-metal LVKLibrary glfw)

lvk_metal_target_add_common(MachineLearning)
