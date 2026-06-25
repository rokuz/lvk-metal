#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define CA_PRIVATE_IMPLEMENTATION

#include "lvk/metal/MetalClasses.h"
#include "lvk/metal/MetalLimits.h"
#include "lvk/metal/MetalMappings.h"

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

  const GpuLimits limits = limitsForFamily(detectGpuFamily(device_.get()));
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
  metalLayer_->setFramebufferOnly(true);
  metalLayer_->setDrawableSize(CGSizeMake(width_, height_));
  metalLayer_->setDisplaySyncEnabled(vsync_);
  metalLayer_->setMaximumDrawableCount(framesInFlight_);

  if (!createQueue())
    return false;
  immediate_ = std::make_unique<MetalImmediateCommands>(device_.get(), commandQueue_.get(), "lvk-metal.immediate");
  if (!createBindlessHeaps())
    return false;

  ArgumentTableDesc defaultDesc;
  defaultDesc.numKinds = 6;
  defaultDesc.kinds[0] = ArgumentKind::Constants;
  defaultDesc.kinds[1] = ArgumentKind::Buffers;
  defaultDesc.kinds[2] = ArgumentKind::Textures2D;
  defaultDesc.kinds[3] = ArgumentKind::Textures3D;
  defaultDesc.kinds[4] = ArgumentKind::TexturesCube;
  defaultDesc.kinds[5] = ArgumentKind::Samplers;
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
  return true;
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
        at.table->setAddress(textureHeap_->gpuAddress(), i);
        break;
      case ArgumentKind::Samplers:
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
  immediate_->waitAll();
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), textureHeap_->contents(), NS::UInteger(texturesCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nb.get(), "lvk-metal.bindless.textures");
  textureHeap_ = nb;
  texturesCapacity_ = newCap;
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
  immediate_->waitAll();
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), samplerHeap_->contents(), NS::UInteger(samplersCapacity_) * sizeof(MTL::ResourceID));
  ns::setLabel(nb.get(), "lvk-metal.bindless.samplers");
  samplerHeap_ = nb;
  samplersCapacity_ = newCap;
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
  immediate_->waitAll();
  NS::SharedPtr<MTL::Buffer> nb =
      NS::TransferPtr(device_->newBuffer(NS::UInteger(newCap) * sizeof(MTL::GPUAddress), MTL::ResourceStorageModeShared));
  std::memcpy(nb->contents(), bufferHeap_->contents(), NS::UInteger(buffersCapacity_) * sizeof(MTL::GPUAddress));
  ns::setLabel(nb.get(), "lvk-metal.bindless.buffers");
  bufferHeap_ = nb;
  buffersCapacity_ = newCap;
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
  pushesPerFrameCapacity_ = newPerFrame;
  constantsFrameRegionBytes_ = newRegionBytes;
  constantsCursor_ = currentWrapper_->bufferIndex * newRegionBytes;
}

IMetalCommandBuffer& MetalContext::acquireMetalCommandBuffer(bool) {
  LVK_ASSERT_MSG(!framePool_, "previous command buffer was not submitted");
  for (size_t i = 0; i < retiredConstantRings_.size();) {
    if (immediate_->isReady(retiredConstantRings_[i].handle)) {
      retiredConstantRings_[i] = retiredConstantRings_.back();
      retiredConstantRings_.pop_back();
    } else {
      ++i;
    }
  }
  framePool_ = NS::AutoreleasePool::alloc()->init();
  currentWrapper_ = &immediate_->acquire();
  constantsCursor_ = currentWrapper_->bufferIndex * constantsFrameRegionBytes_;
  return cmdBuffer_;
}

