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

constant int kShape [[function_constant(0)]];

struct VertexOut {
  float4 position [[position]];
  float4 color;
};

vertex VertexOut vertexMain(uint vid [[vertex_id]]) {
  float2 pos;
  float3 col;
  if (kShape == 1) {
    const float2 p[6] = { float2(-0.6, -0.6), float2( 0.6, -0.6), float2( 0.6, 0.6),
                          float2(-0.6, -0.6), float2( 0.6,  0.6), float2(-0.6, 0.6) };
    const float3 c[6] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0),
                          float3(1.0, 0.0, 0.0), float3(0.0, 0.0, 1.0), float3(1.0, 1.0, 0.0) };
    pos = p[vid];
    col = c[vid];
  } else {
    const float2 p[3] = { float2(0.0, 0.6), float2(-0.6, -0.6), float2(0.6, -0.6) };
    const float3 c[3] = { float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0) };
    pos = p[vid];
    col = c[vid];
  }
  VertexOut out;
  out.position = float4(pos, 0.0, 1.0);
  out.color = float4(col, 1.0);
  return out;
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
  return in.color;
}
)";

class HelloTriangle final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;
    window_ = window;

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

    int32_t shape = 0;
    const auto makePipeline = [&](int32_t value) {
      shape = value;
      lvk::SpecializationConstantDesc spec = {};
      spec.entries[0] = {.constantId = 0, .offset = 0, .size = sizeof(int32_t)};
      spec.data = &shape;
      spec.dataSize = sizeof(shape);
      return ctx.createRenderPipeline({
          .smVert = vert,
          .smFrag = frag,
          .specInfo = spec,
          .color = {{.format = ctx.getSwapchainFormat()}},
          .debugName = value == 1 ? "quad" : "triangle",
      });
    };
    pipelineTriangle_ = makePipeline(0);
    pipelineQuad_ = makePipeline(1);

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
      LLOGL("HelloTriangle: press SPACE to toggle triangle/quad (specialization constant)");
    }
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.15f, 0.18f, 0.22f, 1.0f}}}}},
        {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(drawQuad_ ? pipelineQuad_ : pipelineTriangle_);
    cmd.cmdDraw(drawQuad_ ? 6 : 3);
    cmd.cmdEndRendering();
  }

 private:
  static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    HelloTriangle* self = static_cast<HelloTriangle*>(glfwGetWindowUserPointer(window));
    if (self && key == GLFW_KEY_SPACE && action == GLFW_PRESS) {
      self->drawQuad_ = !self->drawQuad_;
    }
  }

  lvk::Holder<lvk::RenderPipelineHandle> pipelineTriangle_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineQuad_;
  GLFWwindow* window_ = nullptr;
  bool drawQuad_ = false;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(HelloTriangle)
