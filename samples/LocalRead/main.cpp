#include <cstdint>
#include <cstdlib>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct PerFrame {
  float4x4 mvp;
  float4x4 model;
  float4 cameraPos;
  uint texAlbedo;
  uint texNormal;
  uint texHeight;
};

struct DeferredPC {
  device const PerFrame* perFrame;
};

constant float3 kPositions[24] = {
  float3(-1,-1, 1), float3( 1,-1, 1), float3( 1, 1, 1), float3(-1, 1, 1),
  float3( 1,-1,-1), float3(-1,-1,-1), float3(-1, 1,-1), float3( 1, 1,-1),
  float3( 1,-1, 1), float3( 1,-1,-1), float3( 1, 1,-1), float3( 1, 1, 1),
  float3(-1,-1,-1), float3(-1,-1, 1), float3(-1, 1, 1), float3(-1, 1,-1),
  float3(-1, 1, 1), float3( 1, 1, 1), float3( 1, 1,-1), float3(-1, 1,-1),
  float3(-1,-1,-1), float3( 1,-1,-1), float3( 1,-1, 1), float3(-1,-1, 1),
};

constant float3 kNormals[24] = {
  float3( 0, 0, 1), float3( 0, 0, 1), float3( 0, 0, 1), float3( 0, 0, 1),
  float3( 0, 0,-1), float3( 0, 0,-1), float3( 0, 0,-1), float3( 0, 0,-1),
  float3( 1, 0, 0), float3( 1, 0, 0), float3( 1, 0, 0), float3( 1, 0, 0),
  float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0),
  float3( 0, 1, 0), float3( 0, 1, 0), float3( 0, 1, 0), float3( 0, 1, 0),
  float3( 0,-1, 0), float3( 0,-1, 0), float3( 0,-1, 0), float3( 0,-1, 0),
};

constant float2 kUVs[24] = {
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
  float2(0,1), float2(1,1), float2(1,0), float2(0,0),
};

struct DeferredOut {
  float4 position [[position]];
  float2 uv;
  float3 normal;
  float3 worldPos;
  float3 eyeDir;
};

vertex DeferredOut deferredVertexMain(uint vid [[vertex_id]], constant DeferredPC& pc [[buffer(0)]]) {
  device const PerFrame& pf = *pc.perFrame;
  const float3 p = kPositions[vid];
  const float3 wp = (pf.model * float4(p, 1.0)).xyz;
  DeferredOut o;
  o.position = pf.mvp * float4(p, 1.0);
  o.uv       = kUVs[vid];
  o.normal   = (pf.model * float4(kNormals[vid], 0.0)).xyz;
  o.worldPos = wp;
  o.eyeDir   = pf.cameraPos.xyz - wp;
  return o;
}

struct GBufferOut {
  float4 albedo   [[color(1)]];
  float4 normal   [[color(2)]];
  float4 worldPos [[color(3)]];
};

constant float kHeightScale = 0.03;
constant int kParallaxSteps = 8;

float3x3 cotangentFrame(float3 N, float3 p, float2 uv) {
  const float3 dp1 = dfdx(p);
  const float3 dp2 = dfdy(p);
  const float2 duv1 = dfdx(uv);
  const float2 duv2 = dfdy(uv);
  const float3 dp2perp = cross(dp2, N);
  const float3 dp1perp = cross(N, dp1);
  const float3 T = normalize(dp2perp * duv1.x + dp1perp * duv2.x);
  const float3 B = normalize(dp2perp * duv1.y + dp1perp * duv2.y);
  return float3x3(T, B, N);
}

fragment GBufferOut deferredFragmentMain(DeferredOut in [[stage_in]], constant DeferredPC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  device const PerFrame& pf = *pc.perFrame;
  const float3 N = normalize(in.normal);
  const float3 V = normalize(in.eyeDir);
  const float3x3 TBN = cotangentFrame(N, in.worldPos, in.uv);
  const float3 viewTS = V * TBN;

  const float layerStep = 1.0 / float(kParallaxSteps);
  const float invViewZ = 1.0 / max(viewTS.z, 0.01);
  const float2 deltaUV = viewTS.xy * (invViewZ * kHeightScale * layerStep);

  float2 cur = in.uv;
  float curLayer = 0.0;
  float curDepth = 1.0 - textureBindless2DLod(pf.texHeight, 0, cur, 0.0).r;
  float2 prevCur = cur;
  float prevLayer = curLayer;
  float prevDepth = curDepth;
  for (int i = 0; i < kParallaxSteps && curLayer < curDepth; ++i) {
    prevCur = cur; prevLayer = curLayer; prevDepth = curDepth;
    cur -= deltaUV; curLayer += layerStep;
    curDepth = 1.0 - textureBindless2DLod(pf.texHeight, 0, cur, 0.0).r;
  }
  for (int k = 0; k < 3; ++k) {
    const float2 midCur = 0.5 * (prevCur + cur);
    const float midLayer = 0.5 * (prevLayer + curLayer);
    const float midDepth = 1.0 - textureBindless2DLod(pf.texHeight, 0, midCur, 0.0).r;
    const bool advance = midLayer < midDepth;
    prevCur = advance ? midCur : prevCur;
    prevLayer = advance ? midLayer : prevLayer;
    prevDepth = advance ? midDepth : prevDepth;
    cur = advance ? cur : midCur;
    curLayer = advance ? curLayer : midLayer;
    curDepth = advance ? curDepth : midDepth;
  }
  const float after = curDepth - curLayer;
  const float before = prevDepth - prevLayer;
  const float weight = after / (after - before);
  const float2 uv = mix(cur, prevCur, weight);

  const float3 nm = textureBindless2D(pf.texNormal, 0, uv).xyz * 2.0 - 1.0;
  const float3 n = normalize(TBN * nm);
  GBufferOut o;
  o.albedo   = 10.0 * textureBindless2D(pf.texAlbedo, 0, uv);
  o.normal   = float4(n * 0.5 + 0.5, 1.0);
  o.worldPos = float4(in.worldPos, 1.0);
  return o;
}

