#pragma once

#include <lvk/LVK-Metal.h>
#include <lvk/LVK.h>

#include <Metal/Metal.hpp>

namespace lvk::metal {

inline uint32_t bytesPerMetalPixel(MTL::PixelFormat fmt) {
  switch (fmt) {
  case MTL::PixelFormatR8Unorm:
  case MTL::PixelFormatR8Uint:
    return 1;
  case MTL::PixelFormatR16Float:
  case MTL::PixelFormatR16Unorm:
  case MTL::PixelFormatRG8Unorm:
    return 2;
  case MTL::PixelFormatRGBA8Unorm:
  case MTL::PixelFormatRGBA8Unorm_sRGB:
  case MTL::PixelFormatBGRA8Unorm:
  case MTL::PixelFormatBGRA8Unorm_sRGB:
  case MTL::PixelFormatRG16Float:
  case MTL::PixelFormatR32Float:
    return 4;
  case MTL::PixelFormatRGBA16Float:
    return 8;
  case MTL::PixelFormatRGBA32Float:
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
  case Format_R_UI16:
    return MTL::PixelFormatR16Uint;
  case Format_R_UI32:
    return MTL::PixelFormatR32Uint;
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
  case Format_RG_UI16:
    return MTL::PixelFormatRG16Uint;
  case Format_RG_UI32:
    return MTL::PixelFormatRG32Uint;
  case Format_RG_UN16:
    return MTL::PixelFormatRG16Unorm;
  case Format_RG_F16:
    return MTL::PixelFormatRG16Float;
  case Format_RG_F32:
    return MTL::PixelFormatRG32Float;
  case Format_RGBA_UN8:
    return MTL::PixelFormatRGBA8Unorm;
  case Format_RGBA_UI32:
    return MTL::PixelFormatRGBA32Uint;
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
  case Format_BC7_RGBA:
    return MTL::PixelFormatBC7_RGBAUnorm;
  case Format_BC7_SRGBA:
    return MTL::PixelFormatBC7_RGBAUnorm_sRGB;
  case Format_Z_UN16:
    return MTL::PixelFormatDepth16Unorm;
  case Format_Z_F32:
    return MTL::PixelFormatDepth32Float;
  case Format_Z_UN24_S_UI8:
    return MTL::PixelFormatDepth24Unorm_Stencil8;
  case Format_Z_F32_S_UI8:
    return MTL::PixelFormatDepth32Float_Stencil8;
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
  case MTL::PixelFormatR16Uint:
    return Format_R_UI16;
  case MTL::PixelFormatR32Uint:
    return Format_R_UI32;
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
  case MTL::PixelFormatRG16Uint:
    return Format_RG_UI16;
  case MTL::PixelFormatRG32Uint:
    return Format_RG_UI32;
  case MTL::PixelFormatRG16Unorm:
    return Format_RG_UN16;
  case MTL::PixelFormatRG16Float:
    return Format_RG_F16;
  case MTL::PixelFormatRG32Float:
    return Format_RG_F32;
  case MTL::PixelFormatRGBA8Unorm:
    return Format_RGBA_UN8;
  case MTL::PixelFormatRGBA32Uint:
    return Format_RGBA_UI32;
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
  case MTL::PixelFormatBC7_RGBAUnorm:
    return Format_BC7_RGBA;
  case MTL::PixelFormatBC7_RGBAUnorm_sRGB:
    return Format_BC7_SRGBA;
  case MTL::PixelFormatDepth16Unorm:
    return Format_Z_UN16;
  case MTL::PixelFormatDepth32Float:
    return Format_Z_F32;
  case MTL::PixelFormatDepth24Unorm_Stencil8:
    return Format_Z_UN24_S_UI8;
  case MTL::PixelFormatDepth32Float_Stencil8:
    return Format_Z_F32_S_UI8;
  default:
    return Format_Invalid;
  }
}

} // namespace lvk::metal
