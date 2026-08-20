import glsl;
import lvk_metal_bindless;

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float time;
};

struct PC {
  PerFrame* perFrame;
  uint texCube;
  uint sampler;
};

[[vk::push_constant]] PC pc;

struct V2F {
  float4 position : SV_Position;
  float3 dir : TEXCOORD0;
};

static const float3 kPos[8] = {
  float3(-1.0, -1.0,  1.0),
  float3( 1.0, -1.0,  1.0),
  float3( 1.0,  1.0,  1.0),
  float3(-1.0,  1.0,  1.0),
  float3(-1.0, -1.0, -1.0),
  float3( 1.0, -1.0, -1.0),
  float3( 1.0,  1.0, -1.0),
  float3(-1.0,  1.0, -1.0)
};

static const int kIndices[36] = {
  0, 1, 2, 2, 3, 0,
  1, 5, 6, 6, 2, 1,
  7, 6, 5, 5, 4, 7,
  4, 0, 3, 3, 7, 4,
  4, 5, 1, 1, 0, 4,
  3, 2, 6, 6, 7, 3
};

float4x4 fromMat3(float3x3 m) {
  return float4x4(float4(m[0], 0.0), float4(m[1], 0.0), float4(m[2], 0.0), float4(0.0, 0.0, 0.0, 1.0));
}

[shader("vertex")]
V2F skyVertexMain(uint vid : SV_VertexID) {
  int idx = kIndices[vid];
  V2F o;
  o.position = pc.perFrame.proj * fromMat3((float3x3)pc.perFrame.view) * float4(50.0 * kPos[idx], 1.0);
  o.dir = kPos[idx].zxy;
  return o;
}

[shader("fragment")]
float4 skyFragmentMain(V2F input) : SV_Target {
  return pow(textureBindlessCube(pc.texCube, pc.sampler, input.dir), float4(1.5, 1.5, 1.5, 1.5));
}
