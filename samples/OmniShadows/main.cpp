#include <cstdint>
#include <cstdlib>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <lmath/GeometryShapes.h>

#include <lvk/HelpersImGuiMetal.h>
#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct Vertex {
  packed_float3 pos;
  packed_float2 uv;
  packed_float3 normal;
};

struct ShadowVP {
  float4x4 viewProj[6];
};

struct ShadowPC {
  float4 lightPos;
  device const Vertex* vb;
  device const ShadowVP* vp;
  float shadowFar;
};

struct ShadowOut {
  float4 position [[position]];
  float3 worldPos;
};

vertex ShadowOut shadowVertexMain(uint vid [[vertex_id]], uint ampId [[amplification_id]], constant ShadowPC& pc [[buffer(0)]]) {
  const Vertex v = pc.vb[vid];
  ShadowOut o;
  o.worldPos = float3(v.pos);
  o.position = pc.vp->viewProj[ampId] * float4(o.worldPos, 1.0);
  return o;
}

fragment float shadowFragmentMain(ShadowOut in [[stage_in]], constant ShadowPC& pc [[buffer(0)]]) {
  return length(in.worldPos - pc.lightPos.xyz) / pc.shadowFar;
}

struct MainPC {
  float4x4 viewProj;
  float4 lightPos;
  device const Vertex* vb;
  float shadowFar;
  uint shadowMap;
  uint sampler;
};

struct MainOut {
  float4 position [[position]];
  float3 worldPos;
  float3 color;
  float3 normal;
};

vertex MainOut meshVertexMain(uint vid [[vertex_id]], constant MainPC& pc [[buffer(0)]]) {
  const Vertex v = pc.vb[vid];
  MainOut o;
  o.worldPos = float3(v.pos);
  o.position = pc.viewProj * float4(o.worldPos, 1.0);
  o.color = o.worldPos * 0.03 + 0.6;
  o.normal = normalize(float3(v.normal));
  return o;
}

fragment float4 meshFragmentMain(MainOut in [[stage_in]], constant MainPC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const float3 fragToLight = in.worldPos - pc.lightPos.xyz;
  const float NdotL = max(dot(normalize(in.normal), normalize(-fragToLight)), 0.0);

  const float dist = length(fragToLight);
  const float k = dist * 0.0015;
  const float3 base = float3(fragToLight.x, -fragToLight.y, fragToLight.z);
  const float3 offsets[9] = {
      float3( 0,  0,  0),
      float3( 1,  1,  1), float3( 1,  1, -1), float3( 1, -1,  1), float3( 1, -1, -1),
      float3(-1,  1,  1), float3(-1,  1, -1), float3(-1, -1,  1), float3(-1, -1, -1),
  };
  const float curD = dist - 0.1;
  float factor = 0.0;
  for (int i = 0; i < 9; ++i) {
    const float d = textureBindlessCube(pc.shadowMap, pc.sampler, base + k * offsets[i]).r;
    factor += curD > pc.shadowFar * d ? 0.0 : 1.0;
  }
  factor /= 9.0;

  const float att = max(1.0 - (dist / 50.0) * (dist / 50.0), 0.0);
  const float3 finalColor = in.color * NdotL * factor * att;
  return float4(max(finalColor, in.color * 0.3), 1.0);
}
)";

using glm::mat4;
using glm::vec3;
using glm::vec4;

