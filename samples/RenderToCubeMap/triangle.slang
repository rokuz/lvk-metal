struct TriPush {
  uint face;
  float time;
};

[[vk::push_constant]] TriPush pc;

struct TriOut {
  float4 position : SV_Position;
  float3 color : COLOR;
};

[shader("vertex")]
TriOut triVertexMain(uint vid : SV_VertexID) {
  const float2 p[3] = {float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.0, 0.6)};
  const float3 c[6] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0),
                       float3(1.0, 0.0, 1.0), float3(1.0, 1.0, 0.0), float3(0.0, 1.0, 1.0)};
  TriOut o;
  o.position = float4(p[vid] * (1.5 + sin(pc.time)) * 0.5, 0.0, 1.0);
  o.color = c[pc.face];
  return o;
}

[shader("fragment")]
float4 triFragmentMain(TriOut input) : SV_Target {
  return float4(input.color, 1.0);
}
