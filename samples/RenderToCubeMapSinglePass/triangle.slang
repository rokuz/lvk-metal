struct TriPush {
  float time;
};

[[vk::push_constant]] TriPush pc;

struct FragmentOutput {
  float4 color0 : SV_Target0;
  float4 color1 : SV_Target1;
  float4 color2 : SV_Target2;
  float4 color3 : SV_Target3;
  float4 color4 : SV_Target4;
  float4 color5 : SV_Target5;
};

[shader("vertex")]
float4 triVertexMain(uint vid : SV_VertexID) : SV_Position {
  const float2 p[3] = {float2(-0.6, -0.6), float2(0.6, -0.6), float2(0.0, 0.6)};
  return float4(p[vid] * (1.5 + sin(pc.time)) * 0.5, 0.0, 1.0);
}

[shader("fragment")]
FragmentOutput triFragmentMain() {
  FragmentOutput o;
  o.color0 = float4(1.0, 0.0, 0.0, 1.0);
  o.color1 = float4(0.0, 1.0, 0.0, 1.0);
  o.color2 = float4(0.0, 0.0, 1.0, 1.0);
  o.color3 = float4(1.0, 0.0, 1.0, 1.0);
  o.color4 = float4(1.0, 1.0, 0.0, 1.0);
  o.color5 = float4(0.0, 1.0, 1.0, 1.0);
  return o;
}
