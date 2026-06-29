#pragma once

#include <lvk/LVK-Metal.h>

#include <cstdint>
#include <memory>
#include <string>

struct GLFWwindow;

namespace lvk::metal {

struct AppConfig {
  std::string screenshotFile;
  int screenshotFrame = 1;
  int numFrames = 1;
  bool headless = false;
  int width = 0;
  int height = 0;
  int msaa = 1;

  bool screenshot() const {
    return !screenshotFile.empty();
  }
};

AppConfig parseAppConfig(int argc, char** argv);

bool writeScreenshotPNG(lvk::IContext& ctx, lvk::TextureHandle texture, uint32_t width, uint32_t height, const char* path);

struct ISample {
  virtual ~ISample() = default;
  virtual void init(lvk::metal::IMetalContext& ctx,
                    GLFWwindow* window,
                    uint32_t width,
                    uint32_t height,
                    float displayScale,
                    uint32_t samplesCount) = 0;
  virtual void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float timeSeconds) = 0;
  virtual void destroy() {}

  // Samples that stream assets asynchronously (e.g. background texture loading) return false until
  // everything is resident, so the headless screenshot path can wait for a fully-loaded frame.
  virtual bool isReadyForScreenshot() const {
    return true;
  }
};

int run(int argc, char** argv, const char* title, std::unique_ptr<ISample>& sample);

} // namespace lvk::metal

#define DESKTOP_MAIN(SampleClass)                                                  \
  int main(int argc, char** argv) {                                                \
    std::unique_ptr<lvk::metal::ISample> sample = std::make_unique<SampleClass>(); \
    return lvk::metal::run(argc, argv, "[lvk-metal] " #SampleClass, sample);       \
  }
