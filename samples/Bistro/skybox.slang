import lvk_metal_bindless;

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float4x4 light;
  uint texSkyboxRadiance;
  uint texSkyboxIrradiance;
  uint texShadow;
  uint sampler0;
  uint samplerShadow0;
  uint drawNormals;
};

struct PC {
  PerFrame* perFrame;
};

[[vk::push_constant]] PC pc;

struct V2F {
  float4 position : SV_Position;
  float3 dir : TEXCOORD0;
};

static const float3 positions[8] = {
  float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
  float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0)
};
static const int indices[36] = {0,1,2, 2,3,0, 1,5,6, 6,2,1, 7,6,5, 5,4,7, 4,0,3, 3,7,4, 4,5,1, 1,0,4, 3,2,6, 6,7,3};

[shader("vertex")]
V2F skyVertexMain(uint vid : SV_VertexID) {
  float4x4 view = pc.perFrame.view;
  view[3] = float4(0.0, 0.0, 0.0, 1.0);
  const float3 pos = positions[indices[vid]];
  V2F o;
  o.position = (pc.perFrame.proj * view * float4(pos, 1.0)).xyww;
  o.dir = pos;
  return o;
}

[shader("fragment")]
float4 skyFragmentMain(V2F input) : SV_Target {
  return textureBindlessCube(pc.perFrame.texSkyboxRadiance, pc.perFrame.sampler0, input.dir);
}
