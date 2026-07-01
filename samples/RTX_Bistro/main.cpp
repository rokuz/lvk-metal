#include <cstdint>
#include <cstdlib>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

#include <lvk/LVK-Metal.h>

#include "app_common.h"
#include "bistro_mesh.h"

#ifdef LVK_METAL_SAMPLE_USE_SLANG
#include "slang_runtime.h"
#endif

using glm::mat4;
using glm::vec3;
using glm::vec4;

static const char* kTraceMSL = R"MSL(
using namespace metal;
using namespace raytracing;

struct Vertex {
  packed_float3 position;
  uint uv;
  ushort normal;
  ushort mtlIndex;
};

struct Material {
  float4 ambient;
  float4 diffuse;
};

struct PerFrame {
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct PushConstants {
  float4 lightDir;
  device const PerFrame* perFrame;
  device const Material* materials;
  device const uint* indices;
  device const Vertex* vertices;
  uint outImage;
  uint tlas;
  uint enableShadows;
};

static float2 unpackSnorm2x8(uint d) {
  return float2(uint2(d, d >> 8) & 255u) / 127.5 - 1.0;
}

static float3 unpackOctahedral16(uint data) {
  float2 v = unpackSnorm2x8(data);
  float3 n = float3(v, 1.0 - abs(v.x) - abs(v.y));
  float t = max(-n.z, 0.0);
  n.x += (n.x > 0.0) ? -t : t;
  n.y += (n.y > 0.0) ? -t : t;
  return normalize(n);
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

  const float4 origin = pc.perFrame->viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.perFrame->projInverse * float4(d, 1, 1);
  const float4 direction = pc.perFrame->viewInverse * float4(normalize(targetClip.xyz), 0);

  ray r;
  r.origin = origin.xyz;
  r.direction = direction.xyz;
  r.min_distance = 0.01;
  r.max_distance = 1000.0;

  intersector<instancing, triangle_data> isect;
  isect.assume_geometry_type(geometry_type::triangle);
  isect.force_opacity(forced_opacity::opaque);

  intersection_result<instancing, triangle_data> hit = isect.intersect(r, kTLAS.data[pc.tlas], 0xff);

  float4 color;
  if (hit.type == intersection_type::none) {
    color = float4(0.1, 0.1, 1.0, 1.0);
  } else {
    const uint index = 3 * hit.primitive_id;
    const uint3 ti = uint3(pc.indices[index + 0], pc.indices[index + 1], pc.indices[index + 2]);
    const float2 bc = hit.triangle_barycentric_coord;
    const float3 bary = float3(1.0 - bc.x - bc.y, bc.x, bc.y);
    const float3 n0 = unpackOctahedral16(uint(pc.vertices[ti.x].normal));
    const float3 n1 = unpackOctahedral16(uint(pc.vertices[ti.y].normal));
    const float3 n2 = unpackOctahedral16(uint(pc.vertices[ti.z].normal));
    const float3 normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    const Material mat = pc.materials[uint(pc.vertices[ti.x].mtlIndex)];

    bool shadowed = false;
    if (pc.enableShadows != 0) {
      const float3 hitPoint = r.origin + r.direction * hit.distance;
      ray sr;
      sr.origin = hitPoint;
      sr.direction = pc.lightDir.xyz;
      sr.min_distance = 0.01;
      sr.max_distance = 1000.0;
      intersector<instancing, triangle_data> sIsect;
      sIsect.assume_geometry_type(geometry_type::triangle);
      sIsect.force_opacity(forced_opacity::opaque);
      sIsect.accept_any_intersection(true);
      intersection_result<instancing, triangle_data> sHit = sIsect.intersect(sr, kTLAS.data[pc.tlas], 0xff);
      shadowed = sHit.type != intersection_type::none;
    }

    const float occlusion = shadowed ? 0.5 : 1.0;
    const float NdotL1 = clamp(dot(normal, normalize(float3(-1, 1, 1))), 0.0, 1.0);
    const float NdotL2 = clamp(dot(normal, normalize(float3(-1, 1, -1))), 0.0, 1.0);
    const float NdotL = 0.5 * (NdotL1 + NdotL2);
    color = float4(mat.diffuse.rgb * occlusion * max(NdotL, 0.0), mat.diffuse.a);
  }

  kImages2D.data[pc.outImage].write(color, tid);
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

struct GPUMaterial {
  vec4 ambient;
  vec4 diffuse;
};

static lvk::mat3x4 toLvkTransform(const glm::mat4& m) {
  lvk::mat3x4 out = {};
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c)
      out.matrix[r][c] = m[c][r];
  return out;
}

class RTXBistro final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow*, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    width_ = width;
    height_ = height;

