struct RowPC {
  uint count;
  float rowY;
};

[[vk::push_constant]] RowPC pc;

struct V2F {
  float4 position : SV_Position;
  float4 color : COLOR0;
};

float3 hue(float h) {
  return clamp(abs(frac(h + float3(0.0, 0.6667, 0.3333)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

V2F placeTriangle(uint vid, uint index, uint count, float rowY) {
  const float2 tri[3] = {float2(0.0, 0.16), float2(-0.11, -0.16), float2(0.11, -0.16)};
  const float x = (float(index) + 0.5) / float(count) * 2.0 - 1.0;
  V2F o;
  o.position = float4(float2(x, rowY) + tri[vid], 0.0, 1.0);
  o.color = float4(hue(float(index) / float(count)), 1.0);
  return o;
}

[shader("vertex")]
V2F drawVertexMain(uint vid: SV_VertexID, uint drawID: SV_InstanceID) {
  return placeTriangle(vid, drawID, pc.count, pc.rowY);
}

[shader("fragment")]
float4 drawFragmentMain(V2F input) : SV_Target {
  return input.color;
}

[shader("fragment")]
float4 drawFragmentGray(V2F input) : SV_Target {
  const float g = dot(input.color.rgb, float3(0.299, 0.587, 0.114));
  return float4(g, g, g, 1.0);
}
