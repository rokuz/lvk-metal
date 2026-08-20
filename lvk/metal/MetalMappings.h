#pragma once

#include <lvk/LVK-Metal.h>
#include <lvk/LVK.h>

#include <Metal/Metal.hpp>

namespace lvk::metal {

inline uint32_t bytesPerMetalPixel(MTL::PixelFormat fmt) {
  switch (fmt) {
  case MTL::PixelFormatR8Unorm:
  case MTL::PixelFormatR8Uint:
  case MTL::PixelFormatA8Unorm:
  case MTL::PixelFormatStencil8:
    return 1;
  case MTL::PixelFormatR16Float:
  case MTL::PixelFormatR16Unorm:
  case MTL::PixelFormatR16Uint:
  case MTL::PixelFormatRG8Unorm:
  case MTL::PixelFormatDepth16Unorm:
    return 2;
  case MTL::PixelFormatRGBA8Unorm:
  case MTL::PixelFormatRGBA8Unorm_sRGB:
  case MTL::PixelFormatBGRA8Unorm:
  case MTL::PixelFormatBGRA8Unorm_sRGB:
  case MTL::PixelFormatRG16Float:
  case MTL::PixelFormatRG16Unorm:
  case MTL::PixelFormatRG16Uint:
  case MTL::PixelFormatR32Float:
  case MTL::PixelFormatR32Uint:
  case MTL::PixelFormatRGB10A2Unorm:
  case MTL::PixelFormatDepth32Float:
  case MTL::PixelFormatDepth24Unorm_Stencil8:
    return 4;
  case MTL::PixelFormatRGBA16Float:
  case MTL::PixelFormatRG32Float:
  case MTL::PixelFormatRG32Uint:
  case MTL::PixelFormatDepth32Float_Stencil8:
    return 8;
  case MTL::PixelFormatRGBA32Float:
  case MTL::PixelFormatRGBA32Uint:
    return 16;
  default:
    return 4;
  }
}

inline MTL::TensorDataType toMTLTensorDataType(TensorDataType dataType) {
  switch (dataType) {
  case TensorDataType::Float16:
    return MTL::TensorDataTypeFloat16;
  case TensorDataType::Float32:
    return MTL::TensorDataTypeFloat32;
  case TensorDataType::BFloat16:
    return MTL::TensorDataTypeBFloat16;
  case TensorDataType::Int8:
    return MTL::TensorDataTypeInt8;
  case TensorDataType::UInt8:
    return MTL::TensorDataTypeUInt8;
  case TensorDataType::Int32:
    return MTL::TensorDataTypeInt32;
  }
  return MTL::TensorDataTypeFloat16;
}

inline uint32_t tensorDataTypeByteSize(MTL::TensorDataType dataType) {
  switch (dataType) {
  case MTL::TensorDataTypeInt8:
  case MTL::TensorDataTypeUInt8:
    return 1;
  case MTL::TensorDataTypeFloat16:
  case MTL::TensorDataTypeBFloat16:
  case MTL::TensorDataTypeInt16:
  case MTL::TensorDataTypeUInt16:
    return 2;
  case MTL::TensorDataTypeFloat32:
  case MTL::TensorDataTypeInt32:
  case MTL::TensorDataTypeUInt32:
    return 4;
  default:
    return 4;
  }
}

inline MTL::PixelFormat toMTLPixelFormat(Format format) {
  switch (format) {
  case Format_R_UN8:
    return MTL::PixelFormatR8Unorm;
  case Format_R_UI8:
    return MTL::PixelFormatR8Uint;
  case Format_R_I8:
    return MTL::PixelFormatR8Sint;
  case Format_R_UI16:
    return MTL::PixelFormatR16Uint;
  case Format_R_I16:
    return MTL::PixelFormatR16Sint;
  case Format_R_UI32:
    return MTL::PixelFormatR32Uint;
  case Format_R_I32:
    return MTL::PixelFormatR32Sint;
  case Format_R_UN16:
    return MTL::PixelFormatR16Unorm;
  case Format_R_F16:
    return MTL::PixelFormatR16Float;
  case Format_R_F32:
    return MTL::PixelFormatR32Float;
  case Format_A_UN8:
    return MTL::PixelFormatA8Unorm;
  case Format_RG_UN8:
    return MTL::PixelFormatRG8Unorm;
  case Format_RG_UI8:
    return MTL::PixelFormatRG8Uint;
  case Format_RG_I8:
    return MTL::PixelFormatRG8Sint;
  case Format_RG_UI16:
    return MTL::PixelFormatRG16Uint;
  case Format_RG_I16:
    return MTL::PixelFormatRG16Sint;
  case Format_RG_UI32:
    return MTL::PixelFormatRG32Uint;
  case Format_RG_I32:
    return MTL::PixelFormatRG32Sint;
  case Format_RG_UN16:
    return MTL::PixelFormatRG16Unorm;
  case Format_RG_F16:
    return MTL::PixelFormatRG16Float;
  case Format_RG_F32:
    return MTL::PixelFormatRG32Float;
  case Format_RGBA_UN8:
    return MTL::PixelFormatRGBA8Unorm;
  case Format_RGBA_UI8:
    return MTL::PixelFormatRGBA8Uint;
  case Format_RGBA_I8:
    return MTL::PixelFormatRGBA8Sint;
  case Format_RGBA_UI16:
    return MTL::PixelFormatRGBA16Uint;
  case Format_RGBA_I16:
    return MTL::PixelFormatRGBA16Sint;
  case Format_RGBA_UI32:
    return MTL::PixelFormatRGBA32Uint;
  case Format_RGBA_I32:
    return MTL::PixelFormatRGBA32Sint;
  case Format_RGBA_F16:
    return MTL::PixelFormatRGBA16Float;
  case Format_RGBA_F32:
    return MTL::PixelFormatRGBA32Float;
  case Format_RGBA_SRGB8:
    return MTL::PixelFormatRGBA8Unorm_sRGB;
  case Format_BGRA_UN8:
    return MTL::PixelFormatBGRA8Unorm;
  case Format_BGRA_SRGB8:
    return MTL::PixelFormatBGRA8Unorm_sRGB;
  case Format_A2B10G10R10_UN:
    return MTL::PixelFormatRGB10A2Unorm;
  case Format_A2R10G10B10_UN:
    return MTL::PixelFormatBGR10A2Unorm;
  case Format_B10G11R11_UF:
    return MTL::PixelFormatRG11B10Float;
  case Format_E5B9G9R9_UF:
    return MTL::PixelFormatRGB9E5Float;
  case Format_ETC2_RGB8:
    return MTL::PixelFormatETC2_RGB8;
  case Format_ETC2_SRGB8:
    return MTL::PixelFormatETC2_RGB8_sRGB;
  case Format_BC1_RGBA:
    return MTL::PixelFormatBC1_RGBA;
  case Format_BC1_SRGBA:
    return MTL::PixelFormatBC1_RGBA_sRGB;
  case Format_BC3_RGBA:
    return MTL::PixelFormatBC3_RGBA;
  case Format_BC3_SRGBA:
    return MTL::PixelFormatBC3_RGBA_sRGB;
  case Format_BC4_R:
    return MTL::PixelFormatBC4_RUnorm;
  case Format_BC5_RG:
    return MTL::PixelFormatBC5_RGUnorm;
  case Format_BC7_RGBA:
    return MTL::PixelFormatBC7_RGBAUnorm;
  case Format_BC7_SRGBA:
    return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
  case Format_ASTC_4x4:
    return MTL::PixelFormatASTC_4x4_LDR;
  case Format_ASTC_4x4_SRGB:
    return MTL::PixelFormatASTC_4x4_sRGB;
  case Format_ASTC_5x5:
    return MTL::PixelFormatASTC_5x5_LDR;
  case Format_ASTC_5x5_SRGB:
    return MTL::PixelFormatASTC_5x5_sRGB;
  case Format_ASTC_6x6:
    return MTL::PixelFormatASTC_6x6_LDR;
  case Format_ASTC_6x6_SRGB:
    return MTL::PixelFormatASTC_6x6_sRGB;
  case Format_Z_UN16:
    return MTL::PixelFormatDepth16Unorm;
  case Format_Z_F32:
    return MTL::PixelFormatDepth32Float;
  case Format_Z_UN24_S_UI8:
  case Format_Z_F32_S_UI8:
    return MTL::PixelFormatDepth32Float_Stencil8;
  case Format_S_UI8:
    return MTL::PixelFormatStencil8;
  default:
    return MTL::PixelFormatInvalid;
  }
}

inline MTL::TextureType toMTLTextureType(TextureType type, uint32_t numLayers, uint32_t numSamples) {
  switch (type) {
  case TextureType_2D:
    if (numSamples > 1)
      return numLayers > 1 ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DMultisample;
    return numLayers > 1 ? MTL::TextureType2DArray : MTL::TextureType2D;
  case TextureType_3D:
    return MTL::TextureType3D;
  case TextureType_Cube:
    return numLayers > 6 ? MTL::TextureTypeCubeArray : MTL::TextureTypeCube;
  default:
    return MTL::TextureType2D;
  }
}

inline MTL::TextureUsage toMTLTextureUsage(uint8_t usage) {
  NS::UInteger out = 0;
  if (usage & TextureUsageBits_Sampled)
    out |= MTL::TextureUsageShaderRead;
  if (usage & TextureUsageBits_Storage)
    out |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
  if (usage & TextureUsageBits_Attachment)
    out |= MTL::TextureUsageRenderTarget;
  if (usage & TextureUsageBits_InputAttachment)
    out |= MTL::TextureUsageShaderRead;
  return static_cast<MTL::TextureUsage>(out);
}

inline MTL::StorageMode toMTLStorageMode(StorageType storage) {
  switch (storage) {
  case StorageType_Device:
    return MTL::StorageModePrivate;
  case StorageType_HostVisible:
    return MTL::StorageModeShared;
  case StorageType_Memoryless:
    return MTL::StorageModeMemoryless;
  default:
    return MTL::StorageModeShared;
  }
}

inline MTL::ResourceOptions toMTLBufferResourceOptions(StorageType storage) {
  switch (storage) {
  case StorageType_Device:
    return MTL::ResourceStorageModePrivate;
  case StorageType_HostVisible:
    return MTL::ResourceStorageModeShared;
  default:
    return MTL::ResourceStorageModeShared;
  }
}

inline MTL::PrimitiveType toMTLPrimitiveType(Topology topology) {
  switch (topology) {
  case Topology_Point:
    return MTL::PrimitiveTypePoint;
  case Topology_Line:
    return MTL::PrimitiveTypeLine;
  case Topology_LineStrip:
    return MTL::PrimitiveTypeLineStrip;
  case Topology_Triangle:
    return MTL::PrimitiveTypeTriangle;
  case Topology_TriangleStrip:
    return MTL::PrimitiveTypeTriangleStrip;
  default:
    return MTL::PrimitiveTypeTriangle;
  }
}

inline MTL::CullMode toMTLCullMode(CullMode mode) {
  switch (mode) {
  case CullMode_Front:
    return MTL::CullModeFront;
  case CullMode_Back:
    return MTL::CullModeBack;
  default:
    return MTL::CullModeNone;
  }
}

inline MTL::Winding toMTLWinding(WindingMode mode) {
  return mode == WindingMode_CW ? MTL::WindingClockwise : MTL::WindingCounterClockwise;
}

inline MTL::TriangleFillMode toMTLFillMode(PolygonMode mode) {
  return mode == PolygonMode_Line ? MTL::TriangleFillModeLines : MTL::TriangleFillModeFill;
}

inline MTL::CompareFunction toMTLCompareFunction(CompareOp op) {
  switch (op) {
  case CompareOp_Never:
    return MTL::CompareFunctionNever;
  case CompareOp_Less:
    return MTL::CompareFunctionLess;
  case CompareOp_Equal:
    return MTL::CompareFunctionEqual;
  case CompareOp_LessEqual:
    return MTL::CompareFunctionLessEqual;
  case CompareOp_Greater:
    return MTL::CompareFunctionGreater;
  case CompareOp_NotEqual:
    return MTL::CompareFunctionNotEqual;
  case CompareOp_GreaterEqual:
    return MTL::CompareFunctionGreaterEqual;
  default:
    return MTL::CompareFunctionAlways;
  }
}

inline MTL::StencilOperation toMTLStencilOperation(StencilOp op) {
  switch (op) {
  case StencilOp_Keep:
    return MTL::StencilOperationKeep;
  case StencilOp_Zero:
    return MTL::StencilOperationZero;
  case StencilOp_Replace:
    return MTL::StencilOperationReplace;
  case StencilOp_IncrementClamp:
    return MTL::StencilOperationIncrementClamp;
  case StencilOp_DecrementClamp:
    return MTL::StencilOperationDecrementClamp;
  case StencilOp_Invert:
    return MTL::StencilOperationInvert;
  case StencilOp_IncrementWrap:
    return MTL::StencilOperationIncrementWrap;
  case StencilOp_DecrementWrap:
    return MTL::StencilOperationDecrementWrap;
  default:
    return MTL::StencilOperationKeep;
  }
}

inline MTL::SamplerMinMagFilter toMTLSamplerFilter(SamplerFilter f) {
  return f == SamplerFilter_Nearest ? MTL::SamplerMinMagFilterNearest : MTL::SamplerMinMagFilterLinear;
}

inline MTL::SamplerMipFilter toMTLSamplerMipFilter(SamplerMip m) {
  switch (m) {
  case SamplerMip_Nearest:
    return MTL::SamplerMipFilterNearest;
  case SamplerMip_Linear:
    return MTL::SamplerMipFilterLinear;
  default:
    return MTL::SamplerMipFilterNotMipmapped;
  }
}

inline MTL::SamplerAddressMode toMTLSamplerAddressMode(SamplerWrap w) {
  switch (w) {
  case SamplerWrap_Clamp:
    return MTL::SamplerAddressModeClampToEdge;
  case SamplerWrap_ClampToBorder:
    return MTL::SamplerAddressModeClampToBorderColor;
  case SamplerWrap_MirrorRepeat:
    return MTL::SamplerAddressModeMirrorRepeat;
  case SamplerWrap_MirrorClampToEdge:
    return MTL::SamplerAddressModeMirrorClampToEdge;
  default:
    return MTL::SamplerAddressModeRepeat;
  }
}

inline MTL::BlendOperation toMTLBlendOperation(BlendOp op) {
  switch (op) {
  case BlendOp_Subtract:
    return MTL::BlendOperationSubtract;
  case BlendOp_ReverseSubtract:
    return MTL::BlendOperationReverseSubtract;
  case BlendOp_Min:
    return MTL::BlendOperationMin;
  case BlendOp_Max:
    return MTL::BlendOperationMax;
  default:
    return MTL::BlendOperationAdd;
  }
}

inline MTL::BlendFactor toMTLBlendFactor(BlendFactor f) {
  switch (f) {
  case BlendFactor_Zero:
    return MTL::BlendFactorZero;
  case BlendFactor_One:
    return MTL::BlendFactorOne;
  case BlendFactor_SrcColor:
    return MTL::BlendFactorSourceColor;
  case BlendFactor_OneMinusSrcColor:
    return MTL::BlendFactorOneMinusSourceColor;
  case BlendFactor_SrcAlpha:
    return MTL::BlendFactorSourceAlpha;
  case BlendFactor_OneMinusSrcAlpha:
    return MTL::BlendFactorOneMinusSourceAlpha;
  case BlendFactor_DstColor:
    return MTL::BlendFactorDestinationColor;
  case BlendFactor_OneMinusDstColor:
    return MTL::BlendFactorOneMinusDestinationColor;
  case BlendFactor_DstAlpha:
    return MTL::BlendFactorDestinationAlpha;
  case BlendFactor_OneMinusDstAlpha:
    return MTL::BlendFactorOneMinusDestinationAlpha;
  case BlendFactor_SrcAlphaSaturated:
    return MTL::BlendFactorSourceAlphaSaturated;
  case BlendFactor_BlendColor:
    return MTL::BlendFactorBlendColor;
  case BlendFactor_OneMinusBlendColor:
    return MTL::BlendFactorOneMinusBlendColor;
  case BlendFactor_BlendAlpha:
    return MTL::BlendFactorBlendAlpha;
  case BlendFactor_OneMinusBlendAlpha:
    return MTL::BlendFactorOneMinusBlendAlpha;
  case BlendFactor_Src1Color:
    return MTL::BlendFactorSource1Color;
  case BlendFactor_OneMinusSrc1Color:
    return MTL::BlendFactorOneMinusSource1Color;
  case BlendFactor_Src1Alpha:
    return MTL::BlendFactorSource1Alpha;
  case BlendFactor_OneMinusSrc1Alpha:
    return MTL::BlendFactorOneMinusSource1Alpha;
  default:
    return MTL::BlendFactorOne;
  }
}

inline MTL::IndexType toMTLIndexType(IndexFormat format) {
  return format == IndexFormat_UI16 ? MTL::IndexTypeUInt16 : MTL::IndexTypeUInt32;
}

inline MTL::AccelerationStructureUsage toMTLAccelStructUsage(uint8_t buildFlags) {
  MTL::AccelerationStructureUsage usage = MTL::AccelerationStructureUsageNone;
  if (buildFlags & AccelStructBuildFlagBits_AllowUpdate)
    usage |= MTL::AccelerationStructureUsageRefit;
  if (buildFlags & AccelStructBuildFlagBits_PreferFastBuild)
    usage |= MTL::AccelerationStructureUsagePreferFastBuild;
  if (buildFlags & AccelStructBuildFlagBits_PreferFastTrace)
    usage |= MTL::AccelerationStructureUsagePreferFastIntersection;
  if (buildFlags & AccelStructBuildFlagBits_LowMemory)
    usage |= MTL::AccelerationStructureUsageMinimizeMemory;
  return usage;
}

inline bool isYUVFormat(Format format) {
  return format == Format_YUV_NV12 || format == Format_YUV_420p;
}

inline MTL::TextureSwizzle toMTLSwizzle(Swizzle s, MTL::TextureSwizzle identity) {
  switch (s) {
  case Swizzle_0:
    return MTL::TextureSwizzleZero;
  case Swizzle_1:
    return MTL::TextureSwizzleOne;
  case Swizzle_R:
    return MTL::TextureSwizzleRed;
  case Swizzle_G:
    return MTL::TextureSwizzleGreen;
  case Swizzle_B:
    return MTL::TextureSwizzleBlue;
  case Swizzle_A:
    return MTL::TextureSwizzleAlpha;
  default:
    return identity;
  }
}

inline MTL::TextureSwizzleChannels toMTLSwizzleChannels(const ComponentMapping& c) {
  return MTL::TextureSwizzleChannels::Make(toMTLSwizzle(c.r, MTL::TextureSwizzleRed),
                                           toMTLSwizzle(c.g, MTL::TextureSwizzleGreen),
                                           toMTLSwizzle(c.b, MTL::TextureSwizzleBlue),
                                           toMTLSwizzle(c.a, MTL::TextureSwizzleAlpha));
}

inline Format toLVKFormat(MTL::PixelFormat format) {
  switch (format) {
  case MTL::PixelFormatR8Unorm:
    return Format_R_UN8;
  case MTL::PixelFormatR8Uint:
    return Format_R_UI8;
  case MTL::PixelFormatR8Sint:
    return Format_R_I8;
  case MTL::PixelFormatR16Uint:
    return Format_R_UI16;
  case MTL::PixelFormatR16Sint:
    return Format_R_I16;
  case MTL::PixelFormatR32Uint:
    return Format_R_UI32;
  case MTL::PixelFormatR32Sint:
    return Format_R_I32;
  case MTL::PixelFormatR16Unorm:
    return Format_R_UN16;
  case MTL::PixelFormatR16Float:
    return Format_R_F16;
  case MTL::PixelFormatR32Float:
    return Format_R_F32;
  case MTL::PixelFormatA8Unorm:
    return Format_A_UN8;
  case MTL::PixelFormatRG8Unorm:
    return Format_RG_UN8;
  case MTL::PixelFormatRG8Uint:
    return Format_RG_UI8;
  case MTL::PixelFormatRG8Sint:
    return Format_RG_I8;
  case MTL::PixelFormatRG16Uint:
    return Format_RG_UI16;
  case MTL::PixelFormatRG16Sint:
    return Format_RG_I16;
  case MTL::PixelFormatRG32Uint:
    return Format_RG_UI32;
  case MTL::PixelFormatRG32Sint:
    return Format_RG_I32;
  case MTL::PixelFormatRG16Unorm:
    return Format_RG_UN16;
  case MTL::PixelFormatRG16Float:
    return Format_RG_F16;
  case MTL::PixelFormatRG32Float:
    return Format_RG_F32;
  case MTL::PixelFormatRGBA8Unorm:
    return Format_RGBA_UN8;
  case MTL::PixelFormatRGBA8Uint:
    return Format_RGBA_UI8;
  case MTL::PixelFormatRGBA8Sint:
    return Format_RGBA_I8;
  case MTL::PixelFormatRGBA16Uint:
    return Format_RGBA_UI16;
  case MTL::PixelFormatRGBA16Sint:
    return Format_RGBA_I16;
  case MTL::PixelFormatRGBA32Uint:
    return Format_RGBA_UI32;
  case MTL::PixelFormatRGBA32Sint:
    return Format_RGBA_I32;
  case MTL::PixelFormatRGBA16Float:
    return Format_RGBA_F16;
  case MTL::PixelFormatRGBA32Float:
    return Format_RGBA_F32;
  case MTL::PixelFormatRGBA8Unorm_sRGB:
    return Format_RGBA_SRGB8;
  case MTL::PixelFormatBGRA8Unorm:
    return Format_BGRA_UN8;
  case MTL::PixelFormatBGRA8Unorm_sRGB:
    return Format_BGRA_SRGB8;
  case MTL::PixelFormatRGB10A2Unorm:
    return Format_A2B10G10R10_UN;
  case MTL::PixelFormatBGR10A2Unorm:
    return Format_A2R10G10B10_UN;
  case MTL::PixelFormatRG11B10Float:
    return Format_B10G11R11_UF;
  case MTL::PixelFormatRGB9E5Float:
    return Format_E5B9G9R9_UF;
  case MTL::PixelFormatETC2_RGB8:
    return Format_ETC2_RGB8;
  case MTL::PixelFormatETC2_RGB8_sRGB:
    return Format_ETC2_SRGB8;
  case MTL::PixelFormatBC1_RGBA:
    return Format_BC1_RGBA;
  case MTL::PixelFormatBC1_RGBA_sRGB:
    return Format_BC1_SRGBA;
  case MTL::PixelFormatBC3_RGBA:
    return Format_BC3_RGBA;
  case MTL::PixelFormatBC3_RGBA_sRGB:
    return Format_BC3_SRGBA;
  case MTL::PixelFormatBC4_RUnorm:
    return Format_BC4_R;
  case MTL::PixelFormatBC5_RGUnorm:
    return Format_BC5_RG;
  case MTL::PixelFormatBC7_RGBAUnorm:
    return Format_BC7_RGBA;
  case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
    return Format_BC7_SRGBA;
  case MTL::PixelFormatASTC_4x4_LDR:
    return Format_ASTC_4x4;
  case MTL::PixelFormatASTC_4x4_sRGB:
    return Format_ASTC_4x4_SRGB;
  case MTL::PixelFormatASTC_5x5_LDR:
    return Format_ASTC_5x5;
  case MTL::PixelFormatASTC_5x5_sRGB:
    return Format_ASTC_5x5_SRGB;
  case MTL::PixelFormatASTC_6x6_LDR:
    return Format_ASTC_6x6;
  case MTL::PixelFormatASTC_6x6_sRGB:
    return Format_ASTC_6x6_SRGB;
  case MTL::PixelFormatDepth16Unorm:
    return Format_Z_UN16;
  case MTL::PixelFormatDepth32Float:
    return Format_Z_F32;
  case MTL::PixelFormatDepth24Unorm_Stencil8:
    return Format_Z_UN24_S_UI8;
  case MTL::PixelFormatDepth32Float_Stencil8:
    return Format_Z_F32_S_UI8;
  case MTL::PixelFormatStencil8:
    return Format_S_UI8;
  default:
    return Format_Invalid;
  }
}

} // namespace lvk::metal
