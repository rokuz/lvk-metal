#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifndef LVK_METAL_ML_ASSETS_DIR
#define LVK_METAL_ML_ASSETS_DIR "."
#endif

static const char* kNerfMSL = R"MSL(
using namespace metal;

struct EncodeConstants {
  device float* input;
  uint res;
  uint ns;
  uint encDim;
  uint lEmbed;
  uint rowStride;
  float focal;
  float nearPlane;
  float farPlane;
  float rot[9];
  float origin[3];
};

[[clang::annotate("lvk_numthreads(64,1,1)")]]
kernel void raygenEncode(uint tid [[thread_position_in_grid]], constant EncodeConstants& pc [[buffer(0)]]) {
  const uint numPts = pc.res * pc.res * pc.ns;
  if (tid >= numPts)
    return;
  const uint ray = tid / pc.ns;
  const uint s = tid % pc.ns;
  const uint px = ray % pc.res;
  const uint py = ray / pc.res;
  const float dcx = (float(px) - pc.res * 0.5) / pc.focal;
  const float dcy = -(float(py) - pc.res * 0.5) / pc.focal;
  const float dcz = -1.0;
  const float3 d = float3(pc.rot[0] * dcx + pc.rot[1] * dcy + pc.rot[2] * dcz,
                          pc.rot[3] * dcx + pc.rot[4] * dcy + pc.rot[5] * dcz,
                          pc.rot[6] * dcx + pc.rot[7] * dcy + pc.rot[8] * dcz);
  const float3 o = float3(pc.origin[0], pc.origin[1], pc.origin[2]);
  const float z = pc.nearPlane + (pc.farPlane - pc.nearPlane) * (pc.ns > 1 ? float(s) / float(pc.ns - 1) : 0.0);
  const float3 p = o + d * z;
  device float* out = pc.input + tid * pc.rowStride;
  out[0] = p.x;
  out[1] = p.y;
  out[2] = p.z;
  uint idx = 3;
  for (uint k = 0; k < pc.lEmbed; ++k) {
    const float f = float(1u << k);
    out[idx++] = sin(f * p.x);
    out[idx++] = sin(f * p.y);
    out[idx++] = sin(f * p.z);
    out[idx++] = cos(f * p.x);
    out[idx++] = cos(f * p.y);
    out[idx++] = cos(f * p.z);
  }
}

struct CompositeConstants {
  device const float* output;
  uint res;
  uint ns;
  uint rowStride;
  float nearPlane;
  float farPlane;
  uint outImage;
};

[[clang::annotate("lvk_numthreads(64,1,1)")]]
kernel void composite(uint tid [[thread_position_in_grid]], constant CompositeConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const uint numRays = pc.res * pc.res;
  if (tid >= numRays)
    return;
  device const float* base = pc.output + tid * pc.ns * pc.rowStride;
  float3 acc = float3(0.0);
  float trans = 1.0;
  for (uint s = 0; s < pc.ns; ++s) {
    device const float* o = base + s * pc.rowStride;
    const float sigma = max(o[3], 0.0) + log(1.0 + exp(-abs(o[3])));
    const float z0 = pc.nearPlane + (pc.farPlane - pc.nearPlane) * float(s) / float(pc.ns - 1);
    float dist = 1e10;
    if (s + 1 < pc.ns)
      dist = (pc.farPlane - pc.nearPlane) / float(pc.ns - 1);
    const float alpha = 1.0 - exp(-sigma * dist);
    const float w = alpha * trans;
    acc += w * float3(1.0 / (1.0 + exp(-o[0])), 1.0 / (1.0 + exp(-o[1])), 1.0 / (1.0 + exp(-o[2])));
    trans *= (1.0 - alpha) + 1e-10;
  }
  kImages2D.data[pc.outImage].write(float4(acc, 1.0), uint2(tid % pc.res, tid / pc.res));
}

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

struct PresentConstants {
  uint tex;
  uint smp;
};

vertex VertexOut presentVert(uint vid [[vertex_id]]) {
  VertexOut out;
  out.uv = float2((vid << 1) & 2, vid & 2);
  out.position = float4(out.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return out;
}

fragment float4 presentFrag(VertexOut in [[stage_in]], constant PresentConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindless2D(pc.tex, pc.smp, in.uv);
}
)MSL";

