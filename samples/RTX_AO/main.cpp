#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <GLFW/glfw3.h>

#include <shared/Camera.h>

#include <lvk/HelpersImGuiMetal.h>
#include <lvk/LVK-Metal.h>

#include "app_common.h"
#include "bistro_mesh.h"

using glm::mat4;
using glm::vec2;
using glm::vec3;
using glm::vec4;

static const uint32_t kHashMapSize = 16 * 1024 * 1024;
static const uint32_t kWarmupFrames = 64;

static const char* kTraceMSL = R"MSL(
using namespace metal;
using namespace raytracing;

struct VertexData {
  packed_float3 position;
  uint uv;
  ushort normal;
  ushort mtlIndex;
};
struct Material { float4 ambient; float4 diffuse; };
struct PerFrame { float4x4 proj; float4x4 view; float4x4 viewInverse; float4x4 projInverse; };

struct PC {
  float4 lightDir;
  device const PerFrame* perFrame;
  device const Material* materials;
  device const uint* indices;
  device const VertexData* vertices;
  device atomic_uint* hashHi;
  device atomic_uint* hashLo;
  uint outImage;
  uint tlas;
  uint enableShadows;
  uint enableAO;
  int aoSamples;
  float aoRadius;
  float aoPower;
  uint frameId;
  float sp;
  float smin;
  uint maxSamples;
  uint hashMapSize;
  float resolutionY;
  uint enableFiltering;
  uint enableSpatialHash;
  uint timeVaryingNoise;
};

static float2 unpackSnorm2x8(uint d) { return float2(uint2(d, d >> 8) & 255u) / 127.5 - 1.0; }
static float3 unpackOctahedral16(uint data) {
  float2 v = unpackSnorm2x8(data);
  float3 n = float3(v, 1.0 - abs(v.x) - abs(v.y));
  float t = max(-n.z, 0.0);
  n.x += (n.x > 0.0) ? -t : t;
  n.y += (n.y > 0.0) ? -t : t;
  return normalize(n);
}
static void computeTBN(float3 n, thread float3& x, thread float3& y) {
  float yz = -n.y * n.z;
  y = normalize((abs(n.z) > 0.9999) ? float3(-n.x * n.y, 1.0 - n.y * n.y, yz) : float3(-n.x * n.z, yz, 1.0 - n.z * n.z));
  x = cross(y, n);
}
static uint lcg(thread uint& p) { p = 1664525u * p + 1013904223u; return p & 0x00FFFFFFu; }
static float rnd(thread uint& s) { return float(lcg(s)) / float(0x01000000); }
static uint tea(uint v0, uint v1) {
  uint s0 = 0;
  for (uint n = 0; n < 16; n++) {
    s0 += 0x9e3779b9u;
    v0 += ((v1 << 4) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4u);
    v1 += ((v0 << 4) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761eu);
  }
  return v0;
}
static float3 sampleCosineHemisphere(thread uint& seed, float3 t, float3 b, float3 n) {
  float r1 = rnd(seed), r2 = rnd(seed);
  float sq = sqrt(1.0 - r2), phi = 2.0 * 3.141592653589 * r1;
  float3 d = float3(cos(phi) * sq, sin(phi) * sq, sqrt(r2));
  return d.x * t + d.y * b + d.z * n;
}
static uint pcg(uint v) {
  uint state = v * 747796405u + 2891336453u;
  uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
  return (word >> 22u) ^ word;
}
static uint xxhash32(uint p) {
  const uint P2 = 2246822519u, P3 = 3266489917u, P4 = 668265263u, P5 = 374761393u;
  uint h = p + P5;
  h = P4 * ((h << 17u) | (h >> 15u));
  h = P2 * (h ^ (h >> 15u));
  h = P3 * (h ^ (h >> 13u));
  return h ^ (h >> 16u);
}

