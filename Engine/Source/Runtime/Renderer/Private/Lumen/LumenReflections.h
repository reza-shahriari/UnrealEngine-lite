// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "BlueNoise.h"
#include "ShaderParameterMacros.h"

class FLumenCardTracingInputs;
class FLumenCardTracingParameters;
class FLumenMeshSDFGridParameters;
class FRDGBuilder;
class FScene;
class FSceneTextureParameters;
class FSceneView;
class FSceneViewFamily;
class FViewFamilyInfo;
class FViewInfo;
struct FLumenSceneFrameTemporaries;
struct FSceneTextures;
enum class EDiffuseIndirectMethod;
enum class ERDGPassFlags : uint16;

namespace LumenRadianceCache
{ 
	class FRadianceCacheInterpolationParameters; 
}

namespace LumenReflections
{
	BEGIN_SHADER_PARAMETER_STRUCT(FCompositeParameters, )
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER(float, MaxRoughnessToTraceForFoliage)
		SHADER_PARAMETER(float, InvRoughnessFadeLength)
	END_SHADER_PARAMETER_STRUCT()

	void SetupCompositeParameters(const FViewInfo& View, LumenReflections::FCompositeParameters& OutParameters);
	bool UseAsyncCompute(const FViewFamilyInfo& ViewFamily, EDiffuseIndirectMethod DiffuseIndirectMethod);
}

BEGIN_SHADER_PARAMETER_STRUCT(FLumenReflectionsVisualizeTracesParameters, )
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisualizeTracesData)
	SHADER_PARAMETER(uint32, VisualizeTraceCoherency)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenReflectionTracingParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionsVisualizeTracesParameters, VisualizeTracesParameters)
	SHADER_PARAMETER(FIntPoint, ReflectionDownsampleFactorXY)
	SHADER_PARAMETER(FIntPoint, ReflectionTracingViewMin)
	SHADER_PARAMETER(FIntPoint, ReflectionTracingViewSize)
	SHADER_PARAMETER(FIntPoint, ReflectionTracingBufferSize)
	SHADER_PARAMETER(FVector2f, ReflectionTracingBufferInvSize)
	SHADER_PARAMETER(float, MaxRayIntensity)
	SHADER_PARAMETER(float, ReflectionSmoothBias)
	SHADER_PARAMETER(uint32, ReflectionPass)
	SHADER_PARAMETER(uint32, UseJitter)
	SHADER_PARAMETER(uint32, UseHighResSurface)
	SHADER_PARAMETER(uint32, MaxReflectionBounces)
	SHADER_PARAMETER(uint32, MaxRefractionBounces)
	SHADER_PARAMETER(uint32, ReflectionsStateFrameIndex)
	SHADER_PARAMETER(uint32, ReflectionsStateFrameIndexMod8)
	SHADER_PARAMETER(uint32, ReflectionsRayDirectionFrameIndex)

	SHADER_PARAMETER(float, NearFieldMaxTraceDistance)
	SHADER_PARAMETER(float, NearFieldMaxTraceDistanceDitherScale)
	SHADER_PARAMETER(float, NearFieldSceneRadius)
	SHADER_PARAMETER(float, FarFieldMaxTraceDistance)

	SHADER_PARAMETER_STRUCT_INCLUDE(LumenReflections::FCompositeParameters, ReflectionsCompositeParameters)
	SHADER_PARAMETER_TEXTURE(Texture2D, PreIntegratedGF)
	SHADER_PARAMETER_SAMPLER(SamplerState, PreIntegratedGFSampler)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, RayBuffer)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, RayTraceDistance)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DownsampledDepth)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DownsampledClosureIndex)

	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceHit)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceRadiance)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceMaterialId)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D, TraceBookmark)

	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float3>, RWTraceRadiance)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float3>, RWTraceBackgroundVisibility)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<float>, RWTraceHit)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint>, RWTraceMaterialId)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint2>, RWTraceBookmark)

	SHADER_PARAMETER_STRUCT_REF(FBlueNoise, BlueNoise)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FLumenReflectionTileParameters, )
	SHADER_PARAMETER_RDG_TEXTURE(Texture2DArray<uint>, ResolveTileUsed)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionClearTileData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionResolveTileData)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, ReflectionTracingTileData)
	RDG_BUFFER_ACCESS(ClearIndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(ResolveIndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(TracingIndirectArgs, ERHIAccess::IndirectArgs)
END_SHADER_PARAMETER_STRUCT()

BEGIN_SHADER_PARAMETER_STRUCT(FCompactedReflectionTraceParameters, )
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelData)
	RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	RDG_BUFFER_ACCESS(RayTraceDispatchIndirectArgs, ERHIAccess::IndirectArgs | ERHIAccess::SRVCompute)
END_SHADER_PARAMETER_STRUCT()

namespace LumenReflections
{
	bool UseFarField(const FSceneViewFamily& ViewFamily);
	bool UseHitLighting(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod);
	bool UseTranslucentRayTracing(const FViewInfo& View);
	bool IsHitLightingForceEnabled(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod);
	bool UseSurfaceCacheFeedback();
	bool UseScreenTraces(const FViewInfo& View);
	bool UseDistantScreenTraces(const FViewInfo& View);
	float GetDistantScreenTraceStepOffsetBias();
	bool UseRadianceCache();
	bool UseRadianceCacheSkyVisibility();
	bool UseRadianceCacheStochasticInterpolation();

	float GetSampleSceneColorDepthTreshold();
	float GetSampleSceneColorNormalTreshold();
	float GetFarFieldSampleSceneColorDepthTreshold();
	float GetFarFieldSampleSceneColorNormalTreshold();

	uint32 GetMaxReflectionBounces(const FViewInfo& View);
	uint32 GetMaxRefractionBounces(const FViewInfo& View);

	enum ETraceCompactionMode
	{
		Default,
		FarField,
		HitLighting,

		MAX
	};

	FCompactedReflectionTraceParameters CompactTraces(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FLumenCardTracingParameters& TracingParameters,
		const FLumenReflectionTracingParameters& ReflectionTracingParameters,
		const FLumenReflectionTileParameters& ReflectionTileParameters,
		bool bCullByDistanceFromCamera,
		float CompactionTracingEndDistanceFromCamera,
		float CompactionMaxTraceDistance,
		ERDGPassFlags ComputePassFlags,
		ETraceCompactionMode TraceCompactionMode = ETraceCompactionMode::Default,
		bool bSortByMaterial = false);
};

extern void TraceReflections(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenSceneFrameTemporaries& FrameTemporaries,
	bool bTraceMeshObjects,
	const FSceneTextures& SceneTextures,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters, 
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	const FLumenMeshSDFGridParameters& InMeshSDFGridParameters,
	bool bUseRadianceCache,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	const FBoxSphereBounds& FirstPersonWorldSpaceRepresentationViewBounds,
	ERDGPassFlags ComputePassFlags);

class FLumenReflectionTracingParameters;
class FLumenReflectionTileParameters;
extern void RenderLumenHardwareRayTracingReflections(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	const FSceneTextureParameters& SceneTextureParameters,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenCardTracingParameters& TracingParameters,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	float MaxTraceDistance,
	bool bUseRadianceCache,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	bool bSampleSceneColorAtHit,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	ERDGPassFlags ComputePassFlags);