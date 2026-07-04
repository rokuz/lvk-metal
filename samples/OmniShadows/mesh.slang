import lvk_metal_bindless;

struct Vertex {
  float x, y, z;
  float u, v;
  float nx, ny, nz;
};

struct MainPC {
  float4x4 viewProj;
  float4 lightPos;
  Vertex* vb;
  float shadowFar;
  uint shadowMap;
  uint sampler;
};

[[vk::push_constant]] MainPC pc;

struct VertexStageOutput {
  float4 sv_Position : SV_Position;
  float3 worldPos;
  float3 color;
  float3 normal;
};

[shader("vertex")]
VertexStageOutput vertexMain(uint vertexID : SV_VertexID) {
  const Vertex v = pc.vb[vertexID];
  VertexStageOutput out;
  out.worldPos = float3(v.x, v.y, v.z);
  out.sv_Position = pc.viewProj * float4(out.worldPos, 1.0);
  out.color = out.worldPos * 0.03 + 0.6;
  out.normal = normalize(float3(v.nx, v.ny, v.nz));
  return out;
}

[shader("fragment")]
float4 fragmentMain(VertexStageOutput input) : SV_Target {
  const float3 fragToLight = input.worldPos - pc.lightPos.xyz;
  const float NdotL = max(dot(normalize(input.normal), normalize(-fragToLight)), 0.0);

  const float dist = length(fragToLight);
  const float k = dist * 0.0015;
  const float3 base = float3(fragToLight.x, -fragToLight.y, fragToLight.z);
  const float3 offsets[9] = {
      float3(0, 0, 0),
      float3(1, 1, 1),   float3(1, 1, -1),  float3(1, -1, 1),  float3(1, -1, -1),
      float3(-1, 1, 1),  float3(-1, 1, -1), float3(-1, -1, 1), float3(-1, -1, -1),
  };
  const float curD = dist - 0.1;
  float factor = 0.0;
  [unroll] for (int i = 0; i < 9; ++i) {
    const float d = textureBindlessCube(pc.shadowMap, pc.sampler, base + k * offsets[i]).r;
    factor += curD > pc.shadowFar * d ? 0.0 : 1.0;
  }
  factor /= 9.0;

  const float att = max(1.0 - (dist / 50.0) * (dist / 50.0), 0.0);
  const float3 finalColor = input.color * NdotL * factor * att;
  return float4(max(finalColor, input.color * 0.3), 1.0);
}
