import lvk_metal_bindless;

struct VertexData {
  float px, py, pz;
  uint uv;
  uint16_t normal;
  uint16_t mtlIndex;
};

struct Material {
  float4 ambient;
  float4 diffuse;
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct PC {
  float4 lightDir;
  PerFrame* perFrame;
  Material* materials;
  uint* indices;
  VertexData* vertices;
  Atomic<uint>* hashHi;
  Atomic<uint>* hashLo;
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

[[vk::push_constant]] PC pc;

[ForceInline]
void deviceFence() {
  __target_switch {
  case metal:
    __intrinsic_asm "atomic_thread_fence(metal::mem_flags::mem_device, metal::memory_order_seq_cst)";
  }
}

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
static void computeTBN(float3 n, out float3 x, out float3 y) {
  float yz = -n.y * n.z;
  y = normalize((abs(n.z) > 0.9999) ? float3(-n.x * n.y, 1.0 - n.y * n.y, yz) : float3(-n.x * n.z, yz, 1.0 - n.z * n.z));
  x = cross(y, n);
}
static uint lcg(inout uint p) { p = 1664525u * p + 1013904223u; return p & 0x00FFFFFFu; }
static float rnd(inout uint s) { return float(lcg(s)) / float(0x01000000); }
static uint tea(uint v0, uint v1) {
  uint s0 = 0;
  for (uint n = 0; n < 16; n++) {
    s0 += 0x9e3779b9u;
    v0 += ((v1 << 4) + 0xa341316cu) ^ (v1 + s0) ^ ((v1 >> 5) + 0xc8013ea4u);
    v1 += ((v0 << 4) + 0xad90777du) ^ (v0 + s0) ^ ((v0 >> 5) + 0x7e95761eu);
  }
  return v0;
}
static float3 sampleCosineHemisphere(inout uint seed, float3 t, float3 b, float3 n) {
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

static uint hLoad(Atomic<uint>* a, uint i) { return a[i].load(); }
static void hStore(Atomic<uint>* a, uint i, uint v) { a[i].store(v); }
static uint hAdd(Atomic<uint>* a, uint i, uint v) { return a[i].add(v); }
static uint hCAS(Atomic<uint>* a, uint i, uint cmp, uint val) { return a[i].compareExchange(cmp, val); }

static uint readLoSeq(uint cell, uint checksum) {
  uint v1 = hLoad(pc.hashLo, cell) >> 24u;
  if ((v1 & 1u) != 0u) return 0u;
  deviceFence();
  uint cs = hLoad(pc.hashHi, cell) & 0xFFFFFFu;
  deviceFence();
  uint lo2 = hLoad(pc.hashLo, cell);
  if (v1 != (lo2 >> 24u) || cs != checksum) return 0u;
  return hLoad(pc.hashLo, cell) & 0xFFFFFFu;
}

static bool traceAO(float3 origin, float3 dir, float tmax) {
  RayDesc r;
  r.Origin = origin;
  r.Direction = dir;
  r.TMin = 0.0;
  r.TMax = tmax;
  RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
  q.TraceRayInline(kTLAS.data[pc.tlas], RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, r);
  q.Proceed();
  return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

static void bumpFrame(uint cell, uint frameLow) {
  uint cur = hLoad(pc.hashHi, cell);
  for (uint a = 0; a < 4u; a++) {
    if ((cur >> 24u) == frameLow) return;
    uint fresh = (cur & 0xFFFFFFu) | (frameLow << 24u);
    uint prev = hCAS(pc.hashHi, cell, cur, fresh);
    if (prev == cur) return;
    cur = prev;
  }
}
static void halveLo(uint cell, uint cur) {
  for (uint a = 0; a < 4u; a++) {
    uint s = cur & 0xFFFu;
    uint h = min((cur >> 12u) & 0xFFFu, s);
    uint newLo = (cur & 0xFF000000u) | ((h >> 1u) << 12u) | (s >> 1u);
    uint prev = hCAS(pc.hashLo, cell, cur, newLo);
    if (prev == cur) return;
    cur = prev;
  }
}
static void undoAdd(uint cell, uint cur, uint hit) {
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
static void accumulateAndMaybeHalve(uint cell, uint hit) {
  uint addend = (hit << 12u) + 1u;
  uint prev = hAdd(pc.hashLo, cell, addend);
  uint prevSamples = prev & 0xFFFu;
  if (prevSamples >= pc.maxSamples) { undoAdd(cell, prev + addend, hit); return; }
  if (prevSamples + 1u == pc.maxSamples) halveLo(cell, prev + addend);
}
static bool isCellReady(uint lo) { return (lo & 0xFFFu) >= 4u; }
static float cellVisibility(uint lo) {
  uint samples = lo & 0xFFFu;
  uint hits = min((lo >> 12u) & 0xFFFu, samples);
  if (samples < 4u) return -1.0;
  return float(samples - hits) / float(samples);
}
static void computeBucket(float3 pos, float cs, uint nhPCG, uint nhXXH, out uint baseCell, out uint checksum) {
  int3 p = int3(floor(pos / cs));
  uint csu = uint(cs * 10000.0);
  uint key = pcg(csu + pcg(uint(p.x) + pcg(uint(p.y) + pcg(uint(p.z) + nhPCG))));
  baseCell = (key & ((pc.hashMapSize >> 2u) - 1u)) << 2u;
  checksum = max(xxhash32(csu + xxhash32(uint(p.x) + xxhash32(uint(p.y) + xxhash32(uint(p.z) + nhXXH)))) & 0xFFFFFFu, 1u);
}
static uint spatialHashFindOrInsert(float3 pos, float cs, uint nhPCG, uint nhXXH, out uint outChecksum) {
  uint baseCell, checksum;
  computeBucket(pos, cs, nhPCG, nhXXH, baseCell, checksum);
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
    if ((v & 1u) != 0u) continue;
    if (hCAS(pc.hashLo, cell, lo, (((v + 1u) & 0xFFu) << 24u) | (lo & 0xFFFFFFu)) != lo) continue;
    deviceFence();
    hStore(pc.hashHi, cell, (frameLow << 24u) | checksum);
    deviceFence();
    hStore(pc.hashLo, cell, ((v + 2u) & 0xFFu) << 24u);
    return cell;
  }
  return 0xFFFFFFFFu;
}
static uint spatialHashFind(float3 pos, float cs, uint nhPCG, uint nhXXH, out uint outChecksum) {
  uint baseCell, checksum;
  computeBucket(pos, cs, nhPCG, nhXXH, baseCell, checksum);
  outChecksum = checksum;
  for (uint i = 0; i < 4u; i++) {
    uint stored = hLoad(pc.hashHi, baseCell + i) & 0xFFFFFFu;
    if (stored == checksum) return baseCell + i;
    if (stored == 0u) return 0xFFFFFFFFu;
  }
  return 0xFFFFFFFFu;
}
static float computeCellSize(float3 worldPos) {
  float3 camPos = (pc.perFrame.viewInverse * float4(0, 0, 0, 1)).xyz;
  float dist = distance(camPos, worldPos);
  float h = dist / pc.perFrame.proj[1][1];
  float sw = pc.sp * (h * 2.0) / pc.resolutionY;
  return exp2(floor(log2(max(sw / pc.smin, 1.0)))) * pc.smin;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void traceRays(uint2 tid : SV_DispatchThreadID) {
  uint width;
  uint height;
  kImages2D.data[pc.outImage].GetDimensions(width, height);
  if (tid.x >= width || tid.y >= height)
    return;

  const float2 pixelCenter = float2(tid) + float2(0.5);
  float2 dxy = 2.0 * (pixelCenter / float2(width, height)) - 1.0;
  dxy.y = -dxy.y;
  const float4 rayOrigin = pc.perFrame.viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.perFrame.projInverse * float4(dxy, 1, 1);
  const float4 rayDir = pc.perFrame.viewInverse * float4(normalize(targetClip.xyz), 0);

  RayDesc pr;
  pr.Origin = rayOrigin.xyz;
  pr.Direction = rayDir.xyz;
  pr.TMin = 0.01;
  pr.TMax = 1000.0;
  RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
  q.TraceRayInline(kTLAS.data[pc.tlas], RAY_FLAG_FORCE_OPAQUE, 0xFF, pr);
  while (q.Proceed()) {}

  float4 color = float4(0.4, 0.6, 0.9, 1.0);
  if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    const uint index = 3u * q.CommittedPrimitiveIndex();
    const uint3 ti = uint3(pc.indices[index + 0], pc.indices[index + 1], pc.indices[index + 2]);
    const float2 bc = q.CommittedTriangleBarycentrics();
    const float3 bary = float3(1.0 - bc.x - bc.y, bc.x, bc.y);
    const float3 n0 = unpackOctahedral16(uint(pc.vertices[ti.x].normal));
    const float3 n1 = unpackOctahedral16(uint(pc.vertices[ti.y].normal));
    const float3 n2 = unpackOctahedral16(uint(pc.vertices[ti.z].normal));
    const float3 n = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    const Material mat = pc.materials[uint(pc.vertices[ti.x].mtlIndex)];
    const float3 worldPos = pr.Origin + pr.Direction * q.CommittedRayT();
    const float3 origin = worldPos + n * 0.001;

    float occlusion = 1.0;
    if (pc.enableAO != 0) {
      float3 tangent, bitangent;
      computeTBN(n, tangent, bitangent);
      uint seed = tea(uint(tid.y * 4003u + tid.x), pc.timeVaryingNoise != 0u ? pc.frameId : 0u);

      if (pc.enableSpatialHash != 0) {
        const float swd = computeCellSize(worldPos);
        const int3 nn = int3(floor(n * 3.0));
        const uint nhPCG = pcg(uint(nn.x) + pcg(uint(nn.y) + pcg(uint(nn.z))));
        const uint nhXXH = xxhash32(uint(nn.x) + xxhash32(uint(nn.y) + xxhash32(uint(nn.z))));
        const uint frameLow = pc.frameId & 0xFFu;

        uint cs0;
        const uint cell0 = spatialHashFindOrInsert(worldPos, swd, nhPCG, nhXXH, cs0);
        const uint data0 = (cell0 != 0xFFFFFFFFu) ? readLoSeq(cell0, cs0) : 0u;
        const uint samples0 = data0 & 0xFFFu;
        const bool ready0 = isCellReady(data0);

        uint hit = 0u;
        const bool lod0NeedsRay = cell0 != 0xFFFFFFFFu && samples0 < pc.maxSamples;
        if (!ready0 || lod0NeedsRay) {
          const float3 dir = sampleCosineHemisphere(seed, tangent, bitangent, n);
          hit = traceAO(origin, dir, pc.aoRadius) ? 1u : 0u;
        }
        if (cell0 != 0xFFFFFFFFu) {
          if (lod0NeedsRay) accumulateAndMaybeHalve(cell0, hit);
          bumpFrame(cell0, frameLow);
        }

        uint cachedData[4] = {data0, 0u, 0u, 0u};
        if (!ready0) {
          float lodSize = swd * 2.0;
          for (int lod = 1; lod < 4; lod++) {
            uint csl;
            const uint ci = spatialHashFindOrInsert(worldPos, lodSize, nhPCG, nhXXH, csl);
            if (ci != 0xFFFFFFFFu) {
              cachedData[lod] = readLoSeq(ci, csl);
              if ((cachedData[lod] & 0xFFFu) < pc.maxSamples) accumulateAndMaybeHalve(ci, hit);
              bumpFrame(ci, frameLow);
            }
            lodSize *= 2.0;
          }
        }

        if (ready0 && pc.enableFiltering != 0) {
          const float3 cellPos = worldPos / swd - 0.5;
          const int3 base = int3(floor(cellPos));
          const float3 f = frac(cellPos);
          float totalWeight = 0.0, totalAO = 0.0;
          for (int dz = 0; dz < 2; dz++)
            for (int dy = 0; dy < 2; dy++)
              for (int dx = 0; dx < 2; dx++) {
                const float3 neighborPos = (float3(base + int3(dx, dy, dz)) + 0.5) * swd;
                uint csf;
                const uint ci = spatialHashFind(neighborPos, swd, nhPCG, nhXXH, csf);
                if (ci != 0xFFFFFFFFu) {
                  const float vis = cellVisibility(readLoSeq(ci, csf));
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
          occl += traceAO(origin, dir, pc.aoRadius) ? 1.0 : 0.0;
        }
        occlusion = 1.0 - occl / float(pc.aoSamples);
      }
      occlusion = pow(clamp(occlusion, 0.0, 1.0), pc.aoPower);
    }

    if (pc.enableShadows != 0) {
      if (traceAO(origin, pc.lightDir.xyz, 1000.0)) occlusion *= 0.5;
    }

    const float NdotL1 = clamp(dot(n, normalize(float3(1, 1, 1))), 0.0, 1.0);
    const float NdotL2 = clamp(dot(n, normalize(float3(-1, 1, -1))), 0.0, 1.0);
    const float NdotL = NdotL1 + NdotL2;
    color = mat.ambient + mat.diffuse * NdotL * occlusion;
  }

  kImages2D.data[pc.outImage][tid] = color;
}
