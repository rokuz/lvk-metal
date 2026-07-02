#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"

using glm::mat4;
using glm::vec3;

static const uint32_t kNumInstances = 64;

static const char* kPopulateMSL = R"MSL(
using namespace metal;

struct InstanceDesc {
  packed_float3 transform[4];
  uint options;
  uint mask;
  uint intersectionFunctionTableOffset;
  uint userID;
  uint64_t accelerationStructureID;
};

struct PopulateConstants {
  device InstanceDesc* instances;
  device uint* count;
  uint64_t blasId;
  uint maxInstances;
  float time;
};

kernel void populateInstances(uint2 tid [[thread_position_in_grid]],
                              uint2 gridSize [[threads_per_grid]],
                              constant PopulateConstants& pc [[buffer(0)]]) {
  const uint gid = tid.y * gridSize.x + tid.x;
  if (gid >= pc.maxInstances)
    return;

  const uint active = 1u + uint((0.5 + 0.5 * sin(pc.time * 0.7)) * float(pc.maxInstances - 1u));
  if (gid == 0)
    *pc.count = active;

  const float ring = 6.0;
  const float ang = float(gid) * (6.2831853 / float(pc.maxInstances)) + pc.time * 0.5;
  const float3 t = float3(cos(ang) * ring, 1.5 * sin(pc.time + float(gid) * 0.4), sin(ang) * ring);
  const float s = 0.35;
  const float spin = pc.time * 1.5 + float(gid);
  const float c = cos(spin);
  const float sn = sin(spin);

  InstanceDesc d;
  d.transform[0] = packed_float3(c * s, 0.0, -sn * s);
  d.transform[1] = packed_float3(0.0, s, 0.0);
  d.transform[2] = packed_float3(sn * s, 0.0, c * s);
  d.transform[3] = packed_float3(t);
  d.options = 0u;
  d.mask = 0xFFu;
  d.intersectionFunctionTableOffset = 0u;
  d.userID = gid;
  d.accelerationStructureID = pc.blasId;
  pc.instances[gid] = d;
}
)MSL";

static const char* kTraceMSL = R"MSL(
using namespace metal;
using namespace raytracing;

struct Camera {
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct PushConstants {
  device const Camera* cam;
  uint outImage;
  uint tlas;
  float time;
};

kernel void traceRays(uint2 tid [[thread_position_in_grid]],
                      constant PushConstants& pc [[buffer(0)]],
                      LVK_BINDLESS_ARGS) {
  const uint width = kImages2D.data[pc.outImage].get_width();
  const uint height = kImages2D.data[pc.outImage].get_height();
  if (tid.x >= width || tid.y >= height)
    return;

  const float2 pixelCenter = float2(tid) + float2(0.5);
  float2 d = 2.0 * (pixelCenter / float2(width, height)) - 1.0;
  d.y = -d.y;

  const float4 origin = pc.cam->viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.cam->projInverse * float4(d, 1, 1);
  const float4 direction = pc.cam->viewInverse * float4(normalize(targetClip.xyz), 0);

  ray r;
  r.origin = origin.xyz;
  r.direction = direction.xyz;
  r.min_distance = 0.1;
  r.max_distance = 500.0;

  intersector<instancing, triangle_data> isect;
  isect.assume_geometry_type(geometry_type::triangle);
  isect.force_opacity(forced_opacity::opaque);

  intersection_result<instancing, triangle_data> result = isect.intersect(r, kTLAS.data[pc.tlas], 0xFF);

  float3 color;
  if (result.type == intersection_type::triangle) {
    const float id = float(result.instance_id);
    const float3 tint = 0.5 + 0.5 * cos(float3(0.0, 2.0, 4.0) + id * 0.6);
    const float2 b = result.triangle_barycentric_coord;
    const float shade = 0.35 + 0.65 * (1.0 - b.x - b.y);
    color = tint * shade;
  } else {
    color = float3(0.02, 0.02, 0.05);
  }

  kImages2D.data[pc.outImage].write(float4(color, 1.0), tid);
}
)MSL";

static const char* kPresentMSL = R"MSL(
using namespace metal;

