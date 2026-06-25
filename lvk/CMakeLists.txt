add_library(lvk-metal STATIC metal/MetalClasses.cpp metal/MetalImmediateCommands.cpp)

target_compile_features(lvk-metal PUBLIC cxx_std_20)

target_include_directories(lvk-metal PUBLIC "${LVK_METAL_ROOT}" "${LVK_METAL_METAL_CPP_DIR}"
                                            "$<TARGET_PROPERTY:LVKLibrary,INTERFACE_INCLUDE_DIRECTORIES>")

if(APPLE)
  target_link_libraries(lvk-metal PUBLIC "-framework Foundation" "-framework Metal" "-framework QuartzCore" "-framework Cocoa"
                                         "-framework MetalKit")
endif()