static uint hLoad(device atomic_uint* a, uint i) { return atomic_load_explicit(&a[i], memory_order_relaxed); }
static void hStore(device atomic_uint* a, uint i, uint v) { atomic_store_explicit(&a[i], v, memory_order_relaxed); }
static uint hAdd(device atomic_uint* a, uint i, uint v) { return atomic_fetch_add_explicit(&a[i], v, memory_order_relaxed); }
static uint hCAS(device atomic_uint* a, uint i, uint cmp, uint val) {
  uint e = cmp;
  while (!atomic_compare_exchange_weak_explicit(&a[i], &e, val, memory_order_relaxed, memory_order_relaxed)) {
    if (e != cmp) return e;
  }
  return cmp;
}
static uint readLoSeq(constant PC& pc, uint cell, uint checksum) {
  uint v1 = hLoad(pc.hashLo, cell) >> 24u;
  if (v1 & 1u) return 0u;
  atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst);
  uint cs = hLoad(pc.hashHi, cell) & 0xFFFFFFu;
  atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst);
  uint lo2 = hLoad(pc.hashLo, cell);
  if (v1 != (lo2 >> 24u) || cs != checksum) return 0u;
  return hLoad(pc.hashLo, cell) & 0xFFFFFFu;
}

static bool traceAO(instance_acceleration_structure tlas, float3 origin, float3 dir, float tmax) {
  ray r; r.origin = origin; r.direction = dir; r.min_distance = 0.0; r.max_distance = tmax;
  intersector<instancing, triangle_data> isect;
  isect.assume_geometry_type(geometry_type::triangle);
  isect.force_opacity(forced_opacity::opaque);
  isect.accept_any_intersection(true);
  return isect.intersect(r, tlas, 0xff).type != intersection_type::none;
}

