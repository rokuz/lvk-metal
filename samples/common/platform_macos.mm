#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include <QuartzCore/QuartzCore.hpp>

void lvkMetalAttachLayer(CA::MetalLayer* layer, GLFWwindow* window) {
  NSWindow* nswindow = glfwGetCocoaWindow(window);
  nswindow.contentView.layer = (__bridge CAMetalLayer*)layer;
  nswindow.contentView.wantsLayer = YES;
}
