#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "lvk/metal/MetalClasses.h"
#include "lvk/metal/MetalLimits.h"
#include "lvk/metal/MetalMappings.h"
#include "lvk/metal/MetalStagingDevice.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dispatch/dispatch.h>

#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>

namespace lvk::metal {

namespace {

struct SwapchainColorSpaceInfo {
  MTL::PixelFormat format;
  CFStringRef cgColorSpaceName;
  bool wantsEDR;
};

SwapchainColorSpaceInfo swapchainColorSpaceInfo(ColorSpace colorSpace, MTL::PixelFormat sdrFormat) {
  switch (colorSpace) {
  case ColorSpace_SRGB_EXTENDED_LINEAR:
    return {MTL::PixelFormatRGBA16Float, kCGColorSpaceExtendedLinearSRGB, true};
  case ColorSpace_HDR10:
    return {MTL::PixelFormatRGB10A2Unorm, kCGColorSpaceITUR_2100_PQ, true};
  case ColorSpace_BT709_LINEAR:
    return {sdrFormat, kCGColorSpaceITUR_709, false};
  case ColorSpace_SRGB_NONLINEAR:
  default:
    return {sdrFormat, kCGColorSpaceSRGB, false};
  }
}

MTL::LoadAction toLoadAction(lvk::LoadOp op) {
  switch (op) {
  case lvk::LoadOp_Clear:
    return MTL::LoadActionClear;
  case lvk::LoadOp_Load:
    return MTL::LoadActionLoad;
  default:
    return MTL::LoadActionDontCare;
  }
}

MTL::StoreAction toStoreAction(lvk::StoreOp op) {
  switch (op) {
  case lvk::StoreOp_Store:
    return MTL::StoreActionStore;
  default:
    return MTL::StoreActionDontCare;
  }
}

MTL::StoreAction toStoreAction(lvk::StoreOp op, bool resolve) {
  if (resolve)
    return op == lvk::StoreOp_Store ? MTL::StoreActionStoreAndMultisampleResolve : MTL::StoreActionMultisampleResolve;
  return toStoreAction(op);
}

MTL::MultisampleDepthResolveFilter toMTLDepthResolveFilter(lvk::ResolveMode mode) {
  switch (mode) {
  case lvk::ResolveMode_Min:
    return MTL::MultisampleDepthResolveFilterMin;
  case lvk::ResolveMode_Max:
    return MTL::MultisampleDepthResolveFilterMax;
  default:
    return MTL::MultisampleDepthResolveFilterSample0;
  }
}

bool formatHasStencil(MTL::PixelFormat f) {
  switch (f) {
  case MTL::PixelFormatStencil8:
  case MTL::PixelFormatDepth24Unorm_Stencil8:
  case MTL::PixelFormatDepth32Float_Stencil8:
  case MTL::PixelFormatX24_Stencil8:
  case MTL::PixelFormatX32_Stencil8:
    return true;
  default:
    return false;
  }
}

MTL::PixelFormat toSrgb(MTL::PixelFormat f) {
  switch (f) {
  case MTL::PixelFormatBGRA8Unorm:
    return MTL::PixelFormatBGRA8Unorm_sRGB;
  case MTL::PixelFormatRGBA8Unorm:
    return MTL::PixelFormatRGBA8Unorm_sRGB;
  case MTL::PixelFormatBGRA8Unorm_sRGB:
  case MTL::PixelFormatRGBA8Unorm_sRGB:
    return f;
  default:
    return MTL::PixelFormatInvalid;
  }
}

} // namespace

MetalContext::~MetalContext() {
  if (immediate_)
    immediate_->waitAll();
  waitDeferredTasks();
  if (MetalImage* img = swapchainTextureHandle_.empty() ? nullptr : textures_.get(swapchainTextureHandle_); img && img->drawable) {
    img->drawable->release();
    img->drawable = nullptr;
  }
  if (metalLayer_)
    metalLayer_->release();
}

bool MetalContext::initialize(CA::MetalLayer* layer, uint32_t width, uint32_t height, const ContextConfig& cfg) {
  LVK_ASSERT(layer);
  LVK_ASSERT(cfg.framesInFlight > 0);
  LVK_ASSERT(width > 0 && height > 0);

  framesInFlight_ = cfg.framesInFlight;
  width_ = width;
  height_ = height;
  vsync_ = cfg.vsync;
  swapchainFormat_ = toMTLPixelFormat(cfg.swapchainFormat);
  swapchainColorSpace_ = cfg.swapchainRequestedColorSpace;
  texturesCapacity_ = cfg.initialTexturesPoolSize ? cfg.initialTexturesPoolSize : 1;
  samplersCapacity_ = cfg.initialSamplesPoolSize ? cfg.initialSamplesPoolSize : 1;
  buffersCapacity_ = cfg.initialBuffersPoolSize ? cfg.initialBuffersPoolSize : 1;
  pushConstantsSize_ = cfg.pushConstantsSize;
  pushesPerFrameCapacity_ = cfg.initialPushConstantsPerFrameCount ? cfg.initialPushConstantsPerFrameCount : 1;

  if (!createDevice())
    return false;

  limits_ = limitsForFamily(detectGpuFamily(device_.get()));
  timestampFrequency_ = device_->queryTimestampFrequency();
  const GpuLimits& limits = limits_;
  texturesCapacityMax_ = limits.maxTexturesInArgumentBuffer;
  buffersCapacityMax_ = limits.maxBuffersInArgumentBuffer;
  samplersCapacityMax_ = limits.maxSamplersInArgumentBuffer;
  if (texturesCapacity_ > texturesCapacityMax_)
    texturesCapacity_ = texturesCapacityMax_;
  if (buffersCapacity_ > buffersCapacityMax_)
    buffersCapacity_ = buffersCapacityMax_;
  if (samplersCapacity_ > samplersCapacityMax_)
    samplersCapacity_ = samplersCapacityMax_;

  if (cfg.gammaCorrection) {
    const MTL::PixelFormat srgb = toSrgb(swapchainFormat_);
    if (srgb == MTL::PixelFormatInvalid)
      LLOGE("gammaCorrection requested but format %u has no sRGB variant", (uint32_t)swapchainFormat_);
    else
      swapchainFormat_ = srgb;
  }

  const SwapchainColorSpaceInfo colorSpaceInfo = swapchainColorSpaceInfo(swapchainColorSpace_, swapchainFormat_);
  swapchainFormat_ = colorSpaceInfo.format;

  metalLayer_ = layer;
  metalLayer_->retain();
  metalLayer_->setDevice(device_.get());
  metalLayer_->setPixelFormat(swapchainFormat_);
  if (swapchainColorSpace_ != ColorSpace_SRGB_NONLINEAR) {
    if (CGColorSpaceRef cs = CGColorSpaceCreateWithName(colorSpaceInfo.cgColorSpaceName)) {
      metalLayer_->setColorspace(cs);
      CGColorSpaceRelease(cs);
    }
    using SetBoolMsg = void (*)(void*, SEL, BOOL);
    reinterpret_cast<SetBoolMsg>(objc_msgSend)(
        metalLayer_, sel_registerName("setWantsExtendedDynamicRangeContent:"), colorSpaceInfo.wantsEDR ? YES : NO);
  }
  metalLayer_->setFramebufferOnly(!cfg.headless);
  metalLayer_->setDrawableSize(CGSizeMake(width_, height_));
  metalLayer_->setDisplaySyncEnabled(vsync_);
  metalLayer_->setMaximumDrawableCount(framesInFlight_);

  if (!createQueue())
    return false;

  {
    NS::SharedPtr<MTL4::CompilerDescriptor> compilerDesc = ns::make<MTL4::CompilerDescriptor>();
    ns::setLabel(compilerDesc.get(), "lvk-metal.compiler");
    NS::Error* compilerError = nullptr;
    compiler_ = NS::TransferPtr(device_->newCompiler(compilerDesc.get(), &compilerError));
    if (!compiler_)
      LLOGW("failed to create MTL4 compiler (machine-learning pipelines unavailable): %s",
            compilerError ? compilerError->localizedDescription()->utf8String() : "unknown");
  }

  NS::SharedPtr<MTL::ResidencySetDescriptor> rsd = ns::make<MTL::ResidencySetDescriptor>();
  ns::setLabel(rsd.get(), "lvk-metal.residency");
  NS::Error* rsError = nullptr;
  residencySet_ = NS::TransferPtr(device_->newResidencySet(rsd.get(), &rsError));
  if (!LVK_VERIFY(residencySet_)) {
    LLOGE("failed to create residency set: %s", rsError ? rsError->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  commandQueue_->addResidencySet(residencySet_.get());

  immediate_ = std::make_unique<MetalImmediateCommands>(device_.get(), commandQueue_.get(), "lvk-metal.immediate");
  staging_ = std::make_unique<MetalStagingDevice>(*this);
  if (!createBindlessHeaps())
    return false;

  ArgumentTableDesc defaultDesc;
  defaultDesc.numKinds = 13;
  defaultDesc.kinds[0] = ArgumentKind::Constants;
  defaultDesc.kinds[1] = ArgumentKind::Buffers;
  defaultDesc.kinds[2] = ArgumentKind::Textures2D;
  defaultDesc.kinds[3] = ArgumentKind::Textures3D;
  defaultDesc.kinds[4] = ArgumentKind::TexturesCube;
  defaultDesc.kinds[5] = ArgumentKind::TexturesDepth2D;
  defaultDesc.kinds[6] = ArgumentKind::Samplers;
  defaultDesc.kinds[7] = ArgumentKind::SamplersComparison;
  defaultDesc.kinds[8] = ArgumentKind::Images2D;
  defaultDesc.kinds[9] = ArgumentKind::Images3D;
  defaultDesc.kinds[10] = ArgumentKind::AccelStructs;
  defaultDesc.kinds[11] = ArgumentKind::TexturesYUVChroma;
  defaultDesc.kinds[12] = ArgumentKind::Images3DUint;
  defaultDesc.debugName = "lvk-metal.default-argtable";
  defaultArgumentTable_ = createArgumentTable(defaultDesc, nullptr).release();

  MetalImage swap;
  swap.isSwapchainImage = true;
  swap.format = swapchainFormat_;
  swap.width = width_;
  swap.height = height_;
  swapchainTextureHandle_ = textures_.create(std::move(swap));

  LLOGL("Metal device: %s\n", device_->name()->utf8String());
  return true;
}

bool MetalContext::createDevice() {
  device_ = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
  if (!LVK_VERIFY(device_))
    return false;
  if (!device_->supportsFamily(MTL::GPUFamilyMetal4)) {
    LLOGE("the selected Metal device does not support the Metal 4 GPU family");
    return false;
  }
  return true;
}

bool MetalContext::createQueue() {
  NS::SharedPtr<MTL4::CommandQueueDescriptor> queueDesc = ns::make<MTL4::CommandQueueDescriptor>();
  ns::setLabel(queueDesc.get(), "lvk-metal.queue");

  NS::Error* error = nullptr;
  commandQueue_ = NS::TransferPtr(device_->newMTL4CommandQueue(queueDesc.get(), &error));
  if (!commandQueue_) {
    LLOGE("failed to create MTL4 command queue: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  return true;
}

bool MetalContext::createBindlessHeaps() {
  bufferHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(buffersCapacity_) * sizeof(MTL::GPUAddress), MTL::ResourceStorageModeShared));
  textureHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(texturesCapacity_) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  yuvChromaHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(texturesCapacity_) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  samplerHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(samplersCapacity_) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  accelStructHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(accelStructsCapacity_) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  if (!LVK_VERIFY(bufferHeap_ && textureHeap_ && yuvChromaHeap_ && samplerHeap_ && accelStructHeap_))
    return false;
  ns::setLabel(bufferHeap_.get(), "lvk-metal.bindless.buffers");
  ns::setLabel(textureHeap_.get(), "lvk-metal.bindless.textures");
  ns::setLabel(yuvChromaHeap_.get(), "lvk-metal.bindless.yuvchroma");
  ns::setLabel(samplerHeap_.get(), "lvk-metal.bindless.samplers");
  ns::setLabel(accelStructHeap_.get(), "lvk-metal.bindless.accelstructs");

  constantsFrameRegionBytes_ = pushesPerFrameCapacity_ * pushConstantsSize_;
  constantsRing_ = NS::TransferPtr(device_->newBuffer(NS::UInteger(MetalImmediateCommands::kMaxCommandBuffers) * constantsFrameRegionBytes_,
                                                      MTL::ResourceStorageModeShared));
  if (!LVK_VERIFY(constantsRing_))
    return false;
  ns::setLabel(constantsRing_.get(), "lvk-metal.pushconstants.ring");

  addResident(bufferHeap_.get());
  addResident(textureHeap_.get());
  addResident(yuvChromaHeap_.get());
  addResident(samplerHeap_.get());
  addResident(accelStructHeap_.get());
  addResident(constantsRing_.get());
  return true;
}

void MetalContext::addResident(const MTL::Allocation* allocation) {
  if (residencySet_ && allocation) {
    residencySet_->addAllocation(allocation);
    residencyDirty_ = true;
  }
}

void MetalContext::removeResident(const MTL::Allocation* allocation) {
  if (residencySet_ && allocation) {
    residencySet_->removeAllocation(allocation);
    residencyDirty_ = true;
  }
}

void MetalContext::flushResidency() {
  if (residencyDirty_) {
    residencySet_->commit();
    residencySet_->requestResidency();
    residencyDirty_ = false;
  }
}

void MetalContext::deferredTask(std::function<void()>&& task) {
  deferredTasks_.push_back({std::move(task), SubmitHandle()});
}

void MetalContext::processDeferredTasks() {
  std::vector<DeferredTask>::iterator it = deferredTasks_.begin();
  while (it != deferredTasks_.end() && !it->handle_.empty() && immediate_->isReady(it->handle_)) {
    it->task_();
    ++it;
  }
  deferredTasks_.erase(deferredTasks_.begin(), it);
}

void MetalContext::waitDeferredTasks() {
  for (DeferredTask& t : deferredTasks_) {
    immediate_->wait(t.handle_);
    t.task_();
  }
  deferredTasks_.clear();
}

void MetalContext::growUploadRing(uint32_t minRegionBytes, const MetalImmediateCommands::CommandBufferWrapper* wrapper) {
  static constexpr uint32_t kInitialRegionBytes = 256 * 1024;
  uint32_t newRegionBytes = uploadRingFrameRegionBytes_ ? uploadRingFrameRegionBytes_ * 2 : kInitialRegionBytes;
  while (newRegionBytes < minRegionBytes)
    newRegionBytes *= 2;
  NS::SharedPtr<MTL::Buffer> nb = NS::TransferPtr(
      device_->newBuffer(NS::UInteger(MetalImmediateCommands::kMaxCommandBuffers) * newRegionBytes, MTL::ResourceStorageModeShared));
  ns::setLabel(nb.get(), "lvk-metal.upload.ring");
  if (uploadRing_)
    retiredUploadRings_.push_back({uploadRing_, wrapper->handle});
  uploadRing_ = nb;
  addResident(uploadRing_.get());
  uploadRingFrameRegionBytes_ = newRegionBytes;
  uploadRingCursor_ = wrapper->bufferIndex * newRegionBytes;
}

MetalContext::StagingAlloc MetalContext::writeUploadStaging(const void* data,
                                                            size_t size,
                                                            const MetalImmediateCommands::CommandBufferWrapper* wrapper) {
  LVK_ASSERT(wrapper);
  const uint32_t alignedSize = uint32_t((size + 15) & ~size_t(15));
  const uint32_t regionEnd = (wrapper->bufferIndex + 1) * uploadRingFrameRegionBytes_;
  if (!uploadRing_ || uploadRingCursor_ + alignedSize > regionEnd)
    growUploadRing(alignedSize, wrapper);
  const uint32_t slot = uploadRingCursor_;
  std::memcpy(static_cast<uint8_t*>(uploadRing_->contents()) + slot, data, size);
  uploadRingCursor_ += alignedSize;
  return {uploadRing_.get(), slot};
}

void MetalContext::generateMipmapImmediate(MTL::Texture* texture) {
  flushResidency();
  const MetalImmediateCommands::CommandBufferWrapper& wrapper = immediate_->acquire();
  MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
  enc->barrierAfterQueueStages(MTL::StageBlit | MTL::StageDispatch, MTL::StageBlit, MTL4::VisibilityOptionDevice);
  enc->generateMipmaps(texture);
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
  immediate_->submit(wrapper);
}

void MetalContext::rebindArgumentTableHeaps() {
  for (MetalArgumentTable& at : argumentTables_.objects_) {
    if (!at.table)
      continue;
    for (uint32_t i = 0; i < at.numKinds; ++i) {
      switch (at.kinds[i]) {
      case ArgumentKind::Buffers:
        at.table->setAddress(bufferHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::Textures2D:
      case ArgumentKind::Textures3D:
      case ArgumentKind::TexturesCube:
      case ArgumentKind::Images2D:
      case ArgumentKind::Images3D:
      case ArgumentKind::Images3DUint:
      case ArgumentKind::TexturesDepth2D:
        at.table->setAddress(textureHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::TexturesYUVChroma:
        at.table->setAddress(yuvChromaHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::Samplers:
      case ArgumentKind::SamplersComparison:
        at.table->setAddress(samplerHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::AccelStructs:
        at.table->setAddress(accelStructHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::Constants:
        break;
      }
    }
  }
}

void MetalContext::ensureTextureCapacity(uint32_t index) {
  if (index < texturesCapacity_)
    return;
  uint32_t newCap = texturesCapacity_;
  while (newCap <= index && newCap < texturesCapacityMax_)
    newCap *= 2;
  if (newCap > texturesCapacityMax_)
    newCap = texturesCapacityMax_;
  LVK_ASSERT_MSG(index < newCap, "texture bindless pool exhausted (max %u)", texturesCapacityMax_);
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), textureHeap_->contents(), NS::UInteger(texturesCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nb.get(), "lvk-metal.bindless.textures");
  NS::SharedPtr<MTL::Buffer> nbChroma =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nbChroma->contents(), yuvChromaHeap_->contents(), NS::UInteger(texturesCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nbChroma.get(), "lvk-metal.bindless.yuvchroma");
  removeResident(textureHeap_.get());
  removeResident(yuvChromaHeap_.get());
  textureHeap_ = nb;
  yuvChromaHeap_ = nbChroma;
  texturesCapacity_ = newCap;
  addResident(textureHeap_.get());
  addResident(yuvChromaHeap_.get());
  rebindArgumentTableHeaps();
}

void MetalContext::ensureSamplerCapacity(uint32_t index) {
  if (index < samplersCapacity_)
    return;
  uint32_t newCap = samplersCapacity_;
  while (newCap <= index && newCap < samplersCapacityMax_)
    newCap *= 2;
  if (newCap > samplersCapacityMax_)
    newCap = samplersCapacityMax_;
  LVK_ASSERT_MSG(index < newCap, "sampler bindless pool exhausted (max %u)", samplersCapacityMax_);
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), samplerHeap_->contents(), NS::UInteger(samplersCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nb.get(), "lvk-metal.bindless.samplers");
  removeResident(samplerHeap_.get());
  samplerHeap_ = nb;
  samplersCapacity_ = newCap;
  addResident(samplerHeap_.get());
  rebindArgumentTableHeaps();
}

void MetalContext::ensureBufferCapacity(uint32_t index) {
  if (index < buffersCapacity_)
    return;
  uint32_t newCap = buffersCapacity_;
  while (newCap <= index && newCap < buffersCapacityMax_) {
    const uint32_t doubled = newCap * 2;
    if (doubled <= newCap) {
      newCap = buffersCapacityMax_;
      break;
    }
    newCap = doubled;
  }
  if (newCap > buffersCapacityMax_)
    newCap = buffersCapacityMax_;
  LVK_ASSERT_MSG(index < newCap, "buffer bindless pool exhausted (max %u)", buffersCapacityMax_);
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::GPUAddress), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), bufferHeap_->contents(), NS::UInteger(buffersCapacity_) * sizeof(MTL::GPUAddress));
  ns::setLabel(nb.get(), "lvk-metal.bindless.buffers");
  removeResident(bufferHeap_.get());
  bufferHeap_ = nb;
  buffersCapacity_ = newCap;
  addResident(bufferHeap_.get());
  rebindArgumentTableHeaps();
}

void MetalContext::ensureAccelStructCapacity(uint32_t index) {
  if (index < accelStructsCapacity_)
    return;
  uint32_t newCap = accelStructsCapacity_;
  while (newCap <= index && newCap < accelStructsCapacityMax_)
    newCap *= 2;
  if (newCap > accelStructsCapacityMax_)
    newCap = accelStructsCapacityMax_;
  LVK_ASSERT_MSG(index < newCap, "acceleration structure bindless pool exhausted (max %u)", accelStructsCapacityMax_);
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), accelStructHeap_->contents(), NS::UInteger(accelStructsCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nb.get(), "lvk-metal.bindless.accelstructs");
  removeResident(accelStructHeap_.get());
  accelStructHeap_ = nb;
  accelStructsCapacity_ = newCap;
  addResident(accelStructHeap_.get());
  rebindArgumentTableHeaps();
}

void MetalContext::growConstantsRing(const MetalImmediateCommands::CommandBufferWrapper* wrapper) {
  const uint32_t newPerFrame = pushesPerFrameCapacity_ * 2;
  const uint32_t newRegionBytes = newPerFrame * pushConstantsSize_;
  NS::SharedPtr<MTL::Buffer> nb = NS::TransferPtr(
      device_->newBuffer(NS::UInteger(MetalImmediateCommands::kMaxCommandBuffers) * newRegionBytes, MTL::ResourceStorageModeShared));
  ns::setLabel(nb.get(), "lvk-metal.pushconstants.ring");
  retiredConstantRings_.push_back({constantsRing_, wrapper->handle});
  constantsRing_ = nb;
  addResident(constantsRing_.get());
  pushesPerFrameCapacity_ = newPerFrame;
  constantsFrameRegionBytes_ = newRegionBytes;
  constantsCursor_ = wrapper->bufferIndex * newRegionBytes;
}

const MetalImmediateCommands::CommandBufferWrapper* MetalContext::beginCommandBuffer() {
  LVK_ASSERT_MSG(!autoreleasePool_, "previous command buffer was not submitted");
  for (size_t i = 0; i < retiredConstantRings_.size();) {
    if (immediate_->isReady(retiredConstantRings_[i].handle)) {
      removeResident(retiredConstantRings_[i].buffer.get());
      retiredConstantRings_[i] = retiredConstantRings_.back();
      retiredConstantRings_.pop_back();
    } else {
      ++i;
    }
  }
  for (size_t i = 0; i < retiredUploadRings_.size();) {
    if (immediate_->isReady(retiredUploadRings_[i].handle)) {
      removeResident(retiredUploadRings_[i].buffer.get());
      retiredUploadRings_[i] = retiredUploadRings_.back();
      retiredUploadRings_.pop_back();
    } else {
      ++i;
    }
  }
  flushResidency();
  autoreleasePool_ = NS::AutoreleasePool::alloc()->init();
  const MetalImmediateCommands::CommandBufferWrapper* wrapper = &immediate_->acquire();
  constantsCursor_ = wrapper->bufferIndex * constantsFrameRegionBytes_;
  uploadRingCursor_ = wrapper->bufferIndex * uploadRingFrameRegionBytes_;
  return wrapper;
}

IMetalCommandBuffer& MetalContext::acquireMetalCommandBuffer(bool) {
  CommandBuffer* cmd = new CommandBuffer(this);
  cmd->wrapper_ = beginCommandBuffer();
  return *cmd;
}

SubmitHandle MetalContext::submit(lvk::ICommandBuffer& commandBuffer, TextureHandle present) {
  CommandBuffer& cmd = static_cast<CommandBuffer&>(commandBuffer);
  LVK_ASSERT(cmd.wrapper_);

  flushResidency();
  if (immediate_->hasDeferred())
    immediate_->flushDeferred();

  MetalImage* presentImage = present.empty() ? nullptr : textures_.get(present);
  CA::MetalDrawable* drawable = (presentImage && presentImage->isSwapchainImage) ? presentImage->drawable : nullptr;

  if (drawable)
    commandQueue_->wait(drawable);

  const SubmitHandle handle = immediate_->submit(*cmd.wrapper_);

  if (drawable) {
    commandQueue_->signalDrawable(drawable);
    drawable->present();
    drawable->release();
    presentImage->drawable = nullptr;
    presentImage->texture.reset();
    currentImageIndex_ = framesInFlight_ ? (currentImageIndex_ + 1) % framesInFlight_ : 0;
  }

  autoreleasePool_->release();
  autoreleasePool_ = nullptr;
  delete &cmd;

  for (DeferredTask& t : deferredTasks_) {
    if (t.handle_.empty())
      t.handle_ = handle;
  }
  processDeferredTasks();

  return handle;
}

void MetalContext::wait(SubmitHandle handle) {
  immediate_->wait(handle);
}

static const char* kIndirectEncodeMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct LvkDrawArgs {
  uint vertexCount;
  uint instanceCount;
  uint vertexStart;
  uint baseInstance;
};

struct LvkICBContainer {
  command_buffer cmd [[id(0)]];
};

struct LvkEncodeParams {
  uint commandCount;
};

kernel void lvkEncodeIndirectDraws(uint tid [[thread_position_in_grid]],
                                   constant LvkEncodeParams& params [[buffer(0)]],
                                   device const LvkDrawArgs* args [[buffer(1)]],
                                   device LvkICBContainer& icb [[buffer(2)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_primitives(primitive_type::triangle, a.vertexStart, a.vertexCount, a.instanceCount, a.baseInstance);
}

kernel void lvkEncodeIndirectDrawsTyped(uint tid [[thread_position_in_grid]],
                                        constant LvkEncodeParams& params [[buffer(0)]],
                                        device const LvkDrawArgs* args [[buffer(1)]],
                                        device LvkICBContainer& icb [[buffer(2)]],
                                        device const uint* primitiveTypes [[buffer(3)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_primitives(primitive_type(primitiveTypes[tid]), a.vertexStart, a.vertexCount, a.instanceCount, a.baseInstance);
}

struct LvkDrawIndexedArgs {
  uint indexCount;
  uint instanceCount;
  uint indexStart;
  int  baseVertex;
  uint baseInstance;
};

kernel void lvkEncodeIndexed16(uint tid [[thread_position_in_grid]],
                               constant LvkEncodeParams& params [[buffer(0)]],
                               device const LvkDrawIndexedArgs* args [[buffer(1)]],
                               device LvkICBContainer& icb [[buffer(2)]],
                               device const ushort* indexBuffer [[buffer(4)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawIndexedArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_indexed_primitives(
      primitive_type::triangle, a.indexCount, indexBuffer + a.indexStart, a.instanceCount, a.baseVertex, a.baseInstance);
}

kernel void lvkEncodeIndexed32(uint tid [[thread_position_in_grid]],
                               constant LvkEncodeParams& params [[buffer(0)]],
                               device const LvkDrawIndexedArgs* args [[buffer(1)]],
                               device LvkICBContainer& icb [[buffer(2)]],
                               device const uint* indexBuffer [[buffer(4)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawIndexedArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_indexed_primitives(
      primitive_type::triangle, a.indexCount, indexBuffer + a.indexStart, a.instanceCount, a.baseVertex, a.baseInstance);
}

kernel void lvkEncodeIndexed16Typed(uint tid [[thread_position_in_grid]],
                                    constant LvkEncodeParams& params [[buffer(0)]],
                                    device const LvkDrawIndexedArgs* args [[buffer(1)]],
                                    device LvkICBContainer& icb [[buffer(2)]],
                                    device const uint* primitiveTypes [[buffer(3)]],
                                    device const ushort* indexBuffer [[buffer(4)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawIndexedArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_indexed_primitives(
      primitive_type(primitiveTypes[tid]), a.indexCount, indexBuffer + a.indexStart, a.instanceCount, a.baseVertex, a.baseInstance);
}

kernel void lvkEncodeIndexed32Typed(uint tid [[thread_position_in_grid]],
                                    constant LvkEncodeParams& params [[buffer(0)]],
                                    device const LvkDrawIndexedArgs* args [[buffer(1)]],
                                    device LvkICBContainer& icb [[buffer(2)]],
                                    device const uint* primitiveTypes [[buffer(3)]],
                                    device const uint* indexBuffer [[buffer(4)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkDrawIndexedArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_indexed_primitives(
      primitive_type(primitiveTypes[tid]), a.indexCount, indexBuffer + a.indexStart, a.instanceCount, a.baseVertex, a.baseInstance);
}

struct LvkMeshArgs {
  uint groupCountX;
  uint groupCountY;
  uint groupCountZ;
};

struct LvkMeshThreadgroups {
  packed_uint3 object;
  packed_uint3 mesh;
};

kernel void lvkEncodeMeshTasks(uint tid [[thread_position_in_grid]],
                               constant LvkEncodeParams& params [[buffer(0)]],
                               device const LvkMeshArgs* args [[buffer(1)]],
                               device LvkICBContainer& icb [[buffer(2)]],
                               device const LvkMeshThreadgroups* tg [[buffer(3)]]) {
  if (tid >= params.commandCount) {
    return;
  }
  const LvkMeshArgs a = args[tid];
  render_command cmd(icb.cmd, tid);
  cmd.draw_mesh_threadgroups(uint3(a.groupCountX, a.groupCountY, a.groupCountZ), uint3(tg->object), uint3(tg->mesh));
}
)";

void MetalContext::createIndirectCommandBufferFor(MetalBuffer& mb, uint32_t elementStride) {
  const uint32_t capacity = uint32_t(mb.size / elementStride);
  if (!capacity)
    return;
  NS::SharedPtr<MTL::IndirectCommandBufferDescriptor> icbDesc = ns::make<MTL::IndirectCommandBufferDescriptor>();
  icbDesc->setCommandTypes(MTL::IndirectCommandTypeDraw | MTL::IndirectCommandTypeDrawIndexed |
                           MTL::IndirectCommandTypeDrawMeshThreadgroups);
  icbDesc->setInheritPipelineState(true);
  icbDesc->setInheritBuffers(true);
  icbDesc->setMaxVertexBufferBindCount(0);
  icbDesc->setMaxFragmentBufferBindCount(0);
  mb.icb = NS::TransferPtr(device_->newIndirectCommandBuffer(icbDesc.get(), capacity, MTL::ResourceStorageModePrivate));
  if (!mb.icb)
    return;
  ns::setLabel(mb.icb.get(), "lvk-metal.icb");
  mb.icbCapacity = capacity;
  addResident(mb.icb.get());

  mb.icbContainer = NS::TransferPtr(device_->newBuffer(sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  ns::setLabel(mb.icbContainer.get(), "lvk-metal.icb.container");
  *static_cast<MTL::ResourceID*>(mb.icbContainer->contents()) = mb.icb->gpuResourceID();
  addResident(mb.icbContainer.get());
}

bool MetalContext::ensureIndirectEncoder() {
  if (icbEncodePipeline_ && icbEncodeArgTable_)
    return true;
  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::Library> lib = NS::TransferPtr(device_->newLibrary(ns::string(kIndirectEncodeMSL), nullptr, &error));
  if (!lib) {
    LLOGE("ICB encode library failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  const auto makePipeline = [&](const char* entry) -> NS::SharedPtr<MTL::ComputePipelineState> {
    NS::SharedPtr<MTL::Function> fn = NS::TransferPtr(lib->newFunction(ns::string(entry)));
    NS::Error* err = nullptr;
    NS::SharedPtr<MTL::ComputePipelineState> p = fn ? NS::TransferPtr(device_->newComputePipelineState(fn.get(), &err)) : nullptr;
    if (!p)
      LLOGE("ICB encode pipeline '%s' failed: %s", entry, err ? err->localizedDescription()->utf8String() : "unknown");
    return p;
  };
  icbEncodePipeline_ = makePipeline("lvkEncodeIndirectDraws");
  icbEncodePipelineTyped_ = makePipeline("lvkEncodeIndirectDrawsTyped");
  icbEncodeIndexed16_ = makePipeline("lvkEncodeIndexed16");
  icbEncodeIndexed16Typed_ = makePipeline("lvkEncodeIndexed16Typed");
  icbEncodeIndexed32_ = makePipeline("lvkEncodeIndexed32");
  icbEncodeIndexed32Typed_ = makePipeline("lvkEncodeIndexed32Typed");
  icbEncodeMesh_ = makePipeline("lvkEncodeMeshTasks");
  if (!icbEncodePipeline_ || !icbEncodePipelineTyped_ || !icbEncodeIndexed16_ || !icbEncodeIndexed16Typed_ || !icbEncodeIndexed32_ ||
      !icbEncodeIndexed32Typed_ || !icbEncodeMesh_)
    return false;
  NS::SharedPtr<MTL4::ArgumentTableDescriptor> atd = ns::make<MTL4::ArgumentTableDescriptor>();
  atd->setMaxBufferBindCount(5);
  ns::setLabel(atd.get(), "lvk-metal.icb.encode.argtable");
  icbEncodeArgTable_ = NS::TransferPtr(device_->newArgumentTable(atd.get(), &error));
  if (!icbEncodeArgTable_) {
    LLOGE("ICB encode argtable failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  return true;
}

void MetalContext::encodeIndirectDraws(const Dependencies& deps, const MetalImmediateCommands::CommandBufferWrapper* wrapper) {
  MTL4::ComputeCommandEncoder* enc = nullptr;
  for (size_t i = 0; i < deps.buffers.size(); ++i) {
    const MetalBuffer* b = buffers_.get(deps.buffers[i]);
    if (!b || !b->icb || !(b->usage & BufferUsageBits_Indirect))
      continue;
    if (!ensureIndirectEncoder())
      return;
    if (!enc) {
      enc = wrapper->cmdBuf->computeCommandEncoder();
      enc->barrierAfterQueueStages(MTL::StageDispatch, MTL::StageDispatch, MTL4::VisibilityOptionDevice);
    }
    const MetalBuffer* meshTg = b->icbMeshThreadgroupSizes.empty() ? nullptr : buffers_.get(b->icbMeshThreadgroupSizes);
    const MetalBuffer* indexBuf = b->icbIndexBuffer.empty() ? nullptr : buffers_.get(b->icbIndexBuffer);
    const MetalBuffer* primTypes = b->icbPrimitiveTypes.empty() ? nullptr : buffers_.get(b->icbPrimitiveTypes);
    const bool typed = primTypes != nullptr;
    const uint32_t stride = meshTg ? 12u : (indexBuf ? 20u : 16u);
    const uint32_t count = uint32_t(b->size / stride);
    const MTL::GPUAddress paramsAddr = writePushConstants(&count, sizeof(count), 0, wrapper);
    icbEncodeArgTable_->setAddress(paramsAddr, 0);
    icbEncodeArgTable_->setAddress(b->buffer->gpuAddress(), 1);
    icbEncodeArgTable_->setAddress(b->icbContainer->gpuAddress(), 2);
    MTL::ComputePipelineState* pipeline = nullptr;
    if (meshTg) {
      icbEncodeArgTable_->setAddress(meshTg->buffer->gpuAddress(), 3);
      pipeline = icbEncodeMesh_.get();
    } else if (indexBuf) {
      if (typed)
        icbEncodeArgTable_->setAddress(primTypes->buffer->gpuAddress(), 3);
      icbEncodeArgTable_->setAddress(indexBuf->buffer->gpuAddress(), 4);
      const bool is16 = b->icbIndexFormat == IndexFormat_UI16;
      pipeline =
          (is16 ? (typed ? icbEncodeIndexed16Typed_ : icbEncodeIndexed16_) : (typed ? icbEncodeIndexed32Typed_ : icbEncodeIndexed32_))
              .get();
    } else {
      if (typed)
        icbEncodeArgTable_->setAddress(primTypes->buffer->gpuAddress(), 3);
      pipeline = (typed ? icbEncodePipelineTyped_ : icbEncodePipeline_).get();
    }
    enc->setComputePipelineState(pipeline);
    enc->setArgumentTable(icbEncodeArgTable_.get());
    enc->dispatchThreads(MTL::Size(count, 1, 1), MTL::Size(32, 1, 1));
  }
  if (enc) {
    enc->barrierAfterStages(MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
    enc->endEncoding();
  }
}

Holder<BufferHandle> MetalContext::createBuffer(const BufferDesc& desc, const char* debugName, Result* outResult) {
  NS::SharedPtr<MTL::Buffer> buffer = NS::TransferPtr(device_->newBuffer(desc.size, toMTLBufferResourceOptions(desc.storage)));
  if (!buffer) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newBuffer failed");
    return {};
  }
  ns::setLabel(buffer.get(), debugName ? debugName : desc.debugName);
  addResident(buffer.get());
  if (desc.data) {
    if (buffer->storageMode() == MTL::StorageModeShared)
      std::memcpy(buffer->contents(), desc.data, desc.size);
    else
      staging_->uploadBuffer(buffer.get(), 0, desc.size, desc.data);
  }

  const MTL::GPUAddress address = buffer->gpuAddress();
  MetalBuffer mb;
  mb.buffer = std::move(buffer);
  mb.size = desc.size;
  mb.usage = desc.usage;
  if (desc.usage & BufferUsageBits_Indirect)
    createIndirectCommandBufferFor(mb, 16);
  const BufferHandle handle = buffers_.create(std::move(mb));
  ensureBufferCapacity(handle.index());
  static_cast<MTL::GPUAddress*>(bufferHeap_->contents())[handle.index()] = address;
  return {this, handle};
}

Holder<TextureHandle> MetalContext::createTexture(const TextureDesc& desc, const char* debugName, Result* outResult) {
  if (isYUVFormat(desc.format)) {
    const uint32_t w = desc.dimensions.width;
    const uint32_t h = desc.dimensions.height;
    const uint32_t cw = w / 2;
    const uint32_t ch = h / 2;

    const auto makePlane = [&](MTL::PixelFormat fmt, uint32_t pw, uint32_t p8h) {
      NS::SharedPtr<MTL::TextureDescriptor> td = ns::make<MTL::TextureDescriptor>();
      td->setTextureType(MTL::TextureType2D);
      td->setPixelFormat(fmt);
      td->setWidth(pw);
      td->setHeight(p8h);
      td->setUsage(MTL::TextureUsageShaderRead);
      td->setStorageMode(toMTLStorageMode(desc.storage));
      return NS::TransferPtr(device_->newTexture(td.get()));
    };

    NS::SharedPtr<MTL::Texture> yTex = makePlane(MTL::PixelFormatR8Unorm, w, h);
    NS::SharedPtr<MTL::Texture> cTex = makePlane(MTL::PixelFormatRG8Unorm, cw, ch);
    if (!yTex || !cTex) {
      Result::setResult(outResult, Result::Code::RuntimeError, "newTexture failed (YUV)");
      return {};
    }
    ns::setLabel(yTex.get(), debugName ? debugName : desc.debugName);
    ns::setLabel(cTex.get(), debugName ? debugName : desc.debugName);
    addResident(yTex.get());
    addResident(cTex.get());

    if (desc.data) {
      const uint8_t* src = static_cast<const uint8_t*>(desc.data);
      staging_->uploadTexture(yTex.get(), 0, 0, 0, w, h, 1, 0, 0, src, 0);
      if (desc.format == Format_YUV_NV12) {
        staging_->uploadTexture(cTex.get(), 0, 0, 0, cw, ch, 1, 0, 0, src + size_t(w) * h, 0);
      } else {
        const uint8_t* cb = src + size_t(w) * h;
        const uint8_t* cr = cb + size_t(cw) * ch;
        std::vector<uint8_t> interleaved(size_t(cw) * ch * 2);
        for (size_t i = 0; i < size_t(cw) * ch; ++i) {
          interleaved[i * 2 + 0] = cb[i];
          interleaved[i * 2 + 1] = cr[i];
        }
        staging_->uploadTexture(cTex.get(), 0, 0, 0, cw, ch, 1, 0, 0, interleaved.data(), 0);
      }
    }

    const MTL::ResourceID yRid = yTex->gpuResourceID();
    const MTL::ResourceID cRid = cTex->gpuResourceID();
    MetalImage img;
    img.format = MTL::PixelFormatR8Unorm;
    img.width = w;
    img.height = h;
    img.texture = std::move(yTex);
    img.yuvChroma = std::move(cTex);
    const TextureHandle handle = textures_.create(std::move(img));
    ensureTextureCapacity(handle.index());
    static_cast<MTL::ResourceID*>(textureHeap_->contents())[handle.index()] = yRid;
    static_cast<MTL::ResourceID*>(yuvChromaHeap_->contents())[handle.index()] = cRid;
    return {this, handle};
  }

  const uint32_t numSamples = desc.numSamples ? desc.numSamples : 1;
  NS::SharedPtr<MTL::TextureDescriptor> td = ns::make<MTL::TextureDescriptor>();
  td->setTextureType(toMTLTextureType(desc.type, desc.numLayers, numSamples));
  td->setPixelFormat(toMTLPixelFormat(desc.format));
  td->setWidth(desc.dimensions.width);
  td->setHeight(desc.dimensions.height);
  td->setDepth(desc.type == TextureType_3D ? desc.dimensions.depth : 1);
  td->setMipmapLevelCount(desc.numMipLevels);
  td->setArrayLength(desc.numLayers ? desc.numLayers : 1);
  td->setSampleCount(numSamples);
  td->setUsage(toMTLTextureUsage(desc.usage) | MTL::TextureUsagePixelFormatView);
  td->setStorageMode(toMTLStorageMode(desc.storage));

  NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(device_->newTexture(td.get()));
  if (!texture) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newTexture failed");
  }
  ns::setLabel(texture.get(), debugName ? debugName : desc.debugName);
  if (texture->storageMode() != MTL::StorageModeMemoryless)
    addResident(texture.get());

  if (desc.data) {
    const uint32_t depth = desc.type == TextureType_3D ? desc.dimensions.depth : 1;
    staging_->uploadTexture(texture.get(), 0, 0, 0, desc.dimensions.width, desc.dimensions.height, depth, 0, 0, desc.data, 0);
    if (desc.generateMipmaps && desc.numMipLevels > 1)
      generateMipmapImmediate(texture.get());
  }

  const MTL::ResourceID rid = texture->gpuResourceID();
  MetalImage img;
  img.format = toMTLPixelFormat(desc.format);
  img.width = desc.dimensions.width;
  img.height = desc.dimensions.height;
  img.texture = std::move(texture);
  const TextureHandle handle = textures_.create(std::move(img));
  ensureTextureCapacity(handle.index());
  static_cast<MTL::ResourceID*>(textureHeap_->contents())[handle.index()] = rid;
  static_cast<MTL::ResourceID*>(yuvChromaHeap_->contents())[handle.index()] = rid;
  return {this, handle};
}

Holder<TextureHandle> MetalContext::createTextureView(TextureHandle texture,
                                                      const TextureViewDesc& desc,
                                                      const char* debugName,
                                                      Result* outResult) {
  const MetalImage* parent = textures_.get(texture);
  if (!parent || !parent->texture) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "createTextureView: invalid parent texture");
    return {};
  }

  NS::SharedPtr<MTL::TextureViewDescriptor> vd = ns::make<MTL::TextureViewDescriptor>();
  vd->setPixelFormat(parent->format);
  vd->setTextureType(toMTLTextureType(desc.type, desc.numLayers, 1));
  vd->setLevelRange(NS::Range(desc.mipLevel, desc.numMipLevels));
  vd->setSliceRange(NS::Range(desc.layer, desc.numLayers));
  vd->setSwizzle(toMTLSwizzleChannels(desc.components));

  NS::SharedPtr<MTL::Texture> view = NS::TransferPtr(parent->texture->newTextureView(vd.get()));
  if (!view) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newTextureView failed");
    return {};
  }
  ns::setLabel(view.get(), debugName ? debugName : "");
  addResident(view.get());

  const MTL::ResourceID rid = view->gpuResourceID();
  MetalImage img;
  img.format = parent->format;
  img.width = uint32_t(view->width());
  img.height = uint32_t(view->height());
  img.texture = std::move(view);
  const TextureHandle handle = textures_.create(std::move(img));
  ensureTextureCapacity(handle.index());
  static_cast<MTL::ResourceID*>(textureHeap_->contents())[handle.index()] = rid;
  static_cast<MTL::ResourceID*>(yuvChromaHeap_->contents())[handle.index()] = rid;
  return {this, handle};
}

Holder<SamplerHandle> MetalContext::createSampler(const SamplerStateDesc& desc, Result* outResult) {
  NS::SharedPtr<MTL::SamplerDescriptor> sd = ns::make<MTL::SamplerDescriptor>();
  sd->setMinFilter(toMTLSamplerFilter(desc.minFilter));
  sd->setMagFilter(toMTLSamplerFilter(desc.magFilter));
  sd->setMipFilter(toMTLSamplerMipFilter(desc.mipMap));
  sd->setSAddressMode(toMTLSamplerAddressMode(desc.wrapU));
  sd->setTAddressMode(toMTLSamplerAddressMode(desc.wrapV));
  sd->setRAddressMode(toMTLSamplerAddressMode(desc.wrapW));
  sd->setMaxAnisotropy(desc.maxAnisotropic ? desc.maxAnisotropic : 1);
  sd->setLodMinClamp(float(desc.mipLodMin));
  sd->setLodMaxClamp(float(desc.mipLodMax));
  if (desc.depthCompareEnabled)
    sd->setCompareFunction(toMTLCompareFunction(desc.depthCompareOp));
  sd->setSupportArgumentBuffers(true);
  ns::setLabel(sd.get(), desc.debugName);
  NS::SharedPtr<MTL::SamplerState> sampler = NS::TransferPtr(device_->newSamplerState(sd.get()));
  if (!sampler) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newSamplerState failed");
    return {};
  }
  const MTL::ResourceID rid = sampler->gpuResourceID();
  MetalSampler ms;
  ms.sampler = std::move(sampler);
  const SamplerHandle handle = samplers_.create(std::move(ms));
  ensureSamplerCapacity(handle.index());
  static_cast<MTL::ResourceID*>(samplerHeap_->contents())[handle.index()] = rid;
  return {this, handle};
}

Holder<QueryPoolHandle> MetalContext::createQueryPool(uint32_t numQueries, const char* debugName, Result* outResult) {
  NS::SharedPtr<MTL4::CounterHeapDescriptor> chd = ns::make<MTL4::CounterHeapDescriptor>();
  chd->setType(MTL4::CounterHeapTypeTimestamp);
  chd->setCount(numQueries);

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL4::CounterHeap> heap = NS::TransferPtr(device_->newCounterHeap(chd.get(), &error));
  if (!heap) {
    LLOGE("createQueryPool failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newCounterHeap failed");
    return {};
  }
  ns::setLabel(heap.get(), debugName ? debugName : "");

  MetalQueryPool qp;
  qp.heap = std::move(heap);
  qp.count = numQueries;
  return {this, queryPools_.create(std::move(qp))};
}

void MetalContext::destroy(QueryPoolHandle handle) {
  if (!queryPools_.get(handle))
    return;
  deferredTask([this, handle]() { queryPools_.destroy(handle); });
}

bool MetalContext::getQueryPoolResults(QueryPoolHandle pool,
                                       uint32_t firstQuery,
                                       uint32_t queryCount,
                                       size_t dataSize,
                                       void* outData,
                                       size_t stride) const {
  const MetalQueryPool* qp = queryPools_.get(pool);
  if (!qp || !qp->heap)
    return false;

  NS::Data* resolved = qp->heap->resolveCounterRange(NS::Range(firstQuery, queryCount));
  if (!resolved)
    return false;
  using BytesMsg = const void* (*)(const void*, SEL);
  const void* rawBytes = reinterpret_cast<BytesMsg>(objc_msgSend)(resolved, sel_registerName("bytes"));
  const MTL4::TimestampHeapEntry* entries = static_cast<const MTL4::TimestampHeapEntry*>(rawBytes);
  if (!entries)
    return false;
  uint8_t* dst = static_cast<uint8_t*>(outData);
  for (uint32_t i = 0; i < queryCount; ++i) {
    if ((i + 1) * stride > dataSize)
      break;
    const uint64_t value = entries[i].timestamp;
    std::memcpy(dst + size_t(i) * stride, &value, sizeof(value));
  }
  return true;
}

static const char* kBindlessPreamble = R"(
#include <metal_stdlib>
using namespace metal;

#ifndef LVK_BINDLESS_TEXTURES
#define LVK_BINDLESS_TEXTURES 16384
#endif
#ifndef LVK_BINDLESS_SAMPLERS
#define LVK_BINDLESS_SAMPLERS 1024
#endif
#ifndef LVK_BINDLESS_ACCEL_STRUCTS
#define LVK_BINDLESS_ACCEL_STRUCTS 256
#endif

struct lvkTextures2D         { array<texture2d<float>,                            LVK_BINDLESS_TEXTURES> data; };
struct lvkTextures3D         { array<texture3d<float>,                            LVK_BINDLESS_TEXTURES> data; };
struct lvkTexturesCube       { array<texturecube<float>,                          LVK_BINDLESS_TEXTURES> data; };
struct lvkTexturesDepth2D    { array<depth2d<float>,                              LVK_BINDLESS_TEXTURES> data; };
struct lvkSamplers           { array<sampler,                                     LVK_BINDLESS_SAMPLERS> data; };
struct lvkSamplersComparison { array<sampler,                                     LVK_BINDLESS_SAMPLERS> data; };
struct lvkImages2D           { array<texture2d<float, access::read_write>,        LVK_BINDLESS_TEXTURES> data; };
struct lvkImages3D           { array<texture3d<float, access::read_write>,        LVK_BINDLESS_TEXTURES> data; };
struct lvkAccelStructs       { array<raytracing::instance_acceleration_structure, LVK_BINDLESS_ACCEL_STRUCTS> data; };
struct lvkYUVChroma          { array<texture2d<float>,                            LVK_BINDLESS_TEXTURES> data; };

#define LVK_BINDLESS_ARGS \
  device const lvkTextures2D&         kTextures2D         [[buffer(2)]], \
  device const lvkTextures3D&         kTextures3D         [[buffer(3)]], \
  device const lvkTexturesCube&       kTexturesCube       [[buffer(4)]], \
  device const lvkTexturesDepth2D&    kTexturesDepth2D    [[buffer(5)]], \
  constant lvkSamplers&               kSamplers           [[buffer(6)]], \
  constant lvkSamplersComparison&     kSamplersComparison [[buffer(7)]], \
  device const lvkImages2D&           kImages2D           [[buffer(8)]], \
  device const lvkImages3D&           kImages3D           [[buffer(9)]], \
  device const lvkAccelStructs&       kTLAS               [[buffer(10)]], \
  device const lvkYUVChroma&          kYUVChroma          [[buffer(11)]]

inline float4 lvkSampleYUV(float y, float2 cbcr) {
  const float Y  = (y - 16.0 / 255.0) * (255.0 / 219.0);
  const float Cb = (cbcr.x - 128.0 / 255.0) * (255.0 / 224.0);
  const float Cr = (cbcr.y - 128.0 / 255.0) * (255.0 / 224.0);
  const float3 rgb = float3(Y + 1.5748 * Cr, Y - 0.1873 * Cb - 0.4681 * Cr, Y + 1.8556 * Cb);
  return float4(saturate(rgb), 1.0);
}

#define textureBindless2D(tid, sid, uv)            kTextures2D.data[tid].sample(kSamplers.data[sid], (uv))
#define textureBindless2DLod(tid, sid, uv, lod)    kTextures2D.data[tid].sample(kSamplers.data[sid], (uv), level(lod))
#define textureBindless3D(tid, sid, uvw)           kTextures3D.data[tid].sample(kSamplers.data[sid], (uvw))
#define textureBindlessCube(tid, sid, dir)         kTexturesCube.data[tid].sample(kSamplers.data[sid], (dir))
#define textureBindlessCubeLod(tid, sid, dir, lod) kTexturesCube.data[tid].sample(kSamplers.data[sid], (dir), level(lod))
#define textureBindlessSize2D(tid)                 uint2(kTextures2D.data[tid].get_width(), kTextures2D.data[tid].get_height())
#define textureBindless2DShadow(tid, sid, uvw)     kTexturesDepth2D.data[tid].sample_compare(kSamplersComparison.data[sid], (uvw).xy, (uvw).z)
#define textureBindlessYUV(tid, sid, uv)           lvkSampleYUV(kTextures2D.data[tid].sample(kSamplers.data[sid], (uv)).r, kYUVChroma.data[tid].sample(kSamplers.data[sid], (uv)).rg)
#define imageBindlessLoad2D(tid, pos)              kImages2D.data[tid].read(uint2(pos))
#define imageBindlessStore2D(tid, pos, val)        kImages2D.data[tid].write((val), uint2(pos))
#define imageBindlessLoad3D(tid, pos)              kImages3D.data[tid].read(uint3(pos))
#define imageBindlessStore3D(tid, pos, val)        kImages3D.data[tid].write((val), uint3(pos))
)";

static MTL::Size parseThreadgroupSize(const char* src, const char* entryPoint) {
  const MTL::Size kDefault(16, 16, 1);
  if (!src)
    return kDefault;

  const char* searchEnd = src + std::strlen(src);
  if (entryPoint) {
    if (const char* ep = std::strstr(src, entryPoint))
      searchEnd = ep;
  }

  const char* found = nullptr;
  for (const char* p = src; (p = std::strstr(p, "lvk_numthreads(")) != nullptr && p < searchEnd; ++p)
    found = p;

  if (found) {
    unsigned x = 0, y = 0, z = 0;
    if (std::sscanf(found, "lvk_numthreads( %u , %u , %u", &x, &y, &z) >= 1)
      return MTL::Size(x ? x : 1, y ? y : 1, z ? z : 1);
  }
  return kDefault;
}

Holder<ShaderModuleHandle> MetalContext::createShaderModule(const ShaderModuleDesc& desc, Result* outResult) {
  NS::SharedPtr<MTL::Library> library;
  NS::Error* error = nullptr;

  if (desc.data && desc.dataSize == 0) {
    std::string source;
    if (std::strstr(desc.data, "LVK_BINDLESS")) {
      source += "#define LVK_BINDLESS_TEXTURES " + std::to_string(texturesCapacity_) + "\n";
      source += "#define LVK_BINDLESS_SAMPLERS " + std::to_string(samplersCapacity_) + "\n";
      source += "#define LVK_BINDLESS_ACCEL_STRUCTS " + std::to_string(accelStructsCapacity_) + "\n";
      source += kBindlessPreamble;
    }
    source += desc.data;
    library = NS::TransferPtr(device_->newLibrary(ns::string(source.c_str()), nullptr, &error));
  } else if (desc.data && desc.dataSize) {
    dispatch_data_t dd = dispatch_data_create(desc.data, desc.dataSize, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    library = NS::TransferPtr(device_->newLibrary(dd, &error));
    dispatch_release(dd);
  }

  if (!library) {
    LLOGE("createShaderModule failed: %s", error ? error->localizedDescription()->utf8String() : "no source/data");
    Result::setResult(outResult, Result::Code::RuntimeError, "newLibrary failed");
    return {};
  }
  ns::setLabel(library.get(), desc.debugName);

  const char* entry = desc.entryPointName ? desc.entryPointName : "main";
  NS::SharedPtr<MTL::Function> function = NS::TransferPtr(library->newFunction(ns::string(entry)));
  if (!function) {
    LLOGE("createShaderModule: entry point '%s' not found", entry);
    Result::setResult(outResult, Result::Code::RuntimeError, "newFunction failed");
    return {};
  }

  MetalShaderModule sm;
  sm.library = std::move(library);
  sm.function = std::move(function);
  if (desc.data && desc.dataSize == 0)
    sm.threadgroupSize = parseThreadgroupSize(desc.data, entry);
  return {this, shaderModules_.create(std::move(sm))};
}

void MetalContext::setShaderModuleMetadata(ShaderModuleHandle handle, const ShaderModuleMetadata& metadata) {
  MetalShaderModule* sm = shaderModules_.get(handle);
  if (!sm)
    return;
  const Dimensions& tg = metadata.threadgroupSize;
  sm->threadgroupSize = MTL::Size(tg.width ? tg.width : 1, tg.height ? tg.height : 1, tg.depth ? tg.depth : 1);
  sm->viewCount = metadata.viewCount ? metadata.viewCount : 1;
}

void MetalContext::setIndirectBufferMetadata(BufferHandle indirectBuffer, const IndirectBufferMetadata& metadata) {
  MetalBuffer* b = buffers_.get(indirectBuffer);
  if (!b)
    return;
  b->icbPrimitiveTypes = metadata.primitiveTypes;
  b->icbIndexBuffer = metadata.indexBuffer;
  b->icbIndexFormat = metadata.indexFormat;
  b->icbMeshThreadgroupSizes = metadata.meshThreadgroupSizes;
  if (!b->icbMeshThreadgroupSizes.empty() && b->icbCapacity < b->size / 12) {
    if (b->icb)
      removeResident(b->icb.get());
    if (b->icbContainer)
      removeResident(b->icbContainer.get());
    b->icb.reset();
    b->icbContainer.reset();
    createIndirectCommandBufferFor(*b, 12);
  }
}

NS::SharedPtr<MTL::Function> MetalContext::specializeFunction(const MetalShaderModule* sm, const SpecializationConstantDesc& spec) {
  if (!sm || !sm->function)
    return {};
  const uint32_t numConstants = spec.getNumSpecializationConstants();
  if (numConstants == 0 || !spec.data)
    return sm->function;

  std::unordered_map<uint32_t, MTL::DataType> typeByIndex;
  if (NS::Dictionary* dict = sm->function->functionConstantsDictionary()) {
    NS::Enumerator<NS::String>* keys = dict->keyEnumerator<NS::String>();
    while (NS::String* key = keys->nextObject()) {
      if (const MTL::FunctionConstant* fc = dict->object<MTL::FunctionConstant>(key))
        typeByIndex[uint32_t(fc->index())] = fc->type();
    }
  }

  NS::SharedPtr<MTL::FunctionConstantValues> values = ns::make<MTL::FunctionConstantValues>();
  const uint8_t* base = static_cast<const uint8_t*>(spec.data);
  for (uint32_t i = 0; i < numConstants; ++i) {
    const SpecializationConstantEntry& e = spec.entries[i];
    const std::unordered_map<uint32_t, MTL::DataType>::const_iterator it = typeByIndex.find(e.constantId);
    if (it == typeByIndex.end())
      continue;
    values->setConstantValue(base + e.offset, it->second, NS::UInteger(e.constantId));
  }

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::Function> fn = NS::TransferPtr(sm->library->newFunction(sm->function->name(), values.get(), &error));
  if (!fn) {
    LLOGE("specializeFunction '%s' failed: %s",
          sm->function->name()->utf8String(),
          error ? error->localizedDescription()->utf8String() : "unknown");
    return sm->function;
  }
  return fn;
}

Holder<ComputePipelineHandle> MetalContext::createComputePipeline(const ComputePipelineDesc& desc, Result* outResult) {
  const MetalShaderModule* comp = shaderModules_.get(desc.smComp);
  if (!comp || !comp->function) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing compute shader");
    return {};
  }
  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::Function> compFn = specializeFunction(comp, desc.specInfo);
  NS::SharedPtr<MTL::ComputePipelineState> pipeline = NS::TransferPtr(device_->newComputePipelineState(compFn.get(), &error));
  if (!pipeline) {
    LLOGE("createComputePipeline failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newComputePipelineState failed");
    return {};
  }
  MetalComputePipeline cp;
  cp.pipeline = std::move(pipeline);
  cp.threadgroupSize = comp->threadgroupSize;
  return {this, computePipelines_.create(std::move(cp))};
}

Holder<RenderPipelineHandle> MetalContext::createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult) {
  const MetalShaderModule* mesh = shaderModules_.get(desc.smMesh);
  if (mesh && mesh->function) {
    const MetalShaderModule* task = shaderModules_.get(desc.smTask);
    const MetalShaderModule* meshFrag = shaderModules_.get(desc.smFrag);
    NS::SharedPtr<MTL::MeshRenderPipelineDescriptor> mpd = ns::make<MTL::MeshRenderPipelineDescriptor>();
    ns::setLabel(mpd.get(), desc.debugName);
    NS::SharedPtr<MTL::Function> objFn = task && task->function ? specializeFunction(task, desc.specInfo) : NS::SharedPtr<MTL::Function>{};
    NS::SharedPtr<MTL::Function> meshFn = specializeFunction(mesh, desc.specInfo);
    NS::SharedPtr<MTL::Function> fragFn = meshFrag && meshFrag->function ? specializeFunction(meshFrag, desc.specInfo)
                                                                         : NS::SharedPtr<MTL::Function>{};
    if (objFn)
      mpd->setObjectFunction(objFn.get());
    mpd->setMeshFunction(meshFn.get());
    if (fragFn)
      mpd->setFragmentFunction(fragFn.get());
    const uint32_t nColorMesh = desc.getNumColorAttachments();
    for (uint32_t i = 0; i < nColorMesh; ++i) {
      const ColorAttachment& ca = desc.color[i];
      MTL::RenderPipelineColorAttachmentDescriptor* att = mpd->colorAttachments()->object(i);
      att->setPixelFormat(toMTLPixelFormat(ca.format));
      att->setBlendingEnabled(ca.blendEnabled);
      att->setRgbBlendOperation(toMTLBlendOperation(ca.rgbBlendOp));
      att->setAlphaBlendOperation(toMTLBlendOperation(ca.alphaBlendOp));
      att->setSourceRGBBlendFactor(toMTLBlendFactor(ca.srcRGBBlendFactor));
      att->setSourceAlphaBlendFactor(toMTLBlendFactor(ca.srcAlphaBlendFactor));
      att->setDestinationRGBBlendFactor(toMTLBlendFactor(ca.dstRGBBlendFactor));
      att->setDestinationAlphaBlendFactor(toMTLBlendFactor(ca.dstAlphaBlendFactor));
    }
    if (desc.depthFormat != Format_Invalid)
      mpd->setDepthAttachmentPixelFormat(toMTLPixelFormat(desc.depthFormat));
    mpd->setRasterSampleCount(desc.samplesCount ? desc.samplesCount : 1);
    mpd->setAlphaToCoverageEnabled(desc.alphaToCoverage);
    const MTL::Size objTG = task && task->function ? task->threadgroupSize : MTL::Size(1, 1, 1);
    const MTL::Size meshTG = mesh->threadgroupSize;
    if (objFn)
      mpd->setMaxTotalThreadsPerObjectThreadgroup(objTG.width * objTG.height * objTG.depth);
    mpd->setMaxTotalThreadsPerMeshThreadgroup(meshTG.width * meshTG.height * meshTG.depth);
    mpd->setSupportIndirectCommandBuffers(true);

    NS::Error* meshError = nullptr;
    NS::SharedPtr<MTL::RenderPipelineState> meshPipeline =
        NS::TransferPtr(device_->newRenderPipelineState(mpd.get(), MTL::PipelineOptionNone, nullptr, &meshError));
    if (!meshPipeline) {
      LLOGE("createRenderPipeline (mesh) failed: %s", meshError ? meshError->localizedDescription()->utf8String() : "unknown");
      Result::setResult(outResult, Result::Code::RuntimeError, "newRenderPipelineState (mesh) failed");
      return {};
    }
    MetalRenderPipeline mp;
    mp.pipeline = std::move(meshPipeline);
    mp.cullMode = toMTLCullMode(desc.cullMode);
    mp.frontFace = toMTLWinding(desc.frontFace);
    mp.fillMode = toMTLFillMode(desc.polygonMode);
    mp.isMesh = true;
    mp.objectThreadsPerThreadgroup = objTG;
    mp.meshThreadsPerThreadgroup = meshTG;
    return {this, renderPipelines_.create(std::move(mp))};
  }

  const MetalShaderModule* vert = shaderModules_.get(desc.smVert);
  const MetalShaderModule* frag = shaderModules_.get(desc.smFrag);
  if (!vert || !vert->function) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing vertex shader");
    return {};
  }

  NS::SharedPtr<MTL::RenderPipelineDescriptor> rpd = ns::make<MTL::RenderPipelineDescriptor>();
  ns::setLabel(rpd.get(), desc.debugName);
  NS::SharedPtr<MTL::Function> vertFn = specializeFunction(vert, desc.specInfo);
  NS::SharedPtr<MTL::Function> fragFn = frag && frag->function ? specializeFunction(frag, desc.specInfo) : NS::SharedPtr<MTL::Function>{};
  rpd->setVertexFunction(vertFn.get());
  if (fragFn)
    rpd->setFragmentFunction(fragFn.get());

  const uint32_t nColor = desc.getNumColorAttachments();
  for (uint32_t i = 0; i < nColor; ++i) {
    const ColorAttachment& ca = desc.color[i];
    MTL::RenderPipelineColorAttachmentDescriptor* att = rpd->colorAttachments()->object(i);
    att->setPixelFormat(toMTLPixelFormat(ca.format));
    att->setBlendingEnabled(ca.blendEnabled);
    att->setRgbBlendOperation(toMTLBlendOperation(ca.rgbBlendOp));
    att->setAlphaBlendOperation(toMTLBlendOperation(ca.alphaBlendOp));
    att->setSourceRGBBlendFactor(toMTLBlendFactor(ca.srcRGBBlendFactor));
    att->setSourceAlphaBlendFactor(toMTLBlendFactor(ca.srcAlphaBlendFactor));
    att->setDestinationRGBBlendFactor(toMTLBlendFactor(ca.dstRGBBlendFactor));
    att->setDestinationAlphaBlendFactor(toMTLBlendFactor(ca.dstAlphaBlendFactor));
  }
  if (desc.depthFormat != Format_Invalid)
    rpd->setDepthAttachmentPixelFormat(toMTLPixelFormat(desc.depthFormat));
  if (desc.stencilFormat != Format_Invalid)
    rpd->setStencilAttachmentPixelFormat(toMTLPixelFormat(desc.stencilFormat));
  rpd->setRasterSampleCount(desc.samplesCount ? desc.samplesCount : 1);
  rpd->setAlphaToCoverageEnabled(desc.alphaToCoverage);
  rpd->setSupportIndirectCommandBuffers(true);
  if (vert->viewCount > 1)
    rpd->setMaxVertexAmplificationCount(vert->viewCount);

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::RenderPipelineState> pipeline = NS::TransferPtr(device_->newRenderPipelineState(rpd.get(), &error));
  if (!pipeline) {
    LLOGE("createRenderPipeline failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newRenderPipelineState failed");
    return {};
  }

  MetalRenderPipeline mp;
  mp.pipeline = std::move(pipeline);
  mp.topology = toMTLPrimitiveType(desc.topology);
  mp.cullMode = toMTLCullMode(desc.cullMode);
  mp.frontFace = toMTLWinding(desc.frontFace);
  mp.fillMode = toMTLFillMode(desc.polygonMode);
  mp.frontStencil = desc.frontFaceStencil;
  mp.backStencil = desc.backFaceStencil;
  return {this, renderPipelines_.create(std::move(mp))};
}

Holder<TilePipelineHandle> MetalContext::createTileRenderPipeline(const TileRenderPipelineDesc& desc, Result* outResult) {
  const MetalShaderModule* tile = shaderModules_.get(desc.smTile);
  if (!tile || !tile->function) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing tile shader");
    return {};
  }

  NS::SharedPtr<MTL::TileRenderPipelineDescriptor> tpd = ns::make<MTL::TileRenderPipelineDescriptor>();
  ns::setLabel(tpd.get(), desc.debugName);
  tpd->setTileFunction(tile->function.get());
  tpd->setThreadgroupSizeMatchesTileSize(false);
  tpd->setMaxTotalThreadsPerThreadgroup(desc.maxThreadsPerThreadgroup);

  const uint32_t nColor = desc.getNumColorAttachments();
  for (uint32_t i = 0; i < nColor; ++i) {
    MTL::TileRenderPipelineColorAttachmentDescriptor* att = tpd->colorAttachments()->object(i);
    att->setPixelFormat(toMTLPixelFormat(desc.color[i]));
  }

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::RenderPipelineState> pipeline =
      NS::TransferPtr(device_->newRenderPipelineState(tpd.get(), MTL::PipelineOptionNone, nullptr, &error));
  if (!pipeline) {
    LLOGE("createTileRenderPipeline failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newRenderPipelineState (tile) failed");
    return {};
  }

  MetalTilePipeline tp;
  tp.pipeline = std::move(pipeline);
  return {this, tilePipelines_.create(std::move(tp))};
}

static NS::SharedPtr<MTL::TensorExtents> makeTensorExtents(const uint32_t* dims, uint32_t rank) {
  NS::Integer values[TensorDesc::kMaxRank] = {};
  for (uint32_t i = 0; i < rank; ++i)
    values[i] = NS::Integer(dims[rank - 1 - i]);
  return NS::TransferPtr(MTL::TensorExtents::alloc()->init(rank, values));
}

static NS::SharedPtr<MTL::TensorExtents> makeContiguousStrides(const uint32_t* dims, uint32_t rank) {
  NS::Integer values[TensorDesc::kMaxRank] = {};
  NS::Integer stride = 1;
  for (uint32_t i = 0; i < rank; ++i) {
    values[i] = stride;
    stride *= NS::Integer(dims[rank - 1 - i]);
  }
  return NS::TransferPtr(MTL::TensorExtents::alloc()->init(rank, values));
}

Holder<TensorHandle> MetalContext::createTensor(const TensorDesc& desc, Result* outResult) {
  if (desc.rank == 0 || desc.rank > TensorDesc::kMaxRank) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "invalid tensor rank");
    return {};
  }
  const MTL::TensorDataType dataType = toMTLTensorDataType(desc.dataType);
  NS::SharedPtr<MTL::TensorExtents> extents = makeTensorExtents(desc.dimensions, desc.rank);

  const MetalBuffer* aliasBuffer = desc.buffer.empty() ? nullptr : buffers_.get(desc.buffer);
  if (!desc.buffer.empty() && (!aliasBuffer || !aliasBuffer->buffer)) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "invalid tensor alias buffer");
    return {};
  }

  NS::SharedPtr<MTL::TensorDescriptor> td = ns::make<MTL::TensorDescriptor>();
  td->setDataType(dataType);
  td->setDimensions(extents.get());
  td->setUsage(MTL::TensorUsageMachineLearning | MTL::TensorUsageCompute);
  td->setStorageMode(aliasBuffer ? aliasBuffer->buffer->storageMode() : toMTLStorageMode(desc.storage));
  if (aliasBuffer) {
    const uint32_t elemSize = tensorDataTypeByteSize(dataType);
    NS::Integer strideVals[TensorDesc::kMaxRank] = {};
    NS::Integer stride = 1;
    for (uint32_t i = 0; i < desc.rank; ++i) {
      strideVals[i] = stride;
      const uint32_t dimSize = desc.dimensions[desc.rank - 1 - i];
      stride = (i == 0) ? NS::Integer(mlTensorRowStrideElements(dimSize, elemSize)) : stride * NS::Integer(dimSize);
    }
    NS::SharedPtr<MTL::TensorExtents> strides = NS::TransferPtr(MTL::TensorExtents::alloc()->init(desc.rank, strideVals));
    td->setStrides(strides.get());
  }

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::Tensor> tensor = aliasBuffer ? NS::TransferPtr(aliasBuffer->buffer->newTensor(td.get(), desc.bufferOffset, &error))
                                                  : NS::TransferPtr(device_->newTensor(td.get(), &error));
  if (!tensor) {
    LLOGE("createTensor failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newTensor failed");
    return {};
  }
  ns::setLabel(tensor.get(), desc.debugName);

  MetalTensor mt;
  mt.tensor = tensor;
  mt.dataType = dataType;
  mt.rank = desc.rank;
  size_t elements = 1;
  for (uint32_t i = 0; i < desc.rank; ++i) {
    mt.dimensions[i] = desc.dimensions[i];
    elements *= desc.dimensions[i];
  }
  mt.byteSize = elements * tensorDataTypeByteSize(dataType);
  addResident(tensor.get());
  return {this, tensors_.create(std::move(mt))};
}

Result MetalContext::upload(TensorHandle handle, const void* data, size_t size) {
  const MetalTensor* mt = tensors_.get(handle);
  if (!mt || !mt->tensor)
    return Result(Result::Code::ArgumentOutOfRange, "invalid tensor");
  if (mt->tensor->storageMode() != MTL::StorageModeShared)
    return Result(Result::Code::RuntimeError, "tensor upload requires StorageType_HostVisible");
  if (size < mt->byteSize)
    return Result(Result::Code::ArgumentOutOfRange, "upload size smaller than tensor");
  const uint32_t zeros[TensorDesc::kMaxRank] = {};
  NS::SharedPtr<MTL::TensorExtents> sliceOrigin = makeTensorExtents(zeros, mt->rank);
  NS::SharedPtr<MTL::TensorExtents> sliceDims = makeTensorExtents(mt->dimensions, mt->rank);
  NS::SharedPtr<MTL::TensorExtents> strides = makeContiguousStrides(mt->dimensions, mt->rank);
  mt->tensor->replaceSliceOrigin(sliceOrigin.get(), sliceDims.get(), data, strides.get());
  return Result();
}

Result MetalContext::download(TensorHandle handle, void* data, size_t size) {
  const MetalTensor* mt = tensors_.get(handle);
  if (!mt || !mt->tensor)
    return Result(Result::Code::ArgumentOutOfRange, "invalid tensor");
  if (mt->tensor->storageMode() != MTL::StorageModeShared)
    return Result(Result::Code::RuntimeError, "tensor download requires StorageType_HostVisible");
  if (size < mt->byteSize)
    return Result(Result::Code::ArgumentOutOfRange, "download size smaller than tensor");
  const uint32_t zeros[TensorDesc::kMaxRank] = {};
  NS::SharedPtr<MTL::TensorExtents> sliceOrigin = makeTensorExtents(zeros, mt->rank);
  NS::SharedPtr<MTL::TensorExtents> sliceDims = makeTensorExtents(mt->dimensions, mt->rank);
  NS::SharedPtr<MTL::TensorExtents> strides = makeContiguousStrides(mt->dimensions, mt->rank);
  mt->tensor->getBytes(data, strides.get(), sliceOrigin.get(), sliceDims.get());
  return Result();
}

Holder<MLPipelineHandle> MetalContext::createMachineLearningPipeline(const MachineLearningPipelineDesc& desc, Result* outResult) {
  if (!compiler_) {
    Result::setResult(outResult, Result::Code::RuntimeError, "MTL4 compiler unavailable");
    return {};
  }
  if (!desc.packagePath || !desc.packagePath[0]) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing ML package path");
    return {};
  }
  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::Library> library =
      NS::TransferPtr(device_->newLibrary(NS::URL::fileURLWithPath(ns::string(desc.packagePath)), &error));
  if (!library) {
    LLOGE("createMachineLearningPipeline: failed to load '%s': %s",
          desc.packagePath,
          error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newLibrary(package) failed");
    return {};
  }
  ns::setLabel(library.get(), desc.debugName);

  NS::SharedPtr<MTL4::LibraryFunctionDescriptor> fnDesc = ns::make<MTL4::LibraryFunctionDescriptor>();
  fnDesc->setName(ns::string(desc.functionName ? desc.functionName : "main"));
  fnDesc->setLibrary(library.get());

  NS::SharedPtr<MTL4::MachineLearningPipelineDescriptor> pd = ns::make<MTL4::MachineLearningPipelineDescriptor>();
  ns::setLabel(pd.get(), desc.debugName);
  pd->setMachineLearningFunctionDescriptor(fnDesc.get());
  NS::SharedPtr<MTL4::PipelineOptions> options = ns::make<MTL4::PipelineOptions>();
  options->setShaderReflection(MTL4::ShaderReflectionBindingInfo);
  pd->setOptions(options.get());
  for (uint32_t i = 0; i < desc.numInputs; ++i) {
    const MetalTensor* mt = tensors_.get(desc.inputs[i]);
    if (!mt || !mt->tensor) {
      Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "invalid ML input tensor");
      return {};
    }
    NS::SharedPtr<MTL::TensorExtents> extents = makeTensorExtents(mt->dimensions, mt->rank);
    pd->setInputDimensions(extents.get(), NS::Integer(i));
  }

  NS::SharedPtr<MTL4::MachineLearningPipelineState> state = NS::TransferPtr(compiler_->newMachineLearningPipelineState(pd.get(), &error));
  if (!state) {
    LLOGE("createMachineLearningPipeline: compile failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newMachineLearningPipelineState failed");
    return {};
  }

  if (getenv("LVKM_ML_REFLECT")) {
    if (MTL4::MachineLearningPipelineReflection* refl = state->reflection()) {
      NS::Array* bindings = refl->bindings();
      const NS::UInteger n = bindings ? bindings->count() : 0;
      LLOGL("ML pipeline '%s' has %lu bindings:", desc.debugName ? desc.debugName : "", (unsigned long)n);
      for (NS::UInteger i = 0; i < n; ++i) {
        const MTL::Binding* b = bindings->object<MTL::Binding>(i);
        LLOGL("  [%lu] index=%lu type=%ld name='%s'",
              (unsigned long)i,
              (unsigned long)b->index(),
              (long)b->type(),
              b->name() ? b->name()->utf8String() : "");
      }
    }
  }

  MetalMachineLearningPipeline mp;
  mp.pipeline = state;
  mp.library = library;
  const NS::UInteger heapSize = state->intermediatesHeapSize();
  if (heapSize > 0) {
    NS::SharedPtr<MTL::HeapDescriptor> hd = ns::make<MTL::HeapDescriptor>();
    hd->setType(MTL::HeapTypePlacement);
    hd->setStorageMode(MTL::StorageModePrivate);
    hd->setSize(heapSize);
    mp.intermediatesHeap = NS::TransferPtr(device_->newHeap(hd.get()));
    if (!mp.intermediatesHeap) {
      Result::setResult(outResult, Result::Code::RuntimeError, "intermediates heap alloc failed");
      return {};
    }
    ns::setLabel(mp.intermediatesHeap.get(), "lvk-metal.ml.intermediates");
    addResident(mp.intermediatesHeap.get());
  }
  addResident(state.get());

  NS::SharedPtr<MTL4::ArgumentTableDescriptor> atd = ns::make<MTL4::ArgumentTableDescriptor>();
  ns::setLabel(atd.get(), desc.debugName);
  atd->setMaxBufferBindCount(desc.numInputs + desc.numOutputs);
  mp.argTable = NS::TransferPtr(device_->newArgumentTable(atd.get(), &error));
  if (!mp.argTable) {
    LLOGE("createMachineLearningPipeline: argument table failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newArgumentTable failed");
    return {};
  }
  for (uint32_t i = 0; i < desc.numInputs; ++i) {
    if (const MetalTensor* mt = tensors_.get(desc.inputs[i]))
      mp.argTable->setResource(mt->tensor->gpuResourceID(), i);
  }
  for (uint32_t i = 0; i < desc.numOutputs; ++i) {
    const MetalTensor* mt = tensors_.get(desc.outputs[i]);
    if (!mt || !mt->tensor) {
      Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "invalid ML output tensor");
      return {};
    }
    mp.argTable->setResource(mt->tensor->gpuResourceID(), desc.numInputs + i);
  }
  return {this, mlPipelines_.create(std::move(mp))};
}

Holder<ArgumentTableHandle> MetalContext::createArgumentTable(const ArgumentTableDesc& desc, Result* outResult) {
  NS::SharedPtr<MTL4::ArgumentTableDescriptor> td = ns::make<MTL4::ArgumentTableDescriptor>();
  ns::setLabel(td.get(), desc.debugName);
  td->setMaxBufferBindCount(desc.numKinds);

  NS::Error* error = nullptr;
  NS::SharedPtr<MTL4::ArgumentTable> table = NS::TransferPtr(device_->newArgumentTable(td.get(), &error));
  if (!table) {
    LLOGE("createArgumentTable failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    Result::setResult(outResult, Result::Code::RuntimeError, "newArgumentTable failed");
    return {};
  }

  MetalArgumentTable at;
  at.table = table;
  at.numKinds = desc.numKinds;
  for (uint32_t i = 0; i < desc.numKinds; ++i) {
    at.kinds[i] = desc.kinds[i];
    switch (desc.kinds[i]) {
    case ArgumentKind::Buffers:
      table->setAddress(bufferHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Textures2D:
    case ArgumentKind::Textures3D:
    case ArgumentKind::TexturesCube:
    case ArgumentKind::Images2D:
    case ArgumentKind::Images3D:
    case ArgumentKind::Images3DUint:
    case ArgumentKind::TexturesDepth2D:
      table->setAddress(textureHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::TexturesYUVChroma:
      table->setAddress(yuvChromaHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Samplers:
    case ArgumentKind::SamplersComparison:
      table->setAddress(samplerHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::AccelStructs:
      table->setAddress(accelStructHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Constants:
      at.constantsIndex = i;
      break;
    }
  }
  return {this, argumentTables_.create(std::move(at))};
}

static MTL::AttributeFormat toMTLAttributeFormat(VertexFormat fmt) {
  switch (fmt) {
  case VertexFormat_Float1:
    return MTL::AttributeFormatFloat;
  case VertexFormat_Float2:
    return MTL::AttributeFormatFloat2;
  case VertexFormat_Float3:
    return MTL::AttributeFormatFloat3;
  case VertexFormat_Float4:
    return MTL::AttributeFormatFloat4;
  case VertexFormat_HalfFloat2:
    return MTL::AttributeFormatHalf2;
  case VertexFormat_HalfFloat3:
    return MTL::AttributeFormatHalf3;
  case VertexFormat_HalfFloat4:
    return MTL::AttributeFormatHalf4;
  default:
    return MTL::AttributeFormatFloat3;
  }
}

static uint32_t vertexFormatByteSize(VertexFormat fmt) {
  switch (fmt) {
  case VertexFormat_Float1:
    return 4;
  case VertexFormat_Float2:
    return 8;
  case VertexFormat_Float3:
    return 12;
  case VertexFormat_Float4:
    return 16;
  case VertexFormat_HalfFloat2:
    return 4;
  case VertexFormat_HalfFloat3:
    return 6;
  case VertexFormat_HalfFloat4:
    return 8;
  default:
    return 12;
  }
}

static MTL::AccelerationStructureInstanceOptions toMTLInstanceOptions(uint32_t flags) {
  uint32_t opt = 0;
  if (flags & AccelStructInstanceFlagBits_TriangleFacingCullDisable)
    opt |= MTL::AccelerationStructureInstanceOptionDisableTriangleCulling;
  if (flags & AccelStructInstanceFlagBits_TriangleFlipFacing)
    opt |= MTL::AccelerationStructureInstanceOptionTriangleFrontFacingWindingCounterClockwise;
  if (flags & AccelStructInstanceFlagBits_ForceOpaque)
    opt |= MTL::AccelerationStructureInstanceOptionOpaque;
  if (flags & AccelStructInstanceFlagBits_ForceNoOpaque)
    opt |= MTL::AccelerationStructureInstanceOptionNonOpaque;
  return MTL::AccelerationStructureInstanceOptions(opt);
}

static NS::SharedPtr<MTL4::PrimitiveAccelerationStructureDescriptor> makeTriangleBlasDescriptor(const AccelStructDesc& desc,
                                                                                                MTL::GPUAddress vbAddr,
                                                                                                MTL::GPUAddress ibAddr,
                                                                                                MTL::GPUAddress xformAddr) {
  NS::SharedPtr<MTL4::AccelerationStructureTriangleGeometryDescriptor> geom =
      ns::make<MTL4::AccelerationStructureTriangleGeometryDescriptor>();
  const uint32_t vstride = desc.vertexStride ? desc.vertexStride : vertexFormatByteSize(desc.vertexFormat);
  const uint32_t triCount = desc.buildRange.primitiveCount;
  geom->setVertexBuffer(MTL4::BufferRange(vbAddr, uint64_t(vstride) * desc.numVertices));
  geom->setVertexFormat(toMTLAttributeFormat(desc.vertexFormat));
  geom->setVertexStride(vstride);
  if (ibAddr) {
    const uint32_t idxSize = desc.indexFormat == IndexFormat_UI16 ? 2 : 4;
    geom->setIndexBuffer(MTL4::BufferRange(ibAddr, uint64_t(idxSize) * triCount * 3));
    geom->setIndexType(toMTLIndexType(desc.indexFormat));
  }
  geom->setTriangleCount(triCount);
  geom->setOpaque(desc.geometryFlags & AccelStructGeometryFlagBits_Opaque);
  if (xformAddr) {
    geom->setTransformationMatrixBuffer(MTL4::BufferRange(xformAddr, sizeof(float) * 12));
    geom->setTransformationMatrixLayout(MTL::MatrixLayoutRowMajor);
  }
  NS::SharedPtr<MTL4::PrimitiveAccelerationStructureDescriptor> prim = ns::make<MTL4::PrimitiveAccelerationStructureDescriptor>();
  const NS::Object* geoms[] = {geom.get()};
  prim->setGeometryDescriptors(NS::Array::array(geoms, 1));
  return prim;
}

static NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> makeInstanceTlasDescriptor(MTL::GPUAddress instAddr,
                                                                                               uint32_t numInstances) {
  const uint32_t stride = sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor);
  NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> d = ns::make<MTL4::InstanceAccelerationStructureDescriptor>();
  d->setInstanceCount(numInstances);
  d->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
  d->setInstanceDescriptorStride(stride);
  d->setInstanceDescriptorBuffer(MTL4::BufferRange(instAddr, uint64_t(stride) * (numInstances ? numInstances : 1)));
  d->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutColumnMajor);
  return d;
}

static NS::SharedPtr<MTL4::IndirectInstanceAccelerationStructureDescriptor> makeIndirectInstanceTlasDescriptor(MTL::GPUAddress instAddr,
                                                                                                               MTL::GPUAddress countAddr,
                                                                                                               uint32_t maxInstances) {
  const uint32_t stride = sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor);
  NS::SharedPtr<MTL4::IndirectInstanceAccelerationStructureDescriptor> d =
      ns::make<MTL4::IndirectInstanceAccelerationStructureDescriptor>();
  d->setMaxInstanceCount(maxInstances);
  d->setInstanceDescriptorType(MTL::AccelerationStructureInstanceDescriptorTypeIndirect);
  d->setInstanceDescriptorStride(stride);
  d->setInstanceDescriptorBuffer(MTL4::BufferRange(instAddr, uint64_t(stride) * (maxInstances ? maxInstances : 1)));
  d->setInstanceTransformationMatrixLayout(MTL::MatrixLayoutColumnMajor);
  if (countAddr)
    d->setInstanceCountBuffer(MTL4::BufferRange(countAddr, sizeof(uint32_t)));
  return d;
}

static void translateInstances(const MetalBuffer* src, MTL::Buffer* dst, uint32_t count) {
  if (!src || !src->buffer || src->buffer->storageMode() != MTL::StorageModeShared || !dst)
    return;
  const AccelStructInstance* in = static_cast<const AccelStructInstance*>(src->buffer->contents());
  MTL::IndirectAccelerationStructureInstanceDescriptor* out =
      static_cast<MTL::IndirectAccelerationStructureInstanceDescriptor*>(dst->contents());
  for (uint32_t i = 0; i < count; ++i) {
    const AccelStructInstance& s = in[i];
    MTL::IndirectAccelerationStructureInstanceDescriptor& d = out[i];
    for (int c = 0; c < 4; ++c)
      for (int r = 0; r < 3; ++r)
        d.transformationMatrix.columns[c][r] = s.transform.matrix[r][c];
    d.options = toMTLInstanceOptions(s.flags);
    d.mask = s.mask;
    d.intersectionFunctionTableOffset = s.instanceShaderBindingTableRecordOffset;
    d.userID = s.instanceCustomIndex;
    d.accelerationStructureID._impl = s.accelerationStructureReference;
  }
}

void MetalContext::buildAccelStructImmediate(MTL::AccelerationStructure* accel,
                                             const MTL4::AccelerationStructureDescriptor* desc,
                                             MTL::Buffer* scratch) {
  flushResidency();
  const MetalImmediateCommands::CommandBufferWrapper& wrapper = immediate_->acquire();
  MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
  enc->buildAccelerationStructure(accel, desc, MTL4::BufferRange(scratch->gpuAddress(), scratch->length()));
  enc->barrierAfterStages(
      MTL::StageAccelerationStructure, MTL::StageDispatch | MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
  immediate_->wait(immediate_->submit(wrapper));
}

Holder<AccelStructHandle> MetalContext::createAccelerationStructure(const AccelStructDesc& desc, Result* outResult) {
  MetalAccelStruct as;
  as.type = desc.type;

  NS::SharedPtr<MTL4::PrimitiveAccelerationStructureDescriptor> blasDesc;
  NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> tlasDesc;
  NS::SharedPtr<MTL4::IndirectInstanceAccelerationStructureDescriptor> gpuTlasDesc;
  MTL4::AccelerationStructureDescriptor* asDesc = nullptr;

  if (desc.type == AccelStructType_BLAS) {
    const MetalBuffer* vb = buffers_.get(desc.vertexBuffer);
    if (!vb || !vb->buffer) {
      Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "BLAS requires a vertex buffer");
      return {};
    }
    const MetalBuffer* ib = buffers_.get(desc.indexBuffer);
    const MetalBuffer* xf = buffers_.get(desc.transformBuffer);
    blasDesc = makeTriangleBlasDescriptor(
        desc, vb->buffer->gpuAddress(), ib && ib->buffer ? ib->buffer->gpuAddress() : 0, xf && xf->buffer ? xf->buffer->gpuAddress() : 0);
    asDesc = blasDesc.get();
  } else if (desc.type == AccelStructType_TLAS) {
    as.numInstances = desc.buildRange.primitiveCount;
    as.indirectTLAS = desc.instancesBuffer.empty();
    if (as.indirectTLAS) {
      gpuTlasDesc = makeIndirectInstanceTlasDescriptor(0, 0, as.numInstances);
      asDesc = gpuTlasDesc.get();
    } else {
      const uint64_t instBytes =
          uint64_t(sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor)) * (as.numInstances ? as.numInstances : 1);
      as.instanceDescriptors = NS::TransferPtr(device_->newBuffer(instBytes, MTL::ResourceStorageModeShared));
      ns::setLabel(as.instanceDescriptors.get(), "lvk-metal.tlas.instances");
      addResident(as.instanceDescriptors.get());
      translateInstances(buffers_.get(desc.instancesBuffer), as.instanceDescriptors.get(), as.numInstances);
      tlasDesc = makeInstanceTlasDescriptor(as.instanceDescriptors->gpuAddress(), as.numInstances);
      as.tlasDescriptor = tlasDesc;
      asDesc = tlasDesc.get();
    }
  } else {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "unsupported acceleration structure type");
    return {};
  }

  const MTL::AccelerationStructureSizes sizes = device_->accelerationStructureSizes(asDesc);
  if (!sizes.accelerationStructureSize) {
    LLOGE("accelerationStructureSizes returned 0 for '%s'", desc.debugName ? desc.debugName : "");
    Result::setResult(outResult, Result::Code::RuntimeError, "accelerationStructureSizes failed");
    return {};
  }
  as.buildScratchSize = sizes.buildScratchBufferSize;
  as.accel = NS::TransferPtr(device_->newAccelerationStructure(sizes.accelerationStructureSize));
  if (!as.accel) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newAccelerationStructure failed");
    return {};
  }
  ns::setLabel(as.accel.get(), desc.debugName);
  addResident(as.accel.get());

  as.scratch = NS::TransferPtr(device_->newBuffer(std::max<uint64_t>(sizes.buildScratchBufferSize, 16), MTL::ResourceStorageModePrivate));
  ns::setLabel(as.scratch.get(), "lvk-metal.accelstruct.scratch");
  addResident(as.scratch.get());

  if (!as.indirectTLAS)
    buildAccelStructImmediate(as.accel.get(), asDesc, as.scratch.get());

  const MTL::ResourceID rid = as.accel->gpuResourceID();
  const AccelStructHandle handle = accelStructs_.create(std::move(as));
  ensureAccelStructCapacity(handle.index());
  static_cast<MTL::ResourceID*>(accelStructHeap_->contents())[handle.index()] = rid;
  return {this, handle};
}

AccelStructSizes MetalContext::getAccelStructSizes(const AccelStructDesc& desc, Result* outResult) const {
  MTL::Device* dev = device_.get();
  MTL::AccelerationStructureSizes sizes = {};
  if (desc.type == AccelStructType_BLAS) {
    const MetalBuffer* vb = buffers_.get(desc.vertexBuffer);
    if (vb && vb->buffer) {
      const MetalBuffer* ib = buffers_.get(desc.indexBuffer);
      const MetalBuffer* xf = buffers_.get(desc.transformBuffer);
      NS::SharedPtr<MTL4::PrimitiveAccelerationStructureDescriptor> d = makeTriangleBlasDescriptor(
          desc, vb->buffer->gpuAddress(), ib && ib->buffer ? ib->buffer->gpuAddress() : 0, xf && xf->buffer ? xf->buffer->gpuAddress() : 0);
      sizes = dev->accelerationStructureSizes(d.get());
    }
  } else if (desc.type == AccelStructType_TLAS) {
    if (desc.instancesBuffer.empty()) {
      NS::SharedPtr<MTL4::IndirectInstanceAccelerationStructureDescriptor> d =
          makeIndirectInstanceTlasDescriptor(0, 0, desc.buildRange.primitiveCount);
      sizes = dev->accelerationStructureSizes(d.get());
    } else {
      NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> d = makeInstanceTlasDescriptor(0, desc.buildRange.primitiveCount);
      sizes = dev->accelerationStructureSizes(d.get());
    }
  }
  Result::setResult(outResult, Result());
  return AccelStructSizes{
      .accelerationStructureSize = sizes.accelerationStructureSize,
      .updateScratchSize = sizes.refitScratchBufferSize,
      .buildScratchSize = sizes.buildScratchBufferSize,
  };
}

uint32_t MetalContext::indirectTLASInstanceDescriptorSize() const {
  return sizeof(MTL::IndirectAccelerationStructureInstanceDescriptor);
}

uint64_t MetalContext::gpuAddress(AccelStructHandle handle) const {
  const MetalAccelStruct* as = accelStructs_.get(handle);
  return as && as->accel ? as->accel->gpuResourceID()._impl : 0;
}

void MetalContext::destroy(AccelStructHandle handle) {
  if (!accelStructs_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalAccelStruct* as = accelStructs_.get(handle)) {
      removeResident(as->accel.get());
      if (as->scratch)
        removeResident(as->scratch.get());
      if (as->instanceDescriptors)
        removeResident(as->instanceDescriptors.get());
    }
    accelStructs_.destroy(handle);
  });
}

void MetalContext::destroy(RenderPipelineHandle handle) {
  if (!renderPipelines_.get(handle))
    return;
  deferredTask([this, handle]() { renderPipelines_.destroy(handle); });
}
void MetalContext::destroy(ComputePipelineHandle handle) {
  if (!computePipelines_.get(handle))
    return;
  deferredTask([this, handle]() { computePipelines_.destroy(handle); });
}
void MetalContext::destroy(ShaderModuleHandle handle) {
  if (!shaderModules_.get(handle))
    return;
  deferredTask([this, handle]() { shaderModules_.destroy(handle); });
}
void MetalContext::destroy(SamplerHandle handle) {
  if (!samplers_.get(handle))
    return;
  deferredTask([this, handle]() { samplers_.destroy(handle); });
}
void MetalContext::destroy(BufferHandle handle) {
  if (!buffers_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalBuffer* mb = buffers_.get(handle)) {
      removeResident(mb->buffer.get());
      if (mb->icb)
        removeResident(mb->icb.get());
      if (mb->icbContainer)
        removeResident(mb->icbContainer.get());
    }
    buffers_.destroy(handle);
  });
}
void MetalContext::destroy(TextureHandle handle) {
  if (!textures_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalImage* img = textures_.get(handle)) {
      if (img->texture->storageMode() != MTL::StorageModeMemoryless)
        removeResident(img->texture.get());
      if (img->yuvChroma)
        removeResident(img->yuvChroma.get());
    }
    textures_.destroy(handle);
  });
}
void MetalContext::destroy(ArgumentTableHandle handle) {
  if (!argumentTables_.get(handle))
    return;
  deferredTask([this, handle]() { argumentTables_.destroy(handle); });
}

