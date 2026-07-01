struct QuadOut {
  float4 sv_Position : SV_Position;
  float2 uv : TEXCOORD0;
};

static const float2 pos[4] = {float2(-1.0, -1.0), float2(-1.0, 1.0), float2(1.0, -1.0), float2(1.0, 1.0)};

[shader("vertex")]
QuadOut vertexMain(uint vid : SV_VertexID) {
  QuadOut o;
  o.sv_Position = float4(pos[vid], 0.0, 1.0);
  o.uv = pos[vid] * 0.5 + 0.5;
  return o;
}

[shader("fragment")]
float4 fragmentMain(QuadOut input) : SV_Target {
  return float4(input.uv, 0.5, 1.0);
}