static void bumpFrame(constant PC& pc, uint cell, uint frameLow) {
  uint cur = hLoad(pc.hashHi, cell);
  for (uint a = 0; a < 4u; a++) {
    if ((cur >> 24u) == frameLow) return;
    uint fresh = (cur & 0xFFFFFFu) | (frameLow << 24u);
    uint prev = hCAS(pc.hashHi, cell, cur, fresh);
    if (prev == cur) return;
    cur = prev;
  }
}
static void halveLo(constant PC& pc, uint cell, uint cur) {
  for (uint a = 0; a < 4u; a++) {
    uint s = cur & 0xFFFu;
    uint h = min((cur >> 12u) & 0xFFFu, s);
    uint newLo = (cur & 0xFF000000u) | ((h >> 1u) << 12u) | (s >> 1u);
    uint prev = hCAS(pc.hashLo, cell, cur, newLo);
    if (prev == cur) return;
    cur = prev;
  }
}
static void undoAdd(constant PC& pc, uint cell, uint cur, uint hit) {
  for (uint a = 0; a < 4u; a++) {
    uint cur_hits = (cur >> 12u) & 0xFFFu;
    uint dec_hits = (hit > 0u && cur_hits > 0u) ? 1u : 0u;
    uint dec = 1u | (dec_hits << 12u);
    uint newLo = (cur & 0xFF000000u) | (((cur & 0x00FFFFFFu) - dec) & 0x00FFFFFFu);
    uint prev = hCAS(pc.hashLo, cell, cur, newLo);
    if (prev == cur) return;
    cur = prev;
  }
}
static void accumulateAndMaybeHalve(constant PC& pc, uint cell, uint hit) {
  uint addend = (hit << 12u) + 1u;
  uint prev = hAdd(pc.hashLo, cell, addend);
  uint prevSamples = prev & 0xFFFu;
  if (prevSamples >= pc.maxSamples) { undoAdd(pc, cell, prev + addend, hit); return; }
  if (prevSamples + 1u == pc.maxSamples) halveLo(pc, cell, prev + addend);
}
static bool isCellReady(uint lo) { return (lo & 0xFFFu) >= 4u; }
static float cellVisibility(uint lo) {
  uint samples = lo & 0xFFFu;
  uint hits = min((lo >> 12u) & 0xFFFu, samples);
  if (samples < 4u) return -1.0;
  return float(samples - hits) / float(samples);
}
static void computeBucket(constant PC& pc, float3 pos, float cs, uint nhPCG, uint nhXXH, thread uint& baseCell, thread uint& checksum) {
  int3 p = int3(floor(pos / cs));
  uint csu = uint(cs * 10000.0);
  uint key = pcg(csu + pcg(uint(p.x) + pcg(uint(p.y) + pcg(uint(p.z) + nhPCG))));
  baseCell = (key & ((pc.hashMapSize >> 2u) - 1u)) << 2u;
  checksum = max(xxhash32(csu + xxhash32(uint(p.x) + xxhash32(uint(p.y) + xxhash32(uint(p.z) + nhXXH)))) & 0xFFFFFFu, 1u);
}
static uint spatialHashFindOrInsert(constant PC& pc, float3 pos, float cs, uint nhPCG, uint nhXXH, thread uint& outChecksum) {
  uint baseCell, checksum;
  computeBucket(pc, pos, cs, nhPCG, nhXXH, baseCell, checksum);
  outChecksum = checksum;
  for (uint i = 0; i < 4u; i++)
    if ((hLoad(pc.hashHi, baseCell + i) & 0xFFFFFFu) == checksum) return baseCell + i;
  uint frameLow = pc.frameId & 0xFFu;
  for (uint i = 0; i < 4u; i++) {
    uint cell = baseCell + i;
    uint h = hLoad(pc.hashHi, cell);
    if ((h & 0xFFFFFFu) == checksum) return cell;
    bool empty = h == 0u;
    bool stale = !empty && (((frameLow - (h >> 24u)) & 0xFFu) > 3u);
    if (!empty && !stale) continue;
    uint lo = hLoad(pc.hashLo, cell);
    uint v = lo >> 24u;
    if (v & 1u) continue;
    if (hCAS(pc.hashLo, cell, lo, (((v + 1u) & 0xFFu) << 24u) | (lo & 0xFFFFFFu)) != lo) continue;
    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst);
    hStore(pc.hashHi, cell, (frameLow << 24u) | checksum);
    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst);
    hStore(pc.hashLo, cell, ((v + 2u) & 0xFFu) << 24u);
    return cell;
  }
  return 0xFFFFFFFFu;
}
static uint spatialHashFind(constant PC& pc, float3 pos, float cs, uint nhPCG, uint nhXXH, thread uint& outChecksum) {
  uint baseCell, checksum;
  computeBucket(pc, pos, cs, nhPCG, nhXXH, baseCell, checksum);
  outChecksum = checksum;
  for (uint i = 0; i < 4u; i++) {
    uint stored = hLoad(pc.hashHi, baseCell + i) & 0xFFFFFFu;
    if (stored == checksum) return baseCell + i;
    if (stored == 0u) return 0xFFFFFFFFu;
  }
  return 0xFFFFFFFFu;
}
static float computeCellSize(constant PC& pc, float3 worldPos) {
  float3x3 v3 = float3x3(pc.perFrame->view[0].xyz, pc.perFrame->view[1].xyz, pc.perFrame->view[2].xyz);
  float3 camPos = -(transpose(v3) * pc.perFrame->view[3].xyz);
  float dist = distance(camPos, worldPos);
  float h = dist / pc.perFrame->proj[1][1];
  float sw = pc.sp * (h * 2.0) / pc.resolutionY;
  return exp2(floor(log2(max(sw / pc.smin, 1.0)))) * pc.smin;
}

