#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "lvk/metal/MetalClasses.h"
#include "lvk/metal/MetalLimits.h"
#include "lvk/metal/MetalMappings.h"
#include "lvk/metal/MetalStagingDevice.h"

#include <cstdio>
#include <cstring>
#include <dispatch/dispatch.h>

namespace lvk::metal {

namespace {

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
  swapchainFormat_ = cfg.swapchainFormat;
  texturesCapacity_ = cfg.initialTexturesPoolSize ? cfg.initialTexturesPoolSize : 1;
  samplersCapacity_ = cfg.initialSamplesPoolSize ? cfg.initialSamplesPoolSize : 1;
  buffersCapacity_ = texturesCapacity_;
  pushConstantsSize_ = cfg.pushConstantsSize;
  pushesPerFrameCapacity_ = cfg.initialPushConstantsPerFrameCount ? cfg.initialPushConstantsPerFrameCount : 1;

  if (!createDevice())
    return false;

  limits_ = limitsForFamily(detectGpuFamily(device_.get()));
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

  metalLayer_ = layer;
  metalLayer_->retain();
  metalLayer_->setDevice(device_.get());
  metalLayer_->setPixelFormat(swapchainFormat_);
  metalLayer_->setFramebufferOnly(!cfg.headless);
  metalLayer_->setDrawableSize(CGSizeMake(width_, height_));
  metalLayer_->setDisplaySyncEnabled(vsync_);
  metalLayer_->setMaximumDrawableCount(framesInFlight_);

  if (!createQueue())
    return false;

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
  defaultDesc.numKinds = 9;
  defaultDesc.kinds[0] = ArgumentKind::Constants;
  defaultDesc.kinds[1] = ArgumentKind::Buffers;
  defaultDesc.kinds[2] = ArgumentKind::Textures2D;
  defaultDesc.kinds[3] = ArgumentKind::Textures3D;
  defaultDesc.kinds[4] = ArgumentKind::TexturesCube;
  defaultDesc.kinds[5] = ArgumentKind::Samplers;
  defaultDesc.kinds[6] = ArgumentKind::Images2D;
  defaultDesc.kinds[7] = ArgumentKind::TexturesDepth2D;
  defaultDesc.kinds[8] = ArgumentKind::SamplersComparison;
  defaultDesc.debugName = "lvk-metal.default-argtable";
  defaultArgumentTable_ = createArgumentTable(defaultDesc, nullptr).release();

  MetalImage swap;
  swap.isSwapchainImage = true;
  swap.format = swapchainFormat_;
  swap.width = width_;
  swap.height = height_;
  swapchainTextureHandle_ = textures_.create(std::move(swap));

  cmdBuffer_ = CommandBuffer(this);

  LLOGL("Metal device: %s", device_->name()->utf8String());
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
  samplerHeap_ =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(samplersCapacity_) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  if (!LVK_VERIFY(bufferHeap_ && textureHeap_ && samplerHeap_))
    return false;
  ns::setLabel(bufferHeap_.get(), "lvk-metal.bindless.buffers");
  ns::setLabel(textureHeap_.get(), "lvk-metal.bindless.textures");
  ns::setLabel(samplerHeap_.get(), "lvk-metal.bindless.samplers");

  constantsFrameRegionBytes_ = pushesPerFrameCapacity_ * pushConstantsSize_;
  constantsRing_ = NS::TransferPtr(device_->newBuffer(NS::UInteger(MetalImmediateCommands::kMaxCommandBuffers) * constantsFrameRegionBytes_,
                                                      MTL::ResourceStorageModeShared));
  if (!LVK_VERIFY(constantsRing_))
    return false;
  ns::setLabel(constantsRing_.get(), "lvk-metal.pushconstants.ring");

