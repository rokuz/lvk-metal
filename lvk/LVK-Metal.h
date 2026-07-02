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
using TilePipelineHandle = ldr::Handle<struct TilePipeline>;
using TensorHandle = ldr::Handle<struct Tensor>;
using MLPipelineHandle = ldr::Handle<struct MLPipeline>;
} // namespace lvk::metal

namespace lvk {
void destroy(lvk::IContext* ctx, lvk::metal::ArgumentTableHandle handle);
void destroy(lvk::IContext* ctx, lvk::metal::TilePipelineHandle handle);
void destroy(lvk::IContext* ctx, lvk::metal::TensorHandle handle);
void destroy(lvk::IContext* ctx, lvk::metal::MLPipelineHandle handle);
} // namespace lvk

#include <lvk/LVK.h>
// clang-format on

namespace CA {
class MetalLayer;
} // namespace CA

namespace lvk::metal {

struct ContextConfig {
  uint32_t framesInFlight = 2;
  Format swapchainFormat = Format_BGRA_UN8;
  ColorSpace swapchainRequestedColorSpace = ColorSpace_SRGB_NONLINEAR;

  uint32_t initialTexturesPoolSize = 16384;
  uint32_t initialBuffersPoolSize = 32;
  uint32_t initialSamplesPoolSize = 32;
  uint32_t initialPushConstantsPerFrameCount = 256;
  uint32_t pushConstantsSize = 128;

  bool vsync = false;
  bool gammaCorrection = false;
  bool headless = false;
  bool validation = false;
};

enum class ArgumentKind : uint8_t {
  Buffers,
  Textures2D,
  Textures3D,
  TexturesCube,
  Samplers,
  Images2D,
  Images3D,
  TexturesDepth2D,
  SamplersComparison,
  Constants,
  AccelStructs,
  TexturesYUVChroma,
};

struct ArgumentTableDesc {
  enum { kMaxBindings = 16 };
  ArgumentKind kinds[kMaxBindings] = {};
  uint32_t numKinds = 0;
  const char* debugName = "";
};

struct ShaderModuleMetadata {
  Dimensions threadgroupSize = {1, 1, 1};
  uint32_t viewCount = 1; // vertex-shader multiview: max vertex amplification count
};

struct IndirectBufferMetadata {
  BufferHandle primitiveTypes = {};
  BufferHandle indexBuffer = {};
  IndexFormat indexFormat = IndexFormat_UI32;
  BufferHandle meshThreadgroupSizes = {};
};

struct TileRenderPipelineDesc {
  ShaderModuleHandle smTile; // the [[kernel]] tile function
  Format color[LVK_MAX_COLOR_ATTACHMENTS] = {}; // attachment formats, must match the enclosing render pass
  uint32_t maxThreadsPerThreadgroup = 1024;
  const char* debugName = "";

  uint32_t getNumColorAttachments() const {
    uint32_t n = 0;
    while (n < LVK_MAX_COLOR_ATTACHMENTS && color[n] != Format_Invalid) {
      n++;
    }
    return n;
  }
};

enum class TensorDataType : uint8_t {
  Float16,
  Float32,
  BFloat16,
  Int8,
  UInt8,
  Int32,
};

// Metal requires the row stride of a MachineLearning-usage tensor to be 64-byte aligned.
// Returns the padded number of inner-dimension elements per row for a buffer-backed ML tensor.
inline uint32_t mlTensorRowStrideElements(uint32_t innerElements, uint32_t elementSize) {
  return (((innerElements * elementSize + 63u) / 64u) * 64u) / elementSize;
}

struct TensorDesc {
  enum { kMaxRank = 4 };
  TensorDataType dataType = TensorDataType::Float16;
  uint32_t dimensions[kMaxRank] = {};
  uint32_t rank = 0;
  StorageType storage = StorageType_HostVisible;
  BufferHandle buffer = {}; // if set, the tensor aliases this buffer's memory (GPU-addressable by compute shaders)
  uint32_t bufferOffset = 0;
  const char* debugName = "";

  uint32_t getNumElements() const {
    uint32_t n = rank ? 1u : 0u;
    for (uint32_t i = 0; i < rank; ++i) {
      n *= dimensions[i];
    }
    return n;
  }
};

struct MachineLearningPipelineDesc {
  enum { kMaxTensors = 8 };
  const char* packagePath = nullptr; // path to an .mtlpackage ML-network library on disk
  const char* functionName = "main";
  TensorHandle inputs[kMaxTensors] = {}; // bound at network buffer indices 0..numInputs-1
  uint32_t numInputs = 0;
  TensorHandle outputs[kMaxTensors] = {}; // bound at network buffer indices numInputs..numInputs+numOutputs-1
  uint32_t numOutputs = 0;
  const char* debugName = "";
};

class IMetalCommandBuffer : public lvk::ICommandBuffer {
 public:
  virtual void cmdBindArgumentTable(ArgumentTableHandle handle) = 0;
  virtual void cmdSetStencilRef(uint32_t ref) = 0;

  virtual void cmdBindTilePipeline(TilePipelineHandle pipeline) = 0;
  virtual void cmdDispatchTile() = 0;

  virtual void cmdBindMachineLearningPipeline(MLPipelineHandle pipeline) = 0;
  virtual void cmdDispatchNetwork() = 0;
};

class IMetalContext : public lvk::IContext {
 public:
  using lvk::IContext::destroy;
  using lvk::IContext::upload;
  using lvk::IContext::download;

  ICommandBuffer& acquireCommandBuffer(bool dedicatedCompute = false) override {
    return acquireMetalCommandBuffer(dedicatedCompute);
  }

  virtual IMetalCommandBuffer& acquireMetalCommandBuffer(bool dedicatedCompute = false) = 0;

  [[nodiscard]] virtual Holder<ArgumentTableHandle> createArgumentTable(const ArgumentTableDesc& desc, Result* outResult = nullptr) = 0;
  virtual void destroy(ArgumentTableHandle handle) = 0;

  [[nodiscard]] virtual Holder<TilePipelineHandle> createTileRenderPipeline(const TileRenderPipelineDesc& desc,
                                                                            Result* outResult = nullptr) = 0;
  virtual void destroy(TilePipelineHandle handle) = 0;

  [[nodiscard]] virtual Holder<TensorHandle> createTensor(const TensorDesc& desc, Result* outResult = nullptr) = 0;
  virtual void destroy(TensorHandle handle) = 0;
  virtual Result upload(TensorHandle handle, const void* data, size_t size) = 0;
  virtual Result download(TensorHandle handle, void* data, size_t size) = 0;

  [[nodiscard]] virtual Holder<MLPipelineHandle> createMachineLearningPipeline(const MachineLearningPipelineDesc& desc,
                                                                                           Result* outResult = nullptr) = 0;
  virtual void destroy(MLPipelineHandle handle) = 0;

  virtual bool startGpuCapture(const char* outputPath) = 0;
  virtual void stopGpuCapture() = 0;

  virtual void setShaderModuleMetadata(ShaderModuleHandle handle, const ShaderModuleMetadata& metadata) = 0;

  virtual void setIndirectBufferMetadata(BufferHandle indirectBuffer, const IndirectBufferMetadata& metadata) = 0;
};

std::unique_ptr<IMetalContext> createContextWithMetalLayer(CA::MetalLayer* layer,
                                                           uint32_t width,
                                                           uint32_t height,
                                                           const ContextConfig& config);

} // namespace lvk::metal
