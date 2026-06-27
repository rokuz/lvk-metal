#pragma once

#include "lvk/LVK-Metal.h"
#include "lvk/metal/Common.h"

#include <vector>

namespace lvk::metal {

class MetalContext;

class MetalStagingDevice final {
 public:
  explicit MetalStagingDevice(MetalContext& ctx);
  ~MetalStagingDevice() = default;

  MetalStagingDevice(const MetalStagingDevice&) = delete;
  MetalStagingDevice& operator=(const MetalStagingDevice&) = delete;

  void uploadBuffer(MTL::Buffer* buffer, size_t dstOffset, size_t size, const void* data);
  void uploadTexture(MTL::Texture* texture, uint32_t width, uint32_t height, uint32_t slice, uint32_t mipLevel, const void* data);

 private:
  enum { kStagingBufferAlignment = 16 };

  struct MemoryRegionDesc {
    uint64_t offset_ = 0;
    uint64_t size_ = 0;
    SubmitHandle handle_ = {};
  };

  MemoryRegionDesc getNextFreeOffset(uint64_t size);
  void ensureStagingBufferSize(uint64_t sizeNeeded);
  void insertRegion(const MemoryRegionDesc& region);
  void waitAndReset();

  MetalContext& ctx_;
  NS::SharedPtr<MTL::Buffer> stagingBuffer_;
  uint64_t stagingBufferSize_ = 0;
  uint32_t stagingBufferCounter_ = 0;
  uint64_t maxBufferSize_ = 128u * 1024u * 1024u;
  uint64_t minBufferSize_ = 4u * 2048u * 2048u;
  std::vector<MemoryRegionDesc> regions_;
};

} // namespace lvk::metal
