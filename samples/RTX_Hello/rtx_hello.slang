import lvk_metal_bindless;

struct Camera {
  float4x4 viewInverse;
  float4x4 projInverse;
};

struct PC {
  Camera* cam;
  uint outImage;
  uint tlas;
  float time;
};

[[vk::push_constant]] PC pc;

static float fmod2(float x, float y) {
  return x - y * floor(x / y);
}

static float2 rotate2d(float2 v, float angle, float2 center) {
  float2x2 r = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
  return mul(v - center, r);
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
  if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
    const float2 b = q.CommittedTriangleBarycentrics();
    color = float3(1.0 - b.x - b.y, b.x, b.y);
  } else {
    const float2 uv = rotate2d(float2(tid), 0.2 * pc.time, 0.5 * float2(width, height));
    const float2 pos = floor(uv / 64.0);
    color = float3(fmod2(pos.x + fmod2(pos.y, 2.0), 2.0));
  }

  kImages2D.data[pc.outImage][tid] = float4(color, 1.0);
}
