#pragma once

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <unordered_map>
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
  uint8_t usage = 0;
  NS::SharedPtr<MTL::IndirectCommandBuffer> icb;
  NS::SharedPtr<MTL::Buffer> icbContainer;
  uint32_t icbCapacity = 0;
  BufferHandle icbPrimitiveTypes;
  BufferHandle icbIndexBuffer;
  IndexFormat icbIndexFormat = IndexFormat_UI32;
  BufferHandle icbMeshThreadgroupSizes;
};

struct MetalImage {
  NS::SharedPtr<MTL::Texture> texture;
  NS::SharedPtr<MTL::Texture> yuvChroma;
  CA::MetalDrawable* drawable = nullptr;
  MTL::PixelFormat format = MTL::PixelFormatInvalid;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t depth = 1;
  bool isSwapchainImage = false;
};

struct MetalAccelStruct {
  NS::SharedPtr<MTL::AccelerationStructure> accel;
  NS::SharedPtr<MTL::Buffer> scratch;
  NS::SharedPtr<MTL::Buffer> instanceDescriptors;
  NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> tlasDescriptor;
  AccelStructType type = AccelStructType_Invalid;
  uint32_t numInstances = 0;
  uint64_t buildScratchSize = 0;
  bool indirectTLAS = false;
};

struct MetalSampler {
  NS::SharedPtr<MTL::SamplerState> sampler;
};

struct MetalQueryPool {
  NS::SharedPtr<MTL4::CounterHeap> heap;
  NS::SharedPtr<MTL::Buffer> resolved;
  uint32_t count = 0;
};

struct MetalShaderModule {
  NS::SharedPtr<MTL::Library> library;
  NS::SharedPtr<MTL::Function> function;
  MTL::Size threadgroupSize = MTL::Size(16, 16, 1);
  uint32_t viewCount = 1;
};

struct MetalRenderPipeline {
  NS::SharedPtr<MTL::RenderPipelineState> pipeline;
  MTL::PrimitiveType topology = MTL::PrimitiveTypeTriangle;
  MTL::CullMode cullMode = MTL::CullModeNone;
  MTL::Winding frontFace = MTL::WindingCounterClockwise;
  MTL::TriangleFillMode fillMode = MTL::TriangleFillModeFill;
  StencilState frontStencil = {};
  StencilState backStencil = {};
  bool isMesh = false;
  MTL::Size objectThreadsPerThreadgroup = MTL::Size(1, 1, 1);
  MTL::Size meshThreadsPerThreadgroup = MTL::Size(1, 1, 1);
};

struct MetalTilePipeline {
  NS::SharedPtr<MTL::RenderPipelineState> pipeline;
};

struct MetalComputePipeline {
  NS::SharedPtr<MTL::ComputePipelineState> pipeline;
  MTL::Size threadgroupSize = MTL::Size(16, 16, 1);
};

struct MetalTensor {
  NS::SharedPtr<MTL::Tensor> tensor;
  MTL::TensorDataType dataType = MTL::TensorDataTypeFloat16;
  uint32_t dimensions[TensorDesc::kMaxRank] = {};
  uint32_t rank = 0;
  size_t byteSize = 0;
};

struct MetalMachineLearningPipeline {
  NS::SharedPtr<MTL4::MachineLearningPipelineState> pipeline;
  NS::SharedPtr<MTL::Heap> intermediatesHeap;
  NS::SharedPtr<MTL::Library> library;
  NS::SharedPtr<MTL4::ArgumentTable> argTable;
  uint32_t numInputs = 0;
  uint32_t numOutputs = 0;
};

struct MetalArgumentTable {
  NS::SharedPtr<MTL4::ArgumentTable> table;
  uint32_t constantsIndex = UINT32_MAX;
  ArgumentKind kinds[ArgumentTableDesc::kMaxBindings] = {};
  uint32_t numKinds = 0;
};

