#include <cstdint>
#include <cstdlib>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct TriPush {
  uint face;
  float time;
};

struct TriOut {
  float4 position [[position]];
  float3 color;
};

vertex TriOut triVertexMain(uint vid [[vertex_id]], constant TriPush& pc [[buffer(0)]]) {
  const float2 p[3] = { float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.0, 0.6) };
  const float3 c[6] = {
    float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0),
    float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0), float3(0.0, 1.0, 1.0)
  };
  TriOut o;
  o.position = float4(p[vid] * (1.5 + sin(pc.time)) * 0.5, 0.0, 1.0);
  o.color = c[pc.face];
  return o;
}

fragment float4 triFragmentMain(TriOut in [[stage_in]]) {
  return float4(in.color, 1.0);
}

struct CubePush {
  float4x4 mvp;
  uint texture0;
};

struct MeshOut {
  float4 position [[position]];
  float3 dir;
};

vertex MeshOut meshVertexMain(uint vid [[vertex_id]], constant CubePush& pc [[buffer(0)]]) {
  const float3 v[8] = {
    float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
    float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0)
  };
  MeshOut o;
  float3 pos = v[vid];
  o.position = pc.mvp * float4(pos, 1.0);
  o.dir = pos;
  return o;
}

fragment float4 meshFragmentMain(MeshOut in [[stage_in]], constant CubePush& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindlessCube(pc.texture0, 0, normalize(in.dir));
}
)";

class RenderToCubeMap final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float displayScale) override {
    width_ = width;
    height_ = height;

    const uint16_t indexData[36] = {0, 1, 2, 2, 3, 0, 1, 5, 6, 6, 2, 1, 7, 6, 5, 5, 4, 7,
                                    4, 0, 3, 3, 7, 4, 4, 5, 1, 1, 0, 4, 3, 2, 6, 6, 7, 3};
    ib_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Index,
        .storage = lvk::StorageType_Device,
        .size = sizeof(indexData),
        .data = indexData,
        .debugName = "Buffer: index",
    });

    cubeMap_ = ctx.createTexture({
        .type = lvk::TextureType_Cube,
        .format = lvk::Format_BGRA_UN8,
        .dimensions = {512, 512},
        .usage = lvk::TextureUsageBits_Sampled | lvk::TextureUsageBits_Attachment,
        .debugName = "CubeMap",
    });

    linearSampler_ = ctx.createSampler({
        .minFilter = lvk::SamplerFilter_Linear,
        .magFilter = lvk::SamplerFilter_Linear,
        .debugName = "Sampler: linear",
    });

    lvk::Holder<lvk::ShaderModuleHandle> triVert;
    lvk::Holder<lvk::ShaderModuleHandle> triFrag;
    lvk::Holder<lvk::ShaderModuleHandle> meshVert;
    lvk::Holder<lvk::ShaderModuleHandle> meshFrag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      triVert = slang.createShaderModule(ctx, "triangle", "triVertexMain", lvk::Stage_Vert, "triangle.vert");
      triFrag = slang.createShaderModule(ctx, "triangle", "triFragmentMain", lvk::Stage_Frag, "triangle.frag");
      meshVert = slang.createShaderModule(ctx, "cubemesh", "meshVertexMain", lvk::Stage_Vert, "mesh.vert");
      meshFrag = slang.createShaderModule(ctx, "cubemesh", "meshFragmentMain", lvk::Stage_Frag, "mesh.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      triVert = ctx.createShaderModule({kShaderMSL, "triVertexMain", lvk::Stage_Vert, "triangle.vert"});
      triFrag = ctx.createShaderModule({kShaderMSL, "triFragmentMain", lvk::Stage_Frag, "triangle.frag"});
      meshVert = ctx.createShaderModule({kShaderMSL, "meshVertexMain", lvk::Stage_Vert, "mesh.vert"});
      meshFrag = ctx.createShaderModule({kShaderMSL, "meshFragmentMain", lvk::Stage_Frag, "mesh.frag"});
    }

    pipelineTriangle_ = ctx.createRenderPipeline({
        .smVert = triVert,
        .smFrag = triFrag,
        .color = {{.format = ctx.getFormat(cubeMap_)}},
        .debugName = "Pipeline: triangle",
    });

    pipelineMesh_ = ctx.createRenderPipeline({
        .smVert = meshVert,
        .smFrag = meshFrag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .cullMode = lvk::CullMode_Back,
        .frontFace = lvk::WindingMode_CW,
        .debugName = "Pipeline: mesh",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float aspect = float(width_) / float(height_);
    const glm::mat4 proj = glm::perspectiveLH(glm::radians(45.0f), aspect, 0.1f, 500.0f);
    const glm::mat4 view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f));
    const glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::normalize(glm::vec3(1.0f, 1.0f, 1.0f)));

    cmd.cmdPushDebugGroupLabel("Render to Cube Map");
    for (uint8_t face = 0; face != 6; ++face) {
      const lvk::ClearColorValue colors[6] = {{0.3f, 0.1f, 0.1f, 1.0f},
                                              {0.1f, 0.3f, 0.1f, 1.0f},
                                              {0.1f, 0.1f, 0.3f, 1.0f},
                                              {0.3f, 0.1f, 0.3f, 1.0f},
                                              {0.3f, 0.3f, 0.1f, 1.0f},
                                              {0.1f, 0.3f, 0.3f, 1.0f}};
      const lvk::RenderPass facePass = {
          .color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .layer = face, .clearColor = colors[face]}},
      };
      const lvk::Framebuffer faceFb = {.color = {{.texture = cubeMap_}}};

      cmd.cmdBeginRendering(facePass, faceFb);
      cmd.cmdBindRenderPipeline(pipelineTriangle_);
      const struct {
        uint32_t face;
        float time;
      } triPush = {.face = face, .time = 10.0f * time};
      cmd.cmdPushConstants(triPush);
      cmd.cmdDraw(3);
      cmd.cmdEndRendering();
    }
    cmd.cmdPopDebugGroupLabel();

    const lvk::RenderPass meshPass = {
        .color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {1.0f, 1.0f, 1.0f, 1.0f}}},
    };
    const lvk::Framebuffer meshFb = {.color = {{.texture = target}}};

    cmd.cmdBeginRendering(meshPass, meshFb, {.sampledImages = {cubeMap_}});
    cmd.cmdBindRenderPipeline(pipelineMesh_);
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdPushDebugGroupLabel("Render Mesh");
    cmd.cmdBindIndexBuffer(ib_, lvk::IndexFormat_UI16);
    const struct {
      glm::mat4 mvp;
      uint32_t texture;
    } meshPush = {.mvp = proj * view * model, .texture = cubeMap_.index()};
    cmd.cmdPushConstants(meshPush);
    cmd.cmdDrawIndexed(36);
    cmd.cmdPopDebugGroupLabel();
    cmd.cmdEndRendering();
  }

  void destroy() override {
    pipelineMesh_.reset();
    pipelineTriangle_.reset();
    linearSampler_.reset();
    cubeMap_.reset();
    ib_.reset();
  }

 private:
  lvk::Holder<lvk::BufferHandle> ib_;
  lvk::Holder<lvk::TextureHandle> cubeMap_;
  lvk::Holder<lvk::SamplerHandle> linearSampler_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineTriangle_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineMesh_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(RenderToCubeMap)
