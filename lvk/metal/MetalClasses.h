#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <ldrutils/lutils/Pool.h>

#include "lvk/LVK-Metal.h"
#include "lvk/metal/Common.h"
#include "lvk/metal/MetalImmediateCommands.h"
#include "lvk/metal/MetalLimits.h"

namespace lvk::metal {

struct MetalBuffer {
  NS::SharedPtr<MTL::Buffer> buffer;
  size_t size = 0;
};

struct MetalImage {
  NS::SharedPtr<MTL::Texture> texture;
  MTL::PixelFormat format = MTL::PixelFormatInvalid;
  uint32_t width = 0;
  uint32_t height = 0;
  bool isSwapchainImage = false;
};

struct MetalSampler {
  NS::SharedPtr<MTL::SamplerState> sampler;
};

struct MetalShaderModule {
  NS::SharedPtr<MTL::Library> library;
  NS::SharedPtr<MTL::Function> function;
  MTL::Size threadgroupSize = MTL::Size(16, 16, 1);
};

struct MetalRenderPipeline {
  NS::SharedPtr<MTL::RenderPipelineState> pipeline;
  MTL::PrimitiveType topology = MTL::PrimitiveTypeTriangle;
  MTL::CullMode cullMode = MTL::CullModeNone;
  MTL::Winding frontFace = MTL::WindingCounterClockwise;
  MTL::TriangleFillMode fillMode = MTL::TriangleFillModeFill;
};

struct MetalComputePipeline {
  NS::SharedPtr<MTL::ComputePipelineState> pipeline;
  MTL::Size threadgroupSize = MTL::Size(16, 16, 1);
};

struct MetalArgumentTable {
  NS::SharedPtr<MTL4::ArgumentTable> table;
  uint32_t constantsIndex = UINT32_MAX;
  ArgumentKind kinds[ArgumentTableDesc::kMaxBindings] = {};
  uint32_t numKinds = 0;
};

class MetalContext;
class MetalStagingDevice;

class CommandBuffer : public IMetalCommandBuffer {
 public:
  CommandBuffer() = default;
  explicit CommandBuffer(MetalContext* ctx) : ctx_(ctx) {}

  void cmdTransitionToGeneral(const ldr::Span<TextureHandle>& textures, lvk::ShaderStage extraDstStage) const override {}
  void cmdTransitionToShaderReadOnly(const ldr::Span<TextureHandle>& textures, lvk::ShaderStage extraDstStage) const override {}
  void cmdTransitionToRenderingLocalRead(const ldr::Span<TextureHandle>& textures) const override {}
  void cmdReleaseToAsyncCompute(const ldr::Span<TextureHandle>& textures) const override {}

  void cmdPushDebugGroupLabel(const char* label, uint32_t colorRGBA = 0xffffffff) const override;
  void cmdInsertDebugEventLabel(const char* label, uint32_t colorRGBA = 0xffffffff) const override;
  void cmdPopDebugGroupLabel() const override;

  void cmdBindRayTracingPipeline(lvk::RayTracingPipelineHandle handle) override {}

  void cmdBindComputePipeline(lvk::ComputePipelineHandle handle) override;
  void cmdDispatch(const Dimensions& groupCount, const Dependencies& deps = {}) override;
  void cmdDispatchIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset = 0, const Dependencies& deps = {}) override;