struct DepthStencilStateKey {
  uint32_t depthCompareOp = 0;
  uint32_t depthWriteEnabled = 0;
  uint32_t frontStencilFailureOp = 0;
  uint32_t frontDepthFailureOp = 0;
  uint32_t frontDepthStencilPassOp = 0;
  uint32_t frontStencilCompareOp = 0;
  uint32_t frontReadMask = 0;
  uint32_t frontWriteMask = 0;
  uint32_t backStencilFailureOp = 0;
  uint32_t backDepthFailureOp = 0;
  uint32_t backDepthStencilPassOp = 0;
  uint32_t backStencilCompareOp = 0;
  uint32_t backReadMask = 0;
  uint32_t backWriteMask = 0;

  bool operator==(const DepthStencilStateKey& o) const {
    return std::memcmp(this, &o, sizeof(o)) == 0;
  }
};

struct DepthStencilStateKeyHash {
  size_t operator()(const DepthStencilStateKey& k) const {
    size_t h = 1469598103934665603ull;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&k);
    for (size_t i = 0; i < sizeof(k); ++i) {
      h = (h ^ bytes[i]) * 1099511628211ull;
    }
    return h;
  }
};

class MetalContext;
class MetalStagingDevice;

class CommandBuffer : public IMetalCommandBuffer {
 public:
  CommandBuffer() = default;
  explicit CommandBuffer(MetalContext* ctx) : ctx_(ctx) {}

  friend class MetalContext;
  friend class MetalValidatedContext;

  // --- No-op for Metal 4 or for LVK-Metal
  void cmdTransitionToGeneral(const ldr::Span<TextureHandle>& textures, lvk::ShaderStage extraDstStage) const override {}
  void cmdTransitionToShaderReadOnly(const ldr::Span<TextureHandle>& textures, lvk::ShaderStage extraDstStage) const override {}
  void cmdTransitionToRenderingLocalRead(const ldr::Span<TextureHandle>& textures) const override {}
  void cmdBindRayTracingPipeline(lvk::RayTracingPipelineHandle handle) override {}
  void cmdNextSubpass() override {}

  // --- Not supported for Metal
  void cmdDrawIndexedIndirectCount(BufferHandle indirectBuffer,
                                   size_t indirectBufferOffset,
                                   BufferHandle countBuffer,
                                   size_t countBufferOffset,
                                   uint32_t maxDrawCount,
                                   uint32_t stride = 0) override {}
  void cmdDrawMeshTasksIndirectCount(BufferHandle indirectBuffer,
                                     size_t indirectBufferOffset,
                                     BufferHandle countBuffer,
                                     size_t countBufferOffset,
                                     uint32_t maxDrawCount,
                                     uint32_t stride = 0) override {}
  // ---------------------------------------

  void cmdReleaseToAsyncCompute(const ldr::Span<TextureHandle>& textures) const override {}

  void cmdPushDebugGroupLabel(const char* label, uint32_t colorRGBA = 0xffffffff) const override;
  void cmdInsertDebugEventLabel(const char* label, uint32_t colorRGBA = 0xffffffff) const override;
  void cmdPopDebugGroupLabel() const override;