void MetalContext::destroy(TilePipelineHandle handle) {
  if (!tilePipelines_.get(handle))
    return;
  deferredTask([this, handle]() { tilePipelines_.destroy(handle); });
}

void MetalContext::destroy(TensorHandle handle) {
  if (!tensors_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalTensor* mt = tensors_.get(handle))
      removeResident(mt->tensor.get());
    tensors_.destroy(handle);
  });
}

void MetalContext::destroy(MLPipelineHandle handle) {
  if (!mlPipelines_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalMachineLearningPipeline* mp = mlPipelines_.get(handle)) {
      removeResident(mp->pipeline.get());
      if (mp->intermediatesHeap)
        removeResident(mp->intermediatesHeap.get());
    }
    mlPipelines_.destroy(handle);
  });
}

bool MetalContext::startGpuCapture(const char* outputPath) {
  MTL::CaptureManager* mgr = MTL::CaptureManager::sharedCaptureManager();
  if (outputPath && !mgr->supportsDestination(MTL::CaptureDestinationGPUTraceDocument)) {
    LLOGW("GPU trace capture unsupported; relaunch with MTL_CAPTURE_ENABLED=1");
    return false;
  }
  NS::SharedPtr<MTL::CaptureDescriptor> desc = NS::TransferPtr(MTL::CaptureDescriptor::alloc()->init());
  desc->setCaptureObject(device_.get());
  desc->setDestination(outputPath ? MTL::CaptureDestinationGPUTraceDocument : MTL::CaptureDestinationDeveloperTools);
  if (outputPath) {
    desc->setOutputURL(NS::URL::fileURLWithPath(ns::string(outputPath)));
  }
  NS::Error* error = nullptr;
  if (!mgr->startCapture(desc.get(), &error)) {
    LLOGW("startCapture failed: %s", error ? error->localizedDescription()->utf8String() : "unknown");
    return false;
  }
  return true;
}