kernel void traceRays(uint2 tid [[thread_position_in_grid]], constant PC& pc [[buffer(0)]], LVK_BINDLESS_ARGS) {
  const uint width = kImages2D.data[pc.outImage].get_width();
  const uint height = kImages2D.data[pc.outImage].get_height();
  if (tid.x >= width || tid.y >= height)
    return;

  const float2 pixelCenter = float2(tid) + float2(0.5);
  float2 dxy = 2.0 * (pixelCenter / float2(width, height)) - 1.0;
  dxy.y = -dxy.y;
  const float4 rayOrigin = pc.perFrame->viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.perFrame->projInverse * float4(dxy, 1, 1);
  const float4 rayDir = pc.perFrame->viewInverse * float4(normalize(targetClip.xyz), 0);

  const instance_acceleration_structure tlas = kTLAS.data[pc.tlas];
  ray pr;
  pr.origin = rayOrigin.xyz;
  pr.direction = rayDir.xyz;
  pr.min_distance = 0.01;
  pr.max_distance = 1000.0;
  intersector<instancing, triangle_data> isect;
  isect.assume_geometry_type(geometry_type::triangle);
  isect.force_opacity(forced_opacity::opaque);
  const intersection_result<instancing, triangle_data> phit = isect.intersect(pr, tlas, 0xff);

  float4 color = float4(0.4, 0.6, 0.9, 1.0);
  if (phit.type != intersection_type::none) {
    const uint index = 3u * phit.primitive_id;
    const uint3 ti = uint3(pc.indices[index + 0], pc.indices[index + 1], pc.indices[index + 2]);
    const float2 bc = phit.triangle_barycentric_coord;
    const float3 bary = float3(1.0 - bc.x - bc.y, bc.x, bc.y);
    const float3 n0 = unpackOctahedral16(uint(pc.vertices[ti.x].normal));
    const float3 n1 = unpackOctahedral16(uint(pc.vertices[ti.y].normal));
    const float3 n2 = unpackOctahedral16(uint(pc.vertices[ti.z].normal));
    const float3 n = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    const Material mat = pc.materials[uint(pc.vertices[ti.x].mtlIndex)];
    const float3 worldPos = pr.origin + pr.direction * phit.distance;
    const float3 origin = worldPos + n * 0.001;

    float occlusion = 1.0;
    if (pc.enableAO != 0) {
      float3 tangent, bitangent;
      computeTBN(n, tangent, bitangent);
      uint seed = tea(uint(tid.y * 4003u + tid.x), pc.timeVaryingNoise != 0u ? pc.frameId : 0u);

      if (pc.enableSpatialHash != 0) {
        const float swd = computeCellSize(pc, worldPos);
        const int3 nn = int3(floor(n * 3.0));
        const uint nhPCG = pcg(uint(nn.x) + pcg(uint(nn.y) + pcg(uint(nn.z))));
        const uint nhXXH = xxhash32(uint(nn.x) + xxhash32(uint(nn.y) + xxhash32(uint(nn.z))));
        const uint frameLow = pc.frameId & 0xFFu;

        uint cs0;
        const uint cell0 = spatialHashFindOrInsert(pc, worldPos, swd, nhPCG, nhXXH, cs0);
        const uint data0 = (cell0 != 0xFFFFFFFFu) ? readLoSeq(pc, cell0, cs0) : 0u;
        const uint samples0 = data0 & 0xFFFu;
        const bool ready0 = isCellReady(data0);

        uint hit = 0u;
        const bool lod0NeedsRay = cell0 != 0xFFFFFFFFu && samples0 < pc.maxSamples;
        if (!ready0 || lod0NeedsRay) {
          const float3 dir = sampleCosineHemisphere(seed, tangent, bitangent, n);
          hit = traceAO(tlas, origin, dir, pc.aoRadius) ? 1u : 0u;
        }
        if (cell0 != 0xFFFFFFFFu) {
          if (lod0NeedsRay) accumulateAndMaybeHalve(pc, cell0, hit);
          bumpFrame(pc, cell0, frameLow);
        }

        uint cachedData[4] = {data0, 0u, 0u, 0u};
        if (!ready0) {
          float lodSize = swd * 2.0;
          for (int lod = 1; lod < 4; lod++) {
            uint csl;
            const uint ci = spatialHashFindOrInsert(pc, worldPos, lodSize, nhPCG, nhXXH, csl);
            if (ci != 0xFFFFFFFFu) {
              cachedData[lod] = readLoSeq(pc, ci, csl);
              if ((cachedData[lod] & 0xFFFu) < pc.maxSamples) accumulateAndMaybeHalve(pc, ci, hit);
              bumpFrame(pc, ci, frameLow);
            }
            lodSize *= 2.0;
          }
        }

        if (ready0 && pc.enableFiltering != 0) {
          const float3 cellPos = worldPos / swd - 0.5;
          const int3 base = int3(floor(cellPos));
          const float3 f = fract(cellPos);
          float totalWeight = 0.0, totalAO = 0.0;
          for (int dz = 0; dz < 2; dz++)
            for (int dy = 0; dy < 2; dy++)
              for (int dx = 0; dx < 2; dx++) {
                const float3 neighborPos = (float3(base + int3(dx, dy, dz)) + 0.5) * swd;
                uint csf;
                const uint ci = spatialHashFind(pc, neighborPos, swd, nhPCG, nhXXH, csf);
                if (ci != 0xFFFFFFFFu) {
                  const float vis = cellVisibility(readLoSeq(pc, ci, csf));
                  if (vis >= 0.0) {
                    const float w = (dx == 0 ? (1.0 - f.x) : f.x) * (dy == 0 ? (1.0 - f.y) : f.y) * (dz == 0 ? (1.0 - f.z) : f.z);
                    totalWeight += w;
                    totalAO += w * vis;
                  }
                }
              }
          occlusion = (totalWeight > 0.0) ? (totalAO / totalWeight) : 1.0;
        } else {
          uint renderData = ready0 ? data0 : 0u;
          if (!ready0)
            for (int lod = 3; lod >= 0; lod--)
              if (isCellReady(cachedData[lod])) renderData = cachedData[lod];
          if (renderData != 0u) {
            const float vis = cellVisibility(renderData);
            if (vis >= 0.0) occlusion = vis;
          }
        }
      } else {
        float occl = 0.0;
        for (int i = 0; i < pc.aoSamples; i++) {
          const float3 dir = sampleCosineHemisphere(seed, tangent, bitangent, n);
          occl += traceAO(tlas, origin, dir, pc.aoRadius) ? 1.0 : 0.0;
        }
        occlusion = 1.0 - occl / float(pc.aoSamples);
      }
      occlusion = pow(clamp(occlusion, 0.0, 1.0), pc.aoPower);
    }

    if (pc.enableShadows != 0) {
      if (traceAO(tlas, origin, pc.lightDir.xyz, 1000.0)) occlusion *= 0.5;
    }

    const float NdotL1 = clamp(dot(n, normalize(float3(1, 1, 1))), 0.0, 1.0);
    const float NdotL2 = clamp(dot(n, normalize(float3(-1, 1, -1))), 0.0, 1.0);
    const float NdotL = NdotL1 + NdotL2;
    color = mat.ambient + mat.diffuse * NdotL * occlusion;
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

struct PerFrameCPU {
  mat4 proj;
  mat4 view;
  mat4 viewInverse;
  mat4 projInverse;
};

static const uint32_t kRing = 3;

class RTXAmbientOcclusion final : public lvk::metal::ISample {
 public:
  void init(lvk::metal::IMetalContext& ctx, GLFWwindow* window, uint32_t width, uint32_t height, float, uint32_t) override {
    ctx_ = &ctx;
    window_ = window;
    width_ = width;
    height_ = height;
    positioner_ = CameraPositioner_FirstPerson(vec3(-100, 40, -47), vec3(0, 35, 0), vec3(0, 1, 0));

    lvk::metal::bistro::Mesh mesh;
    if (!mesh.load(std::string(LVK_METAL_SAMPLE_CONTENT_DIR) + "/")) {
      LLOGW("RTX_AO: failed to load Bistro model (deploy lvk content first)");
      std::abort();
    }
    numIndices_ = uint32_t(mesh.indices.size());

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
        .buildRange = {.primitiveCount = numIndices_ / 3},
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

    hashBytes_ = uint64_t(kHashMapSize) * sizeof(uint32_t);
    hashHi_ = ctx.createBuffer(
        {.usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = hashBytes_, .debugName = "hashHi"});
    hashLo_ = ctx.createBuffer(
        {.usage = lvk::BufferUsageBits_Storage, .storage = lvk::StorageType_HostVisible, .size = hashBytes_, .debugName = "hashLo"});
    std::memset(ctx.getMappedPtr(hashHi_), 0, hashBytes_);
    std::memset(ctx.getMappedPtr(hashLo_), 0, hashBytes_);

    for (uint32_t i = 0; i < kRing; ++i)
      perFrame_[i] = ctx.createBuffer({
          .usage = lvk::BufferUsageBits_Storage,
          .storage = lvk::StorageType_HostVisible,
          .size = sizeof(PerFrameCPU),
          .debugName = "Buffer: perFrame",
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

    lvk::Holder<lvk::ShaderModuleHandle> trace = ctx.createShaderModule({kTraceMSL, "traceRays", lvk::Stage_Comp, "ao.trace"});
    pipeline_ = ctx.createComputePipeline({.smComp = trace});

    lvk::Holder<lvk::ShaderModuleHandle> presentVert =
        ctx.createShaderModule({kPresentMSL, "presentVert", lvk::Stage_Vert, "ao.present.vert"});
    lvk::Holder<lvk::ShaderModuleHandle> presentFrag =
        ctx.createShaderModule({kPresentMSL, "presentFrag", lvk::Stage_Frag, "ao.present.frag"});
    present_ = ctx.createRenderPipeline({
        .smVert = presentVert,
        .smFrag = presentFrag,
        .color = {{.format = lvk::Format_BGRA_UN8}},
        .debugName = "Pipeline: present",
    });

    if (window_) {
      glfwSetWindowUserPointer(window_, this);
      glfwSetKeyCallback(window_, keyCallback);
      glfwSetMouseButtonCallback(window_, mouseButtonCallback);
      glfwSetCursorPosCallback(window_, cursorPosCallback);
    }
    imgui_ = std::make_unique<lvk::metal::ImGuiRenderer>(ctx, window_);
  }

  bool isReadyForScreenshot() const override {
    return frameId_ >= kWarmupFrames;
  }

  void render(lvk::ICommandBuffer& cmd, lvk::TextureHandle target, float time) override {
    const float delta = lastTime_ < 0.0f ? 1.0f / 60.0f : time - lastTime_;
    lastTime_ = time;
    const bool look = window_ && mousePressedLeft_ && !ImGui::GetIO().WantCaptureMouse;
    positioner_.update(delta, mousePos_, look);

    if (resetHash_) {
      std::memset(ctx_->getMappedPtr(hashHi_), 0, hashBytes_);
      std::memset(ctx_->getMappedPtr(hashLo_), 0, hashBytes_);
      resetHash_ = false;
    }

    const uint32_t r = frameId_ % kRing;
    const mat4 proj = glm::perspective(glm::radians(45.0f), float(width_) / float(height_), 0.5f, 500.0f);
    const mat4 view = camera_.getViewMatrix();
    const PerFrameCPU perFrame = {
        .proj = proj,
        .view = view,
        .viewInverse = glm::inverse(view),
        .projInverse = glm::inverse(proj),
    };
    std::memcpy(ctx_->getMappedPtr(perFrame_[r]), &perFrame, sizeof(perFrame));

    const struct {
      vec4 lightDir;
      uint64_t perFrame;
      uint64_t materials;
      uint64_t indices;
      uint64_t vertices;
      uint64_t hashHi;
      uint64_t hashLo;
      uint32_t outImage;
      uint32_t tlas;
      uint32_t enableShadows;
      uint32_t enableAO;
      int32_t aoSamples;
      float aoRadius;
      float aoPower;
      uint32_t frameId;
      float sp;
      float smin;
      uint32_t maxSamples;
      uint32_t hashMapSize;
      float resolutionY;
      uint32_t enableFiltering;
      uint32_t enableSpatialHash;
      uint32_t timeVaryingNoise;
    } pc = {
        .lightDir = vec4(glm::normalize(lightDir_), 0.0f),
        .perFrame = ctx_->gpuAddress(perFrame_[r]),
        .materials = ctx_->gpuAddress(materials_),
        .indices = ctx_->gpuAddress(indexBuffer_),
        .vertices = ctx_->gpuAddress(vertexBuffer_),
        .hashHi = ctx_->gpuAddress(hashHi_),
        .hashLo = ctx_->gpuAddress(hashLo_),
        .outImage = storageImage_.index(),
        .tlas = tlas_.index(),
        .enableShadows = enableShadows_ ? 1u : 0u,
        .enableAO = enableAO_ ? 1u : 0u,
        .aoSamples = aoSamples_,
        .aoRadius = aoRadius_,
        .aoPower = aoPower_,
        .frameId = frameId_++,
        .sp = sp_,
        .smin = smin_,
        .maxSamples = uint32_t(maxSamples_),
        .hashMapSize = kHashMapSize,
        .resolutionY = float(height_),
        .enableFiltering = enableFiltering_ ? 1u : 0u,
        .enableSpatialHash = enableSpatialHash_ ? 1u : 0u,
        .timeVaryingNoise = timeVaryingNoise_ ? 1u : 0u,
    };

    cmd.cmdBindComputePipeline(pipeline_);
    cmd.cmdPushConstants(pc);
    cmd.cmdTraceRays(width_, height_, 1, {.storageImages = {storageImage_}});

    const struct {
      uint32_t tex;
      uint32_t smp;
    } presentPC = {storageImage_.index(), sampler_.index()};

    const lvk::Framebuffer fb = {.color = {{.texture = target}}};
    imgui_->beginFrame(fb);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_FirstUseEver);
    ImGui::Begin("RTX Ambient Occlusion", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::Text("W/A/S/D move, 1/2 up/down, Shift fast");
    ImGui::Text("hold LMB + drag to look around");
    ImGui::Separator();
    ImGui::Checkbox("Ray traced shadows", &enableShadows_);
    ImGui::SliderFloat3("Light dir", &lightDir_.x, -1.0f, 1.0f);
    ImGui::Separator();
    ImGui::Checkbox("Ray traced AO", &enableAO_);
    ImGui::Checkbox("Time-varying noise", &timeVaryingNoise_);
    ImGui::SliderFloat("AO power", &aoPower_, 0.5f, 2.0f);
    ImGui::SliderFloat("AO radius", &aoRadius_, 1.0f, 16.0f);
    ImGui::SliderInt("AO samples (no hash)", &aoSamples_, 1, 32);
    ImGui::Separator();
    ImGui::Checkbox("Spatial hashing", &enableSpatialHash_);
    ImGui::SliderFloat("Pixel size (sp)", &sp_, 1.0f, 10.0f);
    ImGui::SliderFloat("Min cell size", &smin_, 0.05f, 0.5f);
    ImGui::SliderInt("Max samples/cell", &maxSamples_, 16, 250);
    ImGui::Checkbox("Trilinear filtering", &enableFiltering_);
    if (ImGui::Button("Reset hash map"))
      resetHash_ = true;
    ImGui::End();

    cmd.cmdBeginRendering({.color = {{.loadOp = lvk::LoadOp_Clear, .storeOp = lvk::StoreOp_Store, .clearColor = {0, 0, 0, 1}}}},
                          fb,
                          {.sampledImages = {storageImage_}});
    cmd.cmdBindRenderPipeline(present_);
    cmd.cmdBindViewport({.width = float(width_), .height = float(height_)});
    cmd.cmdPushConstants(presentPC);
    cmd.cmdDraw(3);
    imgui_->endFrame(cmd);
    cmd.cmdEndRendering();
  }

 private:
  static lvk::mat3x4 toLvkTransform(const glm::mat4& m) {
    lvk::mat3x4 out = {};
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 4; ++c)
        out.matrix[r][c] = m[c][r];
    return out;
  }

  static void keyCallback(GLFWwindow* window, int key, int, int action, int) {
    RTXAmbientOcclusion* self = static_cast<RTXAmbientOcclusion*>(glfwGetWindowUserPointer(window));
    if (!self || ImGui::GetIO().WantCaptureKeyboard)
      return;
    const bool pressed = action != GLFW_RELEASE;
    CameraPositioner_FirstPerson::Movement& m = self->positioner_.movement_;
    switch (key) {
    case GLFW_KEY_W:
      m.forward_ = pressed;
      break;
    case GLFW_KEY_S:
      m.backward_ = pressed;
      break;
    case GLFW_KEY_A:
      m.left_ = pressed;
      break;
    case GLFW_KEY_D:
      m.right_ = pressed;
      break;
    case GLFW_KEY_1:
      m.up_ = pressed;
      break;
    case GLFW_KEY_2:
      m.down_ = pressed;
      break;
    case GLFW_KEY_LEFT_SHIFT:
    case GLFW_KEY_RIGHT_SHIFT:
      m.fastSpeed_ = pressed;
      break;
    default:
      break;
    }
  }
  static void mouseButtonCallback(GLFWwindow* window, int button, int action, int) {
    RTXAmbientOcclusion* self = static_cast<RTXAmbientOcclusion*>(glfwGetWindowUserPointer(window));
    if (self && button == GLFW_MOUSE_BUTTON_LEFT)
      self->mousePressedLeft_ = action == GLFW_PRESS;
  }
  static void cursorPosCallback(GLFWwindow* window, double x, double y) {
    RTXAmbientOcclusion* self = static_cast<RTXAmbientOcclusion*>(glfwGetWindowUserPointer(window));
    if (!self)
      return;
    int w = 0, h = 0;
    glfwGetWindowSize(window, &w, &h);
    self->mousePos_ = vec2(w > 0 ? float(x) / w : 0.0f, h > 0 ? 1.0f - float(y) / h : 0.0f);
  }

  lvk::metal::IMetalContext* ctx_ = nullptr;
  GLFWwindow* window_ = nullptr;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  uint32_t numIndices_ = 0;
  uint32_t frameId_ = 0;
  float lastTime_ = -1.0f;
  uint64_t hashBytes_ = 0;

  vec2 mousePos_ = vec2(0);
  bool mousePressedLeft_ = false;
  CameraPositioner_FirstPerson positioner_;
  Camera camera_ = Camera(positioner_);

  bool enableShadows_ = true;
  bool enableAO_ = true;
  bool timeVaryingNoise_ = true;
  float aoPower_ = 1.0f;
  float aoRadius_ = 8.0f;
  int aoSamples_ = 2;
  bool enableSpatialHash_ = true;
  float sp_ = 4.0f;
  float smin_ = 0.15f;
  int maxSamples_ = 192;
  bool enableFiltering_ = true;
  glm::vec3 lightDir_ = glm::normalize(vec3(0.032f, 0.835f, 0.549f));
  bool resetHash_ = false;

  lvk::Holder<lvk::BufferHandle> vertexBuffer_;
  lvk::Holder<lvk::BufferHandle> indexBuffer_;
  lvk::Holder<lvk::BufferHandle> materials_;
  lvk::Holder<lvk::BufferHandle> instances_;
  lvk::Holder<lvk::BufferHandle> perFrame_[kRing];
  lvk::Holder<lvk::BufferHandle> hashHi_;
  lvk::Holder<lvk::BufferHandle> hashLo_;
  lvk::Holder<lvk::AccelStructHandle> blas_;
  lvk::Holder<lvk::AccelStructHandle> tlas_;
  lvk::Holder<lvk::TextureHandle> storageImage_;
  lvk::Holder<lvk::SamplerHandle> sampler_;
  lvk::Holder<lvk::ComputePipelineHandle> pipeline_;
  lvk::Holder<lvk::RenderPipelineHandle> present_;
  std::unique_ptr<lvk::metal::ImGuiRenderer> imgui_;
};

DESKTOP_MAIN(RTXAmbientOcclusion)
