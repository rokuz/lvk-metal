#include <cstdint>
#include <cstdlib>
#include <vector>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct PC {
  device const float4* tints;
  device const float4* copyTint;
  device const float4* fillBrightness;
  uint texCopied;
  uint texCleared;
  uint smp;
};

struct V2F {
  float4 position [[position]];
  float2 uv;
  float4 tint;
};

vertex V2F transferVertexMain(uint vid [[vertex_id]], constant PC& pc [[buffer(0)]]) {
  const float2 p[6] = { float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(1, 1), float2(-1, 1) };
  const float2 uvs[6] = { float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 1), float2(1, 0), float2(0, 0) };
  V2F o;
  o.position = float4(p[vid], 0.0, 1.0);
  o.uv = uvs[vid];
  o.tint = pc.tints[vid];
  return o;
}

fragment float4 transferFragmentMain(V2F in [[stage_in]], constant PC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const float4 texColor = in.uv.x < 0.5
      ? textureBindless2D(pc.texCopied, pc.smp, in.uv * float2(2.0, 1.0))
      : textureBindless2D(pc.texCleared, pc.smp, (in.uv - float2(0.5, 0.0)) * float2(2.0, 1.0));
  const float3 copyTint = pc.copyTint[0].rgb;
  const float brightness = pc.fillBrightness[0].r;
  return float4(in.tint.rgb * texColor.rgb * copyTint * brightness, 1.0);
}
)";

class TransferOps final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "transfer", "transferVertexMain", lvk::Stage_Vert, "transfer.vert");
      frag = slang.createShaderModule(ctx, "transfer", "transferFragmentMain", lvk::Stage_Frag, "transfer.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "transferVertexMain", lvk::Stage_Vert, "transfer.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "transferFragmentMain", lvk::Stage_Frag, "transfer.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "transfer",
    });

    sampler_ = ctx.createSampler({.minFilter = lvk::SamplerFilter_Linear, .magFilter = lvk::SamplerFilter_Linear, .debugName = "linear"});

    bufTint_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                 .storage = lvk::StorageType_Device,
                                 .size = sizeof(float) * 4 * 6,
                                 .debugName = "Buffer: tints (cmdUpdateBuffer dst)"});
    bufFill_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                 .storage = lvk::StorageType_Device,
                                 .size = sizeof(float) * 4,
                                 .debugName = "Buffer: fill brightness (cmdFillBuffer dst)"});

    const float copySrc[4] = {1.0f, 0.92f, 0.85f, 1.0f};
    bufCopySrc_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                    .storage = lvk::StorageType_Device,
                                    .size = sizeof(copySrc),
                                    .data = copySrc,
                                    .debugName = "Buffer: copy source"});
    bufCopyDst_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                    .storage = lvk::StorageType_Device,
                                    .size = sizeof(copySrc),
                                    .debugName = "Buffer: copy dest (cmdCopyBuffer dst)"});

    std::vector<uint8_t> pattern(size_t(kTexW) * kTexH * 4);
    for (uint32_t y = 0; y < kTexH; ++y) {
      for (uint32_t x = 0; x < kTexW; ++x) {
        uint8_t* px = &pattern[(size_t(y) * kTexW + x) * 4];
        const bool left = x < kTexW / 2;
        px[0] = left ? 255 : 0; // B
        px[1] = left ? 0 : 255; // G
        px[2] = 255; // R
        px[3] = 255; // A
      }
    }
    texSrc_ = ctx.createTexture({.type = lvk::TextureType_2D,
                                 .format = lvk::Format_BGRA_UN8,
                                 .dimensions = {kTexW, kTexH},
                                 .usage = lvk::TextureUsageBits_Sampled,
                                 .storage = lvk::StorageType_Device,
                                 .data = pattern.data(),
                                 .debugName = "Texture: copy source"});
    texCopied_ = ctx.createTexture({.type = lvk::TextureType_2D,
                                    .format = lvk::Format_BGRA_UN8,
                                    .dimensions = {kTexW, kTexH},
                                    .usage = lvk::TextureUsageBits_Sampled,
                                    .storage = lvk::StorageType_Device,
                                    .debugName = "Texture: copy dest (cmdCopyImage dst)"});
    texCleared_ = ctx.createTexture({.type = lvk::TextureType_2D,
                                     .format = lvk::Format_BGRA_UN8,
                                     .dimensions = {kTexW, kTexH},
                                     .usage = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Attachment,
                                     .storage = lvk::StorageType_Device,
                                     .debugName = "Texture: cleared (cmdClearColorImage dst)"});
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    const float tints[6][4] = {
        {1.0f, 0.85f, 0.85f, 1.0f},
        {0.85f, 1.0f, 0.85f, 1.0f},
        {0.85f, 0.85f, 1.0f, 1.0f},
        {0.85f, 1.0f, 0.85f, 1.0f},
        {1.0f, 1.0f, 0.85f, 1.0f},
        {0.85f, 0.85f, 1.0f, 1.0f},
    };
    cmd.cmdUpdateBuffer(bufTint_, 0, sizeof(tints), tints);
    cmd.cmdFillBuffer(bufFill_, 0, lvk::LVK_WHOLE_SIZE, 0x3f3f3f3f);
    cmd.cmdCopyBuffer(bufCopySrc_, bufCopyDst_, 0, 0, sizeof(float) * 4);

    cmd.cmdCopyImage(texSrc_, texCopied_, {kTexW, kTexH, 1});
    cmd.cmdClearColorImage(texCleared_, {.float32 = {0.0f, 0.55f, 0.55f, 1.0f}});

    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.05f, 0.05f, 0.07f, 1.0f}}}}},
        {.color = {{.texture = target}}},
        {.sampledImages = {texCopied_, texCleared_}, .buffers = {bufTint_, bufCopyDst_, bufFill_}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(pipeline_);
    const struct {
      uint64_t tints;
      uint64_t copyTint;
      uint64_t fillBrightness;
      uint32_t texCopied;
      uint32_t texCleared;
      uint32_t smp;
    } pc = {
        .tints = ctx_->gpuAddress(bufTint_),
        .copyTint = ctx_->gpuAddress(bufCopyDst_),
        .fillBrightness = ctx_->gpuAddress(bufFill_),
        .texCopied = texCopied_.index(),
        .texCleared = texCleared_.index(),
        .smp = sampler_.index(),
    };
    cmd.cmdPushConstants(pc);
    cmd.cmdDraw(6);
    cmd.cmdEndRendering();
  }

 private:
  static constexpr uint32_t kTexW = 64;
  static constexpr uint32_t kTexH = 64;

  lvk::metal::IMetalContext* ctx_ = nullptr;
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::BufferHandle> bufTint_;
  lvk::Holder<lvk::BufferHandle> bufFill_;
  lvk::Holder<lvk::BufferHandle> bufCopySrc_;
  lvk::Holder<lvk::BufferHandle> bufCopyDst_;
  lvk::Holder<lvk::TextureHandle> texSrc_;
  lvk::Holder<lvk::TextureHandle> texCopied_;
  lvk::Holder<lvk::TextureHandle> texCleared_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(TransferOps)