  addResident(bufferHeap_.get());
  addResident(textureHeap_.get());
  addResident(samplerHeap_.get());
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
      case ArgumentKind::TexturesDepth2D:
        at.table->setAddress(textureHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::Samplers:
      case ArgumentKind::SamplersComparison:
        at.table->setAddress(samplerHeap_->gpuAddress(), i);
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
  removeResident(textureHeap_.get());
  textureHeap_ = nb;
  texturesCapacity_ = newCap;
  addResident(textureHeap_.get());
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

void MetalContext::growConstantsRing() {
  const uint32_t newPerFrame = pushesPerFrameCapacity_ * 2;
  const uint32_t newRegionBytes = newPerFrame * pushConstantsSize_;
  NS::SharedPtr<MTL::Buffer> nb = NS::TransferPtr(
      device_->newBuffer(NS::UInteger(MetalImmediateCommands::kMaxCommandBuffers) * newRegionBytes, MTL::ResourceStorageModeShared));
  ns::setLabel(nb.get(), "lvk-metal.pushconstants.ring");
  retiredConstantRings_.push_back({constantsRing_, currentWrapper_->handle});
  constantsRing_ = nb;
  addResident(constantsRing_.get());
  pushesPerFrameCapacity_ = newPerFrame;
  constantsFrameRegionBytes_ = newRegionBytes;
  constantsCursor_ = currentWrapper_->bufferIndex * newRegionBytes;
}

IMetalCommandBuffer& MetalContext::acquireMetalCommandBuffer(bool) {
  LVK_ASSERT_MSG(!framePool_, "previous command buffer was not submitted");
  for (size_t i = 0; i < retiredConstantRings_.size();) {
    if (immediate_->isReady(retiredConstantRings_[i].handle)) {
      removeResident(retiredConstantRings_[i].buffer.get());
      retiredConstantRings_[i] = retiredConstantRings_.back();
      retiredConstantRings_.pop_back();
    } else {
      ++i;
    }
  }
  flushResidency();
  framePool_ = NS::AutoreleasePool::alloc()->init();
  currentWrapper_ = &immediate_->acquire();
  constantsCursor_ = currentWrapper_->bufferIndex * constantsFrameRegionBytes_;
  return cmdBuffer_;
}

SubmitHandle MetalContext::submit(lvk::ICommandBuffer&, TextureHandle present) {
  LVK_ASSERT(currentWrapper_);

  flushResidency();
  if (immediate_->hasDeferred())
    immediate_->flushDeferred();

  if (currentDrawable_)
    commandQueue_->wait(currentDrawable_);

  const SubmitHandle handle = immediate_->submit(*currentWrapper_);

  if (currentDrawable_ && !present.empty()) {
    commandQueue_->signalDrawable(currentDrawable_);
    currentDrawable_->present();
  }
  if (currentDrawable_) {
    currentDrawable_->release();
    currentDrawable_ = nullptr;
  }

  currentWrapper_ = nullptr;
  framePool_->release();
  framePool_ = nullptr;

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
  const BufferHandle handle = buffers_.create(std::move(mb));
  ensureBufferCapacity(handle.index());
  static_cast<MTL::GPUAddress*>(bufferHeap_->contents())[handle.index()] = address;
  return {this, handle};
}

Holder<TextureHandle> MetalContext::createTexture(const TextureDesc& desc, const char* debugName, Result* outResult) {
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
  td->setUsage(toMTLTextureUsage(desc.usage));
  td->setStorageMode(toMTLStorageMode(desc.storage));

  NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(device_->newTexture(td.get()));
  if (!texture) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newTexture failed");
  }
  ns::setLabel(texture.get(), debugName ? debugName : desc.debugName);
  addResident(texture.get());

  if (desc.data) {
    const uint32_t depth = desc.type == TextureType_3D ? desc.dimensions.depth : 1;
    staging_->uploadTexture(texture.get(), 0, 0, 0, desc.dimensions.width, desc.dimensions.height, depth, 0, 0, desc.data, 0);
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

static const char* kBindlessPreamble = R"(
#include <metal_stdlib>
using namespace metal;

#ifndef LVK_BINDLESS_TEXTURES
#define LVK_BINDLESS_TEXTURES 16384
#endif
#ifndef LVK_BINDLESS_SAMPLERS
#define LVK_BINDLESS_SAMPLERS 1024
#endif

struct lvkTextures2D     { array<texture2d<float>,                       LVK_BINDLESS_TEXTURES> data; };
struct lvkTextures3D     { array<texture3d<float>,                       LVK_BINDLESS_TEXTURES> data; };
struct lvkTexturesCube   { array<texturecube<float>,                     LVK_BINDLESS_TEXTURES> data; };
struct lvkSamplers       { array<sampler,                                LVK_BINDLESS_SAMPLERS> data; };
struct lvkImages2D       { array<texture2d<float, access::read_write>,    LVK_BINDLESS_TEXTURES> data; };
struct lvkTexturesDepth2D { array<depth2d<float>,                         LVK_BINDLESS_TEXTURES> data; };

#define LVK_BINDLESS_ARGS \
  device const lvkTextures2D&      kTextures2D      [[buffer(2)]], \
  device const lvkTextures3D&      kTextures3D      [[buffer(3)]], \
  device const lvkTexturesCube&    kTexturesCube    [[buffer(4)]], \
  constant lvkSamplers&            kSamplers        [[buffer(5)]], \
  device const lvkTexturesDepth2D& kTexturesDepth2D [[buffer(7)]]

#define LVK_BINDLESS_COMPUTE_ARGS \
  device const lvkTextures2D&   kTextures2D   [[buffer(2)]], \
  device const lvkTextures3D&   kTextures3D   [[buffer(3)]], \
  device const lvkTexturesCube& kTexturesCube [[buffer(4)]], \
  constant lvkSamplers&         kSamplers     [[buffer(5)]], \
  device const lvkImages2D&     kImages2D     [[buffer(6)]]

#define textureBindless2D(tid, sid, uv)            kTextures2D.data[tid].sample(kSamplers.data[sid], (uv))
#define textureBindless2DLod(tid, sid, uv, lod)    kTextures2D.data[tid].sample(kSamplers.data[sid], (uv), level(lod))
#define textureBindless3D(tid, sid, uvw)           kTextures3D.data[tid].sample(kSamplers.data[sid], (uvw))
#define textureBindlessCube(tid, sid, dir)         kTexturesCube.data[tid].sample(kSamplers.data[sid], (dir))
#define textureBindlessCubeLod(tid, sid, dir, lod) kTexturesCube.data[tid].sample(kSamplers.data[sid], (dir), level(lod))
#define textureBindlessSize2D(tid)                 uint2(kTextures2D.data[tid].get_width(), kTextures2D.data[tid].get_height())
#define textureBindless2DShadow(tid, sid, uvw)     kTexturesDepth2D.data[tid].sample_compare(kSamplers.data[sid], (uvw).xy, (uvw).z)
#define imageBindlessLoad2D(tid, pos)              kImages2D.data[tid].read(uint2(pos))
#define imageBindlessStore2D(tid, pos, val)        kImages2D.data[tid].write((val), uint2(pos))
)";

static MTL::Size parseThreadgroupSize(const char* src) {
  MTL::Size s(16, 16, 1);
  if (!src)
    return s;
  unsigned x = 0, y = 0, z = 0;
  if (const char* p = std::strstr(src, "LVK_NUMTHREADS")) {
    if (std::sscanf(p, "LVK_NUMTHREADS %u %u %u", &x, &y, &z) >= 1)
      return MTL::Size(x ? x : 1, y ? y : 1, z ? z : 1);
  }
  if (const char* p = std::strstr(src, "numthreads(")) {
    if (std::sscanf(p, "numthreads( %u , %u , %u", &x, &y, &z) >= 1)
      return MTL::Size(x ? x : 1, y ? y : 1, z ? z : 1);
  }
  if (const char* px = std::strstr(src, "local_size_x"))
    std::sscanf(px, "local_size_x = %u", &x);
  if (const char* py = std::strstr(src, "local_size_y"))
    std::sscanf(py, "local_size_y = %u", &y);
  if (const char* pz = std::strstr(src, "local_size_z"))
    std::sscanf(pz, "local_size_z = %u", &z);
  if (x || y || z)
    return MTL::Size(x ? x : 1, y ? y : 1, z ? z : 1);
  return s;
}

Holder<ShaderModuleHandle> MetalContext::createShaderModule(const ShaderModuleDesc& desc, Result* outResult) {
  NS::SharedPtr<MTL::Library> library;
  NS::Error* error = nullptr;

  if (desc.data && desc.dataSize == 0) {
    std::string source;
    if (std::strstr(desc.data, "LVK_BINDLESS")) {
      source += "#define LVK_BINDLESS_TEXTURES " + std::to_string(texturesCapacity_) + "\n";
      source += "#define LVK_BINDLESS_SAMPLERS " + std::to_string(samplersCapacity_) + "\n";
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
    sm.threadgroupSize = parseThreadgroupSize(desc.data);
  return {this, shaderModules_.create(std::move(sm))};
}

Holder<ComputePipelineHandle> MetalContext::createComputePipeline(const ComputePipelineDesc& desc, Result* outResult) {
  const MetalShaderModule* comp = shaderModules_.get(desc.smComp);
  if (!comp || !comp->function) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing compute shader");
    return {};
  }
  NS::Error* error = nullptr;
  NS::SharedPtr<MTL::ComputePipelineState> pipeline = NS::TransferPtr(device_->newComputePipelineState(comp->function.get(), &error));
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
  const MetalShaderModule* vert = shaderModules_.get(desc.smVert);
  const MetalShaderModule* frag = shaderModules_.get(desc.smFrag);
  if (!vert || !vert->function) {
    Result::setResult(outResult, Result::Code::ArgumentOutOfRange, "missing vertex shader");
    return {};
  }

  NS::SharedPtr<MTL::RenderPipelineDescriptor> rpd = ns::make<MTL::RenderPipelineDescriptor>();
  ns::setLabel(rpd.get(), desc.debugName);
  rpd->setVertexFunction(vert->function.get());
  if (frag && frag->function)
    rpd->setFragmentFunction(frag->function.get());

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
    case ArgumentKind::TexturesDepth2D:
      table->setAddress(textureHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Samplers:
    case ArgumentKind::SamplersComparison:
      table->setAddress(samplerHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Constants:
      at.constantsIndex = i;
      break;
    }
  }
  return {this, argumentTables_.create(std::move(at))};
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
    if (const MetalBuffer* mb = buffers_.get(handle))
      removeResident(mb->buffer.get());
    buffers_.destroy(handle);
  });
}
void MetalContext::destroy(TextureHandle handle) {
  if (!textures_.get(handle))
    return;
  deferredTask([this, handle]() {
    if (const MetalImage* img = textures_.get(handle))
      removeResident(img->texture.get());
    textures_.destroy(handle);
  });
}
void MetalContext::destroy(ArgumentTableHandle handle) {
  if (!argumentTables_.get(handle))
    return;
  deferredTask([this, handle]() { argumentTables_.destroy(handle); });
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

MTL::GPUAddress MetalContext::writePushConstants(const void* data, size_t size, size_t offset) {
  LVK_ASSERT(currentWrapper_);
  LVK_ASSERT(offset + size <= pushConstantsSize_);
  const uint32_t regionEnd = (currentWrapper_->bufferIndex + 1) * constantsFrameRegionBytes_;
  if (constantsCursor_ + pushConstantsSize_ > regionEnd)
    growConstantsRing();
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
  if (!currentDrawable_) {
    CA::MetalDrawable* drawable = metalLayer_->nextDrawable();
    if (!drawable)
      return {};
    currentDrawable_ = drawable;
    currentDrawable_->retain();
  }
  MetalImage* img = textures_.get(swapchainTextureHandle_);
  img->texture = ns::retain(currentDrawable_->texture());
  img->format = swapchainFormat_;
  img->width = width_;
  img->height = height_;
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
  endComputeEncoder();

  NS::SharedPtr<MTL4::RenderPassDescriptor> rpd = ns::make<MTL4::RenderPassDescriptor>();

  uint32_t rtWidth = 0;
  uint32_t rtHeight = 0;
  const uint32_t nColor = framebuffer.getNumColorAttachments();
  for (uint32_t i = 0; i < nColor; ++i) {
    const MetalImage* img = ctx_->getImage(framebuffer.color[i].texture);
    LVK_ASSERT(img && img->texture);
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(i);
    att->setTexture(img->texture.get());
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

  encoder_ = ctx_->commandBuffer()->renderCommandEncoder(rpd.get());
  depthStencilDirty_ = true;
  lastDepthStencilState_ = nullptr;
  encoder_->setStencilReferenceValue(stencilRef_);

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
  depthStencilDirty_ = true;
}

void CommandBuffer::endComputeEncoder() {
  if (computeEncoder_) {
    computeEncoder_->endEncoding();
    computeEncoder_ = nullptr;
  }
}

void CommandBuffer::setArgumentTableOnActiveEncoder(MTL4::ArgumentTable* table) {
  if (computeEncoder_)
    computeEncoder_->setArgumentTable(table);
  else if (encoder_)
    encoder_->setArgumentTable(table, static_cast<MTL::RenderStages>(MTL::RenderStageVertex | MTL::RenderStageFragment));
}

void CommandBuffer::cmdBindComputePipeline(ComputePipelineHandle handle) {
  const MetalComputePipeline* p = ctx_->getComputePipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  if (!p || !p->pipeline)
    return;
  if (!computeEncoder_) {
    computeEncoder_ = ctx_->commandBuffer()->computeCommandEncoder();
    bindArgumentTableInternal(ctx_->defaultArgumentTable());
  }
  computeEncoder_->setComputePipelineState(p->pipeline.get());
  computeThreadgroupSize_ = p->threadgroupSize;
}

void CommandBuffer::cmdDispatch(const Dimensions& groupCount, const Dependencies&) {
  if (!computeEncoder_)
    return;
  computeEncoder_->dispatchThreadgroups(MTL::Size(groupCount.width, groupCount.height, groupCount.depth), computeThreadgroupSize_);
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
  const MTL::GPUAddress addr = ctx_->writePushConstants(data, size, offset);
  at->table->setAddress(addr, at->constantsIndex);
  setArgumentTableOnActiveEncoder(at->table.get());
}

void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t baseInstance) {
  applyDepthStencilState();
  encoder_->drawPrimitives(topology_, firstVertex, vertexCount, instanceCount, baseInstance);
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
  if (encoder_)
    encoder_->setDepthBias(constantFactor, slopeFactor, clamp);
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

IMetalCommandBuffer& MetalValidatedContext::acquireMetalCommandBuffer(bool dedicatedCompute) {
  MetalContext::acquireMetalCommandBuffer(dedicatedCompute);
  return validatedCmdBuffer_;
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

} // namespace lvk
