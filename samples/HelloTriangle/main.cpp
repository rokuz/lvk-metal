#include <cstdint>
#include <cstdlib>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float4 color;
};

vertex VertexOut vertexMain(uint vid [[vertex_id]]) {
  const float2 positions[3] = { float2(0.0, 0.6), float2(-0.6, -0.6), float2(0.6, -0.6) };
  const float3 colors[3] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };
  VertexOut out;
  out.position = float4(positions[vid], 0.0, 1.0);
  out.color = float4(colors[vid], 1.0);
  return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
  return in.color;
}
)";

class HelloTriangle final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float displayScale, uint32_t) override {
    width_ = width;
    height_ = height;

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "triangle", "vertexMain", lvk::Stage_Vert, "triangle.vert");
      frag = slang.createShaderModule(ctx, "triangle", "fragmentMain", lvk::Stage_Frag, "triangle.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "vertexMain", lvk::Stage_Vert, "triangle.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "fragmentMain", lvk::Stage_Frag, "triangle.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "triangle",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.15f, 0.18f, 0.22f, 1.0f}}}}},
        {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(pipeline_);
    cmd.cmdDraw(3);
    cmd.cmdEndRendering();
  }

  void destroy() override {
    pipeline_.reset();
  }

 private:
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(HelloTriangle)
