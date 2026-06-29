#include <cstdint>
#include <cstdlib>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

static const char* kShaderMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float3 color;
};

struct Payload {
  uint unused;
};

[[object, max_total_threads_per_threadgroup(1), clang::annotate("lvk_numthreads(1,1,1)")]]
void objectMain(object_data Payload& payload [[payload]], mesh_grid_properties mgp) {
  mgp.set_threadgroups_per_grid(uint3(1, 1, 1));
}

using TriangleMesh = metal::mesh<VertexOut, void, 3, 1, metal::topology::triangle>;

[[mesh, max_total_threads_per_threadgroup(1), clang::annotate("lvk_numthreads(1,1,1)")]]
void meshMain(TriangleMesh outMesh, const object_data Payload& payload [[payload]]) {
  const float2 pos[3] = {float2(-0.6, -0.4), float2(0.6, -0.4), float2(0.0, 0.6)};
  const float3 col[3] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0)};
  outMesh.set_primitive_count(1);
  for (uint i = 0; i < 3; ++i) {
    VertexOut v;
    v.position = float4(pos[i], 0.0, 1.0);
    v.color = col[i];
    outMesh.set_vertex(i, v);
  }
  outMesh.set_index(0, 0);
  outMesh.set_index(1, 1);
  outMesh.set_index(2, 2);
}

fragment float4 fragmentMain(VertexOut in [[stage_in]]) {
  return float4(in.color, 1.0);
}
)MSL";

class MeshShaders final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    width_ = width;
    height_ = height;

    lvk::Holder<lvk::ShaderModuleHandle> task;
    lvk::Holder<lvk::ShaderModuleHandle> mesh;
    lvk::Holder<lvk::ShaderModuleHandle> frag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      task = slang.createShaderModule(ctx, "triangle", "taskMain", lvk::Stage_Task, "triangle.task");
      mesh = slang.createShaderModule(ctx, "triangle", "meshMain", lvk::Stage_Mesh, "triangle.mesh");
      frag = slang.createShaderModule(ctx, "triangle", "fragmentMain", lvk::Stage_Frag, "triangle.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      task = ctx.createShaderModule({kShaderMSL, "objectMain", lvk::Stage_Task, "object"});
      mesh = ctx.createShaderModule({kShaderMSL, "meshMain", lvk::Stage_Mesh, "mesh"});
      frag = ctx.createShaderModule({kShaderMSL, "fragmentMain", lvk::Stage_Frag, "frag"});
    }

    pipeline_ = ctx.createRenderPipeline({
        .smTask = task,
        .smMesh = mesh,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "mesh triangle",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {1.0f, 1.0f, 1.0f, 1.0f}}}}},
        {.color = {{.texture = target}}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdBindRenderPipeline(pipeline_);
    cmd.cmdPushDebugGroupLabel("Render Triangle (mesh shaders)", 0xff0000ff);
    cmd.cmdDrawMeshTasks({1, 1, 1});
    cmd.cmdPopDebugGroupLabel();
    cmd.cmdEndRendering();
  }

 private:
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(MeshShaders)
