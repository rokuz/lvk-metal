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

static const char* kShaderMSL = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct DrawArgs {
  uint vertexCount;
  uint instanceCount;
  uint vertexStart;
  uint baseInstance;
};

struct CullPC {
  device DrawArgs* cmds;
  device uint* primTypes;
  uint count;
};

[[clang::annotate("lvk_numthreads(64,1,1)")]]
kernel void fillCommands(uint i [[thread_position_in_grid]], constant CullPC& pc [[buffer(0)]]) {
  if (i >= pc.count) {
    return;
  }
  DrawArgs a;
  a.vertexCount = 3;
  a.instanceCount = 1;
  a.vertexStart = 0;
  a.baseInstance = i;
  pc.cmds[i] = a;
  pc.primTypes[i] = (i & 1u) ? 2u : 3u;
}

struct RowPC {
  uint count;
  float rowY;
};

struct V2F {
  float4 position [[position]];
  float4 color;
};

static float3 lvkHue(float h) {
  return clamp(abs(fract(h + float3(0.0, 0.6667, 0.3333)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

static V2F lvkPlaceTriangle(uint vid, uint index, uint count, float rowY) {
  const float2 tri[3] = { float2(0.0, 0.16), float2(-0.11, -0.16), float2(0.11, -0.16) };
  const float x = (float(index) + 0.5) / float(count) * 2.0 - 1.0;
  V2F o;
  o.position = float4(float2(x, rowY) + tri[vid], 0.0, 1.0);
  o.color = float4(lvkHue(float(index) / float(count)), 1.0);
  return o;
}

vertex V2F drawVertexMain(uint vid [[vertex_id]], uint drawID [[instance_id]], constant RowPC& pc [[buffer(0)]]) {
  return lvkPlaceTriangle(vid, drawID, pc.count, pc.rowY);
}

fragment float4 drawFragmentMain(V2F in [[stage_in]]) {
  return in.color;
}

fragment float4 drawFragmentGray(V2F in [[stage_in]]) {
  const float g = dot(in.color.rgb, float3(0.299, 0.587, 0.114));
  return float4(g, g, g, 1.0);
}

fragment float4 meshFragmentMain(V2F in [[stage_in]]) {
  return in.color;
}

struct Payload {
  uint cell;
};

[[object, max_total_threads_per_threadgroup(1), clang::annotate("lvk_numthreads(1,1,1)")]]
void meshObjectMain(object_data Payload& payload [[payload]], uint3 tgPos [[threadgroup_position_in_grid]], mesh_grid_properties mgp) {
  payload.cell = tgPos.x;
  mgp.set_threadgroups_per_grid(uint3(1, 1, 1));
}

using TriMesh = metal::mesh<V2F, void, 3, 1, metal::topology::triangle>;

[[mesh, max_total_threads_per_threadgroup(1), clang::annotate("lvk_numthreads(1,1,1)")]]
void meshMeshMain(TriMesh out, const object_data Payload& payload [[payload]], constant RowPC& pc [[buffer(0)]]) {
  out.set_primitive_count(1);
  for (uint v = 0; v < 3; ++v) {
    out.set_vertex(v, lvkPlaceTriangle(v, payload.cell, pc.count, pc.rowY));
  }
  out.set_index(0, 0);
  out.set_index(1, 1);
  out.set_index(2, 2);
}
)MSL";

class IndirectDraws final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;

    lvk::Holder<lvk::ShaderModuleHandle> fill;
    lvk::Holder<lvk::ShaderModuleHandle> vert;
    lvk::Holder<lvk::ShaderModuleHandle> frag;
    lvk::Holder<lvk::ShaderModuleHandle> fragGray;
    lvk::Holder<lvk::ShaderModuleHandle> meshObj;
    lvk::Holder<lvk::ShaderModuleHandle> meshMesh;
    lvk::Holder<lvk::ShaderModuleHandle> meshFrag;

#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      fill = slang.createShaderModule(ctx, "indirect_cull", "fillCommands", lvk::Stage_Comp, "indirect_cull.comp");
      vert = slang.createShaderModule(ctx, "indirect", "drawVertexMain", lvk::Stage_Vert, "indirect.vert");
      frag = slang.createShaderModule(ctx, "indirect", "drawFragmentMain", lvk::Stage_Frag, "indirect.frag");
      fragGray = slang.createShaderModule(ctx, "indirect", "drawFragmentGray", lvk::Stage_Frag, "indirect.fraggray");
      meshObj = slang.createShaderModule(ctx, "indirect_mesh", "meshObjectMain", lvk::Stage_Task, "indirect_mesh.task");
      meshMesh = slang.createShaderModule(ctx, "indirect_mesh", "meshMeshMain", lvk::Stage_Mesh, "indirect_mesh.mesh");
      meshFrag = slang.createShaderModule(ctx, "indirect_mesh", "meshFragmentMain", lvk::Stage_Frag, "indirect_mesh.frag");
    } else
#endif
    {
      LLOGL("[lvk-metal] shaders: native MSL");
      fill = ctx.createShaderModule({kShaderMSL, "fillCommands", lvk::Stage_Comp, "indirect.comp"});
      vert = ctx.createShaderModule({kShaderMSL, "drawVertexMain", lvk::Stage_Vert, "indirect.vert"});
      frag = ctx.createShaderModule({kShaderMSL, "drawFragmentMain", lvk::Stage_Frag, "indirect.frag"});
      fragGray = ctx.createShaderModule({kShaderMSL, "drawFragmentGray", lvk::Stage_Frag, "indirect.fraggray"});
      meshObj = ctx.createShaderModule({kShaderMSL, "meshObjectMain", lvk::Stage_Task, "indirect.task"});
      meshMesh = ctx.createShaderModule({kShaderMSL, "meshMeshMain", lvk::Stage_Mesh, "indirect.mesh"});
      meshFrag = ctx.createShaderModule({kShaderMSL, "meshFragmentMain", lvk::Stage_Frag, "indirect.meshfrag"});
    }

    fillPipeline_ = ctx.createComputePipeline({.smComp = fill});
    pipeline_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = frag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "indirect draw",
    });
    pipelineGray_ = ctx.createRenderPipeline({
        .smVert = vert,
        .smFrag = fragGray,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "indirect draw (grayscale)",
    });
    meshPipeline_ = ctx.createRenderPipeline({
        .smTask = meshObj,
        .smMesh = meshMesh,
        .smFrag = meshFrag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "indirect mesh",
    });

    argsDraw_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_Indirect,
                                  .storage = lvk::StorageType_Device,
                                  .size = kCount * 16,
                                  .debugName = "Buffer: draw args (compute-filled)"});
    primTypes_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                   .storage = lvk::StorageType_Device,
                                   .size = kCount * sizeof(uint32_t),
                                   .debugName = "Buffer: per-draw primitive types"});
    ctx.setIndirectBufferMetadata(argsDraw_, {.primitiveTypes = primTypes_});

    std::vector<uint32_t> indexedArgs(kCount * 5);
    for (uint32_t i = 0; i < kCount; ++i) {
      uint32_t* a = &indexedArgs[i * 5];
      a[0] = 3;
      a[1] = 1;
      a[2] = 0;
      a[3] = 0;
      a[4] = i;
    }
    argsIndexed_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_Indirect,
                                     .storage = lvk::StorageType_Device,
                                     .size = indexedArgs.size() * sizeof(uint32_t),
                                     .data = indexedArgs.data(),
                                     .debugName = "Buffer: indexed draw args"});
    const uint32_t indices[3] = {0, 1, 2};
    indexBuffer_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Index,
                                     .storage = lvk::StorageType_Device,
                                     .size = sizeof(indices),
                                     .data = indices,
                                     .debugName = "Buffer: indices"});
    ctx.setIndirectBufferMetadata(argsIndexed_, {.indexBuffer = indexBuffer_, .indexFormat = lvk::IndexFormat_UI32});

    const uint32_t meshArgs[6] = {kCount, 1, 1, kCount, 1, 1};
    argsMesh_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_Indirect,
                                  .storage = lvk::StorageType_Device,
                                  .size = sizeof(meshArgs),
                                  .data = meshArgs,
                                  .debugName = "Buffer: mesh task args"});
    const uint32_t tg[6] = {1, 1, 1, 1, 1, 1};
    meshThreadgroups_ = ctx.createBuffer({.usage = lvk::BufferUsageBits_Storage,
                                          .storage = lvk::StorageType_Device,
                                          .size = sizeof(tg),
                                          .data = tg,
                                          .debugName = "Buffer: mesh threadgroup sizes"});
    ctx.setIndirectBufferMetadata(argsMesh_, {.meshThreadgroupSizes = meshThreadgroups_});

    lvk::ICommandBuffer& fillCmd = ctx.acquireCommandBuffer();
    fillCmd.cmdBindComputePipeline(fillPipeline_);
    const struct {
      uint64_t cmds;
      uint64_t primTypes;
      uint32_t count;
    } cullPC = {.cmds = ctx.gpuAddress(argsDraw_), .primTypes = ctx.gpuAddress(primTypes_), .count = kCount};
    fillCmd.cmdPushConstants(cullPC);
    fillCmd.cmdDispatch({(kCount + 63) / 64, 1, 1}, {.buffers = {argsDraw_, primTypes_}});
    ctx.wait(ctx.submit(fillCmd, {}));
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    cmd.cmdBeginRendering(
        {.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {.float32 = {0.05f, 0.06f, 0.09f, 1.0f}}}}},
        {.color = {{.texture = target}}},
        {.buffers = {argsDraw_, primTypes_, argsIndexed_, argsMesh_}});
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});

    const struct {
      uint32_t count;
      float rowY;
    } topRow = {.count = kCount, .rowY = 0.6f};
    cmd.cmdBindRenderPipeline(pipeline_);
    cmd.cmdPushConstants(topRow);
    cmd.cmdDrawIndirect(argsDraw_, 0, kCount / 2, 16);
    cmd.cmdBindRenderPipeline(pipelineGray_);
    cmd.cmdPushConstants(topRow);
    cmd.cmdDrawIndirect(argsDraw_, (kCount / 2) * 16, kCount / 2, 16);

    cmd.cmdBindRenderPipeline(pipeline_);
    const struct {
      uint32_t count;
      float rowY;
    } midRow = {.count = kCount, .rowY = 0.0f};
    cmd.cmdPushConstants(midRow);
    cmd.cmdDrawIndexedIndirect(argsIndexed_, 0, kCount, 20);

    cmd.cmdBindRenderPipeline(meshPipeline_);
    const struct {
      uint32_t count;
      float rowY;
    } botRow = {.count = kCount, .rowY = -0.6f};
    cmd.cmdPushConstants(botRow);
    cmd.cmdDrawMeshTasksIndirect(argsMesh_, 0, 2, 12);

    cmd.cmdEndRendering();
  }

  void destroy() override {
    fillPipeline_.reset();
    pipeline_.reset();
    pipelineGray_.reset();
    meshPipeline_.reset();
    argsDraw_.reset();
    primTypes_.reset();
    argsIndexed_.reset();
    indexBuffer_.reset();
    argsMesh_.reset();
    meshThreadgroups_.reset();
  }

 private:
  static constexpr uint32_t kCount = 8;

  lvk::metal::IMetalContext* ctx_ = nullptr;
  lvk::Holder<lvk::ComputePipelineHandle> fillPipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> pipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> pipelineGray_;
  lvk::Holder<lvk::RenderPipelineHandle> meshPipeline_;
  lvk::Holder<lvk::BufferHandle> argsDraw_;
  lvk::Holder<lvk::BufferHandle> primTypes_;
  lvk::Holder<lvk::BufferHandle> argsIndexed_;
  lvk::Holder<lvk::BufferHandle> indexBuffer_;
  lvk::Holder<lvk::BufferHandle> argsMesh_;
  lvk::Holder<lvk::BufferHandle> meshThreadgroups_;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
};

DESKTOP_MAIN(IndirectDraws)
