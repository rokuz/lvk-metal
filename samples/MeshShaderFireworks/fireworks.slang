import lvk_metal_bindless;

struct Vertex {
  float px, py, pz;
  float cr, cg, cb;
  float flare;

  property float3 position { get { return float3(px, py, pz); } }
  property float3 color { get { return float3(cr, cg, cb); } }
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
};

struct PushConstants {
  PerFrame* perFrame;
  Vertex* vb;
  uint texture;
  uint sampler;
};

[[vk::push_constant]] PushConstants pc;

struct MeshOutput {
  float4 position : SV_Position;
  float3 color : COLOR;
  float2 uv : TEXCOORD;
};

static const float2 kOffs[4] = {float2(-1, -1), float2(1, -1), float2(-1, 1), float2(1, 1)};

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void meshMain(uint3 groupID : SV_GroupID,
              uint3 groupThreadID : SV_GroupThreadID,
              OutputVertices<MeshOutput, 128> verts,
              OutputIndices<uint3, 64> triangles) {
  SetMeshOutputCounts(128, 64);

  const uint particleIdx = groupID.x * 32 + groupThreadID.x;
  const uint vertOff = groupThreadID.x * 4;
  const uint triOff = groupThreadID.x * 2;

  const float4x4 proj = pc.perFrame.proj;
  const float4x4 view = pc.perFrame.view;
  const Vertex v = pc.vb[particleIdx];
  const float4 center = view * float4(v.position, 1.0);

  const float2 size = v.flare > 0.5 ? float2(0.08, 0.4) : float2(0.2, 0.2);
  const float3 color = v.flare > 0.5 ? 0.5 * v.color : v.color;

  for (uint i = 0; i < 4; i++) {
    const float4 offset = float4(size * kOffs[i], 0, 0);
    MeshOutput o;
    o.position = proj * (center + offset);
    o.color = color;
    o.uv = (kOffs[i] + 1.0) * 0.5;
    verts[vertOff + i] = o;
  }

  const uint t0 = triOff;
  const uint t1 = triOff + 1;
  triangles[t0] = uint3(vertOff + 0, vertOff + 1, vertOff + 2);
  triangles[t1] = uint3(vertOff + 2, vertOff + 1, vertOff + 3);
}

[shader("fragment")]
float4 fragmentMain(MeshOutput input) : SV_Target {
  const float alpha = textureBindless2D(pc.texture, pc.sampler, input.uv).r;
  return float4(input.color, alpha);
}