  void cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& desc, const Dependencies& deps = {}) override;
  void cmdEndRendering() override;
  void cmdNextSubpass() override {}

  void cmdBindViewport(const Viewport& viewport) override;
  void cmdBindScissorRect(const ScissorRect& rect) override;

  void cmdBindRenderPipeline(lvk::RenderPipelineHandle handle) override;
  void cmdBindDepthState(const DepthState& state) override;

  void cmdBindVertexBuffer(uint32_t index, BufferHandle buffer, uint64_t bufferOffset = 0, uint64_t bufferSize = LVK_WHOLE_SIZE) override {}
  void cmdBindIndexBuffer(BufferHandle indexBuffer,
                          IndexFormat indexFormat,
                          uint64_t bufferOffset = 0,
                          uint64_t bufferSize = LVK_WHOLE_SIZE) override;
  void cmdPushConstants(const void* data, size_t size, size_t offset = 0) override;

  void cmdCopyBuffer(BufferHandle srcBuffer, BufferHandle dstBuffer, size_t srcOffset, size_t dstOffset, size_t size) override {}
  void cmdFillBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, uint32_t data) override {}
  void cmdUpdateBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, const void* data) override {}

  void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t baseInstance = 0) override;
  void cmdDrawIndexed(uint32_t indexCount,
                      uint32_t instanceCount = 1,
                      uint32_t firstIndex = 0,
                      int32_t vertexOffset = 0,
                      uint32_t baseInstance = 0) override;
  void cmdDrawIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override {}
  void cmdDrawIndexedIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override {}
  void cmdDrawIndexedIndirectCount(BufferHandle indirectBuffer,
                                   size_t indirectBufferOffset,
                                   BufferHandle countBuffer,
                                   size_t countBufferOffset,
                                   uint32_t maxDrawCount,
                                   uint32_t stride = 0) override {}
  void cmdDrawMeshTasks(const Dimensions& threadgroupCount) override {}
  void cmdDrawMeshTasksIndirect(BufferHandle indirectBuffer,
                                size_t indirectBufferOffset,
                                uint32_t drawCount,
                                uint32_t stride = 0) override {}
  void cmdDrawMeshTasksIndirectCount(BufferHandle indirectBuffer,
                                     size_t indirectBufferOffset,
                                     BufferHandle countBuffer,
                                     size_t countBufferOffset,
                                     uint32_t maxDrawCount,
                                     uint32_t stride = 0) override {}
  void cmdTraceRays(uint32_t width, uint32_t height, uint32_t depth = 1, const Dependencies& deps = {}) override {}

  void cmdSetBlendColor(const float color[4]) override;
  void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp = 0.0f) override;
  void cmdSetDepthBiasEnable(bool enable) override {}

  void cmdResetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) override {}
  void cmdWriteTimestamp(QueryPoolHandle pool, uint32_t query) override {}

  void cmdClearColorImage(TextureHandle tex, const ClearColorValue& value, const TextureLayers& layers = {}) override {}
  void cmdCopyImage(TextureHandle src,
                    TextureHandle dst,
                    const Dimensions& extent,
                    const Offset3D& srcOffset = {},
                    const Offset3D& dstOffset = {},
                    const TextureLayers& srcLayers = {},
                    const TextureLayers& dstLayers = {}) override {}
  void cmdGenerateMipmap(TextureHandle handle) override {}
  void cmdUpdateTLAS(AccelStructHandle handle, BufferHandle instancesBuffer) override {}

  void cmdBindArgumentTable(ArgumentTableHandle handle) override;

 protected:
  MetalContext* context() const {
    return ctx_;
  }

 private:
  void bindArgumentTableInternal(ArgumentTableHandle handle);
  void resetArgumentTableIfOverridden();
  void setArgumentTableOnActiveEncoder(MTL4::ArgumentTable* table);
  void endComputeEncoder();

  MetalContext* ctx_ = nullptr;
  MTL4::RenderCommandEncoder* encoder_ = nullptr;
  MTL4::ComputeCommandEncoder* computeEncoder_ = nullptr;
  MTL::Size computeThreadgroupSize_ = MTL::Size(16, 16, 1);
  MTL::PrimitiveType topology_ = MTL::PrimitiveTypeTriangle;
  ArgumentTableHandle currentArgTable_;
  bool argTableOverridden_ = false;
  bool isRendering_ = false;
  BufferHandle boundIndexBuffer_;
  MTL::IndexType boundIndexType_ = MTL::IndexTypeUInt32;
  uint64_t boundIndexOffset_ = 0;
};

class MetalContext : public IMetalContext {
 public:
  MetalContext() = default;
  ~MetalContext() override;

  friend class MetalStagingDevice;

  uint32_t pushConstantsSize() const {
    return pushConstantsSize_;
  }
  const GpuLimits& limits() const {
    return limits_;
  }
  void setRenderEncoderOpen(bool open) {
    renderEncoderOpen_ = open;
  }

  [[nodiscard]] bool initialize(CA::MetalLayer* layer, uint32_t width, uint32_t height, const ContextConfig& cfg);

  IMetalCommandBuffer& acquireMetalCommandBuffer(bool dedicatedCompute = false) override;
  SubmitHandle submit(lvk::ICommandBuffer& commandBuffer, TextureHandle present = {}) override;
  void wait(SubmitHandle handle) override;

