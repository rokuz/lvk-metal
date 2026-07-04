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

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

using glm::mat4;
using glm::vec3;

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

static float fmod2(float x, float y) {
  return x - y * floor(x / y);
}

static float2 rotate2d(float2 v, float angle, float2 center) {
  float2x2 r = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
  return r * (v - center);
}

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
    const float2 b = result.triangle_barycentric_coord;
    color = float3(1.0 - b.x - b.y, b.x, b.y);
  } else {
    const float2 uv = rotate2d(float2(tid), 0.2 * pc.time, 0.5 * float2(width, height));
    const float2 pos = floor(uv / 64.0);
    color = float3(fmod2(pos.x + fmod2(pos.y, 2.0), 2.0));
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

class RTXHello final : public lvk::metal::ISample {
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
        .viewInverse = glm::inverse(glm::translate(glm::mat4(1.0f), vec3(0, 0, -5.0f))),
        .projInverse = glm::inverse(glm::perspective(glm::radians(60.0f), float(width_) / float(height_), 0.1f, 1000.0f)),
    };
    camera_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(uniformData),
        .data = &uniformData,
        .debugName = "Buffer: camera",
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

    lvk::Holder<lvk::ShaderModuleHandle> trace;
#ifdef LVK_METAL_SAMPLE_USE_SLANG
    const char* useSlang = getenv("LVK_METAL_USE_SLANG");
    if (useSlang && useSlang[0] && useSlang[0] != '0') {
      LLOGL("[lvk-metal] shaders: Slang -> MSL");
      SlangRuntime slang(LVK_METAL_SAMPLE_SHADERS_DIR);
      trace = slang.createShaderModule(ctx, "rtx_hello", "traceRays", lvk::Stage_Comp, "rtx.trace");
    } else
#endif
    {
      trace = ctx.createShaderModule({kTraceMSL, "traceRays", lvk::Stage_Comp, "rtx.trace"});
    }
    pipeline_ = ctx.createComputePipeline({.smComp = trace});

    lvk::Holder<lvk::ShaderModuleHandle> presentVert =
        ctx.createShaderModule({kPresentMSL, "presentVert", lvk::Stage_Vert, "rtx.present.vert"});
    lvk::Holder<lvk::ShaderModuleHandle> presentFrag =
        ctx.createShaderModule({kPresentMSL, "presentFrag", lvk::Stage_Frag, "rtx.present.frag"});
    present_ = ctx.createRenderPipeline({
        .smVert = presentVert,
        .smFrag = presentFrag,
        .color = {{.format = ctx.getSwapchainFormat()}},
        .debugName = "Pipeline: present",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const lvk::mat3x4 transform = toLvkTransform(glm::rotate(glm::mat4(1.0f), time, vec3(1, 1, 1)));
    ctx_->upload(instances_, &transform, sizeof(transform), offsetof(lvk::AccelStructInstance, transform));

    const struct {
      uint64_t cam;
      uint32_t outImage;
      uint32_t tlas;
      float time;
    } pc = {
        .cam = ctx_->gpuAddress(camera_),
        .outImage = storageImage_.index(),
        .tlas = tlas_.index(),
        .time = time,
    };

    cmd.cmdUpdateTLAS(tlas_, instances_);
    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdPushConstants(pc);
    cmd.cmdTraceRays(width_, height_, 1, {.storageImages = {storageImage_}});

    const struct {
      uint32_t tex;
      uint32_t smp;
    } presentPC = {
        .tex = storageImage_.index(),
        .smp = sampler_.index(),
    };
    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          {.color = {{.texture = target}}});
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

    const lvk::AccelStructInstance instance = {
        .transform = toLvkTransform(glm::mat4(1.0f)),
        .instanceCustomIndex = 0,
        .mask = 0xff,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = lvk::AccelStructInstanceFlagBits_TriangleFacingCullDisable,
        .accelerationStructureReference = ctx_->gpuAddress(blas_),
    };
    instances_ = ctx_->createBuffer({
        .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_HostVisible,
        .size = sizeof(instance),
        .data = &instance,
        .debugName = "Buffer: TLAS instances",
    });

    tlas_ = ctx_->createAccelerationStructure({
        .type = lvk::AccelStructType_TLAS,
        .geometryType = lvk::AccelStructGeomType_Instances,
        .instancesBuffer = instances_,
        .buildRange = {.primitiveCount = 1},
        .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastTrace,
        .debugName = "TLAS",
    });
  }

  lvk::metal::IMetalContext* ctx_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  lvk::Holder<lvk::BufferHandle> vertexBuffer_;
  lvk::Holder<lvk::BufferHandle> indexBuffer_;
  lvk::Holder<lvk::BufferHandle> instances_;
  lvk::Holder<lvk::BufferHandle> camera_;
  lvk::Holder<lvk::AccelStructHandle> blas_;
  lvk::Holder<lvk::AccelStructHandle> tlas_;
  lvk::Holder<lvk::TextureHandle> storageImage_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::ComputePipelineHandle> pipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> present_;
};

DESKTOP_MAIN(RTXHello)