void MetalContext::stopGpuCapture() {
  MTL::CaptureManager::sharedCaptureManager()->stopCapture();
}

Result MetalContext::upload(BufferHandle handle, const void* data, size_t size, size_t offset) {
  MetalBuffer* mb = buffers_.get(handle);
  if (!mb || !mb->buffer)
    return Result(Result::Code::ArgumentOutOfRange, "invalid buffer");
  if (mb->buffer->storageMode() == MTL::StorageModeShared) {
    std::memcpy(static_cast<uint8_t*>(mb->buffer->contents()) + offset, data, size);
    return Result();
  }
  staging_->uploadBuffer(mb->buffer.get(), offset, size, data);
  return Result();
}

Result MetalContext::download(BufferHandle handle, void* data, size_t size, size_t offset) {
  const MetalBuffer* mb = buffers_.get(handle);
  if (!mb || !mb->buffer)
    return Result(Result::Code::ArgumentOutOfRange, "invalid buffer");
  if (mb->buffer->storageMode() == MTL::StorageModeShared) {
    std::memcpy(data, static_cast<const uint8_t*>(mb->buffer->contents()) + offset, size);
    return Result();
  }
  NS::SharedPtr<MTL::Buffer> staging = NS::TransferPtr(device_->newBuffer(size, MTL::ResourceStorageModeShared));
  if (!staging)
    return Result(Result::Code::RuntimeError, "download staging buffer alloc failed");
  residencySet_->addAllocation(staging.get());
  residencySet_->commit();
  residencySet_->requestResidency();
  const MetalImmediateCommands::CommandBufferWrapper& wrapper = immediate_->acquire();
  MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
  enc->copyFromBuffer(mb->buffer.get(), offset, staging.get(), 0, size);
  enc->endEncoding();
  const SubmitHandle sh = immediate_->submit(wrapper);
  immediate_->wait(sh);
  residencySet_->removeAllocation(staging.get());
  residencySet_->commit();
  std::memcpy(data, staging->contents(), size);
  return Result();
}

