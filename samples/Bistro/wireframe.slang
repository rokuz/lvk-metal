import glsl;
import lvk_metal_bindless;

struct VertexData {
  float px, py, pz;
  uint uv;
  uint16_t normal;
  uint16_t mtlIndex;

  property float3 position { get { return float3(px, py, pz); } }
};

struct PerFrame {
  float4x4 proj;
  float4x4 view;
  float4x4 light;
  uint texSkyboxRadiance;
  uint texSkyboxIrradiance;
  uint texShadow;
  uint sampler0;
  uint samplerShadow0;
  uint drawNormals;
};

struct PerObject {
  float4x4 model;
  float4x4 normalMatrix;
};

struct Material {
  float4 ambient;
  float4 diffuse;
  uint texAmbient;
  uint texDiffuse;
  uint texAlpha;
  uint padding;
};

struct PC {
  PerFrame* perFrame;
  PerObject* perObject;
  Material* materials;
  VertexData* verts;
};

[[vk::push_constant]] PC pc;

[shader("vertex")]
float4 wireVertexMain(uint vid : SV_VertexID) : SV_Position {
  return pc.perFrame.proj * pc.perFrame.view * pc.perObject.model * float4(pc.verts[vid].position, 1.0);
}

[shader("fragment")]
float4 wireFragmentMain() : SV_Target {
  return float4(1.0);
}