struct VertexOut {
  float4 position [[position]];
  float2 uv;
};

struct PresentConstants {
  uint tex;
  uint smp;
};

vertex VertexOut presentVert(uint vid [[vertex_id]]) {
  VertexOut out;
  out.uv = float2((vid << 1) & 2, vid & 2);
  out.position = float4(out.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  return out;
}

fragment float4 presentFrag(VertexOut in [[stage_in]], constant PresentConstants& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  return textureBindless2D(pc.tex, pc.smp, in.uv);
}
)MSL";

static lvk::mat3x4 toLvkTransform(const glm::mat4& m) {
  lvk::mat3x4 out = {};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      out.matrix[r][c] = m[c][r];
  return out;
}

class RTXIndirectTLAS final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;

    createAccelerationStructures();

    const struct UniformData {
      glm::mat4 viewInverse;
      glm::mat4 projInverse;
    } uniformData = {
        .viewInverse = glm::inverse(glm::lookAt(vec3(0, 7, 16), vec3(0, 0, 0), vec3(0, 1, 0))),
        .projInverse = glm::inverse(glm::perspective(glm::radians(60.0f), float(width_) / float(height_), 0.1f, 1000.0f)),
    };
    camera_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(uniformData),
        .data = &uniformData,
        .debugName = "Buffer: camera",
    });

    instanceBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = uint64_t(kNumInstances) * ctx.indirectTLASInstanceDescriptorSize(),
        .debugName = "Buffer: TLAS instances (GPU-written)",
    });
    countBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(uint32_t),
        .debugName = "Buffer: TLAS instance count (GPU-written)",
    });

    storageImage_ = ctx.createTexture({
        .type = lvk::TextureType_2D,
        .format = lvk::Format_BGRA_UN8,
        .dimensions = {width_, height_, 1},
        .usage = lvk::TextureUsageBits_Storage | lvk::TextureUsageBits_Sampled,
        .storage = lvk::StorageType_Device,
        .debugName = "storageImage",
    });
    sampler_ = ctx.createSampler({.debugName = "sampler"});

    lvk::Holder<lvk::ShaderModuleHandle> populate =
        ctx.createShaderModule({kPopulateMSL, "populateInstances", lvk::Stage_Comp, "rtx.populate"});
    populate_ = ctx.createComputePipeline({.smComp = populate});

    lvk::Holder<lvk::ShaderModuleHandle> trace = ctx.createShaderModule({kTraceMSL, "traceRays", lvk::Stage_Comp, "rtx.trace"});
    trace_ = ctx.createComputePipeline({.smComp = trace});

    lvk::Holder<lvk::ShaderModuleHandle> presentVert =
        ctx.createShaderModule({kPresentMSL, "presentVert", lvk::Stage_Vert, "rtx.present.vert"});
    lvk::Holder<lvk::ShaderModuleHandle> presentFrag =
        ctx.createShaderModule({kPresentMSL, "presentFrag", lvk::Stage_Frag, "rtx.present.frag"});
    present_ = ctx.createRenderPipeline({
        .smVert = presentVert,
        .smFrag = presentFrag,
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "Pipeline: present",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    lvk::metal::IMetalCommandBuffer& mcmd = static_cast<lvk::metal::IMetalCommandBuffer&>(cmd);

    const struct {
      uint64_t instances;
      uint64_t count;
      uint64_t blasId;
      uint32_t maxInstances;
      float time;
    } populatePC = {
        .instances = ctx_->gpuAddress(instanceBuffer_),
        .count = ctx_->gpuAddress(countBuffer_),
        .blasId = ctx_->gpuAddress(blas_),
        .maxInstances = kNumInstances,
        .time = time,
    };
    cmd.cmdBindComputePipeline(populate_);
    cmd.cmdPushConstants(populatePC);
    cmd.cmdDispatch({(kNumInstances + 255) / 256, 1, 1}, {.buffers = {instanceBuffer_, countBuffer_}});

    mcmd.cmdBuildIndirectTLAS(tlas_, instanceBuffer_, kNumInstances, countBuffer_);

    const struct {
      uint64_t cam;
      uint32_t outImage;
      uint32_t tlas;
      float time;
    } tracePC = {
        .cam = ctx_->gpuAddress(camera_),
        .outImage = storageImage_.index(),
        .tlas = tlas_.index(),
        .time = time,
    };
    cmd.cmdBindComputePipeline(trace_);
    cmd.cmdPushConstants(tracePC);
    cmd.cmdTraceRays(width_, height_, 1, {.storageImages = {storageImage_}});

    const struct {
      uint32_t tex;
      uint32_t smp;
    } presentPC = {
        .tex = storageImage_.index(),
        .smp = sampler_.index(),
    };
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          {.color = {{.texture = target}}},
                          {.sampledImages = {storageImage_}});
    cmd.cmdBindRenderPipeline(present_);
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdPushConstants(presentPC);
    cmd.cmdDraw(3);
    cmd.cmdEndRendering();
  }

 private:
  void createAccelerationStructures() {
    struct Vertex {
      float pos[3];
    };
    const float t = (1.0f + std::sqrt(5.0f)) / 2.0f;
    const Vertex vertices[] = {
        {-1, t, 0},
        {1, t, 0},
        {-1, -t, 0},
        {1, -t, 0},
        {0, -1, t},
        {0, 1, t},
        {0, -1, -t},
        {0, 1, -t},
        {t, 0, -1},
        {t, 0, 1},
        {-t, 0, -1},
        {-t, 0, 1},
    };
    const uint32_t indices[] = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4,  11, 10, 2,  10, 7, 6, 7, 1, 8,
                                3, 9,  4, 3, 4, 2, 3, 2, 6, 3, 6, 8,  3, 8,  9,  4, 9, 5, 2, 4,  11, 6,  2,  10, 8,  6, 7, 9, 8, 1};

    vertexBuffer_ = ctx_->createBuffer({
        .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_HostVisible,
        .size = sizeof(vertices),
        .data = vertices,
        .debugName = "Buffer: BLAS vertices",
    });
    indexBuffer_ = ctx_->createBuffer({
        .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_HostVisible,
        .size = sizeof(indices),
        .data = indices,
        .debugName = "Buffer: BLAS indices",
    });

    blas_ = ctx_->createAccelerationStructure({
        .type = lvk::AccelStructType_BLAS,
        .geometryType = lvk::AccelStructGeomType_Triangles,
        .vertexFormat = lvk::VertexFormat_Float3,
        .vertexBuffer = vertexBuffer_,
        .numVertices = uint32_t(LVK_ARRAY_NUM_ELEMENTS(vertices)),
        .indexFormat = lvk::IndexFormat_UI32,
        .indexBuffer = indexBuffer_,
        .buildRange = {.primitiveCount = uint32_t(LVK_ARRAY_NUM_ELEMENTS(indices)) / 3},
        .debugName = "BLAS",
    });

    tlas_ = ctx_->createAccelerationStructure({
        .type = lvk::AccelStructType_TLAS,
        .geometryType = lvk::AccelStructGeomType_Instances,
        .buildRange = {.primitiveCount = kNumInstances},
        .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastBuild,
        .debugName = "TLAS (indirect, GPU-built)",
    });
  }

  lvk::metal::IMetalContext* ctx_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  lvk::Holder<lvk::BufferHandle> vertexBuffer_;
  lvk::Holder<lvk::BufferHandle> indexBuffer_;
  lvk::Holder<lvk::BufferHandle> instanceBuffer_;
  lvk::Holder<lvk::BufferHandle> countBuffer_;
  lvk::Holder<lvk::BufferHandle> camera_;
  lvk::Holder<lvk::AccelStructHandle> blas_;
  lvk::Holder<lvk::AccelStructHandle> tlas_;
  lvk::Holder<lvk::TextureHandle> storageImage_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::ComputePipelineHandle> populate_;
  lvk::Holder<lvk::ComputePipelineHandle> trace_;
  lvk::Holder<lvk::RenderPipelineHandle> present_;
};

DESKTOP_MAIN(RTXIndirectTLAS)
