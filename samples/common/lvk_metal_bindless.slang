module lvk_metal_bindless;

#ifndef LVK_BINDLESS_TEXTURES
#define LVK_BINDLESS_TEXTURES 16384
#endif
#ifndef LVK_BINDLESS_SAMPLERS
#define LVK_BINDLESS_SAMPLERS 1024
#endif

public struct lvkBuffers      { uint64_t     data[1]; };
public struct lvkTextures2D   { Texture2D    data[LVK_BINDLESS_TEXTURES]; };
public struct lvkTextures3D   { Texture3D    data[LVK_BINDLESS_TEXTURES]; };
public struct lvkTexturesCube { TextureCube  data[LVK_BINDLESS_TEXTURES]; };
public struct lvkSamplers     { SamplerState data[LVK_BINDLESS_SAMPLERS]; };

// Slang assigns indices by order. Push constants have index 0.
public ParameterBlock<lvkBuffers> kBuffers; // 1
public ParameterBlock<lvkTextures2D> kTextures2D; // 2
public ParameterBlock<lvkTextures3D> kTextures3D; // 3
public ParameterBlock<lvkTexturesCube> kTexturesCube; // 4
public ParameterBlock<lvkSamplers> kSamplers; // 5

public float4 textureBindless2D(uint t, uint s, float2 uv) {
  return kTextures2D.data[t].Sample(kSamplers.data[s], uv);
}
public float4 textureBindless2DLod(uint t, uint s, float2 uv, float lod) {
  return kTextures2D.data[t].SampleLevel(kSamplers.data[s], uv, lod);
}
public float4 textureBindless3D(uint t, uint s, float3 uvw) {
  return kTextures3D.data[t].Sample(kSamplers.data[s], uvw);
}
public float4 textureBindlessCube(uint t, uint s, float3 dir) {
  return kTexturesCube.data[t].Sample(kSamplers.data[s], dir);
}
public float4 textureBindlessCubeLod(uint t, uint s, float3 dir, float lod) {
  return kTexturesCube.data[t].SampleLevel(kSamplers.data[s], dir, lod);
}
public uint2 textureBindlessSize2D(uint t) {
  uint w;
  uint h;
  kTextures2D.data[t].GetDimensions(w, h);
  return uint2(w, h);
}

public float2x2 operator*(float2x2 a, float2x2 b) { return mul(b, a); }
public float2   operator*(float2x2 a, float2   b) { return mul(b, a); }
public float3x3 operator*(float3x3 a, float3x3 b) { return mul(b, a); }
public float3   operator*(float3x3 a, float3   b) { return mul(b, a); }
public float4x4 operator*(float4x4 a, float4x4 b) { return mul(b, a); }
public float4   operator*(float4x4 a, float4   b) { return mul(b, a); }
public float3   operator*(float4x3 a, float4   b) { return mul(b, a); }