uint8_t* MetalContext::getMappedPtr(BufferHandle handle) const {
  const MetalBuffer* mb = buffers_.get(handle);
  if (!mb || !mb->buffer || mb->buffer->storageMode() != MTL::StorageModeShared)
    return nullptr;
  return static_cast<uint8_t*>(mb->buffer->contents());
}

uint64_t MetalContext::gpuAddress(BufferHandle handle, size_t offset) const {
  const MetalBuffer* mb = buffers_.get(handle);
  return mb && mb->buffer ? mb->buffer->gpuAddress() + offset : 0;
}

Result MetalContext::upload(TextureHandle handle, const TextureRangeDesc& range, const void* data, uint32_t bufferRowLength) {
  MetalImage* img = textures_.get(handle);
  if (!img || !img->texture)
    return Result(Result::Code::ArgumentOutOfRange, "invalid texture");
  const bool is3D = img->texture->textureType() == MTL::TextureType3D;
  const uint32_t depth = is3D ? range.dimensions.depth : 1;
  const uint32_t z = is3D ? uint32_t(range.offset.z) : 0;
  const uint32_t slice = is3D ? 0 : range.layer;
  if (img->texture->storageMode() == MTL::StorageModePrivate) {
    staging_->uploadTexture(img->texture.get(),
                            uint32_t(range.offset.x),
                            uint32_t(range.offset.y),
                            z,
                            range.dimensions.width,
                            range.dimensions.height,
                            depth,
                            slice,
                            range.mipLevel,
                            data,
                            bufferRowLength);
    return Result();
  }
  const NS::UInteger bpp = bytesPerMetalPixel(img->texture->pixelFormat());
  const NS::UInteger bytesPerRow = NS::UInteger(bufferRowLength ? bufferRowLength : range.dimensions.width) * bpp;
  const NS::UInteger bytesPerImage = is3D ? bytesPerRow * range.dimensions.height : 0;
  const MTL::Region region(uint32_t(range.offset.x), uint32_t(range.offset.y), z, range.dimensions.width, range.dimensions.height, depth);
  img->texture->replaceRegion(region, range.mipLevel, slice, data, bytesPerRow, bytesPerImage);
  return Result();
}

