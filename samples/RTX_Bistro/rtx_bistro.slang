import lvk_metal_bindless;

struct Vertex {
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
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct PC {
  float4 lightDir;
  PerFrame* perFrame;
  Material* materials;
  uint* indices;
  Vertex* vertices;
  uint outImage;
  uint tlas;
  uint enableShadows;
};

[[vk::push_constant]] PC pc;

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

[shader("compute")]
[numthreads(8, 8, 1)]
void traceRays(uint2 tid : SV_DispatchThreadID) {
  uint width;
  uint height;
  kImages2D.data[pc.outImage].GetDimensions(width, height);
  if (tid.x >= width || tid.y >= height)
    return;

  const float2 pixelCenter = float2(tid) + float2(0.5);
  float2 d = 2.0 * (pixelCenter / float2(width, height)) - 1.0;
  d.y = -d.y;

  const float4 origin = pc.perFrame.viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.perFrame.projInverse * float4(d, 1, 1);
  const float4 direction = pc.perFrame.viewInverse * float4(normalize(targetClip.xyz), 0);

  RayDesc ray;
  ray.Origin = origin.xyz;
  ray.Direction = direction.xyz;
  ray.TMin = 0.01;
  ray.TMax = 1000.0;

  RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
  q.TraceRayInline(kTLAS.data[pc.tlas], RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
  while (q.Proceed()) {}

  float4 color;
  if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    color = float4(0.1, 0.1, 1.0, 1.0);
  } else {
    const uint index = 3 * q.CommittedPrimitiveIndex();
    const uint3 ti = uint3(pc.indices[index + 0], pc.indices[index + 1], pc.indices[index + 2]);
    const float2 bc = q.CommittedTriangleBarycentrics();
    const float3 bary = float3(1.0 - bc.x - bc.y, bc.x, bc.y);
    const float3 n0 = unpackOctahedral16(uint(pc.vertices[ti.x].normal));
    const float3 n1 = unpackOctahedral16(uint(pc.vertices[ti.y].normal));
    const float3 n2 = unpackOctahedral16(uint(pc.vertices[ti.z].normal));
    const float3 normal = normalize(n0 * bary.x + n1 * bary.y + n2 * bary.z);
    const Material mat = pc.materials[uint(pc.vertices[ti.x].mtlIndex)];

    bool shadowed = false;
    if (pc.enableShadows != 0) {
      const float3 hitPoint = ray.Origin + ray.Direction * q.CommittedRayT();
      RayDesc sr;
      sr.Origin = hitPoint;
      sr.Direction = pc.lightDir.xyz;
      sr.TMin = 0.01;
      sr.TMax = 1000.0;
      RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> sq;
      sq.TraceRayInline(kTLAS.data[pc.tlas], RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, sr);
      sq.Proceed();
      shadowed = sq.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    }

    const float occlusion = shadowed ? 0.5 : 1.0;
    const float NdotL1 = clamp(dot(normal, normalize(float3(-1, 1, 1))), 0.0, 1.0);
    const float NdotL2 = clamp(dot(normal, normalize(float3(-1, 1, -1))), 0.0, 1.0);
    const float NdotL = 0.5 * (NdotL1 + NdotL2);
    color = float4(mat.diffuse.rgb * occlusion * max(NdotL, 0.0), mat.diffuse.a);
  }

  kImages2D.data[pc.outImage][tid] = color;
}