namespace {

struct NerfParams {
  uint32_t inputIndex = 0;
  uint32_t outputIndex = 1;
  uint32_t lEmbed = 0;
  uint32_t encDim = 0;
  float fovDeg = 0.0f;
  float nearPlane = 0.0f;
  float farPlane = 0.0f;
  float radius = 0.0f;
  float elevationDeg = 0.0f;
};

bool loadParams(const std::string& path, NerfParams& p) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f)
    return false;
  uint32_t u[4] = {};
  float v[5] = {};
  bool ok = fread(u, sizeof(u), 1, f) == 1;
  ok = ok && fread(v, sizeof(v), 1, f) == 1;
  fclose(f);
  p.inputIndex = u[0];
  p.outputIndex = u[1];
  p.lEmbed = u[2];
  p.encDim = u[3];
  p.fovDeg = v[0];
  p.nearPlane = v[1];
  p.farPlane = v[2];
  p.radius = v[3];
  p.elevationDeg = v[4];
  return ok;
}

// Camera-to-world matrix orbiting the origin, matching tools/ML/nerf.py::pose_spherical.
glm::mat4 poseSpherical(float azimuthDeg, float elevationDeg, float radius) {
  const glm::mat4 flip = glm::mat4(glm::vec4(-1, 0, 0, 0), glm::vec4(0, 0, 1, 0), glm::vec4(0, 1, 0, 0), glm::vec4(0, 0, 0, 1));
  const glm::mat4 rotY = glm::rotate(glm::mat4(1.0f), glm::radians(azimuthDeg), glm::vec3(0, 1, 0));
  const glm::mat4 rotX = glm::rotate(glm::mat4(1.0f), glm::radians(-elevationDeg), glm::vec3(1, 0, 0));
  const glm::mat4 trans = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, radius));
  return flip * rotY * rotX * trans;
}

struct EncodeConstants {
  uint64_t input;
  uint32_t res;
  uint32_t ns;
  uint32_t encDim;
  uint32_t lEmbed;
  uint32_t rowStride;
  float focal;
  float nearPlane;
  float farPlane;
  float rot[9];
  float origin[3];
};

struct CompositeConstants {
  uint64_t output;
  uint32_t res;
  uint32_t ns;
  uint32_t rowStride;
  float nearPlane;
  float farPlane;
  uint32_t outImage;
};

} // namespace

class MachineLearning final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t, uint32_t, float, uint32_t) override {
    ctx_ = &ctx;

    const std::string dir = LVK_METAL_ML_ASSETS_DIR;
    if (!loadParams(dir + "/nerf.bin", params_)) {
      LLOGE("MachineLearning: failed to load %s/nerf.bin", dir.c_str());
      return;
    }
    const uint32_t encDim = params_.encDim;
    const uint32_t numPts = kRes * kRes * kSamples;
    inputRowStride_ = lvk::metal::mlTensorRowStrideElements(encDim, sizeof(float));
    outputRowStride_ = lvk::metal::mlTensorRowStrideElements(4, sizeof(float));

    inputBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = size_t(numPts) * inputRowStride_ * sizeof(float),
        .debugName = "nerf.input.buffer",
    });
    outputBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = size_t(numPts) * outputRowStride_ * sizeof(float),
        .debugName = "nerf.output.buffer",
    });
    input_ = ctx.createTensor({
        .dataType = lvk::metal::TensorDataType::Float32,
        .dimensions = {numPts, encDim},
        .rank = 2,
        .buffer = inputBuffer_,
        .debugName = "nerf.input",
    });
    output_ = ctx.createTensor({
        .dataType = lvk::metal::TensorDataType::Float32,
        .dimensions = {numPts, 4},
        .rank = 2,
        .buffer = outputBuffer_,
        .debugName = "nerf.output",
    });

    const std::string pkgPath = dir + "/nerf.mtlpackage";
    lvk::Result res;
    pipeline_ = ctx.createMachineLearningPipeline(
        {
            .packagePath = pkgPath.c_str(),
            .functionName = "main",
            .inputs = {input_},
            .numInputs = 1,
            .outputs = {output_},
            .numOutputs = 1,
            .debugName = "nerf",
        },
        &res);
    if (!pipeline_.valid()) {
      LLOGE("MachineLearning: createMachineLearningPipeline failed");
      return;
    }

    image_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_BGRA_UN8,
        .dimensions = {kRes, kRes, 1},
        .usage = lvk::TextureUsageBits_Storage | lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .debugName = "nerf.image",
    });
    sampler_ = ctx.createSampler({.debugName = "nerf.sampler"});

    lvk::Holder<lvk::ShaderModuleHandle> raygen = ctx.createShaderModule({kNerfMSL, "raygenEncode", lvk::Stage_Comp, "nerf.raygen"});
    lvk::Holder<lvk::ShaderModuleHandle> comp = ctx.createShaderModule({kNerfMSL, "composite", lvk::Stage_Comp, "nerf.composite"});
    raygenPipeline_ = ctx.createComputePipeline({.smComp = raygen});
    compositePipeline_ = ctx.createComputePipeline({.smComp = comp});

    lvk::Holder<lvk::ShaderModuleHandle> vert = ctx.createShaderModule({kNerfMSL, "presentVert", lvk::Stage_Vert, "nerf.present.vert"});
    lvk::Holder<lvk::ShaderModuleHandle> frag = ctx.createShaderModule({kNerfMSL, "presentFrag", lvk::Stage_Frag, "nerf.present.frag"});
    present_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "nerf.present",
    });
    ready_ = true;
    LLOGL("MachineLearning: GPU-driven NeRF %ux%u, %u samples/ray, %u pts/frame (compute->ML->compute->present)",
          kRes, kRes, kSamples, numPts);
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float timeSeconds) override {
    if (!ready_) {
      cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .clearColor = {.float32 = {0.7f, 0.1f, 0.1f, 1.0f}}}}},
                            {.color = {{.texture = target}}});
      cmd.cmdEndRendering();
      return;
    }
    lvk::metal::IMetalCommandBuffer& mcmd = static_cast<lvk::metal::IMetalCommandBuffer&>(cmd);

    const uint32_t numPts = kRes * kRes * kSamples;
    const uint32_t numRays = kRes * kRes;
    const float focal = 0.5f * kRes / std::tan(0.5f * params_.fovDeg * float(M_PI) / 180.0f);

    const glm::mat4 c2w = poseSpherical(timeSeconds * (360.0f / kPeriodSec), params_.elevationDeg, params_.radius);

    const glm::mat3 correction = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(180.0f), glm::vec3(0, 1, 0)) *
                                           glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1, 0, 0)));
    const glm::mat3 basis = correction * glm::mat3(c2w);
    const glm::vec3 origin = correction * glm::vec3(c2w[3]);

    EncodeConstants enc = {
        .input = ctx_->gpuAddress(inputBuffer_),
        .res = kRes,
        .ns = kSamples,
        .encDim = params_.encDim,
        .lEmbed = params_.lEmbed,
        .rowStride = inputRowStride_,
        .focal = focal,
        .nearPlane = params_.nearPlane,
        .farPlane = params_.farPlane,
        .rot = {basis[0][0], basis[1][0], basis[2][0], basis[0][1], basis[1][1], basis[2][1], basis[0][2], basis[1][2], basis[2][2]},
        .origin = {origin.x, origin.y, origin.z},
    };
    cmd.cmdBindComputePipeline(raygenPipeline_);
    cmd.cmdPushConstants(enc);
    cmd.cmdDispatch({(numPts + 63) / 64, 1, 1});

    mcmd.cmdBindMachineLearningPipeline(pipeline_);
    mcmd.cmdDispatchNetwork();

    const CompositeConstants cc = {
        .output = ctx_->gpuAddress(outputBuffer_),
        .res = kRes,
        .ns = kSamples,
        .rowStride = outputRowStride_,
        .nearPlane = params_.nearPlane,
        .farPlane = params_.farPlane,
        .outImage = image_.index(),
    };
    cmd.cmdBindComputePipeline(compositePipeline_);
    cmd.cmdPushConstants(cc);
    cmd.cmdDispatch({(numRays + 63) / 64, 1, 1}, {.storageImages = {image_}});

    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.05f, 0.05f, 0.07f, 1.0f}}}}},
        {.color = {{.texture = target}}});
    const PresentConstantsCpu pc = {.tex = image_.index(), .smp = sampler_.index()};
    cmd.cmdBindRenderPipeline(present_);
    cmd.cmdPushConstants(pc);
    cmd.cmdDraw(3);
    cmd.cmdEndRendering();
  }

 private:
  struct PresentConstantsCpu {
    uint32_t tex;
    uint32_t smp;
  };

  static constexpr uint32_t kRes = 112;
  static constexpr uint32_t kSamples = 48;
  static constexpr float kPeriodSec = 8.0f;

  lvk::metal::IMetalContext* ctx_ = nullptr;
  NerfParams params_;
  uint32_t inputRowStride_ = 0;
  uint32_t outputRowStride_ = 0;
  lvk::Holder<lvk::metal::MLPipelineHandle> pipeline_;
  lvk::Holder<lvk::BufferHandle> inputBuffer_;
  lvk::Holder<lvk::BufferHandle> outputBuffer_;
  lvk::Holder<lvk::metal::TensorHandle> input_;
  lvk::Holder<lvk::metal::TensorHandle> output_;
  lvk::Holder<lvk::TextureHandle> image_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::ComputePipelineHandle> raygenPipeline_;
  lvk::Holder<lvk::ComputePipelineHandle> compositePipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> present_;
  bool ready_ = false;
};

DESKTOP_MAIN(MachineLearning)
