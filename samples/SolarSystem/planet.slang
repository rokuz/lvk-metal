import lvk_metal_bindless;

struct Vertex {
  float4 posU;
  float4 nrmV;
};

struct Mats {
  float4x4 mvp;
  float4x4 model;
};

struct PlanetPush {
  Mats* mats;
  Vertex* verts;
  uint texture0;
  uint emissive;
};

[[vk::push_constant]] PlanetPush pc;

struct PlanetOut {
  float4 position : SV_Position;
  float3 worldPos : WORLDPOS;
  float3 normal : NORMAL;
  float2 uv : UV;
};

[shader("vertex")]
PlanetOut planetVertexMain(uint vid : SV_VertexID) {
  Vertex v = pc.verts[vid];
  PlanetOut o;
  o.position = pc.mats.mvp * float4(v.posU.xyz, 1.0);
  o.worldPos = (pc.mats.model * float4(v.posU.xyz, 1.0)).xyz;
  o.normal = normalize((pc.mats.model * float4(v.nrmV.xyz, 0.0)).xyz);
  o.uv = float2(v.posU.w, v.nrmV.w);
  return o;
}

struct PlanetIn {
  float3 worldPos : WORLDPOS;
  float3 normal : NORMAL;
  float2 uv : UV;
};

[shader("fragment")]
float4 planetFragmentMain(PlanetIn input) : SV_Target {
  const float3 albedo = textureBindless2D(pc.texture0, 0, input.uv).rgb;
  if (pc.emissive != 0) {
    return float4(albedo, 1.0);
  }
  const float3 L = normalize(-input.worldPos);
  const float ndotl = max(dot(normalize(input.normal), L), 0.0);
  return float4(albedo * (0.04 + 0.96 * ndotl), 1.0);
}
