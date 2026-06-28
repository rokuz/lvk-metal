import lvk_metal_bindless;

struct Vertex {
  float3 pos;
  float2 uv;
  float3 normal;
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
};

[[vk::push_constant]] PC pc;

[shader("vertex")]
float4 orbitVertexMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) : SV_Position {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  return pc.perFrame.proj * pc.perFrame.view * pc.model[dd.idMatrix] * float4(pc.verts[vid].pos, 1.0);
}

[shader("fragment")]
float4 orbitFragmentMain() : SV_Target {
  return float4(1.0, 1.0, 1.0, 0.2);
}
