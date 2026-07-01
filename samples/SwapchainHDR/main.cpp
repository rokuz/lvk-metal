#include <cstdint>
#include <cstdlib>

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

struct PushConstants {
  float4 col0;
  float4 col1;
  float4 col2;
};

struct VSOut {
  float4 position [[position]];
  float3 color;
};

vertex VSOut hdrVertexMain(uint vid [[vertex_id]], constant PushConstants& pc [[buffer(0)]]) {
  const float2 pos[3] = { float2(-0.6, -0.4), float2(0.6, -0.4), float2(0.0, 0.6) };
  const float3 col[3] = { pc.col0.rgb, pc.col1.rgb, pc.col2.rgb };
  VSOut o;
  o.position = float4(pos[vid], 0.0, 1.0);
  o.color = col[vid];
  return o;
}

fragment float4 hdrFragmentMain(VSOut in [[stage_in]]) {
  return float4(in.color, 1.0);
}
)";

class SwapchainHDR final : public lvk::metal::ISample {
 public:
  void configureContext(lvk::metal::ContextConfig& cfg) const override {
    cfg.swapchainRequestedColorSpace = lvk::ColorSpace_HDR10;
  }

  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    LLOGL("Swapchain format     : %u", ctx.getSwapchainFormat());
    LLOGL("Swapchain color space: %u", ctx.getSwapchainColorSpace());

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "hdr", "vertexMain", lvk::Stage_Vert, "hdr.vert");
      frag = slang.createShaderModule(ctx, "hdr", "fragmentMain", lvk::Stage_Frag, "hdr.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "hdrVertexMain", lvk::Stage_Vert, "hdr.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "hdrFragmentMain", lvk::Stage_Frag, "hdr.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "Pipeline: HDR triangle",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {1, 1, 1, 1}}}},
                          {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(pipeline_);
    const struct {
      float col0[4];
      float col1[4];
      float col2[4];
    } pc = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}};
    cmd.cmdPushConstants(pc);
    cmd.cmdDraw(3);
    cmd.cmdEndRendering();
  }

 private:
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(SwapchainHDR)
