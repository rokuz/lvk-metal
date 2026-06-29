#include "app_common.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

extern void lvkMetalAttachLayer(CA::MetalLayer* layer, GLFWwindow* window);

namespace lvk::metal {

AppConfig parseAppConfig(int argc, char** argv) {
  AppConfig cfg;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--headless") {
      cfg.headless = true;
    } else if (arg == "--screenshot-file" && i + 1 < argc) {
      cfg.screenshotFile = argv[++i];
    } else if (arg == "--screenshot-frame" && i + 1 < argc) {
      cfg.screenshotFrame = std::atoi(argv[++i]);
    } else if (arg == "--frames" && i + 1 < argc) {
      cfg.numFrames = std::atoi(argv[++i]);
    } else if (arg == "--width" && i + 1 < argc) {
      cfg.width = std::atoi(argv[++i]);
    } else if (arg == "--height" && i + 1 < argc) {
      cfg.height = std::atoi(argv[++i]);
    } else if (arg == "--msaa" && i + 1 < argc) {
      cfg.msaa = std::atoi(argv[++i]);
    }
  }
  return cfg;
}

bool writeScreenshotPNG(lvk::IContext& ctx, lvk::TextureHandle texture, uint32_t width, uint32_t height, const char* path) {
  std::vector<uint8_t> pixels(size_t(width) * size_t(height) * 4);
  const lvk::Result result = ctx.download(texture, {.dimensions = {width, height, 1}}, pixels.data());
  if (!result.isOk()) {
    LLOGE("writeScreenshotPNG: download failed: %s", result.message);
    return false;
  }

  for (size_t i = 0; i < size_t(width) * size_t(height); ++i) {
    std::swap(pixels[i * 4 + 0], pixels[i * 4 + 2]);
    pixels[i * 4 + 3] = 255;
  }

  if (!stbi_write_png(path, int(width), int(height), 4, pixels.data(), int(width) * 4)) {
    LLOGE("writeScreenshotPNG: stbi_write_png failed for '%s'", path);
    return false;
  }

  LLOGL("writeScreenshotPNG: wrote '%s' (%ux%u)", path, width, height);
  return true;
}

int run(int argc, char** argv, const char* title, std::unique_ptr<ISample>& sample) {
  const AppConfig app = parseAppConfig(argc, argv);

  if (!glfwInit()) {
    LLOGE("glfwInit failed");
    return EXIT_FAILURE;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  if (app.headless)
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);

  GLFWwindow* window = glfwCreateWindow(1024, 768, title, nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return EXIT_FAILURE;
  }

  int fbWidth = 0;
  int fbHeight = 0;
  glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
  if (app.width > 0 && app.height > 0) {
    fbWidth = app.width;
    fbHeight = app.height;
  }

  CA::MetalLayer* layer = CA::MetalLayer::layer();
  lvkMetalAttachLayer(layer, window);

  float scaleX, scaleY;
  glfwGetWindowContentScale(window, &scaleX, &scaleY);

  std::unique_ptr<lvk::metal::IMetalContext> ctx = lvk::metal::createContextWithMetalLayer(
      layer, uint32_t(fbWidth), uint32_t(fbHeight), {.vsync = false, .headless = app.headless, .validation = true});
  if (!ctx) {
    LLOGE("createContextWithMetalLayer failed");
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }

  sample->init(*ctx,
               app.headless ? nullptr : window,
               uint32_t(fbWidth),
               uint32_t(fbHeight),
               std::max(scaleX, scaleY),
               uint32_t(app.msaa > 0 ? app.msaa : 1));

  int exitCode = 0;
  if (app.screenshot()) {
    const char* gpuCapturePath = getenv("LVKM_GPU_CAPTURE");
    const int numFrames = app.numFrames > 1 ? app.numFrames : 1;

    while (!sample->isReadyForScreenshot()) {
      lvk::ICommandBuffer& cmd = ctx->acquireCommandBuffer();
      sample->render(cmd, ctx->getCurrentSwapchainTexture(), float(app.screenshotFrame) / 60.0f);
      ctx->wait(ctx->submit(cmd, {}));
    }

    lvk::TextureHandle swapchain;
    for (int f = 1; f <= numFrames; ++f) {
      const bool last = f == numFrames;
      if (gpuCapturePath && last)
        ctx->startGpuCapture(gpuCapturePath);
      const float time = float(numFrames > 1 ? f : app.screenshotFrame) / 60.0f;
      lvk::ICommandBuffer& cmd = ctx->acquireCommandBuffer();
      swapchain = ctx->getCurrentSwapchainTexture();
      sample->render(cmd, swapchain, time);
      const lvk::SubmitHandle handle = ctx->submit(cmd, {});
      ctx->wait(handle);
    }
    if (gpuCapturePath)
      ctx->stopGpuCapture();
    exitCode = writeScreenshotPNG(*ctx, swapchain, uint32_t(fbWidth), uint32_t(fbHeight), app.screenshotFile.c_str()) ? 0 : 1;
  } else {
    while (!glfwWindowShouldClose(window)) {
      glfwPollEvents();

      lvk::ICommandBuffer& cmd = ctx->acquireCommandBuffer();
      const lvk::TextureHandle swapchain = ctx->getCurrentSwapchainTexture();
      sample->render(cmd, swapchain, float(glfwGetTime()));
      ctx->submit(cmd, swapchain);
    }
  }

  sample->destroy();
  sample.reset();

  ctx.reset();

  glfwDestroyWindow(window);
  glfwTerminate();
  return exitCode;
}

} // namespace lvk::metal