struct ComposeOut {
  float4 position [[position]];
};

vertex ComposeOut composeVertexMain(uint vid [[vertex_id]]) {
  const float2 uv = float2((vid << 1) & 2, vid & 2);
  ComposeOut o;
  o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}

struct GBufferIn {
  float4 albedo   [[color(1)]];
  float4 normal   [[color(2)]];
  float4 worldPos [[color(3)]];
};

fragment float4 composeFragmentMain(GBufferIn gbuf) {
  const float3 albedo   = gbuf.albedo.rgb;
  const float3 N        = normalize(gbuf.normal.xyz * 2.0 - 1.0);
  const float3 worldPos = gbuf.worldPos.xyz;
  const float3 L = normalize(float3(1.0, 1.0, -1.0) - worldPos);
  const float NdotL = clamp(dot(N, L), 0.3, 1.0);
  return float4(NdotL * albedo, 1.0);
}
)";

struct PerFrame {
  glm::mat4 mvp;
  glm::mat4 model;
  glm::vec4 cameraPos;
  uint32_t texAlbedo;
  uint32_t texNormal;
  uint32_t texHeight;
};

class LocalRead final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    const uint16_t indexData[36] = {0,  1,  2,  2,  3,  0,  4,  5,  6,  6,  7,  4,  8,  9,  10, 10, 11, 8,
                                    12, 13, 14, 14, 15, 12, 16, 17, 18, 18, 19, 16, 20, 21, 22, 22, 23, 20};
    ib_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Index,
        .storage = lvk::StorageType_Device,
        .size = sizeof(indexData),
        .data = indexData,
        .debugName = "Buffer: index",
    });

    const auto loadTexture = [&](const char* file) {
      const std::string path = std::string(LVK_METAL_LOCALREAD_TEXTURES_DIR) + "/" + file;
      int w = 0, h = 0, comp = 0;
      stbi_uc* pixels = stbi_load(path.c_str(), &w, &h, &comp, 4);
      if (!pixels) {
        LLOGW("LocalRead: failed to load %s", path.c_str());
        return lvk::Holder<lvk::TextureHandle>{};
      }
      lvk::Holder<lvk::TextureHandle> tex = ctx.createTexture({
          .type = lvk::TextureType_2D,
          .format = lvk::Format_RGBA_UN8,
          .dimensions = {uint32_t(w), uint32_t(h)},
          .usage = lvk::TextureUsageBits_Sampled,
          .numMipLevels = lvk::calcNumMipLevels(uint32_t(w), uint32_t(h)),
          .storage = lvk::StorageType_Device,
          .data = pixels,
          .generateMipmaps = true,
          .debugName = file,
      });
      stbi_image_free(pixels);
      return tex;
    };
    texAlbedo_ = loadTexture("Iron_Bars_001_basecolor.jpg");
    texNormal_ = loadTexture("Iron_Bars_001_normal.jpg");
    texHeight_ = loadTexture("Iron_Bars_001_height.png");

    perFrameBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Uniform,
        .storage = lvk::StorageType_Device,
        .size = sizeof(PerFrame),
        .debugName = "Buffer: perFrame",
    });
    addrPerFrame_ = ctx.gpuAddress(perFrameBuffer_);

    const auto makeGBuffer = [&](lvk::Format format, const char* name) {
      return ctx.createTexture({
          .type = lvk::TextureType_2D,
          .format = format,
          .dimensions = {width_, height_},
          .usage = lvk::TextureUsageBits_Attachment,
          .storage = lvk::StorageType_Memoryless,
          .debugName = name,
      });
    };
    gAlbedo_ = makeGBuffer(lvk::Format_BGRA_UN8, "GBuffer: albedo");
    gNormal_ = makeGBuffer(lvk::Format_RGBA_UN8, "GBuffer: normal");
    gWorldPos_ = makeGBuffer(lvk::Format_RGBA_F32, "GBuffer: worldPos");

    depth_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_Z_F32,
        .dimensions = {width_, height_},
        .usage = lvk::TextureUsageBits_Attachment,
        .storage = lvk::StorageType_Memoryless,
        .debugName = "depth",
    });

    lvk::Holder<lvk::ShaderModuleHandle> deferredVert;
    lvk::Holder<lvk::ShaderModuleHandle> deferredFrag;
    lvk::Holder<lvk::ShaderModuleHandle> composeVert;
    lvk::Holder<lvk::ShaderModuleHandle> composeFrag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      deferredVert = slang.createShaderModule(ctx, "localread", "deferredVertexMain", lvk::Stage_Vert, "deferred.vert");
      deferredFrag = slang.createShaderModule(ctx, "localread", "deferredFragmentMain", lvk::Stage_Frag, "deferred.frag");
      composeVert = slang.createShaderModule(ctx, "localread", "composeVertexMain", lvk::Stage_Vert, "compose.vert");
      composeFrag = slang.createShaderModule(ctx, "localread", "composeFragmentMain", lvk::Stage_Frag, "compose.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      deferredVert = ctx.createShaderModule({kShaderMSL, "deferredVertexMain", lvk::Stage_Vert, "deferred.vert"});
      deferredFrag = ctx.createShaderModule({kShaderMSL, "deferredFragmentMain", lvk::Stage_Frag, "deferred.frag"});
      composeVert = ctx.createShaderModule({kShaderMSL, "composeVertexMain", lvk::Stage_Vert, "compose.vert"});
      composeFrag = ctx.createShaderModule({kShaderMSL, "composeFragmentMain", lvk::Stage_Frag, "compose.frag"});
    }

    const lvk::ColorAttachment attachments[4] = {
        {.format = ctx.getSwapchainFormat()},
        {.format = ctx.getFormat(gAlbedo_)},
        {.format = ctx.getFormat(gNormal_)},
        {.format = ctx.getFormat(gWorldPos_)},
    };

    pipelineDeferred_ = ctx.createRenderPipeline({
        .smVert = deferredVert,
        .smFrag = deferredFrag,
        .color = {attachments[0], attachments[1], attachments[2], attachments[3]},
        .depthFormat = lvk::Format_Z_F32,
        .debugName = "Pipeline: deferred",
    });
    pipelineCompose_ = ctx.createRenderPipeline({
        .smVert = composeVert,
        .smFrag = composeFrag,
        .color = {attachments[0], attachments[1], attachments[2], attachments[3]},
        .depthFormat = lvk::Format_Z_F32,
        .debugName = "Pipeline: compose",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float aspect = float(width_) / float(height_);
    const glm::mat4 proj = glm::perspectiveLH(glm::radians(45.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f));
    const glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));

    const PerFrame pf = {
        .mvp = proj * view * model,
        .model = model,
        .cameraPos = glm::inverse(view) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f),
        .texAlbedo = texAlbedo_.index(),
        .texNormal = texNormal_.index(),
        .texHeight = texHeight_.index(),
    };
    cmd.cmdUpdateBuffer(perFrameBuffer_, 0, sizeof(pf), &pf);

    const lvk::RenderPass pass = {
        .color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}},
                  {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}},
                  {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}},
                  {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearColor = {0.0f, 0.0f, 0.0f, 1.0f}}},
        .depth = {.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_DontCare, .clearDepth = 1.0f},
    };
    const lvk::Framebuffer fb = {
        .color = {{.texture = target}, {.texture = gAlbedo_}, {.texture = gNormal_}, {.texture = gWorldPos_}},
        .depthStencil = {.texture = depth_},
    };

    cmd.cmdBeginRendering(pass, fb, {.buffers = {perFrameBuffer_}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});

    cmd.cmdPushDebugGroupLabel("G-buffer");
    cmd.cmdBindRenderPipeline(pipelineDeferred_);
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true});
    cmd.cmdBindIndexBuffer(ib_, lvk::IndexFormat_UI16);
    const struct {
      uint64_t perFrame;
    } push = {.perFrame = addrPerFrame_};
    cmd.cmdPushConstants(push);
    cmd.cmdDrawIndexed(36);
    cmd.cmdPopDebugGroupLabel();

    cmd.cmdNextSubpass();

    cmd.cmdPushDebugGroupLabel("Compose (framebuffer fetch)");
    cmd.cmdBindRenderPipeline(pipelineCompose_);
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_AlwaysPass, .isDepthWriteEnabled = false});
    cmd.cmdDraw(3);
    cmd.cmdPopDebugGroupLabel();

    cmd.cmdEndRendering();
  }

 private:
  lvk::Holder<lvk::BufferHandle> ib_;
  lvk::Holder<lvk::TextureHandle> texAlbedo_;
  lvk::Holder<lvk::TextureHandle> texNormal_;
  lvk::Holder<lvk::TextureHandle> texHeight_;
  lvk::Holder<lvk::BufferHandle> perFrameBuffer_;
  uint64_t addrPerFrame_ = 0;
  lvk::Holder<lvk::TextureHandle> gAlbedo_;
  lvk::Holder<lvk::TextureHandle> gNormal_;
  lvk::Holder<lvk::TextureHandle> gWorldPos_;
  lvk::Holder<lvk::TextureHandle> depth_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineDeferred_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineCompose_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(LocalRead)
