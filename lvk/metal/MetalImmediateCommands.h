#pragma once

#include "lvk/LVK-Metal.h"
#include "lvk/metal/Common.h"

namespace lvk::metal {

class MetalImmediateCommands {
 public:
  static constexpr uint32_t kMaxCommandBuffers = 64;

  struct CommandBufferWrapper {
    NS::SharedPtr<MTL4::CommandBuffer> cmdBuf;
    NS::SharedPtr<MTL4::CommandAllocator> allocator;
    SubmitHandle handle;
    uint64_t lastSubmitValue = 0;
    uint32_t bufferIndex = 0;
    bool isEncoding = false;
    bool isPending = false;
  };

  MetalImmediateCommands(MTL::Device* device, MTL4::CommandQueue* queue, const char* debugName);
  ~MetalImmediateCommands();
  MetalImmediateCommands(const MetalImmediateCommands&) = delete;
  MetalImmediateCommands& operator=(const MetalImmediateCommands&) = delete;

  const CommandBufferWrapper& acquire();
  SubmitHandle submit(const CommandBufferWrapper& wrapper);
  void wait(SubmitHandle handle);
  void waitAll();
  [[nodiscard]] bool isReady(SubmitHandle handle) const;
  [[nodiscard]] SubmitHandle getLastSubmitHandle() const {
    return lastSubmitHandle_;
  }
  [[nodiscard]] SubmitHandle getNextSubmitHandle() const {
    return nextSubmitHandle_;
  }
  [[nodiscard]] MTL::SharedEvent* event() const {
    return event_.get();
  }

 private:
  void purge();

  MTL::Device* device_ = nullptr;
  MTL4::CommandQueue* queue_ = nullptr;
  NS::SharedPtr<MTL::SharedEvent> event_;
  const char* debugName_ = "";

  CommandBufferWrapper buffers_[kMaxCommandBuffers];
  SubmitHandle lastSubmitHandle_;
  SubmitHandle nextSubmitHandle_;
  uint32_t numAvailable_ = kMaxCommandBuffers;
  uint64_t submitCounter_ = 1;
};

} // namespace lvk::metal
