import lvk_metal_bindless;

struct PC {
  uint texture;
  uint sampler;
};

[[vk::push_constant]] PC pc;

struct V2F {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
};

[shader("vertex")]
V2F fullscreenVertexMain(uint vid : SV_VertexID) {
  V2F o;
  o.uv = float2((vid << 1) & 2, vid & 2);
  o.position = float4(o.uv * float2(2, -2) + float2(-1, 1), 0.0, 1.0);
  return o;
}

[shader("fragment")]
float4 fullscreenFragmentMain(V2F input) : SV_Target {
  return textureBindless2D(pc.texture, pc.sampler, input.uv);
}