class OmniShadows final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    sampler_ = ctx.createSampler({
        .minFilter = lvk::SamplerFilter_Linear,
        .magFilter = lvk::SamplerFilter_Linear,
        .wrapU = lvk::SamplerWrap_Clamp,
        .wrapV = lvk::SamplerWrap_Clamp,
        .wrapW = lvk::SamplerWrap_Clamp,
        .debugName = "Sampler: linear",
    });

    std::vector<GeometryShapes::Vertex> verts;
    const int sizeX = 4;
    const int sizeY = 4;
    const float sx = 0.5f, sy = 0.5f, h = 3.0f, spacing = 4.0f;
    for (int x = 0; x != sizeX; ++x)
      for (int y = 0; y != sizeY; ++y)
        GeometryShapes::addAxisAlignedBox(
            verts, vec3(spacing * (x - (sizeX - 1) / 2.0f), spacing * (y - (sizeY - 1) / 2.0f), 0.0f), vec3(sx, sy, h));
    GeometryShapes::addAxisAlignedBox(verts, vec3(-sizeX * spacing, 0, 0), vec3(sx / 2, 2 * sizeY * spacing * sx, 2 * h));
    GeometryShapes::addAxisAlignedBox(verts, vec3(+sizeX * spacing, 0, 0), vec3(sx / 2, 2 * sizeY * spacing * sx, 2 * h));
    GeometryShapes::addAxisAlignedBox(verts, vec3(0, -sizeY * spacing, 0), vec3(2 * sizeX * spacing * sx, sy / 2, h));
    GeometryShapes::addAxisAlignedBox(verts, vec3(0, +sizeY * spacing, 0), vec3(2 * sizeX * spacing * sx, sy / 2, h));
    GeometryShapes::addAxisAlignedBox(verts, vec3(0, 0, -h), vec3(2 * sizeX * spacing * sx, 2 * sizeY * spacing * sx, (sx + sy) / 4));
    vertexCount_ = uint32_t(verts.size());

    vb_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(GeometryShapes::Vertex) * verts.size(),
        .data = verts.data(),
        .debugName = "Buffer: vertices",
    });
    bufShadowVP_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(mat4) * 6,
        .debugName = "Buffer: shadow view-projections",
    });

    shadowColor_ = ctx.createTexture({
        .type = lvk::TextureType_Cube,
        .format = lvk::Format_R_UN16,
        .dimensions = {kShadowSize, kShadowSize},
        .usage = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Attachment,
        .debugName = "Texture: shadow cube",
    });
    depth_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_Z_F32,
        .dimensions = {width, height},
        .usage = lvk::TextureUsageBits_Attachment,
        .debugName = "Texture: depth",
    });

    for (uint32_t l = 0; l < 6; ++l)
      shadowViews_[l] = ctx.createTextureView(
          shadowColor_,
          {.layer = l, .components = {.r = lvk::Swizzle_R, .g = lvk::Swizzle_R, .b = lvk::Swizzle_R, .a = lvk::Swizzle_1}},
          "View: shadow face");

    lvk::Holder<lvk::ShaderModuleHandle> shadowVert, shadowFrag, meshVert, meshFrag;
