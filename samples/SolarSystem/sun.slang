import glsl;
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
  float v_Time : TEXCOORD1;
  nointerpolation uint v_Texture0 : TEXCOORD2;
  nointerpolation uint v_Texture1 : TEXCOORD3;
};

[shader("vertex")]
V2F sunVertexMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const Vertex v = pc.verts[vid];
  const Material m = pc.materials[dd.idMaterial];
  V2F o;
  o.position = pc.perFrame.proj * pc.perFrame.view * pc.model[dd.idMatrix] * float4(v.pos, 1.0);
  o.v_TexCoord = v.uv + 0.05 * v.normal.xz;
  o.v_Time = pc.perFrame.time;
  o.v_Texture0 = m.texEmissive;
  o.v_Texture1 = m.texDiffuse;
  return o;
}

[shader("fragment")]
float4 sunFragmentMain(V2F input) : SV_Target {
  float2 t = float2(cos(0.05 * input.v_Time));
  float3 K1 = textureBindless2D(input.v_Texture0, pc.sampler, sin(input.v_TexCoord + t)).rgb;
  float3 K2 = textureBindless2D(input.v_Texture1, pc.sampler, input.v_TexCoord - 2.0 * t).rgb;
  return float4(K1 * K2, 1.0);
}