  Holder<BufferHandle> createBuffer(const BufferDesc& desc, const char* debugName = nullptr, Result* outResult = nullptr) override;
  Holder<SamplerHandle> createSampler(const SamplerStateDesc& desc, Result* outResult = nullptr) override;
  Holder<TextureHandle> createTexture(const TextureDesc& desc, const char* debugName = nullptr, Result* outResult = nullptr) override;
  Holder<TextureHandle> createTextureView(TextureHandle texture,
                                          const TextureViewDesc& desc,
                                          const char* debugName = nullptr,
                                          Result* outResult = nullptr) override {
    Result::setResult(outResult, Result::Code::RuntimeError, "createTextureView not implemented");
    return {};
  }
  Holder<ComputePipelineHandle> createComputePipeline(const ComputePipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<RenderPipelineHandle> createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<RayTracingPipelineHandle> createRayTracingPipeline(const RayTracingPipelineDesc& desc, Result* outResult = nullptr) override {
    Result::setResult(outResult, Result::Code::RuntimeError, "createRayTracingPipeline not implemented");
    return {};
  }
  Holder<ShaderModuleHandle> createShaderModule(const ShaderModuleDesc& desc, Result* outResult = nullptr) override;
  Holder<QueryPoolHandle> createQueryPool(uint32_t numQueries, const char* debugName, Result* outResult = nullptr) override {
    Result::setResult(outResult, Result::Code::RuntimeError, "createQueryPool not implemented");
    return {};
  }
  Holder<AccelStructHandle> createAccelerationStructure(const AccelStructDesc& desc, Result* outResult = nullptr) override {
    Result::setResult(outResult, Result::Code::RuntimeError, "createAccelerationStructure not implemented");
    return {};
  }
  Holder<ArgumentTableHandle> createArgumentTable(const ArgumentTableDesc& desc, Result* outResult = nullptr) override;

  bool startGpuCapture(const char* outputPath) override;
  void stopGpuCapture() override;

  void destroy(ComputePipelineHandle handle) override;
  void destroy(RenderPipelineHandle handle) override;
  void destroy(RayTracingPipelineHandle handle) override {}
  void destroy(ShaderModuleHandle handle) override;
  void destroy(SamplerHandle handle) override;
  void destroy(BufferHandle handle) override;
  void destroy(TextureHandle handle) override;
  void destroy(QueryPoolHandle handle) override {}
  void destroy(AccelStructHandle handle) override {}
  void destroy(Framebuffer& fb) override {}
  void destroy(ArgumentTableHandle handle) override;

  uint64_t gpuAddress(AccelStructHandle handle) const override {
    return 0;
  }
  AccelStructSizes getAccelStructSizes(const AccelStructDesc& desc, Result* outResult = nullptr) const override {
    return {};
  }

  Result upload(BufferHandle handle, const void* data, size_t size, size_t offset = 0) override;
  Result download(BufferHandle handle, void* data, size_t size, size_t offset) override;
  uint8_t* getMappedPtr(BufferHandle handle) const override;
  uint64_t gpuAddress(BufferHandle handle, size_t offset = 0) const override;
  void flushMappedMemory(BufferHandle handle, size_t offset, size_t size) const override {}
  uint32_t getMaxStorageBufferRange() const override {
    return UINT32_MAX;
  }

  Result upload(TextureHandle handle, const TextureRangeDesc& range, const void* data, uint32_t bufferRowLength = 0) override;
  Result download(TextureHandle handle, const TextureRangeDesc& range, void* outData) override;
  Dimensions getDimensions(TextureHandle handle) const override;
  float getAspectRatio(TextureHandle handle) const override;
  Format getFormat(TextureHandle handle) const override;

  TextureHandle getCurrentSwapchainTexture() override;
  Format getSwapchainFormat() const override;
  ColorSpace getSwapchainColorSpace() const override {
    return ColorSpace_SRGB_NONLINEAR;
  }
  uint32_t getSwapchainCurrentImageIndex() const override {
    return 0;
  }
  uint32_t getNumSwapchainImages() const override {
    return framesInFlight_;
  }
  void recreateSwapchain(int newWidth, int newHeight) override;
  bool setCurrentPresentMode(PresentMode mode) override {
    return true;
  }
  PresentMode getCurrentPresentMode() const override {
    return PresentMode_FIFO;
  }

  uint32_t getFramebufferMSAABitMask() const override {
    return 1u | 2u | 4u | 8u;
  }

  bool isExtensionEnabled(const char* ext) const override {
    return false;
  }
  bool supportsAsyncCompute() const override {
    return false;
  }

  double getTimestampPeriodToMs() const override {
    return 0.0;
  }
  bool getQueryPoolResults(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* outData, size_t stride)
      const override {
    return false;
  }

  MTL4::CommandBuffer* commandBuffer() const {
    return currentWrapper_ ? currentWrapper_->cmdBuf.get() : nullptr;
  }
  const MetalImage* getImage(TextureHandle handle) const {
    return textures_.get(handle);
  }
  const MetalRenderPipeline* getRenderPipeline(RenderPipelineHandle handle) const {
    return renderPipelines_.get(handle);
  }
  const MetalComputePipeline* getComputePipeline(ComputePipelineHandle handle) const {
    return computePipelines_.get(handle);
  }
  const MetalBuffer* getBuffer(BufferHandle handle) const {
    return buffers_.get(handle);
  }
  const MetalArgumentTable* getArgumentTable(ArgumentTableHandle handle) const {
    return argumentTables_.get(handle);
  }
  ArgumentTableHandle defaultArgumentTable() const {
    return defaultArgumentTable_;
  }
  MTL::GPUAddress writePushConstants(const void* data, size_t size, size_t offset);
  NS::SharedPtr<MTL::DepthStencilState> makeDepthStencilState(const DepthState& state);

 private:
  [[nodiscard]] bool createDevice();
  [[nodiscard]] bool createQueue();
  [[nodiscard]] bool createBindlessHeaps();
  void ensureTextureCapacity(uint32_t index);
  void ensureSamplerCapacity(uint32_t index);
  void ensureBufferCapacity(uint32_t index);
  void rebindArgumentTableHeaps();
  void growConstantsRing();
  void addResident(const MTL::Allocation* allocation);
  void removeResident(const MTL::Allocation* allocation);
  void flushResidency();

  NS::SharedPtr<MTL::Device> device_;
  NS::SharedPtr<MTL4::CommandQueue> commandQueue_;
  NS::SharedPtr<MTL::ResidencySet> residencySet_;
  bool residencyDirty_ = false;
  std::unique_ptr<MetalImmediateCommands> immediate_;
  std::unique_ptr<MetalStagingDevice> staging_;
  const MetalImmediateCommands::CommandBufferWrapper* currentWrapper_ = nullptr;
  bool renderEncoderOpen_ = false;

  CA::MetalLayer* metalLayer_ = nullptr;
  CA::MetalDrawable* currentDrawable_ = nullptr;
  NS::AutoreleasePool* framePool_ = nullptr;

  uint32_t framesInFlight_ = 0;

  MTL::PixelFormat swapchainFormat_ = MTL::PixelFormatBGRA8Unorm;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool vsync_ = false;

  NS::SharedPtr<MTL::Buffer> bufferHeap_;
  NS::SharedPtr<MTL::Buffer> textureHeap_;
  NS::SharedPtr<MTL::Buffer> samplerHeap_;
  uint32_t buffersCapacity_ = 0;
  uint32_t texturesCapacity_ = 0;
  uint32_t samplersCapacity_ = 0;
  uint32_t buffersCapacityMax_ = 0;
  uint32_t texturesCapacityMax_ = 0;
  uint32_t samplersCapacityMax_ = 0;
  GpuLimits limits_;

  NS::SharedPtr<MTL::Buffer> constantsRing_;
  uint32_t pushConstantsSize_ = 128;
  uint32_t pushesPerFrameCapacity_ = 256;
  uint32_t constantsFrameRegionBytes_ = 0;
  uint32_t constantsCursor_ = 0;

  struct RetiredBuffer {
    NS::SharedPtr<MTL::Buffer> buffer;
    SubmitHandle handle;
  };
  std::vector<RetiredBuffer> retiredConstantRings_;

  ldr::Pool<lvk::Buffer, MetalBuffer> buffers_;
  ldr::Pool<lvk::Texture, MetalImage> textures_;
  ldr::Pool<lvk::Sampler, MetalSampler> samplers_;
  ldr::Pool<lvk::ShaderModule, MetalShaderModule> shaderModules_;
  ldr::Pool<lvk::RenderPipeline, MetalRenderPipeline> renderPipelines_;
  ldr::Pool<lvk::ComputePipeline, MetalComputePipeline> computePipelines_;
  ldr::Pool<lvk::metal::ArgumentTable, MetalArgumentTable> argumentTables_;

  TextureHandle swapchainTextureHandle_;
  ArgumentTableHandle defaultArgumentTable_;
  CommandBuffer cmdBuffer_;
};

class MetalValidatedCommandBuffer final : public CommandBuffer {
 public:
  using CommandBuffer::CommandBuffer;

  void cmdPushConstants(const void* data, size_t size, size_t offset = 0) override;
  void cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& desc, const Dependencies& deps = {}) override;
};

class MetalValidatedContext final : public MetalContext {
 public:
  IMetalCommandBuffer& acquireMetalCommandBuffer(bool dedicatedCompute = false) override;

  Holder<TextureHandle> createTexture(const TextureDesc& desc, const char* debugName = nullptr, Result* outResult = nullptr) override;
  Holder<RenderPipelineHandle> createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult = nullptr) override;

 private:
  MetalValidatedCommandBuffer validatedCmdBuffer_{this};
};

} // namespace lvk::metal
