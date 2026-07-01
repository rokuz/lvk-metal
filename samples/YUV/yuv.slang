import lvk_metal_bindless;

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
  o.uv.y = 1.0 - o.uv.y;
  return o;
}

struct YUVPush {
  uint texId;
  uint smpId;
};

[[vk::push_constant]] YUVPush pc;

[shader("fragment")]
float4 fragmentMain(QuadOut input) : SV_Target {
  return textureBindlessYUV(pc.texId, pc.smpId, input.uv);
}
