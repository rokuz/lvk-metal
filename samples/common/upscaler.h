#pragma once

#include <cstdint>

#include <lvk/LVK-Metal.h>

namespace lvk::metal {

class Upscaler {
 public:
  bool init(IMetalContext& ctx, uint32_t fullWidth, uint32_t fullHeight, const char* packagePath, bool luma = false);
  void upscale(ICommandBuffer& cmd, TextureHandle src, TextureHandle dst);

  bool valid() const {
    return pipeline_.valid();
  }
  uint32_t srcWidth() const {
    return inW_;
  }
  uint32_t srcHeight() const {
    return inH_;
  }

 private:
  IMetalContext* ctx_ = nullptr;
  uint32_t inW_ = 0, inH_ = 0, outW_ = 0, outH_ = 0, inStride_ = 0, outStride_ = 0;
  uint32_t inCh_ = 3, outCh_ = 3;
  bool luma_ = false;
  Holder<BufferHandle> inBuf_;
  Holder<BufferHandle> outBuf_;
  Holder<TensorHandle> inTensor_;
  Holder<TensorHandle> outTensor_;
  Holder<MLPipelineHandle> pipeline_;
  Holder<SamplerHandle> sampler_;
  Holder<ComputePipelineHandle> pack_;
  Holder<ComputePipelineHandle> combine_;
};

} // namespace lvk::metal
