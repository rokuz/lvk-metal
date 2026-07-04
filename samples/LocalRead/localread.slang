import lvk_metal_bindless;

#ifndef LVK_INPUT_ATTACHMENT_FIRST_INDEX
#define LVK_INPUT_ATTACHMENT_FIRST_INDEX 1
#endif

struct PerFrame {
  float4x4 mvp;
  float4x4 model;
  float4 cameraPos;
  uint texAlbedo;
  uint texNormal;
  uint texHeight;
  uint sampler;
};

struct DeferredPC {
  PerFrame* perFrame;
};

[[vk::push_constant]] DeferredPC pc;

static const float3 kPositions[24] = {
  float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1),
  float3(1, -1, -1),  float3(-1, -1, -1), float3(-1, 1, -1), float3(1, 1, -1),
  float3(1, -1, 1),   float3(1, -1, -1), float3(1, 1, -1),  float3(1, 1, 1),
  float3(-1, -1, -1), float3(-1, -1, 1), float3(-1, 1, 1),  float3(-1, 1, -1),
  float3(-1, 1, 1),   float3(1, 1, 1),   float3(1, 1, -1),  float3(-1, 1, -1),
  float3(-1, -1, -1), float3(1, -1, -1), float3(1, -1, 1),  float3(-1, -1, 1),
};

static const float3 kNormals[24] = {
  float3(0, 0, 1),  float3(0, 0, 1),  float3(0, 0, 1),  float3(0, 0, 1),
  float3(0, 0, -1), float3(0, 0, -1), float3(0, 0, -1), float3(0, 0, -1),
  float3(1, 0, 0),  float3(1, 0, 0),  float3(1, 0, 0),  float3(1, 0, 0),
  float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0), float3(-1, 0, 0),
  float3(0, 1, 0),  float3(0, 1, 0),  float3(0, 1, 0),  float3(0, 1, 0),
  float3(0, -1, 0), float3(0, -1, 0), float3(0, -1, 0), float3(0, -1, 0),
};

static const float2 kUVs[24] = {
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
  float2(0, 1), float2(1, 1), float2(1, 0), float2(0, 0),
};

struct DeferredOut {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float3 normal : NORMAL;
  float3 worldPos : TEXCOORD1;
  float3 eyeDir : TEXCOORD2;
};

[shader("vertex")]
DeferredOut deferredVertexMain(uint vid : SV_VertexID) {
  const float3 p = kPositions[vid];
  const float3 wp = (pc.perFrame.model * float4(p, 1.0)).xyz;
  DeferredOut o;
  o.position = pc.perFrame.mvp * float4(p, 1.0);
  o.uv = kUVs[vid];
  o.normal = (pc.perFrame.model * float4(kNormals[vid], 0.0)).xyz;
  o.worldPos = wp;
  o.eyeDir = pc.perFrame.cameraPos.xyz - wp;
  return o;
}

struct GBufferOut {
  float4 albedo : SV_Target1;
  float4 normal : SV_Target2;
  float4 worldPos : SV_Target3;
};

static const float kHeightScale = 0.03;
static const int kParallaxSteps = 8;

float3x3 cotangentFrame(float3 N, float3 p, float2 uv) {
  const float3 dp1 = ddx(p);
  const float3 dp2 = ddy(p);
  const float2 duv1 = ddx(uv);
  const float2 duv2 = ddy(uv);
  const float3 dp2perp = cross(dp2, N);
  const float3 dp1perp = cross(N, dp1);
  const float3 T = normalize(dp2perp * duv1.x + dp1perp * duv2.x);
  const float3 B = normalize(dp2perp * duv1.y + dp1perp * duv2.y);
  return float3x3(T, B, N);
}

