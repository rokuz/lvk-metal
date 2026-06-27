import lvk_metal_bindless;

struct SkyPush {
  float4x4 vp;
  uint texture0;
};

[[vk::push_constant]] SkyPush pc;

struct SkyOut {
  float4 position : SV_Position;
  float3 dir : DIR;
};

static const float3 kVerts[8] = {
  float3(-1.0, -1.0,  1.0), float3( 1.0, -1.0,  1.0), float3( 1.0,  1.0,  1.0), float3(-1.0,  1.0,  1.0),
  float3(-1.0, -1.0, -1.0), float3( 1.0, -1.0, -1.0), float3( 1.0,  1.0, -1.0), float3(-1.0,  1.0, -1.0)
};
static const uint kIdx[36] = {0,1,2, 2,3,0, 1,5,6, 6,2,1, 7,6,5, 5,4,7, 4,0,3, 3,7,4, 4,5,1, 1,0,4, 3,2,6, 6,7,3};

[shader("vertex")]
SkyOut skyVertexMain(uint vid : SV_VertexID) {
  const float3 pos = kVerts[kIdx[vid]];
  SkyOut o;
  o.position = (pc.vp * float4(pos, 1.0)).xyww;
  o.dir = pos;
  return o;
}

[shader("fragment")]
float4 skyFragmentMain(SkyOut input) : SV_Target {
  const float3 d = normalize(input.dir);
  const float2 uv = float2(atan2(d.z, d.x) / (2.0 * 3.14159265) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / 3.14159265);
  return textureBindless2D(pc.texture0, 0, uv);
}
