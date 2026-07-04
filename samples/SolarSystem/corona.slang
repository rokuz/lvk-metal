import lvk_metal_bindless;

struct Vertex {
  float px, py, pz;
  float u, v;
  float nx, ny, nz;

  property float3 pos { get { return float3(px, py, pz); } }
  property float3 normal { get { return float3(nx, ny, nz); } }
  property float2 uv { get { return float2(u, v); } }
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float time;
};

struct Material {
  float4 emissive;
  float4 diffuse;
  uint texEmissive;
  uint texDiffuse;
  uint pad0;
  uint pad1;
};

struct DrawData {
  uint idMatrix;
  uint idMaterial;
};

struct PC {
  Vertex* verts;
  float4x4* model;
  float4x4* normalMat;
  PerFrame* perFrame;
  DrawData* drawData;
  Material* materials;
  uint firstDrawData;
  uint sampler;
};

[[vk::push_constant]] PC pc;

struct V2F {
  float4 position : SV_Position;
  float2 v_TexCoord : TEXCOORD0;
  nointerpolation uint v_Texture0 : TEXCOORD1;
};

float3 getBillboardOffset(float4x4 mv, float2 uv, float2 sizeXY) {
  float3 x = float3(mv[0][0], mv[1][0], mv[2][0]);
  float3 y = float3(mv[0][1], mv[1][1], mv[2][1]);
  float2 coef = float2(2.0, 2.0) * (uv - float2(0.5, 0.5)) * sizeXY;
  return coef.x * x + coef.y * y;
}

[shader("vertex")]
V2F coronaVertexMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const Vertex vtx = pc.verts[vid];
  float4x4 mv = pc.perFrame.view * pc.model[dd.idMatrix];
  float3 v = getBillboardOffset(mv, vtx.uv, float2(0.28, 0.28));
  V2F o;
  o.position = pc.perFrame.proj * mv * float4(v, 1.0);
  o.v_TexCoord = vtx.uv;
  o.v_Texture0 = pc.materials[dd.idMaterial].texEmissive;
  return o;
}

[shader("fragment")]
float4 coronaFragmentMain(V2F input) : SV_Target {
  float4 K1 = textureBindless2D(input.v_Texture0, pc.sampler, input.v_TexCoord);
  K1.a = dot(K1.xyz, float3(0.333, 0.333, 0.333));
  return K1;
}