SubmitHandle MetalContext::submit(lvk::ICommandBuffer&, TextureHandle present) {
  LVK_ASSERT(currentWrapper_);

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
  if (desc.data && buffer->contents())
    std::memcpy(buffer->contents(), desc.data, desc.size);

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
  NS::SharedPtr<MTL::TextureDescriptor> td = ns::make<MTL::TextureDescriptor>();
  td->setTextureType(toMTLTextureType(desc.type, desc.numLayers));
  td->setPixelFormat(toMTLPixelFormat(desc.format));
  td->setWidth(desc.dimensions.width);
  td->setHeight(desc.dimensions.height);
  td->setDepth(desc.type == TextureType_3D ? desc.dimensions.depth : 1);
  td->setMipmapLevelCount(desc.numMipLevels);
  td->setArrayLength(desc.numLayers ? desc.numLayers : 1);
  td->setSampleCount(desc.numSamples);
  td->setUsage(toMTLTextureUsage(desc.usage));
  td->setStorageMode(toMTLStorageMode(desc.storage));

  NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(device_->newTexture(td.get()));
  if (!texture) {
    Result::setResult(outResult, Result::Code::RuntimeError, "newTexture failed");
    return {};
  }
  ns::setLabel(texture.get(), debugName ? debugName : desc.debugName);

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

Holder<ShaderModuleHandle> MetalContext::createShaderModule(const ShaderModuleDesc& desc, Result* outResult) {
  NS::SharedPtr<MTL::Library> library;
  NS::Error* error = nullptr;

  if (desc.data && desc.dataSize == 0) {
    library = NS::TransferPtr(device_->newLibrary(ns::string(desc.data), nullptr, &error));
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
  return {this, shaderModules_.create(std::move(sm))};
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
      table->setAddress(textureHeap_->gpuAddress(), i);
      break;
    case ArgumentKind::Samplers:
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
  renderPipelines_.destroy(handle);
}
void MetalContext::destroy(ShaderModuleHandle handle) {
  shaderModules_.destroy(handle);
}
void MetalContext::destroy(SamplerHandle handle) {
  samplers_.destroy(handle);
}
void MetalContext::destroy(BufferHandle handle) {
  buffers_.destroy(handle);
}
void MetalContext::destroy(TextureHandle handle) {
  textures_.destroy(handle);
}
void MetalContext::destroy(ArgumentTableHandle handle) {
  argumentTables_.destroy(handle);
}

Result MetalContext::upload(BufferHandle handle, const void* data, size_t size, size_t offset) {
  MetalBuffer* mb = buffers_.get(handle);
  if (!mb || !mb->buffer || !mb->buffer->contents())
    return Result(Result::Code::ArgumentOutOfRange, "invalid buffer");
  std::memcpy(static_cast<uint8_t*>(mb->buffer->contents()) + offset, data, size);
  return Result();
}

Result MetalContext::download(BufferHandle handle, void* data, size_t size, size_t offset) {
  const MetalBuffer* mb = buffers_.get(handle);
  if (!mb || !mb->buffer || !mb->buffer->contents())
    return Result(Result::Code::ArgumentOutOfRange, "invalid buffer");
  std::memcpy(data, static_cast<const uint8_t*>(mb->buffer->contents()) + offset, size);
  return Result();
}

uint8_t* MetalContext::getMappedPtr(BufferHandle handle) const {
  const MetalBuffer* mb = buffers_.get(handle);
  return mb && mb->buffer ? static_cast<uint8_t*>(mb->buffer->contents()) : nullptr;
}

uint64_t MetalContext::gpuAddress(BufferHandle handle, size_t offset) const {
  const MetalBuffer* mb = buffers_.get(handle);
  return mb && mb->buffer ? mb->buffer->gpuAddress() + offset : 0;
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

NS::SharedPtr<MTL::DepthStencilState> MetalContext::makeDepthStencilState(const DepthState& state) {
  NS::SharedPtr<MTL::DepthStencilDescriptor> dsd = ns::make<MTL::DepthStencilDescriptor>();
  dsd->setDepthCompareFunction(toMTLCompareFunction(state.compareOp));
  dsd->setDepthWriteEnabled(state.isDepthWriteEnabled);
  return NS::TransferPtr(device_->newDepthStencilState(dsd.get()));
}

TextureHandle MetalContext::getCurrentSwapchainTexture() {
  if (!currentDrawable_) {
    CA::MetalDrawable* drawable = metalLayer_->nextDrawable();
    if (!LVK_VERIFY(drawable))
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

void CommandBuffer::cmdBeginRendering(const lvk::RenderPass& renderPass, const lvk::Framebuffer& framebuffer, const lvk::Dependencies&) {
  NS::SharedPtr<MTL4::RenderPassDescriptor> rpd = ns::make<MTL4::RenderPassDescriptor>();

  uint32_t rtWidth = 0;
  uint32_t rtHeight = 0;
  const uint32_t nColor = framebuffer.getNumColorAttachments();
  for (uint32_t i = 0; i < nColor; ++i) {
    const MetalImage* img = ctx_->getImage(framebuffer.color[i].texture);
    LVK_ASSERT(img && img->texture);
    MTL::RenderPassColorAttachmentDescriptor* att = rpd->colorAttachments()->object(i);
    att->setTexture(img->texture.get());
    att->setLoadAction(toLoadAction(renderPass.color[i].loadOp));
    att->setStoreAction(toStoreAction(renderPass.color[i].storeOp));
    const float* c = renderPass.color[i].clearColor.float32;
    att->setClearColor(MTL::ClearColor(c[0], c[1], c[2], c[3]));
    rtWidth = img->width;
    rtHeight = img->height;
  }

  if (framebuffer.depthStencil.texture) {
    const MetalImage* img = ctx_->getImage(framebuffer.depthStencil.texture);
    LVK_ASSERT(img && img->texture);
    MTL::RenderPassDepthAttachmentDescriptor* att = rpd->depthAttachment();
    att->setTexture(img->texture.get());
    att->setLoadAction(toLoadAction(renderPass.depth.loadOp));
    att->setStoreAction(toStoreAction(renderPass.depth.storeOp));
    att->setClearDepth(renderPass.depth.clearDepth);
    rtWidth = img->width;
    rtHeight = img->height;
  }

  rpd->setRenderTargetWidth(rtWidth);
  rpd->setRenderTargetHeight(rtHeight);

  encoder_ = ctx_->commandBuffer()->renderCommandEncoder(rpd.get());
  isRendering_ = true;
  argTableOverridden_ = false;
  bindArgumentTableInternal(ctx_->defaultArgumentTable());
}

void CommandBuffer::cmdEndRendering() {
  if (encoder_) {
    encoder_->endEncoding();
    encoder_ = nullptr;
  }
  isRendering_ = false;
}

void CommandBuffer::cmdBindRenderPipeline(RenderPipelineHandle handle) {
  const MetalRenderPipeline* p = ctx_->getRenderPipeline(handle);
  LVK_ASSERT(p && p->pipeline);
  encoder_->setRenderPipelineState(p->pipeline.get());
  encoder_->setCullMode(p->cullMode);
  encoder_->setFrontFacingWinding(p->frontFace);
  encoder_->setTriangleFillMode(p->fillMode);
  topology_ = p->topology;
}

void CommandBuffer::cmdBindDepthState(const DepthState& state) {
  if (!encoder_)
    return;
  NS::SharedPtr<MTL::DepthStencilState> dss = ctx_->makeDepthStencilState(state);
  if (dss)
    encoder_->setDepthStencilState(dss.get());
}

void CommandBuffer::bindArgumentTableInternal(ArgumentTableHandle handle) {
  currentArgTable_ = handle;
  const MetalArgumentTable* at = ctx_->getArgumentTable(handle);
  if (at && at->table)
    encoder_->setArgumentTable(at->table.get(), static_cast<MTL::RenderStages>(MTL::RenderStageVertex | MTL::RenderStageFragment));
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
  encoder_->setArgumentTable(at->table.get(), static_cast<MTL::RenderStages>(MTL::RenderStageVertex | MTL::RenderStageFragment));
}

void CommandBuffer::cmdDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t baseInstance) {
  encoder_->drawPrimitives(topology_, firstVertex, vertexCount, instanceCount, baseInstance);
  resetArgumentTableIfOverridden();
}

void CommandBuffer::cmdDrawIndexed(uint32_t indexCount,
                                   uint32_t instanceCount,
                                   uint32_t firstIndex,
                                   int32_t vertexOffset,
                                   uint32_t baseInstance) {
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
  std::unique_ptr<MetalContext> ctx = std::make_unique<MetalContext>();
  if (!ctx->initialize(layer, width, height, config))
    return nullptr;
  return ctx;
}

} // namespace lvk::metal

namespace lvk {

void destroy(lvk::IContext* ctx, lvk::metal::ArgumentTableHandle handle) {
  if (ctx)
    static_cast<lvk::metal::IMetalContext*>(ctx)->destroy(handle);
}

} // namespace lvk