#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      shadowVert = slang.createShaderModule(ctx, "shadow", "vertexMain", lvk::Stage_Vert, "shadow.vert");
      shadowFrag = slang.createShaderModule(ctx, "shadow", "fragmentMain", lvk::Stage_Frag, "shadow.frag");
      meshVert = slang.createShaderModule(ctx, "mesh", "vertexMain", lvk::Stage_Vert, "mesh.vert");
      meshFrag = slang.createShaderModule(ctx, "mesh", "fragmentMain", lvk::Stage_Frag, "mesh.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      shadowVert = ctx.createShaderModule({kShaderMSL, "shadowVertexMain", lvk::Stage_Vert, "shadow.vert"});
      shadowFrag = ctx.createShaderModule({kShaderMSL, "shadowFragmentMain", lvk::Stage_Frag, "shadow.frag"});
      meshVert = ctx.createShaderModule({kShaderMSL, "meshVertexMain", lvk::Stage_Vert, "mesh.vert"});
      meshFrag = ctx.createShaderModule({kShaderMSL, "meshFragmentMain", lvk::Stage_Frag, "mesh.frag"});
    }

    ctx.setShaderModuleMetadata(shadowVert, {.viewCount = 6});

    pipelineShadow_ = ctx.createRenderPipeline({
        .smVert = shadowVert,
        .smFrag = shadowFrag,
        .color = {{
            .format = lvk::Format_R_UN16,
            .blendEnabled = true,
            .rgbBlendOp = lvk::BlendOp_Min,
            .alphaBlendOp = lvk::BlendOp_Min,
            .srcRGBBlendFactor = lvk::BlendFactor_One,
            .dstRGBBlendFactor = lvk::BlendFactor_One,
        }},
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: shadow",
    });
    pipelineMesh_ = ctx.createRenderPipeline({
        .smVert = meshVert,
        .smFrag = meshFrag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .depthFormat = lvk::Format_Z_F32,
        .cullMode = lvk::CullMode_None,
        .debugName = "Pipeline: mesh",
    });

    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window);
    ctx_ = &ctx;
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float shadowNear = 0.1f, shadowFar = 100.0f;
    const vec3 lightPos = vec3(4.5f * cosf(time), 4.5f * sinf(time), 5.0f);

    const mat4 proj = glm::perspectiveLH_ZO(glm::radians(45.0f), float(width_) / float(height_), 0.1f, 100.0f);
    const mat4 view = glm::lookAtLH(vec3(-12, 10, 10), vec3(0, 0, 0), vec3(0, 0, 1));
    const mat4 viewProj = proj * view;

    const mat4 shadowProj = glm::perspectiveLH_ZO(glm::radians(90.0f), 1.0f, shadowNear, shadowFar);
    const vec3 fwd[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const vec3 up[6] = {{0, 1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}, {0, 1, 0}, {0, 1, 0}};
    mat4 shadowVP[6];
    for (int i = 0; i < 6; ++i)
      shadowVP[i] = shadowProj * glm::lookAtLH(lightPos, lightPos + fwd[i], up[i]);

    cmd.cmdUpdateBuffer(bufShadowVP_, 0, sizeof(shadowVP), shadowVP);

    // 1. Shadow cube (single multiview pass, all 6 faces via vertex amplification)
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {1, 1, 1, 1}}},
                           .layerCount = 6,
                           .viewMask = 0b111111},
                          {.color = {{.texture = shadowColor_}}},
                          {.buffers = {bufShadowVP_}});
    cmd.cmdBindRenderPipeline(pipelineShadow_);
    cmd.cmdBindViewport({.width = float(kShadowSize), .height = float(kShadowSize)});
    cmd.cmdBindScissorRect({.width = kShadowSize, .height = kShadowSize});
    {
      const ShadowPC pc = {
          .lightPos = vec4(lightPos, 1.0f), .vb = ctx_->gpuAddress(vb_), .vp = ctx_->gpuAddress(bufShadowVP_), .shadowFar = shadowFar};
      cmd.cmdPushConstants(pc);
    }
    cmd.cmdDraw(vertexCount_);
    cmd.cmdEndRendering();

    // 2. Lit scene, sampling the shadow cube
    const lvk::Framebuffer fb = {.color = {{.texture = target}}, .depthStencil = {depth_}};
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {1, 1, 1, 1}}},
                           .depth = {.loadOp = lvk::LoadOp_Clear, .clearDepth = 1.0f}},
                          fb,
                          {.sampledImages = {shadowColor_}});
    cmd.cmdBindRenderPipeline(pipelineMesh_);
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindScissorRect({.width = width_, .height = height_});
    cmd.cmdBindDepthState({.compareOp = lvk::CompareOp_Less, .isDepthWriteEnabled = true});
    {
      const MainPC pc = {.viewProj = viewProj,
                         .lightPos = vec4(lightPos, 1.0f),
                         .vb = ctx_->gpuAddress(vb_),
                         .shadowFar = shadowFar,
                         .shadowMap = shadowColor_.index(),
                         .sampler = sampler_.index()};
      cmd.cmdPushConstants(pc);
    }
    cmd.cmdDraw(vertexCount_);

    // 3. ImGui overlay (color-only pass) showing the 6 cube faces via texture views
    imgui_->beginFrame(fb);
    ImGui::SetNextWindowPos({0, 0});
    ImGui::Begin("Shadow cube faces", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNavInputs);
    for (uint32_t l = 0; l < 6; ++l) {
      ImGui::Image(ImTextureID(shadowViews_[l].index()), ImVec2(96, 96));
      if (l % 3 != 2)
        ImGui::SameLine();
    }
    ImGui::End();
    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();

    ++frame_;
  }

  // ImGui windows use AlwaysAutoResize, which needs one frame to measure content before it draws.
  // Warm up so the single-frame headless screenshot captures a settled UI.
  bool isReadyForScreenshot() const override {
    return frame_ >= 2;
  }

  void destroy() override {
    imgui_.reset();
  }

 private:
  struct ShadowPC {
    vec4 lightPos;
    uint64_t vb;
    uint64_t vp;
    float shadowFar;
  };
  struct MainPC {
    mat4 viewProj;
    vec4 lightPos;
    uint64_t vb;
    float shadowFar;
    uint32_t shadowMap;
    uint32_t sampler;
  };

  static constexpr uint32_t kShadowSize = 1024;

  lvk::metal::IMetalContext* ctx_ = nullptr;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineShadow_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineMesh_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::BufferHandle> vb_;
  lvk::Holder<lvk::BufferHandle> bufShadowVP_;
  lvk::Holder<lvk::TextureHandle> shadowColor_;
  lvk::Holder<lvk::TextureHandle> depth_;
  lvk::Holder<lvk::TextureHandle> shadowViews_[6];
  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;
  uint32_t vertexCount_ = 0;
  uint32_t frame_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(OmniShadows)
