module lvk_metal_bindless;

#ifndef LVK_BINDLESS_TEXTURES
#define LVK_BINDLESS_TEXTURES 16384
#endif
#ifndef LVK_BINDLESS_SAMPLERS
#define LVK_BINDLESS_SAMPLERS 1024
#endif
#ifndef LVK_BINDLESS_ACCEL_STRUCTS
#define LVK_BINDLESS_ACCEL_STRUCTS 256
#endif
#ifndef LVK_BINDLESS_BUFFERS
#define LVK_BINDLESS_BUFFERS 16384
#endif

public struct lvkBuffers            { public uint64_t                        data[LVK_BINDLESS_BUFFERS]; };
public struct lvkTextures2D         { public Texture2D                       data[LVK_BINDLESS_TEXTURES]; };
public struct lvkTextures3D         { public Texture3D                       data[LVK_BINDLESS_TEXTURES]; };
public struct lvkTexturesCube       { public TextureCube                     data[LVK_BINDLESS_TEXTURES]; };
public struct lvkSamplers           { public SamplerState                    data[LVK_BINDLESS_SAMPLERS]; };
public struct lvkImages2D           { public RWTexture2D<float4>             data[LVK_BINDLESS_TEXTURES]; };
public struct lvkImages3D           { public RWTexture3D<float4>             data[LVK_BINDLESS_TEXTURES]; };
public struct lvkTexturesDepth2D    { public Texture2D                       data[LVK_BINDLESS_TEXTURES]; };
public struct lvkSamplersComparison { public SamplerComparisonState          data[LVK_BINDLESS_SAMPLERS]; };
public struct lvkAccelStructs       { public RaytracingAccelerationStructure data[LVK_BINDLESS_ACCEL_STRUCTS]; };

// Slang assigns indices by order. Push constants have index 0.
public ParameterBlock<lvkBuffers> kBuffers; // 1
public ParameterBlock<lvkTextures2D> kTextures2D; // 2
public ParameterBlock<lvkTextures3D> kTextures3D; // 3
public ParameterBlock<lvkTexturesCube> kTexturesCube; // 4
public ParameterBlock<lvkTexturesDepth2D> kTexturesDepth2D; // 5
public ParameterBlock<lvkSamplers> kSamplers; // 6
public ParameterBlock<lvkSamplersComparison> kSamplersComparison; // 7
public ParameterBlock<lvkImages2D> kImages2D; // 8
public ParameterBlock<lvkImages3D> kImages3D; // 9
public ParameterBlock<lvkAccelStructs> kTLAS; // 10

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
public float textureBindless2DShadow(uint t, uint s, float3 uvw) {
  return kTexturesDepth2D.data[t].SampleCmp(kSamplersComparison.data[s], uvw.xy, uvw.z);
}

public float2x2 operator*(float2x2 a, float2x2 b) { return mul(b, a); }
public float2   operator*(float2x2 a, float2   b) { return mul(b, a); }
public float3x3 operator*(float3x3 a, float3x3 b) { return mul(b, a); }
public float3   operator*(float3x3 a, float3   b) { return mul(b, a); }
public float4x4 operator*(float4x4 a, float4x4 b) { return mul(b, a); }
public float4   operator*(float4x4 a, float4   b) { return mul(b, a); }
public float3   operator*(float4x3 a, float4   b) { return mul(b, a); }
