struct PushConstants {
  float4 col0;
  float4 col1;
  float4 col2;
};

[[vk::push_constant]] PushConstants pc;

struct VSOut {
  float4 sv_Position : SV_Position;
  float3 color : COLOR0;
};

static const float2 pos[3] = {float2(-0.6, -0.4), float2(0.6, -0.4), float2(0.0, 0.6)};

[shader("vertex")]
VSOut vertexMain(uint vid : SV_VertexID) {
  const float3 col[3] = {pc.col0.rgb, pc.col1.rgb, pc.col2.rgb};
  VSOut o;
  o.sv_Position = float4(pos[vid], 0.0, 1.0);
  o.color = col[vid];
  return o;
}

[shader("fragment")]
float4 fragmentMain(VSOut input) : SV_Target {
  return float4(input.color, 1.0);
}
