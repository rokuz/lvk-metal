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
  float3 v_WorldPos : TEXCOORD1;
  float3 v_WorldNormal : TEXCOORD2;
  nointerpolation uint v_MaterialIndex : TEXCOORD3;
};

[shader("vertex")]
V2F defaultVertexMain(uint vid : SV_VertexID, uint iid : SV_InstanceID) {
  const DrawData dd = pc.drawData[pc.firstDrawData + iid];
  const Vertex v = pc.verts[vid];
  const float4 inVertex = float4(v.pos, 1.0);
  const float4x4 model = pc.model[dd.idMatrix];
  const float4x4 normal = pc.normalMat[dd.idMatrix];
  V2F o;
  o.v_WorldPos = (model * inVertex).xyz;
  o.v_WorldNormal = (float3x3)normal * v.normal;
  o.position = pc.perFrame.proj * pc.perFrame.view * model * inVertex;
  o.v_TexCoord = v.uv;
  o.v_MaterialIndex = dd.idMaterial;
  return o;
}

float getDiffuseFactor(float3 toLight, float3 normal, bool twoSided) {
  float d = dot(toLight, normal);
  return clamp(twoSided ? max(d, -d) : d, 0.0, 1.0);
}

float pointLight(float3 lightPos, float lightRadius, float3 worldPos, float3 worldNormal, bool twoSided) {
  float3 toLight = lightPos - worldPos;
  float distanceToLight = length(toLight);
  float s = distanceToLight / lightRadius;
  float attenuation = max(1.0 - s * s, 0.0);
  return attenuation * getDiffuseFactor(normalize(toLight), normalize(worldNormal), twoSided);
}

[shader("fragment")]
float4 defaultFragmentMain(V2F input) : SV_Target {
  Material m = pc.materials[input.v_MaterialIndex];
  float4 Ke = m.emissive * textureBindless2D(m.texEmissive, pc.sampler, input.v_TexCoord);
  float4 Kd = m.diffuse * textureBindless2D(m.texDiffuse, pc.sampler, input.v_TexCoord);
  const float3 lightPos = float3(0.0, 0.0, 0.0);
  const float4 lightColor = float4(1.0, 1.0, 1.0, 1.0);
  const float lightRadius = 10.0;
  bool twoSided = m.diffuse.w > 0.5;
  float4 color = Ke + Kd * lightColor * pointLight(lightPos, lightRadius, input.v_WorldPos, input.v_WorldNormal, twoSided);
  color.a = Ke.a;
  return color;
}
