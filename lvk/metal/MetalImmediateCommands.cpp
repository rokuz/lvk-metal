#include "lvk/metal/MetalImmediateCommands.h"

namespace lvk::metal {

namespace {

SubmitHandle makeSubmitHandle(uint64_t submitId, uint32_t bufferIndex) {
  SubmitHandle handle;
  handle.submitId_ = static_cast<uint32_t>(submitId);
  handle.bufferIndex_ = bufferIndex;
  return handle;
}

} // namespace

MetalImmediateCommands::MetalImmediateCommands(MTL::Device* device, MTL4::CommandQueue* queue, const char* debugName)
: device_(device)
, queue_(queue)
, debugName_(debugName) {
  event_ = NS::TransferPtr(device_->newSharedEvent());
  event_->setSignaledValue(0);
  ns::setLabel(event_.get(), debugName);
  for (uint32_t i = 0; i < kMaxCommandBuffers; ++i)
    buffers_[i].bufferIndex = i;
}

MetalImmediateCommands::~MetalImmediateCommands() {
  waitAll();
}

void MetalImmediateCommands::purge() {
  const uint64_t completed = event_->signaledValue();
  for (CommandBufferWrapper& buf : buffers_) {
    if (!buf.isPending || buf.isEncoding)
      continue;
    if (completed >= buf.lastSubmitValue) {
      buf.allocator->reset();
      buf.isPending = false;
      numAvailable_++;
    }
  }
}

const MetalImmediateCommands::CommandBufferWrapper& MetalImmediateCommands::acquire() {
  if (!numAvailable_)
    purge();

  while (!numAvailable_) {
    uint64_t minPending = UINT64_MAX;
    for (const CommandBufferWrapper& buf : buffers_)
      if (buf.isPending && buf.lastSubmitValue < minPending)
        minPending = buf.lastSubmitValue;
    if (minPending != UINT64_MAX)
      event_->waitUntilSignaledValue(minPending, UINT64_MAX);
    purge();
  }

  CommandBufferWrapper* current = nullptr;
  for (CommandBufferWrapper& buf : buffers_) {
    if (!buf.isEncoding && !buf.isPending && !buf.isDeferred) {
      current = &buf;
      break;
    }
  }
  LVK_ASSERT(current);

  if (!current->allocator) {
    current->allocator = NS::TransferPtr(device_->newCommandAllocator());
    current->cmdBuf = NS::TransferPtr(device_->newCommandBuffer());
  }

  current->handle = makeSubmitHandle(submitCounter_, current->bufferIndex);
  current->isEncoding = true;
  numAvailable_--;
  current->cmdBuf->beginCommandBuffer(current->allocator.get());

  nextSubmitHandle_ = current->handle;
  return *current;
}

SubmitHandle MetalImmediateCommands::submit(const CommandBufferWrapper& wrapper) {
  CommandBufferWrapper& buf = buffers_[wrapper.bufferIndex];
  LVK_ASSERT(buf.isEncoding);

  buf.cmdBuf->endCommandBuffer();

  MTL4::CommandBuffer* cb = buf.cmdBuf.get();
  queue_->commit(&cb, 1);
  queue_->signalEvent(event_.get(), submitCounter_);

  buf.handle = makeSubmitHandle(submitCounter_, buf.bufferIndex);
  buf.lastSubmitValue = submitCounter_;
  buf.isEncoding = false;
  buf.isPending = true;

  lastSubmitHandle_ = buf.handle;
  submitCounter_++;
  if (submitCounter_ == 0)
    submitCounter_ = 1;

  return lastSubmitHandle_;
}

SubmitHandle MetalImmediateCommands::submitDeferred(const CommandBufferWrapper& wrapper) {
  CommandBufferWrapper& buf = buffers_[wrapper.bufferIndex];
  LVK_ASSERT(buf.isEncoding);

  buf.cmdBuf->endCommandBuffer();

  buf.handle = makeSubmitHandle(submitCounter_, buf.bufferIndex);
  buf.lastSubmitValue = submitCounter_;
  buf.isEncoding = false;
  buf.isDeferred = true;

  deferred_.push_back(buf.bufferIndex);

  submitCounter_++;
  if (submitCounter_ == 0)
    submitCounter_ = 1;

  return buf.handle;
}

void MetalImmediateCommands::flushDeferred() {
  for (uint32_t idx : deferred_) {
    CommandBufferWrapper& buf = buffers_[idx];
    MTL4::CommandBuffer* cb = buf.cmdBuf.get();
    queue_->commit(&cb, 1);
    queue_->signalEvent(event_.get(), buf.lastSubmitValue);
    buf.isDeferred = false;
    buf.isPending = true;
    lastSubmitHandle_ = buf.handle;
  }
  deferred_.clear();
}

bool MetalImmediateCommands::isReady(SubmitHandle handle) const {
  if (handle.empty())
    return true;
  return event_->signaledValue() >= handle.submitId_;
}

void MetalImmediateCommands::wait(SubmitHandle handle) {
  if (handle.empty()) {
    waitAll();
    return;
  }
  if (!isReady(handle))
    event_->waitUntilSignaledValue(handle.submitId_, UINT64_MAX);
  purge();
}

void MetalImmediateCommands::waitAll() {
  if (!lastSubmitHandle_.empty())
    event_->waitUntilSignaledValue(lastSubmitHandle_.submitId_, UINT64_MAX);
  purge();
}

} // namespace lvk::metal
