#pragma once

#include <lvk/LVK-Metal.h>

#include <cstdint>
#include <string>

namespace lvk::metal {

struct AppConfig {
  std::string screenshotFile;
  int screenshotFrame = 1;
  bool headless = false;
  int width = 0;
  int height = 0;

  bool screenshot() const {
    return !screenshotFile.empty();
  }
};

AppConfig parseAppConfig(int argc, char** argv);

bool writeScreenshotPNG(lvk::IContext& ctx, lvk::TextureHandle texture, uint32_t width, uint32_t height, const char* path);

struct ISample {
  virtual ~ISample() = default;
  virtual void init(lvk::metal::IMetalContext& ctx, uint32_t width, uint32_t height) = 0;
  virtual void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float timeSeconds) = 0;
  virtual void destroy() = 0;
};

int run(int argc, char** argv, const char* title, ISample& sample);

} // namespace lvk::metal

#define DESKTOP_MAIN(SampleClass)                                            \
  int main(int argc, char** argv) {                                          \
    SampleClass sample;                                                      \
    return lvk::metal::run(argc, argv, "[lvk-metal] " #SampleClass, sample); \
  }