  void cmdBindComputePipeline(lvk::ComputePipelineHandle handle) override;
  void cmdDispatch(const Dimensions& groupCount, const Dependencies& deps = {}) override;
  void cmdDispatchIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset = 0, const Dependencies& deps = {}) override;

  void cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& desc, const Dependencies& deps = {}) override;
  void cmdEndRendering() override;

  void cmdBindViewport(const Viewport& viewport) override;
  void cmdBindScissorRect(const ScissorRect& rect) override;

  void cmdBindRenderPipeline(lvk::RenderPipelineHandle handle) override;
  void cmdBindDepthState(const DepthState& state) override;
  void cmdSetStencilRef(uint32_t ref) override;

  void cmdBindVertexBuffer(uint32_t index, BufferHandle buffer, uint64_t bufferOffset = 0, uint64_t bufferSize = LVK_WHOLE_SIZE) override {}
  void cmdBindIndexBuffer(BufferHandle indexBuffer,
                          IndexFormat indexFormat,
                          uint64_t bufferOffset = 0,
                          uint64_t bufferSize = LVK_WHOLE_SIZE) override;
  void cmdPushConstants(const void* data, size_t size, size_t offset = 0) override;

  void cmdCopyBuffer(BufferHandle srcBuffer, BufferHandle dstBuffer, size_t srcOffset, size_t dstOffset, size_t size) override;
  void cmdFillBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, uint32_t data) override;
  void cmdUpdateBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, const void* data) override;

  void cmdDraw(uint32_t vertexCount, uint32_t instanceCount = 1, uint32_t firstVertex = 0, uint32_t baseInstance = 0) override;
  void cmdDrawIndexed(uint32_t indexCount,
                      uint32_t instanceCount = 1,
                      uint32_t firstIndex = 0,
                      int32_t vertexOffset = 0,
                      uint32_t baseInstance = 0) override;
  void cmdDrawIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdDrawIndexedIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdDrawMeshTasks(const Dimensions& threadgroupCount) override;
  void cmdDrawMeshTasksIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdTraceRays(uint32_t width, uint32_t height, uint32_t depth = 1, const Dependencies& deps = {}) override {
    const uint32_t tx = uint32_t(computeThreadgroupSize_.width);
    const uint32_t ty = uint32_t(computeThreadgroupSize_.height);
    const uint32_t tz = uint32_t(computeThreadgroupSize_.depth);
    cmdDispatch({(width + tx - 1) / tx, (height + ty - 1) / ty, (depth + tz - 1) / tz}, deps);
  }

  void cmdSetBlendColor(const float color[4]) override;
  void cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp = 0.0f) override;
  void cmdSetDepthBiasEnable(bool enable) override;

  void cmdResetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) override;
  void cmdWriteTimestamp(QueryPoolHandle pool, uint32_t query) override;

  void cmdClearColorImage(TextureHandle tex, const ClearColorValue& value, const TextureLayers& layers = {}) override;
  void cmdCopyImage(TextureHandle src,
                    TextureHandle dst,
                    const Dimensions& extent,
                    const Offset3D& srcOffset = {},
                    const Offset3D& dstOffset = {},
                    const TextureLayers& srcLayers = {},
                    const TextureLayers& dstLayers = {}) override;
  void cmdGenerateMipmap(TextureHandle handle) override;
  void cmdUpdateTLAS(AccelStructHandle handle, BufferHandle instancesBuffer) override;
  void cmdBuildIndirectTLAS(lvk::AccelStructHandle tlas,
                            lvk::BufferHandle instanceDescriptors,
                            uint32_t instanceCount,
                            lvk::BufferHandle instanceCountBuffer = {}) override;

  void cmdBindArgumentTable(ArgumentTableHandle handle) override;
  void cmdBindTilePipeline(TilePipelineHandle pipeline) override;
  void cmdDispatchTile() override;

  void cmdBindMachineLearningPipeline(MLPipelineHandle pipeline,
                                      const TensorHandle* inputs,
                                      uint32_t numInputs,
                                      const TensorHandle* outputs,
                                      uint32_t numOutputs) override;
  void cmdDispatchNetwork() override;

 protected:
  MetalContext* context() const {
    return ctx_;
  }

 private:
  void bindArgumentTableInternal(ArgumentTableHandle handle);
  void resetArgumentTableIfOverridden();
  void setArgumentTableOnActiveEncoder(MTL4::ArgumentTable* table);
  void endComputeEncoder();
  void endMachineLearningEncoder();
  MTL4::ComputeCommandEncoder* beginTransferEncoder();
  void applyDepthStencilState();
  void applyDepthBias();
  void resolveTimestamps();

  struct TimestampSpan {
    QueryPoolHandle pool;
    uint32_t lo;
    uint32_t hi;
  };
  std::vector<TimestampSpan> timestampSpans_;

  MetalContext* ctx_ = nullptr;
  const MetalImmediateCommands::CommandBufferWrapper* wrapper_ = nullptr;
  MTL4::RenderCommandEncoder* encoder_ = nullptr;
  MTL4::ComputeCommandEncoder* computeEncoder_ = nullptr;
  MTL4::MachineLearningCommandEncoder* mlEncoder_ = nullptr;
  const MetalMachineLearningPipeline* mlPipeline_ = nullptr;
  MTL::Size computeThreadgroupSize_ = MTL::Size(16, 16, 1);
  MTL::Size meshObjectThreadsPerThreadgroup_ = MTL::Size(1, 1, 1);
  MTL::Size meshThreadsPerThreadgroup_ = MTL::Size(1, 1, 1);
  bool currentRenderPipelineIsMesh_ = false;
  MTL::PrimitiveType topology_ = MTL::PrimitiveTypeTriangle;
  ArgumentTableHandle currentArgTable_;
  bool argTableOverridden_ = false;
  bool isRendering_ = false;
  BufferHandle boundIndexBuffer_;
  MTL::IndexType boundIndexType_ = MTL::IndexTypeUInt32;
  uint64_t boundIndexOffset_ = 0;
  DepthState depthState_ = {};
  StencilState frontStencil_ = {};
  StencilState backStencil_ = {};
  bool depthStencilDirty_ = true;
  MTL::DepthStencilState* lastDepthStencilState_ = nullptr;
  uint32_t stencilRef_ = 0;
  float depthBiasConstantFactor_ = 0.0f;
  float depthBiasSlopeFactor_ = 0.0f;
  float depthBiasClamp_ = 0.0f;
  bool depthBiasEnabled_ = false;
};

