#pragma once

#include <cstdint>

#include <Metal/Metal.hpp>

namespace lvk::metal {

enum class GpuFamily : uint8_t {
  Apple7,
  Apple8,
  Apple9,
  Apple10,
};

// https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf
struct GpuLimits {
  uint32_t maxVertexAttributesPerVertexDescriptor = 0;
  uint32_t maxBufferArgumentTableEntriesPerFunction = 0;
  uint32_t maxTextureArgumentTableEntriesPerFunction = 0;
  uint32_t maxSamplerStateArgumentTableEntriesPerFunction = 0;
  uint32_t maxThreadgroupMemoryArgumentTableEntriesPerFunction = 0;
  uint32_t maxConstantBufferArguments = 0;
  uint32_t maxInlinedBufferContentsLength = 0;
  uint32_t maxThreadsPerThreadgroup = 0;
  uint32_t maxTotalThreadgroupMemoryAllocation = 0;
  uint32_t maxExplicitImageBlockAllocation = 0;
  uint32_t maxImplicitImageBlockAllocation = 0;
  uint32_t threadgroupMemoryLengthAlignment = 0;
  uint32_t maxScalarOrVectorInputsToFragment = 0;
  uint32_t maxInputComponentsToFragment = 0;
  uint32_t maxFunctionConstants = 0;
  uint32_t maxTessellationFactor = 0;
  uint32_t maxViewportsAndScissorRectanglesPerVertex = 0;
  uint32_t maxRasterOrderGroupsPerFragment = 0;
  uint32_t minBufferLayoutDescriptorStrideAlignment = 0;
  uint32_t maxBufferLayoutDescriptorStride = 0;

  uint32_t maxBuffersInArgumentBuffer = 0;
  uint32_t maxTexturesInArgumentBuffer = 0;
  uint32_t maxSamplersInArgumentBuffer = 0;

  uint32_t minConstantBufferOffsetAlignment = 0;
  uint32_t max1DTextureWidth = 0;
  uint32_t max2DTextureWidthHeight = 0;
  uint32_t maxCubeMapTextureWidthHeight = 0;
  uint32_t max3DTextureWidthHeightDepth = 0;
  uint32_t maxTextureBufferWidth = 0;
  uint32_t maxLayersPerTextureArray = 0;
  uint32_t bufferAlignmentForCopyingTextureToBuffer = 0;
  uint32_t maxCounterSampleBufferLength = 0;
  uint32_t maxSampleBuffers = 0;
  uint32_t maxResidencySetsPerQueue = 0;
  uint32_t maxResidencySetsPerBuffer = 0;
  uint32_t maxIndirectCommandBufferLength = 0;

  uint32_t maxColorRenderTargetsPerRenderPass = 0;
  uint32_t maxPointPrimitiveSize = 0;
  uint32_t maxExplicitImageBlockSizePerSample = 0;
  uint32_t maxImplicitImageBlockSizePerSample = 0;
  uint32_t maxVisibilityQueryOffset = 0;
  uint32_t maxSampleCountWithMsaa = 0;
  uint32_t maxTileSizeNoMsaaWidth = 0;
  uint32_t maxTileSizeNoMsaaHeight = 0;
  uint32_t maxTileSize2xMsaaWidth = 0;
  uint32_t maxTileSize2xMsaaHeight = 0;
  uint32_t maxTileSize4xMsaaWidth = 0;
  uint32_t maxTileSize4xMsaaHeight = 0;
  uint32_t maxTileSize8xMsaaWidth = 0;
  uint32_t maxTileSize8xMsaaHeight = 0;

