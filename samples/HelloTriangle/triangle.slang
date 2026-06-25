struct VertexOut {
  float4 position : SV_Position;
  float4 color : COLOR;
};

static const float2 kPositions[3] = {float2(0.0, 0.6), float2(-0.6, -0.6), float2(0.6, -0.6)};
static const float3 kColors[3] = {float3(1.0, 0.0, 0.0), float3(0.0, 1.0, 0.0), float3(0.0, 0.0, 1.0)};

[shader("vertex")]
VertexOut vertexMain(uint vid : SV_VertexID) {
  VertexOut out;
  out.position = float4(kPositions[vid], 0.0, 1.0);
  out.color = float4(kColors[vid], 1.0);
  return out;
}

[shader("fragment")]
float4 fragmentMain(VertexOut input) : SV_Target {
  return input.color;
}
