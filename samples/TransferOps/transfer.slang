import lvk_metal_bindless;

struct PC {
  float4* tints;
  float4* copyTint;
  float4* fillBrightness;
  uint texCopied;
  uint texCleared;
  uint smp;
};

[[vk::push_constant]] PC pc;

struct V2F {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float4 tint : TEXCOORD1;
};

[shader("vertex")]
V2F transferVertexMain(uint vid : SV_VertexID) {
  const float2 p[6] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, -1), float2(1, 1), float2(-1, 1)};
  const float2 uvs[6] = {float2(0, 1), float2(1, 1), float2(0, 0), float2(1, 1), float2(1, 0), float2(0, 0)};
  V2F o;
  o.position = float4(p[vid], 0.0, 1.0);
  o.uv = uvs[vid];
  o.tint = pc.tints[vid];
  return o;
}

[shader("fragment")]
float4 transferFragmentMain(V2F input) : SV_Target {
  const float4 texColor =
      input.uv.x < 0.5 ? textureBindless2D(pc.texCopied, pc.smp, input.uv * float2(2.0, 1.0))
                       : textureBindless2D(pc.texCleared, pc.smp, (input.uv - float2(0.5, 0.0)) * float2(2.0, 1.0));
  const float3 copyTint = pc.copyTint[0].rgb;
  const float brightness = pc.fillBrightness[0].r;
  return float4(input.tint.rgb * texColor.rgb * copyTint * brightness, 1.0);
}