Result MetalContext::download(TextureHandle handle, const TextureRangeDesc& range, void* outData) {
  const MetalImage* img = textures_.get(handle);
  if (!img || !img->texture)
    return Result(Result::Code::ArgumentOutOfRange, "invalid texture");
  const uint32_t w = range.dimensions.width;
  const uint32_t h = range.dimensions.height;

  MTL::Texture* src = img->texture.get();
  if (src->storageMode() == MTL::StorageModeShared) {
    const MTL::Region region(uint32_t(range.offset.x), uint32_t(range.offset.y), w, h);
    src->getBytes(outData, NS::UInteger(w) * 4, region, range.mipLevel);
    return Result();
  }

  const NS::UInteger bytesPerRow = NS::UInteger(w) * 4;
  const NS::UInteger bytesPerImage = bytesPerRow * h;
  NS::SharedPtr<MTL::Buffer> staging = NS::TransferPtr(device_->newBuffer(bytesPerImage, MTL::ResourceStorageModeShared));
  if (!staging)
    return Result(Result::Code::RuntimeError, "download staging buffer alloc failed");

  const bool manageSrcResidency = img->isSwapchainImage;
  residencySet_->addAllocation(staging.get());
  if (manageSrcResidency)
    residencySet_->addAllocation(src);
  residencySet_->commit();
  residencySet_->requestResidency();

  const MetalImmediateCommands::CommandBufferWrapper& wrapper = immediate_->acquire();
  MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
  enc->copyFromTexture(src,
                       range.layer,
                       range.mipLevel,
                       MTL::Origin(uint32_t(range.offset.x), uint32_t(range.offset.y), 0),
                       MTL::Size(w, h, 1),
                       staging.get(),
                       0,
                       bytesPerRow,
                       bytesPerImage);
  enc->endEncoding();
  const SubmitHandle sh = immediate_->submit(wrapper);
  immediate_->wait(sh);

  residencySet_->removeAllocation(staging.get());
  if (manageSrcResidency)
    residencySet_->removeAllocation(src);
  residencySet_->commit();

  std::memcpy(outData, staging->contents(), bytesPerImage);
  return Result();
}

