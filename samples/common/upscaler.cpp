#include "upscaler.h"

namespace lvk::metal {

static const char* kUpscaleMSL = R"MSL(
using namespace metal;

constant float3 kLuma = float3(0.299, 0.587, 0.114);
constant float3 kRyU = float3(-0.169, -0.331, 0.5);
constant float3 kRyV = float3(0.5, -0.419, -0.081);
constant float3 kYrR = float3(1.0, -0.00093, 1.401687);
constant float3 kYrG = float3(1.0, -0.3437, -0.71417);
constant float3 kYrB = float3(1.0, 1.77216, 0.00099);

struct PackConstants {
  device float* input;
  uint w;
  uint h;
  uint rowStride;
  uint srcImage;
  uint luma;
};

[[clang::annotate("lvk_numthreads(64,1,1)")]]
kernel void packInput(uint tid [[thread_position_in_grid]], constant PackConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  if (tid >= pc.w * pc.h)
    return;
  const uint x = tid % pc.w;
  const uint y = tid / pc.w;
  const float3 rgb = kImages2D.data[pc.srcImage].read(uint2(x, y)).rgb;
  device float* in = pc.input + y * pc.rowStride + x;
  if (pc.luma) {
    in[0] = dot(kLuma, rgb);
  } else {
    const uint plane = pc.h * pc.rowStride;
    in[0] = rgb.r;
    in[plane] = rgb.g;
    in[2 * plane] = rgb.b;
  }
}

struct CombineConstants {
  device const float* residual;
  uint w;
  uint h;
  uint rowStride;
  uint srcImage;
  uint srcSampler;
  uint dstImage;
  uint luma;
};

[[clang::annotate("lvk_numthreads(64,1,1)")]]
kernel void combineOutput(uint tid [[thread_position_in_grid]], constant CombineConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  if (tid >= pc.w * pc.h)
    return;
  const uint x = tid % pc.w;
  const uint y = tid / pc.w;
  device const float* r = pc.residual + y * pc.rowStride + x;
  const uint plane = pc.h * pc.rowStride;
  const float2 uv = float2((float(x) + 0.5) / float(pc.w), (float(y) + 0.5) / float(pc.h));
  const float3 base = textureBindless2D(pc.srcImage, pc.srcSampler, uv).rgb;
  float3 outc;
  if (pc.luma) {
    const float3 yuv = float3(dot(kLuma, base), dot(kRyU, base), dot(kRyV, base));
    const float newY = clamp(yuv.x + r[0], 0.0, 1.0);
    const float3 ny = float3(newY, yuv.yz);
    outc = float3(dot(kYrR, ny), dot(kYrG, ny), dot(kYrB, ny));
  } else {
    outc = base + float3(r[0], r[plane], r[2 * plane]);
  }
  kImages2D.data[pc.dstImage].write(float4(clamp(outc, 0.0, 1.0), 1.0), uint2(x, y));
}
)MSL";

bool Upscaler::init(IMetalContext& ctx, uint32_t fullWidth, uint32_t fullHeight, const char* packagePath, bool luma) {
  ctx_ = &ctx;
  luma_ = luma;
  inCh_ = luma ? 1 : 3;
  outCh_ = luma ? 1 : 3;
  inW_ = fullWidth / 2;
  inH_ = fullHeight / 2;
  outW_ = fullWidth;
  outH_ = fullHeight;
  inStride_ = mlTensorRowStrideElements(inW_, sizeof(float));
  outStride_ = mlTensorRowStrideElements(outW_, sizeof(float));

  inBuf_ = ctx.createBuffer({
      .usage = lvk::BufferUsageBits_Storage,
      .storage = lvk::StorageType_Device,
      .size = size_t(inCh_) * inH_ * inStride_ * sizeof(float),
      .debugName = "upscaler.input.buffer",
  });
  outBuf_ = ctx.createBuffer({
      .usage = lvk::BufferUsageBits_Storage,
      .storage = lvk::StorageType_Device,
      .size = size_t(outCh_) * outH_ * outStride_ * sizeof(float),
      .debugName = "upscaler.output.buffer",
  });
  inTensor_ = ctx.createTensor({
      .dataType = TensorDataType::Float32,
      .dimensions = {1, inCh_, inH_, inW_},
      .rank = 4,
      .buffer = inBuf_,
      .debugName = "upscaler.input",
  });
  outTensor_ = ctx.createTensor({
      .dataType = TensorDataType::Float32,
      .dimensions = {1, outCh_, outH_, outW_},
      .rank = 4,
      .buffer = outBuf_,
      .debugName = "upscaler.output",
  });
  lvk::Result res;
  pipeline_ = ctx.createMachineLearningPipeline(
      {
          .packagePath = packagePath,
          .functionName = "main",
          .inputs = {inTensor_},
          .numInputs = 1,
          .outputs = {outTensor_},
          .numOutputs = 1,
          .debugName = "upscaler.cunny",
      },
      &res);
  if (!pipeline_.valid())
    return false;

  sampler_ = ctx.createSampler({.debugName = "upscaler.sampler"});
  lvk::Holder<lvk::ShaderModuleHandle> pack = ctx.createShaderModule({kUpscaleMSL, "packInput", lvk::Stage_Comp, "upscaler.pack"});
  lvk::Holder<lvk::ShaderModuleHandle> combine =
      ctx.createShaderModule({kUpscaleMSL, "combineOutput", lvk::Stage_Comp, "upscaler.combine"});
  pack_ = ctx.createComputePipeline({.smComp = pack});
  combine_ = ctx.createComputePipeline({.smComp = combine});
  return true;
}

void Upscaler::upscale(ICommandBuffer& cmd, TextureHandle src, TextureHandle dst) {
  IMetalCommandBuffer& mcmd = static_cast<IMetalCommandBuffer&>(cmd);

  const struct {
    uint64_t input;
    uint32_t w;
    uint32_t h;
    uint32_t rowStride;
    uint32_t srcImage;
    uint32_t luma;
  } packPC = {ctx_->gpuAddress(inBuf_), inW_, inH_, inStride_, src.index(), luma_ ? 1u : 0u};
  cmd.cmdBindComputePipeline(pack_);
  cmd.cmdPushConstants(packPC);
  cmd.cmdDispatch({(inW_ * inH_ + 63) / 64, 1, 1});

  mcmd.cmdBindMachineLearningPipeline(pipeline_);
  mcmd.cmdDispatchNetwork();

  const struct {
    uint64_t residual;
    uint32_t w;
    uint32_t h;
    uint32_t rowStride;
    uint32_t srcImage;
    uint32_t srcSampler;
    uint32_t dstImage;
    uint32_t luma;
  } combinePC = {ctx_->gpuAddress(outBuf_), outW_, outH_, outStride_, src.index(), sampler_.index(), dst.index(), luma_ ? 1u : 0u};
  cmd.cmdBindComputePipeline(combine_);
  cmd.cmdPushConstants(combinePC);
  cmd.cmdDispatch({(outW_ * outH_ + 63) / 64, 1, 1}, {.storageImages = {dst}});
}

} // namespace lvk::metal