  uint32_t maxFences = 0;
  uint32_t maxIOCommandsPerBuffer = 0;
  uint32_t maxVertexCountForVertexAmplification = 0;
  uint32_t maxThreadgroupsPerObjectShaderGrid = 0;
  uint32_t maxThreadgroupsPerMeshShaderGrid = 0;
  uint32_t maxPayloadInMeshShaderPipeline = 0;
  uint32_t maxRayTracingIntersectorLevels = 0;
  uint32_t maxRayTracingIntersectionQueryLevels = 0;
  uint32_t maxTextureViewPoolEntries = 0;
  uint32_t maxPerformanceCounterHeapsPerProcess = 0;
  uint32_t minIntersectionFunctionBufferAlignment = 0;
  uint32_t minIntersectionFunctionBufferStrideAlignment = 0;
  uint32_t maxIntersectionFunctionBufferStride = 0;
};

inline GpuLimits limitsForFamily(GpuFamily family) {
  switch (family) {
  case GpuFamily::Apple7:
  case GpuFamily::Apple8:
    return GpuLimits{
        .maxVertexAttributesPerVertexDescriptor = 31,
        .maxBufferArgumentTableEntriesPerFunction = 31,
        .maxTextureArgumentTableEntriesPerFunction = 128,
        .maxSamplerStateArgumentTableEntriesPerFunction = 16,
        .maxThreadgroupMemoryArgumentTableEntriesPerFunction = 31,
        .maxConstantBufferArguments = 31,
        .maxInlinedBufferContentsLength = 4096,
        .maxThreadsPerThreadgroup = 1024,
        .maxTotalThreadgroupMemoryAllocation = 32768,
        .maxExplicitImageBlockAllocation = 32768,
        .maxImplicitImageBlockAllocation = 131072,
        .threadgroupMemoryLengthAlignment = 16,
        .maxScalarOrVectorInputsToFragment = 124,
        .maxInputComponentsToFragment = 124,
        .maxFunctionConstants = 65536,
        .maxTessellationFactor = 64,
        .maxViewportsAndScissorRectanglesPerVertex = 16,
        .maxRasterOrderGroupsPerFragment = 8,
        .minBufferLayoutDescriptorStrideAlignment = 1,
        .maxBufferLayoutDescriptorStride = UINT32_MAX,
        .maxBuffersInArgumentBuffer = UINT32_MAX,
        .maxTexturesInArgumentBuffer = 1000000,
        .maxSamplersInArgumentBuffer = 996,
        .minConstantBufferOffsetAlignment = 4,
        .max1DTextureWidth = 16384,
        .max2DTextureWidthHeight = 16384,
        .maxCubeMapTextureWidthHeight = 16384,
        .max3DTextureWidthHeightDepth = 2048,
        .maxTextureBufferWidth = 256000000,
        .maxLayersPerTextureArray = 2048,
        .bufferAlignmentForCopyingTextureToBuffer = 16,
        .maxCounterSampleBufferLength = 32768,
        .maxSampleBuffers = 32,
        .maxResidencySetsPerQueue = 32,
        .maxResidencySetsPerBuffer = 32,
        .maxIndirectCommandBufferLength = UINT32_MAX,
        .maxColorRenderTargetsPerRenderPass = 8,
        .maxPointPrimitiveSize = 511,
        .maxExplicitImageBlockSizePerSample = 64,
        .maxImplicitImageBlockSizePerSample = 128,
        .maxVisibilityQueryOffset = 262144,
        .maxSampleCountWithMsaa = 4,
        .maxTileSizeNoMsaaWidth = 32,
        .maxTileSizeNoMsaaHeight = 32,
        .maxTileSize2xMsaaWidth = 32,
        .maxTileSize2xMsaaHeight = 32,
        .maxTileSize4xMsaaWidth = 32,
        .maxTileSize4xMsaaHeight = 16,
        .maxTileSize8xMsaaWidth = 0,
        .maxTileSize8xMsaaHeight = 0,
        .maxFences = 32768,
        .maxIOCommandsPerBuffer = 8192,
        .maxVertexCountForVertexAmplification = 8,
        .maxThreadgroupsPerObjectShaderGrid = UINT32_MAX,
        .maxThreadgroupsPerMeshShaderGrid = 1024,
        .maxPayloadInMeshShaderPipeline = 16384,
        .maxRayTracingIntersectorLevels = 32,
        .maxRayTracingIntersectionQueryLevels = 16,
        .maxTextureViewPoolEntries = 128000000,
        .maxPerformanceCounterHeapsPerProcess = 32,
        .minIntersectionFunctionBufferAlignment = 64,
        .minIntersectionFunctionBufferStrideAlignment = 8,
        .maxIntersectionFunctionBufferStride = 4096,
    };
  case GpuFamily::Apple9:
    return GpuLimits{
        .maxVertexAttributesPerVertexDescriptor = 31,
        .maxBufferArgumentTableEntriesPerFunction = 31,
        .maxTextureArgumentTableEntriesPerFunction = 128,
        .maxSamplerStateArgumentTableEntriesPerFunction = 16,
        .maxThreadgroupMemoryArgumentTableEntriesPerFunction = 31,
        .maxConstantBufferArguments = 31,
        .maxInlinedBufferContentsLength = 4096,
        .maxThreadsPerThreadgroup = 1024,
        .maxTotalThreadgroupMemoryAllocation = 32768,
        .maxExplicitImageBlockAllocation = 32768,
        .maxImplicitImageBlockAllocation = 262144,
        .threadgroupMemoryLengthAlignment = 16,
        .maxScalarOrVectorInputsToFragment = 124,
        .maxInputComponentsToFragment = 124,
        .maxFunctionConstants = 65536,
        .maxTessellationFactor = 64,
        .maxViewportsAndScissorRectanglesPerVertex = 16,
        .maxRasterOrderGroupsPerFragment = 8,
        .minBufferLayoutDescriptorStrideAlignment = 1,
        .maxBufferLayoutDescriptorStride = UINT32_MAX,
        .maxBuffersInArgumentBuffer = UINT32_MAX,
        .maxTexturesInArgumentBuffer = 1000000,
        .maxSamplersInArgumentBuffer = 500000,
        .minConstantBufferOffsetAlignment = 4,
        .max1DTextureWidth = 16384,
        .max2DTextureWidthHeight = 16384,
        .maxCubeMapTextureWidthHeight = 16384,
        .max3DTextureWidthHeightDepth = 2048,
        .maxTextureBufferWidth = 256000000,
        .maxLayersPerTextureArray = 2048,
        .bufferAlignmentForCopyingTextureToBuffer = 16,
        .maxCounterSampleBufferLength = 32768,
        .maxSampleBuffers = 32,
        .maxResidencySetsPerQueue = 32,
        .maxResidencySetsPerBuffer = 32,
        .maxIndirectCommandBufferLength = UINT32_MAX,
        .maxColorRenderTargetsPerRenderPass = 8,
        .maxPointPrimitiveSize = 511,
        .maxExplicitImageBlockSizePerSample = 64,
        .maxImplicitImageBlockSizePerSample = 128,
        .maxVisibilityQueryOffset = 262144,
        .maxSampleCountWithMsaa = 4,
        .maxTileSizeNoMsaaWidth = 32,
        .maxTileSizeNoMsaaHeight = 32,
        .maxTileSize2xMsaaWidth = 32,
        .maxTileSize2xMsaaHeight = 32,
        .maxTileSize4xMsaaWidth = 32,
        .maxTileSize4xMsaaHeight = 16,
        .maxTileSize8xMsaaWidth = 0,
        .maxTileSize8xMsaaHeight = 0,
        .maxFences = 32768,
        .maxIOCommandsPerBuffer = 8192,
        .maxVertexCountForVertexAmplification = 8,
        .maxThreadgroupsPerObjectShaderGrid = UINT32_MAX,
        .maxThreadgroupsPerMeshShaderGrid = 1048575,
        .maxPayloadInMeshShaderPipeline = 16384,
        .maxRayTracingIntersectorLevels = 32,
        .maxRayTracingIntersectionQueryLevels = 16,
        .maxTextureViewPoolEntries = 256000000,
        .maxPerformanceCounterHeapsPerProcess = 32,
        .minIntersectionFunctionBufferAlignment = 64,
        .minIntersectionFunctionBufferStrideAlignment = 8,
        .maxIntersectionFunctionBufferStride = 4096,
    };
  case GpuFamily::Apple10:
    return GpuLimits{
        .maxVertexAttributesPerVertexDescriptor = 31,
        .maxBufferArgumentTableEntriesPerFunction = 31,
        .maxTextureArgumentTableEntriesPerFunction = 128,
        .maxSamplerStateArgumentTableEntriesPerFunction = 16,
        .maxThreadgroupMemoryArgumentTableEntriesPerFunction = 31,
        .maxConstantBufferArguments = 31,
        .maxInlinedBufferContentsLength = 4096,
        .maxThreadsPerThreadgroup = 1024,
        .maxTotalThreadgroupMemoryAllocation = 32768,
        .maxExplicitImageBlockAllocation = 32768,
        .maxImplicitImageBlockAllocation = 262144,
        .threadgroupMemoryLengthAlignment = 16,
        .maxScalarOrVectorInputsToFragment = 124,
        .maxInputComponentsToFragment = 124,
        .maxFunctionConstants = 65536,
        .maxTessellationFactor = 64,
        .maxViewportsAndScissorRectanglesPerVertex = 16,
        .maxRasterOrderGroupsPerFragment = 8,
        .minBufferLayoutDescriptorStrideAlignment = 1,
        .maxBufferLayoutDescriptorStride = UINT32_MAX,
        .maxBuffersInArgumentBuffer = UINT32_MAX,
        .maxTexturesInArgumentBuffer = 1000000,
        .maxSamplersInArgumentBuffer = 500000,
        .minConstantBufferOffsetAlignment = 4,
        .max1DTextureWidth = 32768,
        .max2DTextureWidthHeight = 32768,
        .maxCubeMapTextureWidthHeight = 32768,
        .max3DTextureWidthHeightDepth = 2048,
        .maxTextureBufferWidth = 256000000,
        .maxLayersPerTextureArray = 2048,
        .bufferAlignmentForCopyingTextureToBuffer = 16,
        .maxCounterSampleBufferLength = 32768,
        .maxSampleBuffers = 32,
        .maxResidencySetsPerQueue = 32,
        .maxResidencySetsPerBuffer = 32,
        .maxIndirectCommandBufferLength = UINT32_MAX,
        .maxColorRenderTargetsPerRenderPass = 8,
        .maxPointPrimitiveSize = 511,
        .maxExplicitImageBlockSizePerSample = 64,
        .maxImplicitImageBlockSizePerSample = 128,
        .maxVisibilityQueryOffset = 262144,
        .maxSampleCountWithMsaa = 8,
        .maxTileSizeNoMsaaWidth = 32,
        .maxTileSizeNoMsaaHeight = 32,
        .maxTileSize2xMsaaWidth = 32,
        .maxTileSize2xMsaaHeight = 32,
        .maxTileSize4xMsaaWidth = 32,
        .maxTileSize4xMsaaHeight = 16,
        .maxTileSize8xMsaaWidth = 16,
        .maxTileSize8xMsaaHeight = 16,
        .maxFences = 32768,
        .maxIOCommandsPerBuffer = 8192,
        .maxVertexCountForVertexAmplification = 8,
        .maxThreadgroupsPerObjectShaderGrid = UINT32_MAX,
        .maxThreadgroupsPerMeshShaderGrid = 4194303,
        .maxPayloadInMeshShaderPipeline = 16384,
        .maxRayTracingIntersectorLevels = 32,
        .maxRayTracingIntersectionQueryLevels = 16,
        .maxTextureViewPoolEntries = 256000000,
        .maxPerformanceCounterHeapsPerProcess = 32,
        .minIntersectionFunctionBufferAlignment = 64,
        .minIntersectionFunctionBufferStrideAlignment = 8,
        .maxIntersectionFunctionBufferStride = 4096,
    };
  }
  return limitsForFamily(GpuFamily::Apple7);
}

inline GpuFamily detectGpuFamily(MTL::Device* device) {
  if (device->supportsFamily(MTL::GPUFamilyApple10))
    return GpuFamily::Apple10;
  if (device->supportsFamily(MTL::GPUFamilyApple9))
    return GpuFamily::Apple9;
  if (device->supportsFamily(MTL::GPUFamilyApple8))
    return GpuFamily::Apple8;
  return GpuFamily::Apple7;
}

} // namespace lvk::metal