Dimensions MetalContext::getDimensions(TextureHandle handle) const {
  const MetalImage* img = textures_.get(handle);
  if (!img)
    return {};
  return {img->width, img->height, 1};
}

float MetalContext::getAspectRatio(TextureHandle handle) const {
  const MetalImage* img = textures_.get(handle);
  if (!img || !img->height)
    return 1.0f;
  return float(img->width) / float(img->height);
}

Format MetalContext::getFormat(TextureHandle handle) const {
  const MetalImage* img = textures_.get(handle);
  return img ? toLVKFormat(img->format) : Format_Invalid;
}

MTL::GPUAddress MetalContext::writePushConstants(const void* data,
                                                 size_t size,
                                                 size_t offset,
                                                 const MetalImmediateCommands::CommandBufferWrapper* wrapper) {
  LVK_ASSERT(wrapper);
  LVK_ASSERT(offset + size <= pushConstantsSize_);
  const uint32_t regionEnd = (wrapper->bufferIndex + 1) * constantsFrameRegionBytes_;
  if (constantsCursor_ + pushConstantsSize_ > regionEnd)
    growConstantsRing(wrapper);
  const uint32_t slot = constantsCursor_;
  std::memcpy(static_cast<uint8_t*>(constantsRing_->contents()) + slot + offset, data, size);
  constantsCursor_ += pushConstantsSize_;
  return constantsRing_->gpuAddress() + slot;
}

NS::SharedPtr<MTL::DepthStencilState> MetalContext::makeDepthStencilState(const DepthState& depth,
                                                                          const StencilState& front,
                                                                          const StencilState& back) {
  NS::SharedPtr<MTL::DepthStencilDescriptor> dsd = ns::make<MTL::DepthStencilDescriptor>();
  dsd->setDepthCompareFunction(toMTLCompareFunction(depth.compareOp));
  dsd->setDepthWriteEnabled(depth.isDepthWriteEnabled);

  const auto fillStencil = [](MTL::StencilDescriptor* sd, const StencilState& s) {
    sd->setStencilCompareFunction(toMTLCompareFunction(s.stencilCompareOp));
    sd->setStencilFailureOperation(toMTLStencilOperation(s.stencilFailureOp));
    sd->setDepthFailureOperation(toMTLStencilOperation(s.depthFailureOp));
    sd->setDepthStencilPassOperation(toMTLStencilOperation(s.depthStencilPassOp));
    sd->setReadMask(s.readMask);
    sd->setWriteMask(s.writeMask);
  };
  NS::SharedPtr<MTL::StencilDescriptor> f = ns::make<MTL::StencilDescriptor>();
  fillStencil(f.get(), front);
  dsd->setFrontFaceStencil(f.get());
  NS::SharedPtr<MTL::StencilDescriptor> b = ns::make<MTL::StencilDescriptor>();
  fillStencil(b.get(), back);
  dsd->setBackFaceStencil(b.get());

  return NS::TransferPtr(device_->newDepthStencilState(dsd.get()));
}

MTL::DepthStencilState* MetalContext::getDepthStencilState(const DepthState& depth, const StencilState& front, const StencilState& back) {
  const DepthStencilStateKey key = {
      .depthCompareOp = uint32_t(depth.compareOp),
      .depthWriteEnabled = uint32_t(depth.isDepthWriteEnabled),
      .frontStencilFailureOp = uint32_t(front.stencilFailureOp),
      .frontDepthFailureOp = uint32_t(front.depthFailureOp),
      .frontDepthStencilPassOp = uint32_t(front.depthStencilPassOp),
      .frontStencilCompareOp = uint32_t(front.stencilCompareOp),
      .frontReadMask = front.readMask,
      .frontWriteMask = front.writeMask,
      .backStencilFailureOp = uint32_t(back.stencilFailureOp),
      .backDepthFailureOp = uint32_t(back.depthFailureOp),
      .backDepthStencilPassOp = uint32_t(back.depthStencilPassOp),
      .backStencilCompareOp = uint32_t(back.stencilCompareOp),
      .backReadMask = back.readMask,
      .backWriteMask = back.writeMask,
  };
  const auto it = depthStencilCache_.find(key);
  if (it != depthStencilCache_.end())
    return it->second.get();
  NS::SharedPtr<MTL::DepthStencilState> dss = makeDepthStencilState(depth, front, back);
  MTL::DepthStencilState* raw = dss.get();
  depthStencilCache_[key] = std::move(dss);
  return raw;
}

TextureHandle MetalContext::getCurrentSwapchainTexture() {
  MetalImage* img = textures_.get(swapchainTextureHandle_);
  if (!img->drawable) {
    CA::MetalDrawable* drawable = metalLayer_->nextDrawable();
    if (!drawable)
      return {};
    drawable->retain();
    img->drawable = drawable;
    img->texture = ns::retain(drawable->texture());
    img->format = swapchainFormat_;
    img->width = width_;
    img->height = height_;
  }
  return swapchainTextureHandle_;
}

Format MetalContext::getSwapchainFormat() const {
  return toLVKFormat(swapchainFormat_);
}

void MetalContext::recreateSwapchain(int newWidth, int newHeight) {
  width_ = uint32_t(newWidth);
  height_ = uint32_t(newHeight);
  if (metalLayer_)
    metalLayer_->setDrawableSize(CGSizeMake(newWidth, newHeight));
}

