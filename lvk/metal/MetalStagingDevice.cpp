#include "lvk/metal/MetalStagingDevice.h"

#include "lvk/metal/MetalClasses.h"
#include "lvk/metal/MetalMappings.h"

#include <algorithm>
#include <cstring>

namespace lvk::metal {

namespace {
uint64_t getAlignedSize(uint64_t value, uint64_t alignment) {
  return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

MetalStagingDevice::MetalStagingDevice(MetalContext& ctx) : ctx_(ctx) {
  minBufferSize_ = std::min(minBufferSize_, maxBufferSize_);
}

void MetalStagingDevice::uploadBuffer(MTL::Buffer* buffer, size_t dstOffset, size_t size, const void* data) {
  while (size) {
    MemoryRegionDesc desc = getNextFreeOffset(size);
    const uint64_t chunkSize = std::min<uint64_t>(size, desc.size_);

    std::memcpy(static_cast<uint8_t*>(stagingBuffer_->contents()) + desc.offset_, data, chunkSize);

    ctx_.flushResidency();

    const MetalImmediateCommands::CommandBufferWrapper& wrapper = ctx_.immediate_->acquire();
    MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
    enc->copyFromBuffer(stagingBuffer_.get(), desc.offset_, buffer, dstOffset, chunkSize);
    enc->barrierAfterStages(MTL::StageBlit | MTL::StageDispatch,
                            MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch,
                            MTL4::VisibilityOptionDevice);
    enc->endEncoding();

    desc.handle_ = ctx_.immediate_->submit(wrapper);
    insertRegion(desc);

    size -= chunkSize;
    data = static_cast<const uint8_t*>(data) + chunkSize;
    dstOffset += chunkSize;
  }
}

void MetalStagingDevice::uploadTexture(MTL::Texture* texture,
                                       uint32_t width,
                                       uint32_t height,
                                       uint32_t slice,
                                       uint32_t mipLevel,
                                       const void* data) {
  const uint64_t bytesPerRow = uint64_t(width) * bytesPerMetalPixel(texture->pixelFormat());
  const uint64_t storageSize = bytesPerRow * height;

  ensureStagingBufferSize(storageSize);
  LVK_ASSERT(storageSize <= stagingBufferSize_);

  MemoryRegionDesc desc = getNextFreeOffset(storageSize);
  if (desc.size_ < storageSize) {
    waitAndReset();
    desc = getNextFreeOffset(storageSize);
  }
  LVK_ASSERT(desc.size_ >= storageSize);

  std::memcpy(static_cast<uint8_t*>(stagingBuffer_->contents()) + desc.offset_, data, storageSize);

  ctx_.flushResidency();

  const MetalImmediateCommands::CommandBufferWrapper& wrapper = ctx_.immediate_->acquire();
  MTL4::ComputeCommandEncoder* enc = wrapper.cmdBuf->computeCommandEncoder();
  enc->copyFromBuffer(stagingBuffer_.get(),
                      desc.offset_,
                      bytesPerRow,
                      storageSize,
                      MTL::Size(width, height, 1),
                      texture,
                      slice,
                      mipLevel,
                      MTL::Origin(0, 0, 0));
  enc->barrierAfterStages(MTL::StageBlit | MTL::StageDispatch,
                          MTL::StageVertex | MTL::StageFragment | MTL::StageDispatch,
                          MTL4::VisibilityOptionDevice);
  enc->endEncoding();

  desc.handle_ = ctx_.immediate_->submit(wrapper);
  insertRegion(desc);
}

void MetalStagingDevice::ensureStagingBufferSize(uint64_t sizeNeeded) {
  const uint64_t alignedSize = std::max(getAlignedSize(sizeNeeded, kStagingBufferAlignment), minBufferSize_);

  sizeNeeded = alignedSize < maxBufferSize_ ? alignedSize : maxBufferSize_;

  if (stagingBuffer_) {
    const bool isEnoughSize = sizeNeeded <= stagingBufferSize_;
    const bool isMaxSize = stagingBufferSize_ == maxBufferSize_;
    if (isEnoughSize || isMaxSize) {
      return;
    }
  }

  waitAndReset();

  if (stagingBuffer_) {
    ctx_.removeResident(stagingBuffer_.get());
    stagingBuffer_ = nullptr;
  }

  stagingBufferSize_ = sizeNeeded;

  stagingBuffer_ = NS::TransferPtr(ctx_.device_->newBuffer(stagingBufferSize_, MTL::ResourceStorageModeShared));
  char debugName[64] = {0};
  (void)snprintf(debugName, sizeof(debugName) - 1, "Buffer: staging buffer %u", stagingBufferCounter_++);
  ns::setLabel(stagingBuffer_.get(), debugName);
  ctx_.addResident(stagingBuffer_.get());

  regions_.clear();
  regions_.push_back({0, stagingBufferSize_, SubmitHandle()});
}

void MetalStagingDevice::insertRegion(const MemoryRegionDesc& region) {
  std::vector<MemoryRegionDesc>::iterator it = regions_.begin();
  while (it != regions_.end() && it->offset_ < region.offset_) {
    ++it;
  }
  regions_.insert(it, region);
}

MetalStagingDevice::MemoryRegionDesc MetalStagingDevice::getNextFreeOffset(uint64_t size) {
  const uint64_t requestedAlignedSize = getAlignedSize(size, kStagingBufferAlignment);

  ensureStagingBufferSize(requestedAlignedSize);

  LVK_ASSERT(!regions_.empty());

  if (regions_.size() > 1) {
    size_t write = 0;
    bool curReady = ctx_.immediate_->isReady(regions_[0].handle_);
    for (size_t read = 1; read < regions_.size(); ++read) {
      MemoryRegionDesc& cur = regions_[write];
      const MemoryRegionDesc& next = regions_[read];
      const bool nextReady = ctx_.immediate_->isReady(next.handle_);
      if (curReady && nextReady && cur.offset_ + cur.size_ == next.offset_) {
        cur.size_ += next.size_;
        cur.handle_ = SubmitHandle();
      } else {
        regions_[++write] = next;
        curReady = nextReady;
      }
    }
    regions_.resize(write + 1);
  }

  std::vector<MemoryRegionDesc>::iterator bestNextIt = regions_.end();

  for (std::vector<MemoryRegionDesc>::iterator it = regions_.begin(); it != regions_.end(); ++it) {
    if (ctx_.immediate_->isReady(it->handle_)) {
      if (it->size_ >= requestedAlignedSize) {
        const uint64_t unusedSize = it->size_ - requestedAlignedSize;
        const uint64_t unusedOffset = it->offset_ + requestedAlignedSize;
        const MemoryRegionDesc result = {it->offset_, requestedAlignedSize, SubmitHandle()};
        regions_.erase(it);
        if (unusedSize > 0) {
          insertRegion({unusedOffset, unusedSize, SubmitHandle()});
        }
        return result;
      }
      if (bestNextIt == regions_.end() || it->size_ > bestNextIt->size_) {
        bestNextIt = it;
      }
    }
  }

  if (bestNextIt != regions_.end() && ctx_.immediate_->isReady(bestNextIt->handle_)) {
    const MemoryRegionDesc result = {bestNextIt->offset_, bestNextIt->size_, SubmitHandle()};
    regions_.erase(bestNextIt);
    return result;
  }

  waitAndReset();

  regions_.clear();

  const uint64_t unusedSize = stagingBufferSize_ > requestedAlignedSize ? stagingBufferSize_ - requestedAlignedSize : 0;

  if (unusedSize) {
    const uint64_t unusedOffset = stagingBufferSize_ - unusedSize;
    regions_.insert(regions_.begin(), {unusedOffset, unusedSize, SubmitHandle()});
  }

  return {0, stagingBufferSize_ - unusedSize, SubmitHandle()};
}

void MetalStagingDevice::waitAndReset() {
  for (const MemoryRegionDesc& r : regions_) {
    ctx_.immediate_->wait(r.handle_);
  }
  regions_.clear();
  regions_.push_back({0, stagingBufferSize_, SubmitHandle()});
}

} // namespace lvk::metal
