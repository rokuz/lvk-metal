import lvk_metal_bindless;

struct CubePush {
  float4x4 mvp;
  uint texture0;
};

[[vk::push_constant]] CubePush pc;

struct MeshOut {
  float4 position : SV_Position;
  float3 dir;
};

[shader("vertex")]
MeshOut meshVertexMain(uint vid : SV_VertexID) {
  const float3 v[8] = {float3(-1.0, -1.0, 1.0),  float3(1.0, -1.0, 1.0),  float3(1.0, 1.0, 1.0),  float3(-1.0, 1.0, 1.0),
                       float3(-1.0, -1.0, -1.0), float3(1.0, -1.0, -1.0), float3(1.0, 1.0, -1.0), float3(-1.0, 1.0, -1.0)};
  MeshOut o;
  o.position = pc.mvp * float4(v[vid], 1.0);
  o.dir = v[vid];
  return o;
}

[shader("fragment")]
float4 meshFragmentMain(MeshOut input) : SV_Target {
  return textureBindlessCube(pc.texture0, 0, normalize(input.dir));
}