void CommandBuffer::cmdBeginRendering(const lvk::RenderPass& renderPass,
                                      const lvk::Framebuffer& framebuffer,
                                      const lvk::Dependencies& deps) {
  endMachineLearningEncoder();
  endComputeEncoder();
  pendingMLBarrier_ = false;
  ctx_->encodeIndirectDraws(deps, wrapper_);

  NS::SharedPtr<MTL4::RenderPassDescriptor> rpd = ns::make<MTL4::RenderPassDescriptor>();

  const uint32_t viewCount = renderPass.viewMask ? uint32_t(__builtin_popcount(renderPass.viewMask)) : 1;
  const bool multiview = viewCount > 1;
  LVK_ASSERT_MSG(viewCount <= 32, "viewMask has more than 32 views");

  uint32_t rtWidth = 0;
  uint32_t rtHeight = 0;
  const uint32_t nColor = framebuffer.getNumColorAttachments();
  for (uint32_t i = 0; i < nColor; ++i) {
    const MetalImage* img = ctx_->getImage(framebuffer.color[i].texture);
    LVK_ASSERT(img && img->texture);
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(i);
    att->setTexture(img->texture.get());
    if (!multiview)
      att->setSlice(renderPass.color[i].layer);
    att->setLevel(renderPass.color[i].level);
    att->setLoadAction(toLoadAction(renderPass.color[i].loadOp));
    const bool resolve = !framebuffer.color[i].resolveTexture.empty();
    if (resolve) {
      const MetalImage* resolveImg = ctx_->getImage(framebuffer.color[i].resolveTexture);
      LVK_ASSERT(resolveImg && resolveImg->texture);
      att->setResolveTexture(resolveImg->texture.get());
    }
    att->setStoreAction(toStoreAction(renderPass.color[i].storeOp, resolve));
    const float* c = renderPass.color[i].clearColor.float32;
    att->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
    rtWidth = img->width;
    rtHeight = img->height;
  }

  if (framebuffer.depthStencil.texture) {
    const MetalImage* img = ctx_->getImage(framebuffer.depthStencil.texture);
    LVK_ASSERT(img && img->texture);
    const bool resolve = !framebuffer.depthStencil.resolveTexture.empty();
    const MetalImage* resolveImg = resolve ? ctx_->getImage(framebuffer.depthStencil.resolveTexture) : nullptr;
    MTL::RenderPassDepthAttachmentDescriptor* att = rpd->depthAttachment();
    att->setTexture(img->texture.get());
    if (!multiview)
      att->setSlice(renderPass.depth.layer);
    att->setLevel(renderPass.depth.level);
    att->setLoadAction(toLoadAction(renderPass.depth.loadOp));
    if (resolve) {
      LVK_ASSERT(resolveImg && resolveImg->texture);
      att->setResolveTexture(resolveImg->texture.get());
      att->setDepthResolveFilter(toMTLDepthResolveFilter(renderPass.depth.resolveMode));
    }
    att->setStoreAction(toStoreAction(renderPass.depth.storeOp, resolve));
    att->setClearDepth(renderPass.depth.clearDepth);
    if (formatHasStencil(img->format)) {
      MTL::RenderPassStencilAttachmentDescriptor* satt = rpd->stencilAttachment();
      satt->setTexture(img->texture.get());
      if (!multiview)
        satt->setSlice(renderPass.stencil.layer);
      satt->setLevel(renderPass.stencil.level);
      satt->setLoadAction(toLoadAction(renderPass.stencil.loadOp));
      if (resolve) {
        satt->setResolveTexture(resolveImg->texture.get());
        satt->setStencilResolveFilter(MTL::MultisampleStencilResolveFilterSample0);
      }
      satt->setStoreAction(toStoreAction(renderPass.stencil.storeOp, resolve));
      satt->setClearStencil(renderPass.stencil.clearStencil);
    }
    rtWidth = img->width;
    rtHeight = img->height;
  }

  rpd->setRenderTargetWidth(rtWidth);
  rpd->setRenderTargetHeight(rtHeight);
  if (multiview)
    rpd->setRenderTargetArrayLength(viewCount);

  encoder_ = wrapper_->cmdBuf.get()->renderCommandEncoder(rpd.get());
  depthStencilDirty_ = true;
  lastDepthStencilState_ = nullptr;
  depthBiasEnabled_ = false;
  encoder_->setStencilReferenceValue(stencilRef_);

  if (multiview) {
    MTL::VertexAmplificationViewMapping mappings[32] = {};
    for (uint32_t v = 0; v < viewCount; ++v)
      mappings[v].renderTargetArrayIndexOffset = v;
    encoder_->setVertexAmplificationCount(viewCount, mappings);
  }

  const MTL::Stages shaderReadStages = MTL::StageVertex | MTL::StageFragment;
  if (!deps.sampledImages.empty()) {
    encoder_->barrierAfterQueueStages(
        MTL::StageFragment | MTL::StageBlit | MTL::StageDispatch, shaderReadStages, MTL4::VisibilityOptionDevice);
  }
  if (!deps.storageImages.empty()) {
    encoder_->barrierAfterQueueStages(MTL::StageFragment | MTL::StageDispatch, shaderReadStages, MTL4::VisibilityOptionDevice);
  }
  if (!deps.buffers.empty()) {
    encoder_->barrierAfterQueueStages(MTL::StageDispatch | MTL::StageBlit, shaderReadStages, MTL4::VisibilityOptionDevice);
  }
  if (!deps.inputAttachments.empty()) {
    encoder_->barrierAfterQueueStages(MTL::StageFragment, MTL::StageFragment, MTL4::VisibilityOptionDevice);
  }

  isRendering_ = true;
  argTableOverridden_ = false;
  currentRenderPipelineIsMesh_ = false;
  ctx_->setRenderEncoderOpen(true);
  bindArgumentTableInternal(ctx_->defaultArgumentTable());
}

void CommandBuffer::cmdEndRendering() {
  if (encoder_) {
    encoder_->endEncoding();
    encoder_ = nullptr;
  }
  isRendering_ = false;
  ctx_->setRenderEncoderOpen(false);
}

void CommandBuffer::cmdBindRenderPipeline(RenderPipelineHandle handle) {
  const MetalRenderPipeline* p = ctx_->getRenderPipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  encoder_->setRenderPipelineState(p->pipeline.get());
  encoder_->setCullMode(p->cullMode);
  encoder_->setFrontFacingWinding(p->frontFace);
  encoder_->setTriangleFillMode(p->fillMode);
  topology_ = p->topology;
  frontStencil_ = p->frontStencil;
  backStencil_ = p->backStencil;
  meshObjectThreadsPerThreadgroup_ = p->objectThreadsPerThreadgroup;
  meshThreadsPerThreadgroup_ = p->meshThreadsPerThreadgroup;
  if (currentRenderPipelineIsMesh_ != p->isMesh) {
    currentRenderPipelineIsMesh_ = p->isMesh;
    bindArgumentTableInternal(currentArgTable_);
  }
  depthStencilDirty_ = true;
}

void CommandBuffer::endComputeEncoder() {
  if (computeEncoder_) {
    computeEncoder_->endEncoding();
    computeEncoder_ = nullptr;
  }
}

void CommandBuffer::endMachineLearningEncoder() {
  if (mlEncoder_) {
    mlEncoder_->endEncoding();
    mlEncoder_ = nullptr;
  }
  mlPipeline_ = nullptr;
}

MTL4::ComputeCommandEncoder* CommandBuffer::beginTransferEncoder() {
  LVK_ASSERT_MSG(!isRendering_, "transfer commands must be recorded outside cmdBeginRendering()/cmdEndRendering()");
  endMachineLearningEncoder();
  endComputeEncoder();
  return wrapper_->cmdBuf.get()->computeCommandEncoder();
}

