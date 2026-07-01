#include <array>
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

struct QuadOut {
  float4 position [[position]];
  float2 uv;
};

vertex QuadOut viewVertexMain(uint vid [[vertex_id]]) {
  const float2 pos[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
  QuadOut o;
  o.position = float4(pos[vid], 0.0, 1.0);
  o.uv = pos[vid] * 0.5 + 0.5;
  return o;
}

struct ViewPush {
  uint texId;
  uint smpId;
};

fragment float4 viewFragmentMain(QuadOut in [[stage_in]], constant ViewPush& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindless2D(pc.texId, pc.smpId, in.uv);
}
)";

class TextureView final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    sampler_ = ctx.createSampler({.debugName = "Sampler: nearest"});

    const uint32_t dim = 64;
    const uint32_t colors[kLayers] = {0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u, 0xFFFFFFFFu}; // RGBA8: R, G, B, white
    array_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_RGBA_UN8,
        .dimensions = {dim, dim},
        .numLayers = kLayers,
        .usage = lvk::TextureUsageBits_Sampled,
        .debugName = "Texture: 2D array",
    });
    for (uint32_t l = 0; l < kLayers; ++l) {
      std::vector<uint32_t> pixels(size_t(dim) * dim, colors[l]);
      ctx.upload(array_, {.dimensions = {dim, dim, 1}, .layer = l}, pixels.data());
    }

    // One 2D view per array layer.
    for (uint32_t l = 0; l < kLayers; ++l)
      views_[l] = ctx.createTextureView(array_, {.type = lvk::TextureType_2D, .layer = l, .numLayers = 1}, "View: layer");

    // A swizzled view of layer 0 (red): swap R and B so it displays as blue.
    views_[kLayers] =
        ctx.createTextureView(array_,
                              {.type = lvk::TextureType_2D,
                               .layer = 0,
                               .numLayers = 1,
                               .components = {.r = lvk::Swizzle_B, .g = lvk::Swizzle_G, .b = lvk::Swizzle_R, .a = lvk::Swizzle_1}},
                              "View: swizzled");

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "textureview", "vertexMain", lvk::Stage_Vert, "view.vert");
      frag = slang.createShaderModule(ctx, "textureview", "fragmentMain", lvk::Stage_Frag, "view.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "viewVertexMain", lvk::Stage_Vert, "view.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "viewFragmentMain", lvk::Stage_Frag, "view.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_TriangleStrip,
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "Pipeline: texture view",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0.1f, 0.1f, 0.1f, 1}}}},
                          {.color = {{.texture = target}}});
    cmd.cmdBindRenderPipeline(pipeline_);
    const uint32_t n = kLayers + 1;
    const float colW = float(width_) / float(n);
    for (uint32_t i = 0; i < n; ++i) {
      cmd.cmdBindViewport({.x = i * colW, .y = 0.0f, .width = colW, .height = float(height_)});
      const struct {
        uint32_t texId;
        uint32_t smpId;
      } pc = {.texId = views_[i].index(), .smpId = sampler_.index()};
      cmd.cmdPushConstants(pc);
      cmd.cmdDraw(4);
    }
    cmd.cmdEndRendering();
  }

 private:
  static constexpr uint32_t kLayers = 4;

  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::TextureHandle> array_;
  std::array<lvk::Holder<lvk::TextureHandle>, kLayers + 1> views_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(TextureView)