    lvk::metal::bistro::Mesh mesh;
    if (!mesh.load(std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/")) {
      LLOGW("RTX_Bistro: failed to load Bistro model (deploy lvk content first)");
      std::abort();
    }

    std::vector<GPUMaterial> materials;
    materials.reserve(mesh.materials.size());
    for (const lvk::metal::bistro::CachedMaterial& m : mesh.materials)
      materials.push_back({vec4(m.ambient, 1.0f), vec4(m.diffuse, 1.0f)});
    materials_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(GPUMaterial) * materials.size(),
        .data = materials.data(),
        .debugName = "Buffer: materials",
    });

    vertexBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_Device,
        .size = sizeof(lvk::metal::bistro::VertexData) * mesh.vertices.size(),
        .data = mesh.vertices.data(),
        .debugName = "Buffer: vertices",
    });
    indexBuffer_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage | lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_Device,
        .size = sizeof(uint32_t) * mesh.indices.size(),
        .data = mesh.indices.data(),
        .debugName = "Buffer: indices",
    });

    blas_ = ctx.createAccelerationStructure({
        .type = lvk::AccelStructType_BLAS,
        .geometryType = lvk::AccelStructGeomType_Triangles,
        .vertexFormat = lvk::VertexFormat_Float3,
        .vertexBuffer = vertexBuffer_,
        .vertexStride = sizeof(lvk::metal::bistro::VertexData),
        .numVertices = uint32_t(mesh.vertices.size()),
        .indexFormat = lvk::IndexFormat_UI32,
        .indexBuffer = indexBuffer_,
        .buildRange = {.primitiveCount = uint32_t(mesh.indices.size()) / 3},
        .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastTrace,
        .debugName = "BLAS",
    });

    const lvk::AccelStructInstance instance = {
        .transform = toLvkTransform(glm::scale(glm::mat4(1.0f), vec3(0.05f))),
        .instanceCustomIndex = 0,
        .mask = 0xff,
        .instanceShaderBindingTableRecordOffset = 0,
        .flags = lvk::AccelStructInstanceFlagBits_TriangleFacingCullDisable,
        .accelerationStructureReference = ctx.gpuAddress(blas_),
    };
    instances_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_AccelStructBuildInputReadOnly,
        .storage = lvk::StorageType_HostVisible,
        .size = sizeof(instance),
        .data = &instance,
        .debugName = "Buffer: TLAS instances",
    });
    tlas_ = ctx.createAccelerationStructure({
        .type = lvk::AccelStructType_TLAS,
        .geometryType = lvk::AccelStructGeomType_Instances,
        .instancesBuffer = instances_,
        .buildRange = {.primitiveCount = 1},
        .buildFlags = lvk::AccelStructBuildFlagBits_PreferFastTrace,
        .debugName = "TLAS",
    });

    const glm::mat4 view = glm::lookAt(vec3(-100, 40, -47), vec3(0, 35, 0), vec3(0, 1, 0));
    const glm::mat4 proj = glm::perspective(glm::radians(45.0f), float(width_) / float(height_), 0.5f, 500.0f);
    const struct {
      glm::mat4 viewInverse;
      glm::mat4 projInverse;
    } perFrame = {glm::inverse(view), glm::inverse(proj)};
    camera_ = ctx.createBuffer({
        .usage = lvk::BufferUsageBits_Storage,
        .storage = lvk::StorageType_Device,
        .size = sizeof(perFrame),
        .data = &perFrame,
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
      trace = slang.createShaderModule(ctx, "rtx_bistro", "traceRays", lvk::Stage_Comp, "rtx.trace");
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
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "Pipeline: present",
    });
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float) override {
    const struct {
      glm::vec4 lightDir;
      uint64_t perFrame;
      uint64_t materials;
      uint64_t indices;
      uint64_t vertices;
      uint32_t outImage;
      uint32_t tlas;
      uint32_t enableShadows;
    } pc = {
        .lightDir = glm::vec4(glm::normalize(vec3(0.032f, 0.835f, 0.549f)), 0.0f),
        .perFrame = ctx_->gpuAddress(camera_),
        .materials = ctx_->gpuAddress(materials_),
        .indices = ctx_->gpuAddress(indexBuffer_),
        .vertices = ctx_->gpuAddress(vertexBuffer_),
        .outImage = storageImage_.index(),
        .tlas = tlas_.index(),
        .enableShadows = 1u,
    };

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
  lvk::metal::IMetalContext* ctx_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;

  lvk::Holder<lvk::BufferHandle> vertexBuffer_;
  lvk::Holder<lvk::BufferHandle> indexBuffer_;
  lvk::Holder<lvk::BufferHandle> materials_;
  lvk::Holder<lvk::BufferHandle> instances_;
  lvk::Holder<lvk::BufferHandle> camera_;
  lvk::Holder<lvk::AccelStructHandle> blas_;
  lvk::Holder<lvk::AccelStructHandle> tlas_;
  lvk::Holder<lvk::TextureHandle> storageImage_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::ComputePipelineHandle> pipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> present_;
};

DESKTOP_MAIN(RTXBistro)
