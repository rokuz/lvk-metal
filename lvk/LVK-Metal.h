#pragma once

#include <cstdint>
#include <memory>

#include <ldrutils/lutils/Handle.h>

// clang-format off
// Define lvk::Holder<T> compatible types BEFORE <lvk/LVK.h>
namespace lvk {
class IContext;
} // namespace lvk

namespace lvk::metal {
using ArgumentTableHandle = ldr::Handle<struct ArgumentTable>;
} // namespace lvk::metal

namespace lvk {
void destroy(lvk::IContext* ctx, lvk::metal::ArgumentTableHandle handle);
} // namespace lvk

#include <lvk/LVK.h>
// clang-format on

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

namespace lvk::metal {

struct ContextConfig {
  uint32_t framesInFlight = 2;
  MTL::PixelFormat swapchainFormat = MTL::PixelFormatBGRA8Unorm;
  bool vsync = false;
  bool gammaCorrection = false;
  bool headless = false;
  bool validation = false;
  uint32_t initialTexturesPoolSize = 16384;
  uint32_t initialSamplesPoolSize = 1024;
  uint32_t initialPushConstantsPerFrameCount = 256;
  uint32_t pushConstantsSize = 128;
};

enum class ArgumentKind : uint8_t {
  Buffers,
  Textures2D,
  Textures3D,
  TexturesCube,
  Samplers,
  Images2D,
  TexturesDepth2D,
  SamplersComparison,
  Constants,
};

struct ArgumentTableDesc {
  enum { kMaxBindings = 16 };
  ArgumentKind kinds[kMaxBindings] = {};
  uint32_t numKinds = 0;
  const char* debugName = "";
};

class IMetalCommandBuffer : public lvk::ICommandBuffer {
 public:
  virtual void cmdBindArgumentTable(ArgumentTableHandle handle) = 0;
  virtual void cmdSetStencilRef(uint32_t ref) = 0;
};

class IMetalContext : public lvk::IContext {
 public:
  using lvk::IContext::destroy;

  ICommandBuffer& acquireCommandBuffer(bool dedicatedCompute = false) override {
    return acquireMetalCommandBuffer(dedicatedCompute);
  }

  virtual IMetalCommandBuffer& acquireMetalCommandBuffer(bool dedicatedCompute = false) = 0;

  [[nodiscard]] virtual Holder<ArgumentTableHandle> createArgumentTable(const ArgumentTableDesc& desc, Result* outResult = nullptr) = 0;
  virtual void destroy(ArgumentTableHandle handle) = 0;

  virtual bool startGpuCapture(const char* outputPath) = 0;
  virtual void stopGpuCapture() = 0;
};

std::unique_ptr<IMetalContext> createContextWithMetalLayer(CA::MetalLayer* layer,
                                                           uint32_t width,
                                                           uint32_t height,
                                                           const ContextConfig& config);

} // namespace lvk::metal