class MetalContext : public IMetalContext {
 public:
  MetalContext() = default;
  ~MetalContext() override;

  friend class MetalStagingDevice;

  // --- No-op for Metal ---
  void flushMappedMemory(BufferHandle handle, size_t offset, size_t size) const override {}

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

  Holder<RayTracingPipelineHandle> createRayTracingPipeline(const RayTracingPipelineDesc& desc, Result* outResult = nullptr) override {
    return {};
  }
  void destroy(RayTracingPipelineHandle handle) override {}
  // -----------------------

  uint32_t pushConstantsSize() const {
    return pushConstantsSize_;
  }
  uint8_t* pushConstantsShadow() {
    return pushConstantsShadow_.data();
  }
  const GpuLimits& limits() const {
    return limits_;
  }
  void setRenderEncoderOpen(bool open) {
    renderEncoderOpen_ = open;
  }
  void setPendingMLBarrier(bool pending) {
    pendingMLBarrier_ = pending;
  }
  bool takePendingMLBarrier() {
    const bool pending = pendingMLBarrier_;
    pendingMLBarrier_ = false;
    return pending;
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
                                          Result* outResult = nullptr) override;
  Holder<ComputePipelineHandle> createComputePipeline(const ComputePipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<RenderPipelineHandle> createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<TilePipelineHandle> createTileRenderPipeline(const TileRenderPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<TensorHandle> createTensor(const TensorDesc& desc, Result* outResult = nullptr) override;
  Holder<MLPipelineHandle> createMachineLearningPipeline(const MachineLearningPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<ShaderModuleHandle> createShaderModule(const ShaderModuleDesc& desc, Result* outResult = nullptr) override;
  Holder<QueryPoolHandle> createQueryPool(uint32_t numQueries, const char* debugName, Result* outResult = nullptr) override;
  Holder<AccelStructHandle> createAccelerationStructure(const AccelStructDesc& desc, Result* outResult = nullptr) override;
  Holder<ArgumentTableHandle> createArgumentTable(const ArgumentTableDesc& desc, Result* outResult = nullptr) override;

  bool startGpuCapture(const char* outputPath) override;
  void stopGpuCapture() override;

  void setShaderModuleMetadata(ShaderModuleHandle handle, const ShaderModuleMetadata& metadata) override;
  void setIndirectBufferMetadata(BufferHandle indirectBuffer, const IndirectBufferMetadata& metadata) override;

  uint32_t indirectTLASInstanceDescriptorSize() const override;

  void destroy(ComputePipelineHandle handle) override;
  void destroy(RenderPipelineHandle handle) override;
  void destroy(ShaderModuleHandle handle) override;
  void destroy(SamplerHandle handle) override;
  void destroy(BufferHandle handle) override;
  void destroy(TextureHandle handle) override;
  void destroy(QueryPoolHandle handle) override;
  void destroy(AccelStructHandle handle) override;
  void destroy(Framebuffer& fb) override {}
  void destroy(ArgumentTableHandle handle) override;
  void destroy(TilePipelineHandle handle) override;
  void destroy(TensorHandle handle) override;
  void destroy(MLPipelineHandle handle) override;

  uint64_t gpuAddress(AccelStructHandle handle) const override;
  AccelStructSizes getAccelStructSizes(const AccelStructDesc& desc, Result* outResult = nullptr) const override;

  Result upload(BufferHandle handle, const void* data, size_t size, size_t offset = 0) override;
  Result download(BufferHandle handle, void* data, size_t size, size_t offset) override;
  uint8_t* getMappedPtr(BufferHandle handle) const override;
  uint64_t gpuAddress(BufferHandle handle, size_t offset = 0) const override;
  uint32_t getMaxStorageBufferRange() const override {
    const uint64_t maxLen = device_ ? uint64_t(device_->maxBufferLength()) : 0;
    return maxLen > UINT32_MAX ? UINT32_MAX : uint32_t(maxLen);
  }

  Result upload(TextureHandle handle, const TextureRangeDesc& range, const void* data, uint32_t bufferRowLength = 0) override;
  Result download(TextureHandle handle, const TextureRangeDesc& range, void* outData) override;

  Result upload(TensorHandle handle, const void* data, size_t size) override;
  Result download(TensorHandle handle, void* data, size_t size) override;
  Dimensions getDimensions(TextureHandle handle) const override;
  float getAspectRatio(TextureHandle handle) const override;
  Format getFormat(TextureHandle handle) const override;

  TextureHandle getCurrentSwapchainTexture() override;
  Format getSwapchainFormat() const override;
  ColorSpace getSwapchainColorSpace() const override {
    return swapchainColorSpace_;
  }
  uint32_t getSwapchainCurrentImageIndex() const override {
    return currentImageIndex_;
  }
  uint32_t getNumSwapchainImages() const override {
    return framesInFlight_;
  }
  void recreateSwapchain(int newWidth, int newHeight) override;

  bool supportsAsyncCompute() const override {
    return false;
  }

  double getTimestampPeriodToMs() const override {
    const uint64_t freq = timestampFrequency_;
    return freq ? 1000.0 / double(freq) : 0.0;
  }
  bool getQueryPoolResults(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* outData, size_t stride)
      const override;

  MTL4::CounterHeap* getQueryHeap(QueryPoolHandle handle) const {
    const MetalQueryPool* qp = queryPools_.get(handle);
    return qp ? qp->heap.get() : nullptr;
  }

  const MetalQueryPool* getQueryPool(QueryPoolHandle handle) const {
    return queryPools_.get(handle);
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
  const MetalTilePipeline* getTilePipeline(TilePipelineHandle handle) const {
    return tilePipelines_.get(handle);
  }
  const MetalTensor* getTensor(TensorHandle handle) const {
    return tensors_.get(handle);
  }
  const MetalMachineLearningPipeline* getMachineLearningPipeline(MLPipelineHandle handle) const {
    return mlPipelines_.get(handle);
  }
  const MetalAccelStruct* getAccelStruct(AccelStructHandle handle) const {
    return accelStructs_.get(handle);
  }
  void buildAccelStructImmediate(MTL::AccelerationStructure* accel,
                                 const MTL4::AccelerationStructureDescriptor* desc,
                                 MTL::Buffer* scratch);
  ArgumentTableHandle defaultArgumentTable() const {
    return defaultArgumentTable_;
  }
  MTL::GPUAddress writePushConstants(const void* data, size_t size, const MetalImmediateCommands::CommandBufferWrapper* wrapper);
  MTL::DepthStencilState* getDepthStencilState(const DepthState& depth, const StencilState& front, const StencilState& back);

  struct StagingAlloc {
    MTL::Buffer* buffer = nullptr;
    uint32_t offset = 0;
  };
  StagingAlloc writeUploadStaging(const void* data, size_t size, const MetalImmediateCommands::CommandBufferWrapper* wrapper);

  void encodeIndirectDraws(const Dependencies& deps, const MetalImmediateCommands::CommandBufferWrapper* wrapper);
  void encodeBufferFill(MTL4::ComputeCommandEncoder* enc,
                        const MetalBuffer* b,
                        size_t offset,
                        size_t size,
                        uint32_t data,
                        const MetalImmediateCommands::CommandBufferWrapper* wrapper);

 protected:
  const MetalImmediateCommands::CommandBufferWrapper* beginCommandBuffer();
  virtual CommandBuffer* createCommandBuffer() {
    return new CommandBuffer(this);
  }

  CommandBuffer* commandBuffers_[MetalImmediateCommands::kMaxCommandBuffers] = {};

 private:
  bool ensureIndirectEncoder();
  bool ensureFillPipeline();
  void createIndirectCommandBufferFor(MetalBuffer& mb, uint32_t elementStride);
  void growUploadRing(uint32_t minRegionBytes, const MetalImmediateCommands::CommandBufferWrapper* wrapper);
  void generateMipmapImmediate(MTL::Texture* texture);
  NS::SharedPtr<MTL::DepthStencilState> makeDepthStencilState(const DepthState& depth, const StencilState& front, const StencilState& back);
  NS::SharedPtr<MTL::Function> specializeFunction(const MetalShaderModule* sm, const SpecializationConstantDesc& spec);

  [[nodiscard]] bool createDevice();
  [[nodiscard]] bool createQueue();
  [[nodiscard]] bool createBindlessHeaps();
  void ensureTextureCapacity(uint32_t index);
  void ensureSamplerCapacity(uint32_t index);
  void ensureBufferCapacity(uint32_t index);
  void ensureAccelStructCapacity(uint32_t index);
  void rebindArgumentTableHeaps();
  void growConstantsRing(const MetalImmediateCommands::CommandBufferWrapper* wrapper);
  void addResident(const MTL::Allocation* allocation);
  void removeResident(const MTL::Allocation* allocation);
  void retireResident(NS::SharedPtr<MTL::Buffer> buffer);
  void flushResidency();
  void deferredTask(std::function<void()>&& task);
  void processDeferredTasks();
  void waitDeferredTasks();

  NS::SharedPtr<MTL::Device> device_;
  NS::SharedPtr<MTL4::CommandQueue> commandQueue_;
  NS::SharedPtr<MTL4::Compiler> compiler_;
  NS::SharedPtr<MTL::ResidencySet> residencySet_;
  bool residencyDirty_ = false;
  std::unique_ptr<MetalImmediateCommands> immediate_;
  std::unique_ptr<MetalStagingDevice> staging_;
  bool renderEncoderOpen_ = false;
  bool pendingMLBarrier_ = false;

  CA::MetalLayer* metalLayer_ = nullptr;
  NS::AutoreleasePool* autoreleasePool_ = nullptr;

  uint32_t framesInFlight_ = 0;
  uint32_t currentImageIndex_ = 0;

  MTL::PixelFormat swapchainFormat_ = MTL::PixelFormatBGRA8Unorm;
  ColorSpace swapchainColorSpace_ = ColorSpace_SRGB_NONLINEAR;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  bool vsync_ = false;

  NS::SharedPtr<MTL::Buffer> bufferHeap_;
  NS::SharedPtr<MTL::Buffer> textureHeap_;
  NS::SharedPtr<MTL::Buffer> yuvChromaHeap_;
  NS::SharedPtr<MTL::Buffer> samplerHeap_;
  NS::SharedPtr<MTL::Buffer> accelStructHeap_;
  uint32_t buffersCapacity_ = 0;
  uint32_t texturesCapacity_ = 0;
  uint32_t samplersCapacity_ = 0;
  uint32_t accelStructsCapacity_ = 256;
  uint32_t buffersCapacityMax_ = 0;
  uint32_t texturesCapacityMax_ = 0;
  uint32_t samplersCapacityMax_ = 0;
  uint32_t accelStructsCapacityMax_ = 65536;
  uint64_t timestampFrequency_ = 0;
  GpuLimits limits_;

  NS::SharedPtr<MTL::Buffer> constantsRing_;
  uint32_t pushConstantsSize_ = 128;
  uint32_t pushesPerFrameCapacity_ = 256;
  uint32_t constantsFrameRegionBytes_ = 0;
  uint32_t constantsCursor_ = 0;
  std::vector<uint8_t> pushConstantsShadow_;

  NS::SharedPtr<MTL::Buffer> uploadRing_;
  uint32_t uploadRingFrameRegionBytes_ = 0;
  uint32_t uploadRingCursor_ = 0;

  NS::SharedPtr<MTL::ComputePipelineState> icbEncodePipeline_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodePipelineTyped_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodeIndexed16_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodeIndexed16Typed_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodeIndexed32_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodeIndexed32Typed_;
  NS::SharedPtr<MTL::ComputePipelineState> icbEncodeMesh_;
  NS::SharedPtr<MTL4::ArgumentTable> icbEncodeArgTable_;

  NS::SharedPtr<MTL::ComputePipelineState> fillPipeline_;
  NS::SharedPtr<MTL4::ArgumentTable> fillArgTable_;

  struct DeferredTask {
    std::function<void()> task_;
    SubmitHandle handle_;
  };
  std::vector<DeferredTask> deferredTasks_;

  std::unordered_map<DepthStencilStateKey, NS::SharedPtr<MTL::DepthStencilState>, DepthStencilStateKeyHash> depthStencilCache_;

  ldr::Pool<lvk::Buffer, MetalBuffer> buffers_;
  ldr::Pool<lvk::Texture, MetalImage> textures_;
  ldr::Pool<lvk::Sampler, MetalSampler> samplers_;
  ldr::Pool<lvk::ShaderModule, MetalShaderModule> shaderModules_;
  ldr::Pool<lvk::RenderPipeline, MetalRenderPipeline> renderPipelines_;
  ldr::Pool<lvk::ComputePipeline, MetalComputePipeline> computePipelines_;
  ldr::Pool<lvk::metal::ArgumentTable, MetalArgumentTable> argumentTables_;
  ldr::Pool<lvk::metal::TilePipeline, MetalTilePipeline> tilePipelines_;
  ldr::Pool<lvk::metal::Tensor, MetalTensor> tensors_;
  ldr::Pool<lvk::metal::MLPipeline, MetalMachineLearningPipeline> mlPipelines_;
  ldr::Pool<lvk::AccelerationStructure, MetalAccelStruct> accelStructs_;
  ldr::Pool<lvk::QueryPool, MetalQueryPool> queryPools_;

  TextureHandle swapchainTextureHandle_;
  ArgumentTableHandle defaultArgumentTable_;
};

class MetalValidatedCommandBuffer final : public CommandBuffer {
 public:
  using CommandBuffer::CommandBuffer;

  void cmdPushConstants(const void* data, size_t size, size_t offset = 0) override;
  void cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& desc, const Dependencies& deps = {}) override;
  void cmdDrawIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdDrawIndexedIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdDrawMeshTasksIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride = 0) override;
  void cmdDrawIndexedIndirectCount(BufferHandle indirectBuffer,
                                   size_t indirectBufferOffset,
                                   BufferHandle countBuffer,
                                   size_t countBufferOffset,
                                   uint32_t maxDrawCount,
                                   uint32_t stride = 0) override;
  void cmdDrawMeshTasksIndirectCount(BufferHandle indirectBuffer,
                                     size_t indirectBufferOffset,
                                     BufferHandle countBuffer,
                                     size_t countBufferOffset,
                                     uint32_t maxDrawCount,
                                     uint32_t stride = 0) override;
  void cmdBuildIndirectTLAS(lvk::AccelStructHandle tlas,
                            lvk::BufferHandle instanceDescriptors,
                            uint32_t instanceCount,
                            lvk::BufferHandle instanceCountBuffer = {}) override;
  void cmdUpdateTLAS(AccelStructHandle handle, BufferHandle instancesBuffer) override;
};

class MetalValidatedContext final : public MetalContext {
 public:
  IMetalCommandBuffer& acquireMetalCommandBuffer(bool dedicatedCompute = false) override;

  Holder<TextureHandle> createTexture(const TextureDesc& desc, const char* debugName = nullptr, Result* outResult = nullptr) override;
  Holder<RenderPipelineHandle> createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<ComputePipelineHandle> createComputePipeline(const ComputePipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<TilePipelineHandle> createTileRenderPipeline(const TileRenderPipelineDesc& desc, Result* outResult = nullptr) override;
  Holder<QueryPoolHandle> createQueryPool(uint32_t numQueries, const char* debugName, Result* outResult = nullptr) override;

 protected:
  CommandBuffer* createCommandBuffer() override {
    return new MetalValidatedCommandBuffer(this);
  }
};

} // namespace lvk::metal
