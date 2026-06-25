#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

extern void lvkMetalAttachLayer(CA::MetalLayer* layer, GLFWwindow* window);

#ifndef LVK_METAL_SAMPLE_USE_SLANG
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
#endif

int main() {
  if (!glfwInit()) {
    fprintf(stderr, "glfwInit failed\n");
    return EXIT_FAILURE;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

  GLFWwindow* window = glfwCreateWindow(1024, 768, "[lvk-metal] HelloTriangle", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return EXIT_FAILURE;
  }

  int fbWidth = 0;
  int fbHeight = 0;
  glfwGetFramebufferSize(window, &fbWidth, &fbHeight);

  CA::MetalLayer* layer = CA::MetalLayer::layer();
  lvkMetalAttachLayer(layer, window);

  std::unique_ptr<lvk::metal::IMetalContext> ctx =
      lvk::metal::createContextWithMetalLayer(layer, uint32_t(fbWidth), uint32_t(fbHeight), {.vsync = true});
  if (!ctx) {
    fprintf(stderr, "createContextWithMetalLayer failed\n");
    glfwDestroyWindow(window);
    glfwTerminate();
    return EXIT_FAILURE;
  }

#ifdef LVK_METAL_SAMPLE_USE_SLANG
  SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
  lvk::Holder<lvk::ShaderModuleHandle> vert = slang.createShaderModule(*ctx, "triangle", "vertexMain", lvk::Stage_Vert, "triangle.vert");
  lvk::Holder<lvk::ShaderModuleHandle> frag = slang.createShaderModule(*ctx, "triangle", "fragmentMain", lvk::Stage_Frag, "triangle.frag");
#else
  lvk::Holder<lvk::ShaderModuleHandle> vert = ctx->createShaderModule({kShaderMSL, "vertexMain", lvk::Stage_Vert, "triangle.vert"});
  lvk::Holder<lvk::ShaderModuleHandle> frag = ctx->createShaderModule({kShaderMSL, "fragmentMain", lvk::Stage_Frag, "triangle.frag"});
#endif

  lvk::Holder<lvk::RenderPipelineHandle> pipeline = ctx->createRenderPipeline({
      .smVert = vert,
      .smFrag = frag,
      .color = {{.format = lvk::Format_BGRA_UN8}},
      .debugName = "triangle",
  });

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    lvk::ICommandBuffer& cmd = ctx->acquireCommandBuffer();
    const lvk::TextureHandle swapchain = ctx->getCurrentSwapchainTexture();

    const lvk::RenderPass renderPass = {
        .color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.15f, 0.18f, 0.22f, 1.0f}}}},
    };
    const lvk::Framebuffer framebuffer = {
        .color = {{.texture = swapchain}},
    };

    cmd.cmdBeginRendering(renderPass, framebuffer);
    cmd.cmdBindViewport({.width = float(fbWidth), .height = float(fbHeight)});
    cmd.cmdBindRenderPipeline(pipeline);
    cmd.cmdDraw(3);
    cmd.cmdEndRendering();

    ctx->submit(cmd, swapchain);
  }

  pipeline.reset();
  vert.reset();
  frag.reset();
  ctx.reset();

  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
