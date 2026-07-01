import lvk_metal_bindless;

struct Camera {
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct Vertex {
  float px, py, pz;
  property float3 position { get { return float3(px, py, pz); } }
};

struct PC {
  Camera* cam;
  uint* indices;
  Vertex* vertices;
  uint outImage;
  uint texBackground;
  uint texObject;
  uint smp;
  uint tlas;
  float time;
};

[[vk::push_constant]] PC pc;

static float4 triplanar(uint tex, uint smp, float3 worldPos, float3 normal) {
  float3 weights = abs(normal);
  weights = pow(weights, float3(8.0));
  weights = weights / (weights.x + weights.y + weights.z);
  float4 cXY = textureBindless2DLod(tex, smp, worldPos.xy, 0);
  float4 cZY = textureBindless2DLod(tex, smp, worldPos.zy, 0);
  float4 cXZ = textureBindless2DLod(tex, smp, worldPos.xz, 0);
  return cXY * weights.z + cZY * weights.x + cXZ * weights.y;
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

  const float4 origin = pc.cam.viewInverse * float4(0, 0, 0, 1);
  const float4 targetClip = pc.cam.projInverse * float4(d, 1, 1);
  const float4 direction = pc.cam.viewInverse * float4(normalize(targetClip.xyz), 0);

  RayDesc ray;
  ray.Origin = origin.xyz;
  ray.Direction = direction.xyz;
  ray.TMin = 0.1;
  ray.TMax = 500.0;

  RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
  q.TraceRayInline(kTLAS.data[pc.tlas], RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
  while (q.Proceed()) {}

  float3 color;
  if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
    const float2 uv = float2(tid) / float2(width, height);
    color = textureBindless2DLod(pc.texBackground, pc.smp, uv, 0).rgb;
  } else {
    const uint pid = q.CommittedPrimitiveIndex();
    const uint i0 = pc.indices[pid * 3 + 0];
    const uint i1 = pc.indices[pid * 3 + 1];
    const uint i2 = pc.indices[pid * 3 + 2];
    const float3 p0 = pc.vertices[i0].position;
    const float3 p1 = pc.vertices[i1].position;
    const float3 p2 = pc.vertices[i2].position;
    const float2 b = q.CommittedTriangleBarycentrics();
    const float3 bary = float3(1.0 - b.x - b.y, b.x, b.y);
    const float3 pos = p0 * bary.x + p1 * bary.y + p2 * bary.z;

    if (q.CommittedInstanceIndex() == 0)
      color = triplanar(pc.texObject, pc.smp, pos, normalize(pos)).rgb;
    else
      color = bary;
  }

  kImages2D.data[pc.outImage][tid] = float4(color, 1.0);
}
