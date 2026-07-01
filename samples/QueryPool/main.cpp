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

struct QuadOut {
  float4 position [[position]];
  float2 uv;
};

vertex QuadOut qpVertexMain(uint vid [[vertex_id]]) {
  const float2 pos[4] = { float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0) };
  QuadOut o;
  o.position = float4(pos[vid], 0.0, 1.0);
  o.uv = pos[vid] * 0.5 + 0.5;
  return o;
}

fragment float4 qpFragmentMain(QuadOut in [[stage_in]]) {
  return float4(in.uv, 0.5, 1.0);
}
)";

class QueryPool final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    queryPool_ = ctx.createQueryPool(2, "queryPoolTimestamps");
    toMs_ = ctx.getTimestampPeriodToMs();

    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      vert = slang.createShaderModule(ctx, "querypool", "vertexMain", lvk::Stage_Vert, "qp.vert");
      frag = slang.createShaderModule(ctx, "querypool", "fragmentMain", lvk::Stage_Frag, "qp.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      vert = ctx.createShaderModule({kShaderMSL, "qpVertexMain", lvk::Stage_Vert, "qp.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "qpFragmentMain", lvk::Stage_Frag, "qp.frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .topology = lvk::Topology_TriangleStrip,
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "Pipeline: query pool",
    });

    ctx_ = &ctx;
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    if (frame_ > 0) {
      uint64_t ts[2] = {0, 0};
      if (ctx_->getQueryPoolResults(queryPool_, 0, 2, sizeof(ts), ts, sizeof(ts[0])) && ts[1] >= ts[0])
        LLOGL("QueryPool: GPU pass time = %.4f ms (ticks %llu -> %llu)\n", double(ts[1] - ts[0]) * toMs_, ts[0], ts[1]);
    }

    cmd.cmdResetQueryPool(queryPool_, 0, 2);
    cmd.cmdWriteTimestamp(queryPool_, 0);
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(pipeline_);
    cmd.cmdDraw(4);
    cmd.cmdEndRendering();
    cmd.cmdWriteTimestamp(queryPool_, 1);
    ++frame_;
  }

 private:
  lvk::metal::IMetalContext* ctx_ = nullptr;
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Holder<lvk::QueryPoolHandle> queryPool_;
  double toMs_ = 0.0;
  uint32_t frame_ = 0;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(QueryPool)