float2 parallaxUV(float2 uv, float3 viewTS, uint heightId, uint samplerId) {
  const float layerStep = 1.0 / float(kParallaxSteps);
  const float invViewZ = 1.0 / max(viewTS.z, 0.01);
  const float2 deltaUV = viewTS.xy * (invViewZ * kHeightScale * layerStep);

  float2 cur = uv;
  float curLayer = 0.0;
  float curDepth = 1.0 - textureBindless2DLod(heightId, samplerId, cur, 0.0).r;
  float2 prevCur = cur;
  float prevLayer = curLayer;
  float prevDepth = curDepth;

  [loop] for (int i = 0; i < kParallaxSteps && curLayer < curDepth; ++i) {
    prevCur = cur; prevLayer = curLayer; prevDepth = curDepth;
    cur -= deltaUV; curLayer += layerStep;
    curDepth = 1.0 - textureBindless2DLod(heightId, samplerId, cur, 0.0).r;
  }

  [unroll] for (int k = 0; k < 3; ++k) {
    const float2 midCur = 0.5 * (prevCur + cur);
    const float midLayer = 0.5 * (prevLayer + curLayer);
    const float midDepth = 1.0 - textureBindless2DLod(heightId, samplerId, midCur, 0.0).r;
    const bool advance = midLayer < midDepth;
    prevCur = advance ? midCur : prevCur;
    prevLayer = advance ? midLayer : prevLayer;
    prevDepth = advance ? midDepth : prevDepth;
    cur = advance ? cur : midCur;
    curLayer = advance ? curLayer : midLayer;
    curDepth = advance ? curDepth : midDepth;
  }

  const float after = curDepth - curLayer;
  const float before = prevDepth - prevLayer;
  const float denom = after - before;
  const float weight = abs(denom) < 1e-6 ? 0.0 : clamp(after / denom, 0.0, 1.0);
  return lerp(cur, prevCur, weight);
}

[shader("fragment")]
GBufferOut deferredFragmentMain(DeferredOut input) {
  const float3 N = normalize(input.normal);
  const float3 V = normalize(input.eyeDir);
  const float3x3 TBN = cotangentFrame(N, input.worldPos, input.uv);
  const float3 viewTS = mul(TBN, V);
  const float2 uv = parallaxUV(input.uv, viewTS, pc.perFrame.texHeight, pc.perFrame.sampler);

  const float3 nm = textureBindless2D(pc.perFrame.texNormal, pc.perFrame.sampler, uv).xyz * 2.0 - 1.0;
  const float3 n = normalize(mul(nm, TBN));

  GBufferOut o;
  o.albedo = 10.0 * textureBindless2D(pc.perFrame.texAlbedo, pc.perFrame.sampler, uv);
  o.normal = float4(n * 0.5 + 0.5, 1.0);
  o.worldPos = float4(input.worldPos, 1.0);
  return o;
}

struct ComposeOut {
  float4 position : SV_Position;
};

[shader("vertex")]
ComposeOut composeVertexMain(uint vid : SV_VertexID) {
  const float2 uv = float2((vid << 1) & 2, vid & 2);
  ComposeOut o;
  o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}

[[vk::input_attachment_index(LVK_INPUT_ATTACHMENT_FIRST_INDEX)]] SubpassInput inputAlbedo;
[[vk::input_attachment_index(LVK_INPUT_ATTACHMENT_FIRST_INDEX + 1)]] SubpassInput inputNormal;
[[vk::input_attachment_index(LVK_INPUT_ATTACHMENT_FIRST_INDEX + 2)]] SubpassInput inputWorldPos;

[shader("fragment")]
float4 composeFragmentMain(ComposeOut input) : SV_Target0 {
  const float3 albedo = inputAlbedo.SubpassLoad().rgb;
  const float3 N = normalize(inputNormal.SubpassLoad().xyz * 2.0 - 1.0);
  const float3 worldPos = inputWorldPos.SubpassLoad().xyz;
  const float3 L = normalize(float3(1.0, 1.0, -1.0) - worldPos);
  const float NdotL = clamp(dot(N, L), 0.3, 1.0);
  return float4(NdotL * albedo, 1.0);
}
