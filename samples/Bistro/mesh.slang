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

float2 unpackSnorm2x8(uint d) {
  return float2(uint2(d, d >> 8) & 255u) / 127.5 - 1.0;
}

float3 unpackOctahedral16(uint data) {
  float2 v = unpackSnorm2x8(data);
  float3 n = float3(v, 1.0 - abs(v.x) - abs(v.y));
  float t = max(-n.z, 0.0);
  n.x += (n.x > 0.0) ? -t : t;
  n.y += (n.y > 0.0) ? -t : t;
  return normalize(n);
}

struct V2F {
  float4 position : SV_Position;
  float2 uv : TEXCOORD0;
  float3 normal : TEXCOORD1;
  float4 shadowCoords : TEXCOORD2;
  nointerpolation uint mtlIndex : TEXCOORD3;
};

[shader("vertex")]
V2F meshVertexMain(uint vid : SV_VertexID) {
  const VertexData v = pc.verts[vid];
  const float4x4 model = pc.perObject.model;
  const float4 pos = float4(v.position, 1.0);
  V2F o;
  o.position = pc.perFrame.proj * pc.perFrame.view * model * pos;
  o.normal = normalize((float3x3)pc.perObject.normalMatrix * unpackOctahedral16(uint(v.normal)));
  o.uv = float2(f16tof32(v.uv), f16tof32(v.uv >> 16));
  o.shadowCoords = pc.perFrame.light * model * pos;
  o.mtlIndex = uint(v.mtlIndex);
  return o;
}

[shader("fragment")]
float4 meshFragmentMain(V2F input) : SV_Target {
  const Material m = pc.materials[input.mtlIndex];
  const uint smp = pc.perFrame.sampler0;
  if (m.texAlpha > 0 && textureBindless2D(m.texAlpha, smp, input.uv).r < 0.5)
    discard;
  const float4 Ka = m.ambient * textureBindless2D(m.texAmbient, smp, input.uv);
  const float4 Kd = m.diffuse * textureBindless2D(m.texDiffuse, smp, input.uv);
  if (Kd.a < 0.5)
    discard;
  const float3 n = normalize(input.normal);
  if (pc.perFrame.drawNormals != 0)
    return float4(0.5 * (n + 1.0), 1.0);

  float sh = 1.0;
  const float4 s = input.shadowCoords / input.shadowCoords.w;
  if (s.z > -1.0 && s.z < 1.0) {
    const float3 uvw = float3(s.x, 1.0 - s.y, s.z - 0.00005);
    const float sz = 1.0 / 4096.0;
    float acc = 0.0;
    for (int v = -1; v <= 1; ++v)
      for (int u = -1; u <= 1; ++u)
        acc += textureBindless2DShadow(pc.perFrame.texShadow, pc.perFrame.samplerShadow0, uvw + sz * float3(u, v, 0));
    sh = lerp(0.3, 1.0, acc / 9.0);
  }

  const float4 f0 = float4(0.04);
  const float4 diffuse = textureBindlessCube(pc.perFrame.texSkyboxIrradiance, smp, n) * Kd * (float4(1.0) - f0);
  return Ka + diffuse * sh;
}