void CommandBuffer::cmdCopyBuffer(BufferHandle srcBuffer, BufferHandle dstBuffer, size_t srcOffset, size_t dstOffset, size_t size) {
  const MetalBuffer* src = ctx_->getBuffer(srcBuffer);
  const MetalBuffer* dst = ctx_->getBuffer(dstBuffer);
  if (!src || !src->buffer || !dst || !dst->buffer)
    return;
  if (size == LVK_WHOLE_SIZE)
    size = src->size - srcOffset;
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->copyFromBuffer(src->buffer.get(), srcOffset, dst->buffer.get(), dstOffset, size);
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdFillBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, uint32_t data) {
  const MetalBuffer* b = ctx_->getBuffer(buffer);
  if (!b || !b->buffer)
    return;
  if (size == LVK_WHOLE_SIZE)
    size = b->size - bufferOffset;
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->fillBuffer(b->buffer.get(), NS::Range(bufferOffset, size), static_cast<uint8_t>(data));
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdUpdateBuffer(BufferHandle buffer, size_t bufferOffset, size_t size, const void* data) {
  const MetalBuffer* b = ctx_->getBuffer(buffer);
  if (!b || !b->buffer || !data || !size)
    return;
  const MetalContext::StagingAlloc staging = ctx_->writeUploadStaging(data, size, wrapper_);
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->copyFromBuffer(staging.buffer, staging.offset, b->buffer.get(), bufferOffset, size);
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdClearColorImage(TextureHandle tex, const ClearColorValue& value, const TextureLayers& layers) {
  const MetalImage* img = ctx_->getImage(tex);
  if (!img || !img->texture)
    return;
  LVK_ASSERT_MSG(!isRendering_, "cmdClearColorImage() must be recorded outside cmdBeginRendering()/cmdEndRendering()");
  endComputeEncoder();
  const uint32_t numLayers = layers.numLayers ? layers.numLayers : 1;
  for (uint32_t i = 0; i < numLayers; ++i) {
    NS::SharedPtr<MTL4::RenderPassDescriptor> rpd = ns::make<MTL4::RenderPassDescriptor>();
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(0);
    att->setTexture(img->texture.get());
    att->setSlice(layers.layer + i);
    att->setLevel(layers.mipLevel);
    att->setLoadAction(MTL::LoadActionClear);
    att->setStoreAction(MTL::StoreActionStore);
    att->setClearColor(MTL::ClearColor(value.float32[0], value.float32[1], value.float32[2], value.float32[3]));
    rpd->setRenderTargetWidth(std::max(1u, img->width >> layers.mipLevel));
    rpd->setRenderTargetHeight(std::max(1u, img->height >> layers.mipLevel));
    MTL4::RenderCommandEncoder* enc = wrapper_->cmdBuf.get()->renderCommandEncoder(rpd.get());
    enc->endEncoding();
  }
}

void CommandBuffer::cmdCopyImage(TextureHandle src,
                                 TextureHandle dst,
                                 const Dimensions& extent,
                                 const Offset3D& srcOffset,
                                 const Offset3D& dstOffset,
                                 const TextureLayers& srcLayers,
                                 const TextureLayers& dstLayers) {
  const MetalImage* s = ctx_->getImage(src);
  const MetalImage* d = ctx_->getImage(dst);
  if (!s || !s->texture || !d || !d->texture)
    return;
  const uint32_t numLayers = std::min(srcLayers.numLayers ? srcLayers.numLayers : 1, dstLayers.numLayers ? dstLayers.numLayers : 1);
  const uint32_t depth = extent.depth ? extent.depth : 1;
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  for (uint32_t i = 0; i < numLayers; ++i) {
    enc->copyFromTexture(s->texture.get(),
                         srcLayers.layer + i,
                         srcLayers.mipLevel,
                         MTL::Origin(uint32_t(srcOffset.x), uint32_t(srcOffset.y), uint32_t(srcOffset.z)),
                         MTL::Size(extent.width, extent.height, depth),
                         d->texture.get(),
                         dstLayers.layer + i,
                         dstLayers.mipLevel,
                         MTL::Origin(uint32_t(dstOffset.x), uint32_t(dstOffset.y), uint32_t(dstOffset.z)));
  }
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdGenerateMipmap(TextureHandle handle) {
  const MetalImage* img = ctx_->getImage(handle);
  if (!img || !img->texture)
    return;
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->generateMipmaps(img->texture.get());
  enc->barrierAfterStages(
      MTL::StageBlit | MTL::StageDispatch, MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdUpdateTLAS(AccelStructHandle handle, BufferHandle instancesBuffer) {
  const MetalAccelStruct* as = ctx_->getAccelStruct(handle);
  if (!as || !as->accel || as->type != AccelStructType_TLAS || !as->tlasDescriptor)
    return;
  translateInstances(ctx_->getBuffer(instancesBuffer), as->instanceDescriptors.get(), as->numInstances);
  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->buildAccelerationStructure(
      as->accel.get(), as->tlasDescriptor.get(), MTL4::BufferRange(as->scratch->gpuAddress(), as->scratch->length()));
  enc->barrierAfterStages(
      MTL::StageAccelerationStructure, MTL::StageDispatch | MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::cmdBuildIndirectTLAS(lvk::AccelStructHandle tlas,
                                         lvk::BufferHandle instanceDescriptors,
                                         uint32_t instanceCount,
                                         lvk::BufferHandle instanceCountBuffer) {
  const MetalAccelStruct* as = ctx_->getAccelStruct(tlas);
  if (!as || !as->accel || as->type != AccelStructType_TLAS)
    return;
  const MetalBuffer* inst = ctx_->getBuffer(instanceDescriptors);
  if (!inst || !inst->buffer)
    return;
  const MetalBuffer* cnt = ctx_->getBuffer(instanceCountBuffer);
  const MTL::GPUAddress instAddr = inst->buffer->gpuAddress();

  NS::SharedPtr<MTL4::IndirectInstanceAccelerationStructureDescriptor> gpuDesc;
  NS::SharedPtr<MTL4::InstanceAccelerationStructureDescriptor> fixedDesc;
  MTL4::AccelerationStructureDescriptor* desc = nullptr;
  if (cnt && cnt->buffer) {
    gpuDesc = makeIndirectInstanceTlasDescriptor(instAddr, cnt->buffer->gpuAddress(), instanceCount);
    desc = gpuDesc.get();
  } else {
    fixedDesc = makeInstanceTlasDescriptor(instAddr, instanceCount);
    desc = fixedDesc.get();
  }

  MTL4::ComputeCommandEncoder* enc = beginTransferEncoder();
  enc->barrierAfterQueueStages(MTL::StageDispatch, MTL::StageAccelerationStructure, MTL4::VisibilityOptionDevice);
  enc->buildAccelerationStructure(as->accel.get(), desc, MTL4::BufferRange(as->scratch->gpuAddress(), as->scratch->length()));
  enc->barrierAfterStages(
      MTL::StageAccelerationStructure, MTL::StageDispatch | MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice);
  enc->endEncoding();
}

void CommandBuffer::setArgumentTableOnActiveEncoder(MTL4::ArgumentTable* table) {
  if (mlEncoder_) {
    mlEncoder_->setArgumentTable(table);
  } else if (computeEncoder_) {
    computeEncoder_->setArgumentTable(table);
  } else if (encoder_) {
    const MTL::RenderStages stages =
        currentRenderPipelineIsMesh_
            ? static_cast<MTL::RenderStages>(MTL::RenderStageMesh | MTL::RenderStageObject | MTL::RenderStageFragment)
            : static_cast<MTL::RenderStages>(MTL::RenderStageVertex | MTL::RenderStageFragment);
    encoder_->setArgumentTable(table, stages);
  }
}

void CommandBuffer::cmdBindComputePipeline(ComputePipelineHandle handle) {
  const MetalComputePipeline* p = ctx_->getComputePipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  if (!p || !p->pipeline)
    return;
  if (!computeEncoder_) {
    computeEncoder_ = wrapper_->cmdBuf.get()->computeCommandEncoder();
    bindArgumentTableInternal(ctx_->defaultArgumentTable());
    if (pendingMLBarrier_) {
      computeEncoder_->barrierAfterQueueStages(
          MTL::StageMachineLearning, MTL::StageDispatch | MTL::StageBlit, MTL4::VisibilityOptionDevice);
      pendingMLBarrier_ = false;
    }
  }
  computeEncoder_->setComputePipelineState(p->pipeline.get());
  computeThreadgroupSize_ = p->threadgroupSize;
}

void CommandBuffer::cmdDispatch(const Dimensions& groupCount, const Dependencies& deps) {
  if (!computeEncoder_)
    return;
  computeEncoder_->dispatchThreadgroups(MTL::Size(groupCount.width, groupCount.height, groupCount.depth), computeThreadgroupSize_);
  if (!deps.storageImages.empty() || !deps.buffers.empty() || !deps.sampledImages.empty())
    computeEncoder_->barrierAfterStages(
        MTL::StageDispatch, MTL::StageDispatch | MTL::StageBlit | MTL::StageVertex | MTL::StageFragment, MTL4::VisibilityOptionDevice);
  endComputeEncoder();
}

void CommandBuffer::cmdDispatchIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, const Dependencies&) {
  if (!computeEncoder_)
    return;
  const MetalBuffer* b = ctx_->getBuffer(indirectBuffer);
  if (!b || !b->buffer)
    return;
  computeEncoder_->dispatchThreadgroups(b->buffer->gpuAddress() + indirectBufferOffset, computeThreadgroupSize_);
  endComputeEncoder();
}

void CommandBuffer::cmdBindDepthState(const DepthState& state) {
  depthState_ = state;
  depthStencilDirty_ = true;
}

void CommandBuffer::cmdSetStencilRef(uint32_t ref) {
  stencilRef_ = ref;
  if (encoder_)
    encoder_->setStencilReferenceValue(ref);
}

void CommandBuffer::applyDepthStencilState() {
  if (!encoder_ || !depthStencilDirty_)
    return;
  MTL::DepthStencilState* dss = ctx_->getDepthStencilState(depthState_, frontStencil_, backStencil_);
  if (dss && dss != lastDepthStencilState_) {
    encoder_->setDepthStencilState(dss);
    lastDepthStencilState_ = dss;
  }
  depthStencilDirty_ = false;
}

void CommandBuffer::bindArgumentTableInternal(ArgumentTableHandle handle) {
  currentArgTable_ = handle;
  const MetalArgumentTable* at = ctx_->getArgumentTable(handle);
  if (at && at->table)
    setArgumentTableOnActiveEncoder(at->table.get());
}

void CommandBuffer::resetArgumentTableIfOverridden() {
  if (argTableOverridden_) {
    bindArgumentTableInternal(ctx_->defaultArgumentTable());
    argTableOverridden_ = false;
  }
}

void CommandBuffer::cmdBindArgumentTable(ArgumentTableHandle handle) {
  bindArgumentTableInternal(handle);
  argTableOverridden_ = true;
}

void CommandBuffer::cmdBindTilePipeline(TilePipelineHandle handle) {
  LVK_ASSERT_MSG(isRendering_, "cmdBindTilePipeline() must be recorded inside cmdBeginRendering()/cmdEndRendering()");
  const MetalTilePipeline* p = ctx_->getTilePipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  encoder_->setRenderPipelineState(p->pipeline.get());
}

void CommandBuffer::cmdDispatchTile() {
  LVK_ASSERT_MSG(isRendering_, "cmdDispatchTile() must be recorded inside cmdBeginRendering()/cmdEndRendering()");
  encoder_->dispatchThreadsPerTile(MTL::Size(encoder_->tileWidth(), encoder_->tileHeight(), 1));
}

void CommandBuffer::cmdBindMachineLearningPipeline(MLPipelineHandle handle) {
  LVK_ASSERT_MSG(!isRendering_, "cmdBindMachineLearningPipeline() must be recorded outside cmdBeginRendering()/cmdEndRendering()");
  const MetalMachineLearningPipeline* p = ctx_->getMachineLearningPipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  if (!p || !p->pipeline)
    return;
  endComputeEncoder();
  if (!mlEncoder_)
    mlEncoder_ = wrapper_->cmdBuf.get()->machineLearningCommandEncoder();
  mlEncoder_->setPipelineState(p->pipeline.get());
  if (p->argTable)
    mlEncoder_->setArgumentTable(p->argTable.get());
  mlPipeline_ = p;
}

void CommandBuffer::cmdDispatchNetwork() {
  if (!mlEncoder_ || !mlPipeline_)
    return;
  mlEncoder_->barrierAfterQueueStages(MTL::StageDispatch | MTL::StageBlit, MTL::StageMachineLearning, MTL4::VisibilityOptionDevice);
  mlEncoder_->dispatchNetwork(mlPipeline_->intermediatesHeap.get());
  endMachineLearningEncoder();
  pendingMLBarrier_ = true;
}

void CommandBuffer::cmdBindViewport(const Viewport& viewport) {
  MTL::Viewport vp;
  vp.originX = viewport.x;
  vp.originY = viewport.y;
  vp.width = viewport.width;
  vp.height = viewport.height;
  vp.znear = viewport.minDepth;
  vp.zfar = viewport.maxDepth;
  encoder_->setViewport(vp);
}

void CommandBuffer::cmdBindScissorRect(const ScissorRect& rect) {
  MTL::ScissorRect r;
  r.x = rect.x;
  r.y = rect.y;
  r.width = rect.width;
  r.height = rect.height;
  encoder_->setScissorRect(r);
}

void CommandBuffer::cmdBindIndexBuffer(BufferHandle indexBuffer, IndexFormat indexFormat, uint64_t bufferOffset, uint64_t) {
  boundIndexBuffer_ = indexBuffer;
  boundIndexType_ = toMTLIndexType(indexFormat);
  boundIndexOffset_ = bufferOffset;
}

void CommandBuffer::cmdPushConstants(const void* data, size_t size, size_t offset) {
  const MetalArgumentTable* at = ctx_->getArgumentTable(currentArgTable_);
  if (!at || at->constantsIndex == UINT32_MAX)
    return;
  const MTL::GPUAddress addr = ctx_->writePushConstants(data, size, offset, wrapper_);
  at->table->setAddress(addr, at->constantsIndex);
  setArgumentTableOnActiveEncoder(at->table.get());
}

void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t baseInstance) {
  applyDepthStencilState();
  encoder_->drawPrimitives(topology_, firstVertex, vertexCount, instanceCount, baseInstance);
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride) {
  const MetalBuffer* b = ctx_->getBuffer(indirectBuffer);
  if (!b || !b->buffer || !drawCount)
    return;
  applyDepthStencilState();
  if (drawCount == 1) {
    encoder_->drawPrimitives(topology_, b->buffer->gpuAddress() + indirectBufferOffset);
  } else if (b->icb) {
    const uint32_t firstCommand = uint32_t(indirectBufferOffset / (stride ? stride : 16));
    encoder_->executeCommandsInBuffer(b->icb.get(), NS::Range(firstCommand, drawCount));
  }
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawIndexedIndirect(BufferHandle indirectBuffer, size_t indirectBufferOffset, uint32_t drawCount, uint32_t stride) {
  const MetalBuffer* b = ctx_->getBuffer(indirectBuffer);
  if (!b || !b->buffer || !drawCount)
    return;
  applyDepthStencilState();
  if (drawCount == 1) {
    const MetalBuffer* idx = b->icbIndexBuffer.empty() ? nullptr : ctx_->getBuffer(b->icbIndexBuffer);
    if (!idx || !idx->buffer)
      return;
    const MTL::IndexType indexType = b->icbIndexFormat == IndexFormat_UI16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
    encoder_->drawIndexedPrimitives(
        topology_, indexType, idx->buffer->gpuAddress(), idx->size, b->buffer->gpuAddress() + indirectBufferOffset);
  } else if (b->icb) {
    const uint32_t firstCommand = uint32_t(indirectBufferOffset / (stride ? stride : 20));
    encoder_->executeCommandsInBuffer(b->icb.get(), NS::Range(firstCommand, drawCount));
  }
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawMeshTasksIndirect(BufferHandle indirectBuffer,
                                             size_t indirectBufferOffset,
                                             uint32_t drawCount,
                                             uint32_t stride) {
  const MetalBuffer* b = ctx_->getBuffer(indirectBuffer);
  if (!b || !b->buffer || !drawCount)
    return;
  applyDepthStencilState();
  if (drawCount == 1) {
    encoder_->drawMeshThreadgroups(
        b->buffer->gpuAddress() + indirectBufferOffset, meshObjectThreadsPerThreadgroup_, meshThreadsPerThreadgroup_);
  } else if (b->icb && !b->icbMeshThreadgroupSizes.empty()) {
    const uint32_t firstCommand = uint32_t(indirectBufferOffset / (stride ? stride : 12));
    encoder_->executeCommandsInBuffer(b->icb.get(), NS::Range(firstCommand, drawCount));
  }
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawMeshTasks(const Dimensions& threadgroupCount) {
  applyDepthStencilState();
  encoder_->drawMeshThreadgroups(MTL::Size(threadgroupCount.width, threadgroupCount.height, threadgroupCount.depth),
                                 meshObjectThreadsPerThreadgroup_,
                                 meshThreadsPerThreadgroup_);
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawIndexed(uint32_t indexCount,
                                   uint32_t instanceCount,
                                   uint32_t firstIndex,
                                   int32_t vertexOffset,
                                   uint32_t baseInstance) {
  applyDepthStencilState();
  const MetalBuffer* ib = ctx_->getBuffer(boundIndexBuffer_);
  if (!ib || !ib->buffer)
    return;
  const uint32_t indexSize = boundIndexType_ == MTL::IndexTypeUInt16 ? 2u : 4u;
  const uint64_t start = boundIndexOffset_ + uint64_t(firstIndex) * indexSize;
  const MTL::GPUAddress addr = ib->buffer->gpuAddress() + start;
  encoder_->drawIndexedPrimitives(
      topology_, indexCount, boundIndexType_, addr, ib->size - start, instanceCount, vertexOffset, baseInstance);
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdSetBlendColor(const float color[4]) {
  if (encoder_)
    encoder_->setBlendColor(color[0], color[1], color[2], color[3]);
}

void CommandBuffer::cmdSetDepthBias(float constantFactor, float slopeFactor, float clamp) {
  depthBiasConstantFactor_ = constantFactor;
  depthBiasSlopeFactor_ = slopeFactor;
  depthBiasClamp_ = clamp;
  applyDepthBias();
}

void CommandBuffer::cmdSetDepthBiasEnable(bool enable) {
  depthBiasEnabled_ = enable;
  applyDepthBias();
}

void CommandBuffer::applyDepthBias() {
  if (!encoder_)
    return;
  if (depthBiasEnabled_)
    encoder_->setDepthBias(depthBiasConstantFactor_, depthBiasSlopeFactor_, depthBiasClamp_);
  else
    encoder_->setDepthBias(0.0f, 0.0f, 0.0f);
}

void CommandBuffer::cmdResetQueryPool(QueryPoolHandle pool, uint32_t firstQuery, uint32_t queryCount) {
  if (MTL4::CounterHeap* heap = ctx_->getQueryHeap(pool))
    heap->invalidateCounterRange(NS::Range(firstQuery, queryCount));
}

void CommandBuffer::cmdWriteTimestamp(QueryPoolHandle pool, uint32_t query) {
  MTL4::CounterHeap* heap = ctx_->getQueryHeap(pool);
  if (!heap)
    return;
  if (computeEncoder_)
    computeEncoder_->writeTimestamp(MTL4::TimestampGranularityPrecise, heap, query);
  else if (encoder_)
    encoder_->writeTimestamp(MTL4::TimestampGranularityPrecise, MTL::RenderStageFragment, heap, query);
  else if (MTL4::CommandBuffer* cb = wrapper_->cmdBuf.get())
    cb->writeTimestampIntoHeap(heap, query);
}

void CommandBuffer::cmdPushDebugGroupLabel(const char* label, uint32_t) const {
  if (encoder_ && label)
    encoder_->pushDebugGroup(ns::string(label));
}

void CommandBuffer::cmdInsertDebugEventLabel(const char*, uint32_t) const {}

void CommandBuffer::cmdPopDebugGroupLabel() const {
  if (encoder_)
    encoder_->popDebugGroup();
}

std::unique_ptr<IMetalContext> createContextWithMetalLayer(CA::MetalLayer* layer,
                                                           uint32_t width,
                                                           uint32_t height,
                                                           const ContextConfig& config) {
  std::unique_ptr<MetalContext> ctx = config.validation ? std::make_unique<MetalValidatedContext>() : std::make_unique<MetalContext>();
  if (!ctx->initialize(layer, width, height, config))
    return nullptr;
  return ctx;
}

void MetalValidatedCommandBuffer::cmdPushConstants(const void* data, size_t size, size_t offset) {
  const uint32_t maxSize = context()->pushConstantsSize();
  if (offset + size > maxSize) {
    LLOGW("validation: cmdPushConstants writes %zu bytes (offset %zu + size %zu) but the max push constants size is %u",
          offset + size,
          offset,
          size,
          maxSize);
  }
  CommandBuffer::cmdPushConstants(data, size, offset);
}

void MetalValidatedCommandBuffer::cmdBeginRendering(const lvk::RenderPass& renderPass,
                                                    const lvk::Framebuffer& desc,
                                                    const Dependencies& deps) {
  const uint32_t maxColor = context()->limits().maxColorRenderTargetsPerRenderPass;
  if (desc.getNumColorAttachments() > maxColor) {
    LLOGW(
        "validation: cmdBeginRendering uses %u color attachments but the GPU supports at most %u", desc.getNumColorAttachments(), maxColor);
  }
  CommandBuffer::cmdBeginRendering(renderPass, desc, deps);
}

static void validateIndirectRange(const MetalBuffer* b, size_t offset, uint32_t drawCount, uint32_t stride, const char* fn) {
  if (!b || !b->buffer)
    return;
  const uint32_t s = stride ? stride : 16;
  const uint32_t capacity = uint32_t(b->size / s);
  const uint32_t firstCommand = uint32_t(offset / s);
  if (firstCommand + drawCount > capacity) {
    LLOGW("validation: %s draws commands [%u, %u) but the indirect buffer holds at most %u (size %zu / stride %u)",
          fn,
          firstCommand,
          firstCommand + drawCount,
          capacity,
          b->size,
          s);
  }
}

void MetalValidatedCommandBuffer::cmdDrawIndirect(BufferHandle indirectBuffer,
                                                  size_t indirectBufferOffset,
                                                  uint32_t drawCount,
                                                  uint32_t stride) {
  validateIndirectRange(context()->getBuffer(indirectBuffer), indirectBufferOffset, drawCount, stride ? stride : 16, "cmdDrawIndirect");
  CommandBuffer::cmdDrawIndirect(indirectBuffer, indirectBufferOffset, drawCount, stride);
}

void MetalValidatedCommandBuffer::cmdDrawIndexedIndirect(BufferHandle indirectBuffer,
                                                         size_t indirectBufferOffset,
                                                         uint32_t drawCount,
                                                         uint32_t stride) {
  validateIndirectRange(
      context()->getBuffer(indirectBuffer), indirectBufferOffset, drawCount, stride ? stride : 20, "cmdDrawIndexedIndirect");
  CommandBuffer::cmdDrawIndexedIndirect(indirectBuffer, indirectBufferOffset, drawCount, stride);
}

void MetalValidatedCommandBuffer::cmdDrawMeshTasksIndirect(BufferHandle indirectBuffer,
                                                           size_t indirectBufferOffset,
                                                           uint32_t drawCount,
                                                           uint32_t stride) {
  const MetalBuffer* b = context()->getBuffer(indirectBuffer);
  validateIndirectRange(b, indirectBufferOffset, drawCount, stride ? stride : 12, "cmdDrawMeshTasksIndirect");
  if (drawCount > 1 && b && b->icbMeshThreadgroupSizes.empty()) {
    LLOGW(
        "validation: cmdDrawMeshTasksIndirect with drawCount>1 needs a per-buffer threadgroup-sizes buffer - call "
        "setIndirectBufferMetadata({.meshThreadgroupSizes=...}); the draw is a no-op");
  }
  CommandBuffer::cmdDrawMeshTasksIndirect(indirectBuffer, indirectBufferOffset, drawCount, stride);
}

void MetalValidatedCommandBuffer::cmdDrawIndexedIndirectCount(BufferHandle, size_t, BufferHandle, size_t, uint32_t, uint32_t) {
  LLOGW(
      "validation: cmdDrawIndexedIndirectCount is not supported by lvk-metal - a GPU-side draw-count buffer cannot be resolved when the "
      "indirect command buffer is encoded at cmdBeginRendering(); use cmdDrawIndexedIndirect with a CPU drawCount instead");
}

void MetalValidatedCommandBuffer::cmdDrawMeshTasksIndirectCount(BufferHandle, size_t, BufferHandle, size_t, uint32_t, uint32_t) {
  LLOGW(
      "validation: cmdDrawMeshTasksIndirectCount is not supported by lvk-metal - a GPU-side draw-count buffer cannot be resolved when the "
      "indirect command buffer is encoded at cmdBeginRendering()");
}

void MetalValidatedCommandBuffer::cmdBuildIndirectTLAS(lvk::AccelStructHandle tlas,
                                                       lvk::BufferHandle instanceDescriptors,
                                                       uint32_t instanceCount,
                                                       lvk::BufferHandle instanceCountBuffer) {
  const MetalAccelStruct* as = context()->getAccelStruct(tlas);
  if (!as || as->type != AccelStructType_TLAS) {
    LLOGW("validation: cmdBuildIndirectTLAS requires a valid TLAS acceleration structure");
  } else {
    if (!as->indirectTLAS)
      LLOGW("validation: cmdBuildIndirectTLAS expects an indirect TLAS (create it with an empty instancesBuffer)");
    if (instanceCount > as->numInstances)
      LLOGW("validation: cmdBuildIndirectTLAS instanceCount %u exceeds the TLAS capacity %u", instanceCount, as->numInstances);
    const MetalBuffer* inst = context()->getBuffer(instanceDescriptors);
    const uint64_t needed = uint64_t(instanceCount) * context()->indirectTLASInstanceDescriptorSize();
    if (!inst || !inst->buffer)
      LLOGW("validation: cmdBuildIndirectTLAS requires a valid instanceDescriptors buffer");
    else if (inst->buffer->length() < needed)
      LLOGW("validation: cmdBuildIndirectTLAS instanceDescriptors buffer is %llu bytes but %llu are needed for %u instances",
            (unsigned long long)inst->buffer->length(),
            (unsigned long long)needed,
            instanceCount);
  }
  CommandBuffer::cmdBuildIndirectTLAS(tlas, instanceDescriptors, instanceCount, instanceCountBuffer);
}

IMetalCommandBuffer& MetalValidatedContext::acquireMetalCommandBuffer(bool) {
  MetalValidatedCommandBuffer* cmd = new MetalValidatedCommandBuffer(this);
  cmd->wrapper_ = beginCommandBuffer();
  return *cmd;
}

Holder<TextureHandle> MetalValidatedContext::createTexture(const TextureDesc& desc, const char* debugName, Result* outResult) {
  uint32_t maxDim = limits().max2DTextureWidthHeight;
  const char* kind = "2D";
  if (desc.type == TextureType_Cube) {
    maxDim = limits().maxCubeMapTextureWidthHeight;
    kind = "cube";
  } else if (desc.type == TextureType_3D) {
    maxDim = limits().max3DTextureWidthHeightDepth;
    kind = "3D";
  }
  const uint32_t depth = desc.type == TextureType_3D ? desc.dimensions.depth : 1;
  if (desc.dimensions.width > maxDim || desc.dimensions.height > maxDim || depth > maxDim) {
    LLOGW("validation: createTexture '%s' is %ux%ux%u but the GPU max %s texture dimension is %u",
          debugName ? debugName : desc.debugName,
          desc.dimensions.width,
          desc.dimensions.height,
          depth,
          kind,
          maxDim);
  }
  if (desc.numLayers > limits().maxLayersPerTextureArray) {
    LLOGW("validation: createTexture numLayers %u exceeds the GPU max texture array layers %u",
          desc.numLayers,
          limits().maxLayersPerTextureArray);
  }
  return MetalContext::createTexture(desc, debugName, outResult);
}

Holder<RenderPipelineHandle> MetalValidatedContext::createRenderPipeline(const RenderPipelineDesc& desc, Result* outResult) {
  if (desc.getNumColorAttachments() > limits().maxColorRenderTargetsPerRenderPass) {
    LLOGW("validation: createRenderPipeline has %u color attachments but the GPU supports at most %u",
          desc.getNumColorAttachments(),
          limits().maxColorRenderTargetsPerRenderPass);
  }
  return MetalContext::createRenderPipeline(desc, outResult);
}

} // namespace lvk::metal

namespace lvk {

void destroy(lvk::IContext* ctx, lvk::metal::ArgumentTableHandle handle) {
  if (ctx)
    static_cast<lvk::metal::IMetalContext*>(ctx)->destroy(handle);
}

void destroy(lvk::IContext* ctx, lvk::metal::TilePipelineHandle handle) {
  if (ctx)
    static_cast<lvk::metal::IMetalContext*>(ctx)->destroy(handle);
}

void destroy(lvk::IContext* ctx, lvk::metal::TensorHandle handle) {
  if (ctx)
    static_cast<lvk::metal::IMetalContext*>(ctx)->destroy(handle);
}

void destroy(lvk::IContext* ctx, lvk::metal::MLPipelineHandle handle) {
  if (ctx)
    static_cast<lvk::metal::IMetalContext*>(ctx)->destroy(handle);
}

} // namespace lvk
