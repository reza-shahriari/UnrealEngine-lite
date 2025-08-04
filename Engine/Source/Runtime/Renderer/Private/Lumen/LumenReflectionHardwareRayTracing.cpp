// Copyright Epic Games, Inc. All Rights Reserved.

#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "SceneTextureParameters.h"
#include "IndirectLightRendering.h"
#include "LumenReflections.h"
#include "RayTracedTranslucency.h"
#include "HairStrands/HairStrandsData.h"
#include "RenderUtils.h"
#include "LumenHardwareRayTracingCommon.h"

#if RHI_RAYTRACING
#include "RayTracing/RaytracingOptions.h"
#include "RayTracing/RayTracingLighting.h"
#endif // RHI_RAYTRACING

static TAutoConsoleVariable<int32> CVarLumenReflectionsHardwareRayTracing(
	TEXT("r.Lumen.Reflections.HardwareRayTracing"),
	1,
	TEXT("Enables hardware ray tracing for Lumen reflections (Default = 1)"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenReflectionsHardwareRayTracingBias(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.Bias"),
	0.1f,
	TEXT("Constant bias for hardware ray traced reflection rays."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenReflectionsHardwareRayTracingNormalBias(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.NormalBias"),
	0.1f,
	TEXT("Normal bias for hardware ray traced reflection rays."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsHardwareRayTracingBucketMaterials(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.BucketMaterials"),
	1,
	TEXT("Determines whether a secondary traces will be bucketed for coherent material access (default = 1)"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsHardwareRayTracingHitLighting(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.HitLighting"),
	0,
	TEXT("Determines whether a second trace will be fired for hit-lighting for invalid surface-cache hits (Default = 0)"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenReflectionsHardwareRayTracingFarField(
	TEXT("r.Lumen.Reflections.HardwareRayTracing.FarField"),
	1,
	TEXT("Determines whether a second trace will be fired for far-field contribution."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

namespace Lumen
{
	bool UseHardwareRayTracedReflections(const FSceneViewFamily& ViewFamily)
	{
		return IsRayTracingEnabled() 
			&& Lumen::UseHardwareRayTracing(ViewFamily) 
			&& (CVarLumenReflectionsHardwareRayTracing.GetValueOnAnyThread() != 0);
	}
}

bool LumenReflections::IsHitLightingForceEnabled(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod)
{
	return LumenHardwareRayTracing::GetHitLightingMode(View, DiffuseIndirectMethod) != LumenHardwareRayTracing::EHitLightingMode::SurfaceCache;
}

bool LumenReflections::UseHitLighting(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod)
{
	if (LumenHardwareRayTracing::IsRayGenSupported())
	{
		return IsHitLightingForceEnabled(View, DiffuseIndirectMethod)
			|| (CVarLumenReflectionsHardwareRayTracingHitLighting.GetValueOnRenderThread() != 0);
	}

	return false;
}

bool LumenReflections::UseTranslucentRayTracing(const FViewInfo& View)
{
	if (DoesProjectSupportLumenRayTracedTranslucentRefraction())
	{
		// >=2 because the first reflection is from the first reflection hit,
		// while the second is the potential translucent object hit after, actually achieving translucency.
		return LumenReflections::GetMaxRefractionBounces(View) >= 2;
	}
	return false;
}

bool LumenReflections::UseFarField(const FSceneViewFamily& ViewFamily)
{
	return Lumen::UseFarField(ViewFamily) && CVarLumenReflectionsHardwareRayTracingFarField.GetValueOnRenderThread();
}

namespace LumenReflections
{
	enum class ERayTracingPass
	{
		Default,
		FarField,
		HitLighting,
		MAX
	};
}

#if RHI_RAYTRACING

class FLumenReflectionHardwareRayTracing : public FLumenHardwareRayTracingShaderBase
{
	DECLARE_LUMEN_RAYTRACING_SHADER(FLumenReflectionHardwareRayTracing)

	class FRayTracingPass : SHADER_PERMUTATION_ENUM_CLASS("RAY_TRACING_PASS", LumenReflections::ERayTracingPass);
	class FUseShaderExecutionReordering : SHADER_PERMUTATION_BOOL("RAY_TRACING_USE_SER");
	class FWriteDataForHitLightingPass : SHADER_PERMUTATION_BOOL("WRITE_DATA_FOR_HIT_LIGHTING_PASS");
	class FRadianceCache : SHADER_PERMUTATION_BOOL("DIM_RADIANCE_CACHE");
	class FRadianceCacheSkyVisibility : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE_SKY_VISIBILITY");
	class FRadianceCacheStochasticInterpolation : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE_STOCHASTIC_INTERPOLATION");
	class FHairStrandsOcclusionDim : SHADER_PERMUTATION_BOOL("DIM_HAIRSTRANDS_VOXEL");
	class FRecursiveReflectionTraces : SHADER_PERMUTATION_BOOL("RECURSIVE_REFLECTION_TRACES");
	class FRecursiveRefractionTraces : SHADER_PERMUTATION_BOOL("RECURSIVE_REFRACTION_TRACES");
	class FSurfaceCacheAlphaMasking : SHADER_PERMUTATION_BOOL("SURFACE_CACHE_ALPHA_MASKING");
	class FDistantScreenTraces : SHADER_PERMUTATION_BOOL("DISTANT_SCREEN_TRACES");
	class FFarFieldOcclusionOnly : SHADER_PERMUTATION_BOOL("FAR_FIELD_OCCLUSION_ONLY");
	using FPermutationDomain = TShaderPermutationDomain<FLumenHardwareRayTracingShaderBase::FBasePermutationDomain, FRayTracingPass, FUseShaderExecutionReordering, FWriteDataForHitLightingPass, FRadianceCache, FRadianceCacheSkyVisibility, FRadianceCacheStochasticInterpolation, FHairStrandsOcclusionDim, FRecursiveReflectionTraces, FRecursiveRefractionTraces, FSurfaceCacheAlphaMasking, FDistantScreenTraces, FFarFieldOcclusionOnly>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingShaderBase::FSharedParameters, SharedParameters)
		RDG_BUFFER_ACCESS(HardwareRayTracingIndirectArgs, ERHIAccess::IndirectArgs | ERHIAccess::SRVCompute)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelData)

		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHZBScreenTraceParameters, HZBScreenTraceParameters)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DistantScreenTraceFurthestHZBTexture)
		SHADER_PARAMETER(float, DistantScreenTraceSlopeCompareTolerance)
		SHADER_PARAMETER(float, DistantScreenTraceMaxTraceDistance)
		SHADER_PARAMETER(float, DistantScreenTracesStartDistance)
		SHADER_PARAMETER(float, DistantScreenTraceStepOffsetBias)

		SHADER_PARAMETER(float, RelativeDepthThickness)
		SHADER_PARAMETER(float, SampleSceneColorNormalTreshold)
		SHADER_PARAMETER(int32, SampleSceneColor)
	
		SHADER_PARAMETER(uint32, HitLightingForceOpaque)
		SHADER_PARAMETER(uint32, HitLightingShadowMode)
		SHADER_PARAMETER(uint32, HitLightingShadowTranslucencyMode)
		SHADER_PARAMETER(uint32, HitLightingDirectLighting)
		SHADER_PARAMETER(uint32, HitLightingSkylight)
		SHADER_PARAMETER(uint32, UseReflectionCaptures)

		SHADER_PARAMETER(float, RayTracingBias)
		SHADER_PARAMETER(float, RayTracingNormalBias)
		SHADER_PARAMETER(float, FarFieldBias)
		SHADER_PARAMETER(float, PullbackBias)
		SHADER_PARAMETER(int, ApplySkyLight)
		SHADER_PARAMETER(int, HitLightingForceEnabled)

		// Reflection-specific includes (includes output targets)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
	END_SHADER_PARAMETER_STRUCT()

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		if (PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::Default)
		{
			PermutationVector.Set<FRecursiveReflectionTraces>(false);
			PermutationVector.Set<FFarFieldOcclusionOnly>(false);
		}
		else if (PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::FarField)
		{
			PermutationVector.Set<FRecursiveReflectionTraces>(false);
			PermutationVector.Set<FRadianceCache>(false);
			PermutationVector.Set<FHairStrandsOcclusionDim>(false);
			PermutationVector.Set<FRecursiveRefractionTraces>(false); // Translucent meshes are only with during near and hit-lighting passes for now.
			PermutationVector.Set<FSurfaceCacheAlphaMasking>(false);
			PermutationVector.Set<FDistantScreenTraces>(false);
		}
		else if (PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::HitLighting)
		{
			PermutationVector.Set<FWriteDataForHitLightingPass>(false);
			PermutationVector.Set<FSurfaceCacheAlphaMasking>(false);
			PermutationVector.Set<FDistantScreenTraces>(false);
			PermutationVector.Set<FFarFieldOcclusionOnly>(false);
		}

		if (PermutationVector.Get<FRayTracingPass>() != LumenReflections::ERayTracingPass::HitLighting)
		{
			PermutationVector.Set<FUseShaderExecutionReordering>(false);
		}

		if (PermutationVector.Get<FWriteDataForHitLightingPass>())
		{
			PermutationVector.Set<FSurfaceCacheAlphaMasking>(false);
		}

		if (PermutationVector.Get<FRadianceCache>())
		{
			// When radiance cache is used, rays are clipped short and fall back to radiance cache if no hit.
			// Since the rays are short, we will get mostly SSR reflections if distant screen traces is enabled.
			PermutationVector.Set<FDistantScreenTraces>(false);
		}
		else
		{
			PermutationVector.Set<FRadianceCacheSkyVisibility>(false);
			PermutationVector.Set<FRadianceCacheStochasticInterpolation>(false);
		}

		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		if (ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline && PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::HitLighting)
		{
			return false;
		}

		// Does platform support SER?
		if (PermutationVector.Get<FUseShaderExecutionReordering>() && !FDataDrivenShaderPlatformInfo::GetSupportsShaderExecutionReordering(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform)
			&& FLumenHardwareRayTracingShaderBase::ShouldCompilePermutation(Parameters, ShaderDispatchType);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType, FShaderCompilerEnvironment& OutEnvironment)
	{
		FLumenHardwareRayTracingShaderBase::ModifyCompilationEnvironment(Parameters, ShaderDispatchType, Lumen::ESurfaceCacheSampling::HighResPages, OutEnvironment);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		OutEnvironment.SetDefine(TEXT("ENABLE_NEAR_FIELD_TRACING"), PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::Default ? 1 : 0);
		OutEnvironment.SetDefine(TEXT("ENABLE_FAR_FIELD_TRACING"), PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::FarField ? 1 : 0);
	}

	static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
	{
		FPermutationDomain PermutationVector(PermutationId);
		if (PermutationVector.Get<FRayTracingPass>() == LumenReflections::ERayTracingPass::HitLighting)
		{
			return ERayTracingPayloadType::RayTracingMaterial;
		}
		else
		{
			return ERayTracingPayloadType::LumenMinimal;	
		}
	}
};

IMPLEMENT_LUMEN_RAYGEN_AND_COMPUTE_RAYTRACING_SHADERS(FLumenReflectionHardwareRayTracing)

IMPLEMENT_GLOBAL_SHADER(FLumenReflectionHardwareRayTracingCS, "/Engine/Private/Lumen/LumenReflectionHardwareRayTracing.usf", "LumenReflectionHardwareRayTracingCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FLumenReflectionHardwareRayTracingRGS, "/Engine/Private/Lumen/LumenReflectionHardwareRayTracing.usf", "LumenReflectionHardwareRayTracingRGS", SF_RayGen);

class FLumenReflectionHardwareRayTracingIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FLumenReflectionHardwareRayTracingIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FLumenReflectionHardwareRayTracingIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWHardwareRayTracingIndirectArgs)
		SHADER_PARAMETER(FIntPoint, OutputThreadGroupSize)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_1D"), GetThreadGroupSize1D());
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE_2D"), GetThreadGroupSize2D());
	}

	static int32 GetThreadGroupSize1D() { return GetThreadGroupSize2D() * GetThreadGroupSize2D(); }
	static int32 GetThreadGroupSize2D() { return 8; }
};

IMPLEMENT_GLOBAL_SHADER(FLumenReflectionHardwareRayTracingIndirectArgsCS, "/Engine/Private/Lumen/LumenReflectionHardwareRayTracing.usf", "FLumenReflectionHardwareRayTracingIndirectArgsCS", SF_Compute);

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingReflections(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (Lumen::UseHardwareRayTracedReflections(*View.Family) && LumenReflections::UseHitLighting(View, GetViewPipelineState(View).DiffuseIndirectMethod))
	{
		const bool bFarFieldOcclusionOnly = Lumen::UseFarFieldOcclusionOnly();

		for (int32 HairOcclusion = 0; HairOcclusion < 2; HairOcclusion++)
		{
			for (int32 RayTracingTranslucent = 0; RayTracingTranslucent < 2; RayTracingTranslucent++)
			{
				FLumenReflectionHardwareRayTracingRGS::FPermutationDomain PermutationVector;
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRayTracingPass>(LumenReflections::ERayTracingPass::HitLighting);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FUseShaderExecutionReordering>(LumenHardwareRayTracing::UseShaderExecutionReordering());
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FWriteDataForHitLightingPass>(false);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCache>(false);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheSkyVisibility>(false);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheStochasticInterpolation>(false);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FHairStrandsOcclusionDim>(HairOcclusion != 0);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveReflectionTraces>(LumenReflections::GetMaxReflectionBounces(View) > 1);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveRefractionTraces>(RayTracingTranslucent > 0);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FDistantScreenTraces>(false);
				PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FFarFieldOcclusionOnly>(bFarFieldOcclusionOnly);
				PermutationVector = FLumenReflectionHardwareRayTracingRGS::RemapPermutation(PermutationVector);

				TShaderRef<FLumenReflectionHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenReflectionHardwareRayTracingRGS>(PermutationVector);
				OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
			}
		}
	}
}

void FDeferredShadingSceneRenderer::PrepareLumenHardwareRayTracingReflectionsLumenMaterial(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (Lumen::UseHardwareRayTracedReflections(*View.Family))
	{
		const bool bUseHitLighting = LumenReflections::UseHitLighting(View, GetViewPipelineState(View).DiffuseIndirectMethod);
		const bool bUseInlineRayTracing = Lumen::UseHardwareInlineRayTracing(*View.Family);

		if (bUseInlineRayTracing && !bUseHitLighting)
		{			
			return;
		}
		
		const bool bUseFarField = LumenReflections::UseFarField(*View.Family);
		const bool bUseDistantScreenTraces = !bUseFarField
			&& LumenReflections::UseDistantScreenTraces(View)
			&& RayTracing::GetCullingMode(View.Family->EngineShowFlags) != RayTracing::ECullingMode::Disabled;

		// Default
		for (int RadianceCache = 0; RadianceCache < 2; ++RadianceCache)
		{
			for (int32 HairOcclusion = 0; HairOcclusion < 2; ++HairOcclusion)
			{
				for (int32 RayTracingTranslucent = 0; RayTracingTranslucent < 2; RayTracingTranslucent++)
				{
					FLumenReflectionHardwareRayTracingRGS::FPermutationDomain PermutationVector;
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRayTracingPass>(LumenReflections::ERayTracingPass::Default);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FWriteDataForHitLightingPass>(bUseHitLighting);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCache>(LumenReflections::UseRadianceCache() && RadianceCache != 0);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheSkyVisibility>(LumenReflections::UseRadianceCacheSkyVisibility());
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheStochasticInterpolation>(LumenReflections::UseRadianceCacheStochasticInterpolation());
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FHairStrandsOcclusionDim>(HairOcclusion != 0);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveReflectionTraces>(false);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveRefractionTraces>(RayTracingTranslucent > 0);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FDistantScreenTraces>(bUseDistantScreenTraces);
					PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FFarFieldOcclusionOnly>(false);
					PermutationVector = FLumenReflectionHardwareRayTracingRGS::RemapPermutation(PermutationVector);

					TShaderRef<FLumenReflectionHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenReflectionHardwareRayTracingRGS>(PermutationVector);
					OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
				}
			}
		}

		// Far-field continuation
		if (bUseFarField)
		{
			FLumenReflectionHardwareRayTracingRGS::FPermutationDomain PermutationVector;
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRayTracingPass>(LumenReflections::ERayTracingPass::FarField);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FWriteDataForHitLightingPass>(bUseHitLighting);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCache>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheSkyVisibility>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRadianceCacheStochasticInterpolation>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FHairStrandsOcclusionDim>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveReflectionTraces>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FRecursiveRefractionTraces>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FDistantScreenTraces>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracingRGS::FFarFieldOcclusionOnly>(Lumen::UseFarFieldOcclusionOnly());
			PermutationVector = FLumenReflectionHardwareRayTracingRGS::RemapPermutation(PermutationVector);

			TShaderRef<FLumenReflectionHardwareRayTracingRGS> RayGenerationShader = View.ShaderMap->GetShader<FLumenReflectionHardwareRayTracingRGS>(PermutationVector);
			OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
		}
	}
}

void DispatchLumenReflectionHardwareRayTracingIndirectArgs(FRDGBuilder& GraphBuilder, const FViewInfo& View, FRDGBufferRef HardwareRayTracingIndirectArgsBuffer, FRDGBufferRef CompactedTraceTexelAllocator, FIntPoint OutputThreadGroupSize, ERDGPassFlags ComputePassFlags)
{
	FLumenReflectionHardwareRayTracingIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FLumenReflectionHardwareRayTracingIndirectArgsCS::FParameters>();
	
	PassParameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(CompactedTraceTexelAllocator, PF_R32_UINT);
	PassParameters->RWHardwareRayTracingIndirectArgs = GraphBuilder.CreateUAV(HardwareRayTracingIndirectArgsBuffer, PF_R32_UINT);
	PassParameters->OutputThreadGroupSize = OutputThreadGroupSize;

	TShaderRef<FLumenReflectionHardwareRayTracingIndirectArgsCS> ComputeShader = View.ShaderMap->GetShader<FLumenReflectionHardwareRayTracingIndirectArgsCS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ReflectionCompactRaysIndirectArgs"),
		ComputePassFlags,
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1));
}

void DispatchRayGenOrComputeShader(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	const FSceneTextureParameters& SceneTextureParameters,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenCardTracingParameters& TracingParameters,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	const FCompactedReflectionTraceParameters& CompactedTraceParameters,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	const FLumenReflectionHardwareRayTracing::FPermutationDomain& PermutationVector,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	uint32 RayCount,
	bool bApplySkyLight,
	bool bIsHitLightingForceEnabled,
	bool bUseRadianceCache,
	bool bInlineRayTracing,
	bool bSampleSceneColorAtHit,
	float SampleSceneColorRelativeDepthThickness,
	float SampleSceneColorNormalTreshold,
	bool bNeedTraceHairVoxel,
	ERDGPassFlags ComputePassFlags)
{
	FRDGBufferRef CompactedTraceTexelAllocator = CompactedTraceParameters.CompactedTraceTexelAllocator->Desc.Buffer;
	FRDGBufferRef CompactedTraceTexelData = CompactedTraceParameters.CompactedTraceTexelData->Desc.Buffer;

	FRDGBufferRef HardwareRayTracingIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.Reflection.CompactTracingIndirectArgs"));
	FIntPoint OutputThreadGroupSize = bInlineRayTracing ? FLumenReflectionHardwareRayTracingCS::GetThreadGroupSize(View.GetShaderPlatform()) : FLumenReflectionHardwareRayTracingRGS::GetThreadGroupSize();
	DispatchLumenReflectionHardwareRayTracingIndirectArgs(GraphBuilder, View, HardwareRayTracingIndirectArgsBuffer, CompactedTraceTexelAllocator, OutputThreadGroupSize, ComputePassFlags);

	FLumenReflectionHardwareRayTracing::FParameters* Parameters = GraphBuilder.AllocParameters<FLumenReflectionHardwareRayTracing::FParameters>();
	{
		SetLumenHardwareRayTracingSharedParameters(
			GraphBuilder,
			SceneTextureParameters,
			View,
			TracingParameters,
			&Parameters->SharedParameters
		);
		Parameters->HardwareRayTracingIndirectArgs = HardwareRayTracingIndirectArgsBuffer;
		Parameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(CompactedTraceTexelAllocator, PF_R32_UINT);
		Parameters->CompactedTraceTexelData = GraphBuilder.CreateSRV(CompactedTraceTexelData, PF_R32_UINT);
			
		Parameters->HZBScreenTraceParameters = SetupHZBScreenTraceParameters(GraphBuilder, View, SceneTextures);

		if (Parameters->HZBScreenTraceParameters.PrevSceneColorTexture->GetParent() == SceneTextures.Color.Resolve || !Parameters->SharedParameters.SceneTextures.GBufferVelocityTexture)
		{
			Parameters->SharedParameters.SceneTextures.GBufferVelocityTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		}

		extern float GLumenReflectionDistantScreenTraceSlopeCompareTolerance;
		extern float GLumenReflectionDistantScreenTraceMaxTraceDistance;
		Parameters->DistantScreenTraceFurthestHZBTexture = GetHZBTexture(View, EHZBType::FurthestHZB);
		Parameters->DistantScreenTraceSlopeCompareTolerance = GLumenReflectionDistantScreenTraceSlopeCompareTolerance;
		Parameters->DistantScreenTraceMaxTraceDistance = GLumenReflectionDistantScreenTraceMaxTraceDistance;
		Parameters->DistantScreenTraceStepOffsetBias = LumenReflections::GetDistantScreenTraceStepOffsetBias();
		Parameters->DistantScreenTracesStartDistance = RayTracing::GetCullingMode(View.Family->EngineShowFlags) != RayTracing::ECullingMode::Disabled ? GetRayTracingCullingRadius() : FLT_MAX;

		Parameters->RelativeDepthThickness = SampleSceneColorRelativeDepthThickness * View.ViewMatrices.GetPerProjectionDepthThicknessScale();
		Parameters->SampleSceneColorNormalTreshold = SampleSceneColorNormalTreshold;
		Parameters->SampleSceneColor = bSampleSceneColorAtHit ? 1 : 0;
		
		Parameters->HitLightingForceOpaque = LumenHardwareRayTracing::UseHitLightingForceOpaque();
		Parameters->HitLightingShadowMode = LumenHardwareRayTracing::GetHitLightingShadowMode();
		Parameters->HitLightingShadowTranslucencyMode = LumenHardwareRayTracing::GetHitLightingShadowTranslucencyMode();
		Parameters->HitLightingDirectLighting = LumenHardwareRayTracing::UseHitLightingDirectLighting() ? 1 : 0;
		Parameters->HitLightingSkylight = LumenHardwareRayTracing::UseHitLightingSkylight(DiffuseIndirectMethod) ? 1 : 0;
		Parameters->UseReflectionCaptures = LumenHardwareRayTracing::UseReflectionCapturesForHitLighting();
		
		Parameters->RayTracingBias = CVarLumenReflectionsHardwareRayTracingBias.GetValueOnRenderThread();
		Parameters->RayTracingNormalBias = CVarLumenReflectionsHardwareRayTracingNormalBias.GetValueOnRenderThread();
		Parameters->FarFieldBias = LumenHardwareRayTracing::GetFarFieldBias();
		Parameters->PullbackBias = Lumen::GetHardwareRayTracingPullbackBias();
		Parameters->ApplySkyLight = bApplySkyLight;
		Parameters->HitLightingForceEnabled = bIsHitLightingForceEnabled;
		
		// Reflection-specific
		Parameters->ReflectionTracingParameters = ReflectionTracingParameters;
		Parameters->ReflectionTileParameters = ReflectionTileParameters;
		Parameters->RadianceCacheParameters = RadianceCacheParameters;

		if (bNeedTraceHairVoxel)
		{
			Parameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
		}
	}
	
	const LumenReflections::ERayTracingPass RayTracingPass = PermutationVector.Get<FLumenReflectionHardwareRayTracingRGS::FRayTracingPass>();
	const FString RayTracingPassName = RayTracingPass == LumenReflections::ERayTracingPass::HitLighting ? TEXT("hit-lighting") : (RayTracingPass == LumenReflections::ERayTracingPass::FarField ? TEXT("far-field") : TEXT("default"));

	if (bInlineRayTracing)
	{
		FLumenReflectionHardwareRayTracingCS::AddLumenRayTracingDispatchIndirect(
			GraphBuilder,
			RDG_EVENT_NAME("ReflectionHardwareRayTracingCS %s", *RayTracingPassName),
			View,
			PermutationVector,
			Parameters,
			Parameters->HardwareRayTracingIndirectArgs,
			0,
			ComputePassFlags);	
	}
	else
	{
		const bool bUseMinimalPayload = RayTracingPass != LumenReflections::ERayTracingPass::HitLighting;
		FLumenReflectionHardwareRayTracingRGS::AddLumenRayTracingDispatchIndirect(
			GraphBuilder,
			RDG_EVENT_NAME("ReflectionHardwareRayTracingRGS %s", *RayTracingPassName),
			View,
			PermutationVector, 
			Parameters,
			Parameters->HardwareRayTracingIndirectArgs,
			0,
			bUseMinimalPayload,
			ComputePassFlags);
	}
}

#endif // RHI_RAYTRACING

void RenderLumenHardwareRayTracingReflections(
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
	ERDGPassFlags ComputePassFlags
)
{
#if RHI_RAYTRACING	
	const bool bUseHitLighting = LumenReflections::UseHitLighting(View, DiffuseIndirectMethod);
	const bool bIsHitLightingForceEnabled = LumenReflections::IsHitLightingForceEnabled(View, DiffuseIndirectMethod);
	const bool bInlineRayTracing = Lumen::UseHardwareInlineRayTracing(*View.Family) && !bUseHitLighting && !bIsHitLightingForceEnabled;
	const bool bUseFarFieldForReflections = LumenReflections::UseFarField(*View.Family);
	extern int32 GLumenReflectionHairStrands_VoxelTrace;
	const bool bNeedTraceHairVoxel = HairStrands::HasViewHairStrandsVoxelData(View) && GLumenReflectionHairStrands_VoxelTrace > 0;
	const bool bTraceTranslucent = bUseHitLighting && LumenReflections::UseTranslucentRayTracing(View);
	const bool bUseDistantScreenTraces = !bUseFarFieldForReflections
		&& LumenReflections::UseDistantScreenTraces(View)
		&& RayTracing::GetCullingMode(View.Family->EngineShowFlags) != RayTracing::ECullingMode::Disabled;

	const FIntPoint BufferSize = ReflectionTracingParameters.ReflectionTracingBufferSize;
	const int32 RayCount = BufferSize.X * BufferSize.Y;

	// Default tracing for near field with only surface cache
	{
		FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
			GraphBuilder,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			false,
			0.0f,
			MaxTraceDistance,
			ComputePassFlags);

		bool bApplySkyLight = !bUseFarFieldForReflections;

		FLumenReflectionHardwareRayTracing::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRayTracingPass>(LumenReflections::ERayTracingPass::Default);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FWriteDataForHitLightingPass>(bUseHitLighting);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCache>(bUseRadianceCache);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheSkyVisibility>(LumenReflections::UseRadianceCacheSkyVisibility());
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheStochasticInterpolation>(LumenReflections::UseRadianceCacheStochasticInterpolation());
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FHairStrandsOcclusionDim>(bNeedTraceHairVoxel);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveReflectionTraces>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveRefractionTraces>(bTraceTranslucent);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FDistantScreenTraces>(bUseDistantScreenTraces);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FFarFieldOcclusionOnly>(false);
		PermutationVector = FLumenReflectionHardwareRayTracing::RemapPermutation(PermutationVector);

		DispatchRayGenOrComputeShader(GraphBuilder, SceneTextures, SceneTextureParameters, Scene, View, TracingParameters, ReflectionTracingParameters, ReflectionTileParameters, CompactedTraceParameters, RadianceCacheParameters,
			PermutationVector, DiffuseIndirectMethod, RayCount, bApplySkyLight, bIsHitLightingForceEnabled, bUseRadianceCache, bInlineRayTracing,
			bSampleSceneColorAtHit,
			LumenReflections::GetSampleSceneColorDepthTreshold(),
			LumenReflections::GetSampleSceneColorNormalTreshold(),
			bNeedTraceHairVoxel,
			ComputePassFlags);
	}

	// Far Field
	if (bUseFarFieldForReflections)
	{
		FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
			GraphBuilder,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			false,
			0.0f,
			Lumen::GetFarFieldMaxTraceDistance(),
			ComputePassFlags,
			LumenReflections::ETraceCompactionMode::FarField);

		bool bApplySkyLight = true;

		FLumenReflectionHardwareRayTracing::FPermutationDomain PermutationVector;
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRayTracingPass>(LumenReflections::ERayTracingPass::FarField);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FWriteDataForHitLightingPass>(bUseHitLighting);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCache>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheSkyVisibility>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheStochasticInterpolation>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FHairStrandsOcclusionDim>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveReflectionTraces>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveRefractionTraces>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FDistantScreenTraces>(false);
		PermutationVector.Set<FLumenReflectionHardwareRayTracing::FFarFieldOcclusionOnly>(Lumen::UseFarFieldOcclusionOnly());
		PermutationVector = FLumenReflectionHardwareRayTracing::RemapPermutation(PermutationVector);

		// Trace continuation rays
		DispatchRayGenOrComputeShader(GraphBuilder, SceneTextures, SceneTextureParameters, Scene, View, TracingParameters, ReflectionTracingParameters, ReflectionTileParameters, CompactedTraceParameters, RadianceCacheParameters,
			PermutationVector, DiffuseIndirectMethod, RayCount, bApplySkyLight, bIsHitLightingForceEnabled, bUseRadianceCache, bInlineRayTracing,
			bSampleSceneColorAtHit,
			LumenReflections::GetFarFieldSampleSceneColorDepthTreshold(),
			LumenReflections::GetFarFieldSampleSceneColorNormalTreshold(),
			bNeedTraceHairVoxel,
			ComputePassFlags);
	}

	// Hit Lighting
	if (bUseHitLighting)
	{
		FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
			GraphBuilder,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			false,
			0.0f,
			bUseFarFieldForReflections ? Lumen::GetFarFieldMaxTraceDistance() : MaxTraceDistance,
			ComputePassFlags,
			LumenReflections::ETraceCompactionMode::HitLighting,
			/*bSortByMaterial*/ CVarLumenReflectionsHardwareRayTracingBucketMaterials.GetValueOnRenderThread() != 0);

		// Trace with hit-lighting
		{
			bool bApplySkyLight = true;
			bool bUseInline = false;

			FLumenReflectionHardwareRayTracing::FPermutationDomain PermutationVector;
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRayTracingPass>(LumenReflections::ERayTracingPass::HitLighting);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FUseShaderExecutionReordering>(LumenHardwareRayTracing::UseShaderExecutionReordering());
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FWriteDataForHitLightingPass>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCache>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheSkyVisibility>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRadianceCacheStochasticInterpolation>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FHairStrandsOcclusionDim>(bNeedTraceHairVoxel);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveReflectionTraces>(ReflectionTracingParameters.MaxReflectionBounces > 1);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FRecursiveRefractionTraces>(bTraceTranslucent);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FSurfaceCacheAlphaMasking>(LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking());
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FDistantScreenTraces>(false);
			PermutationVector.Set<FLumenReflectionHardwareRayTracing::FFarFieldOcclusionOnly>(false);
			PermutationVector = FLumenReflectionHardwareRayTracing::RemapPermutation(PermutationVector);

			DispatchRayGenOrComputeShader(GraphBuilder, SceneTextures, SceneTextureParameters, Scene, View, TracingParameters, ReflectionTracingParameters, ReflectionTileParameters, CompactedTraceParameters, RadianceCacheParameters,
				PermutationVector, DiffuseIndirectMethod, RayCount, bApplySkyLight, bIsHitLightingForceEnabled, bUseRadianceCache, bUseInline,
				bSampleSceneColorAtHit,
				LumenReflections::GetSampleSceneColorDepthTreshold(),
				LumenReflections::GetSampleSceneColorNormalTreshold(),
				bNeedTraceHairVoxel, ComputePassFlags);
		}
	}
#endif
}

#if RHI_RAYTRACING

class FRayTracedTranslucencyHardwareRayTracing : public FLumenHardwareRayTracingShaderBase
{
	DECLARE_LUMEN_RAYTRACING_SHADER(FRayTracedTranslucencyHardwareRayTracing)

	class FUseRayTracedRefraction : SHADER_PERMUTATION_BOOL("USE_RAY_TRACED_REFRACTION");
	class FUseShaderExecutionReordering : SHADER_PERMUTATION_BOOL("RAY_TRACING_USE_SER");
	using FPermutationDomain = TShaderPermutationDomain<FLumenHardwareRayTracingShaderBase::FBasePermutationDomain, FUseRayTracedRefraction, FUseShaderExecutionReordering>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHardwareRayTracingShaderBase::FSharedParameters, SharedParameters)
		RDG_BUFFER_ACCESS(HardwareRayTracingIndirectArgs, ERHIAccess::IndirectArgs | ERHIAccess::SRVCompute)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, CompactedTraceTexelData)

		SHADER_PARAMETER(uint32, TranslucencyForceOpaque)	
		SHADER_PARAMETER(uint32, HitLightingShadowMode)
		SHADER_PARAMETER(uint32, HitLightingShadowTranslucencyMode)
		SHADER_PARAMETER(uint32, HitLightingDirectLighting)
		SHADER_PARAMETER(uint32, HitLightingSkylight)
		SHADER_PARAMETER(uint32, UseReflectionCaptures)
		
		SHADER_PARAMETER(float, SecondaryPathStartBias)
		SHADER_PARAMETER(float, SecondaryPathStartNormalBias)
		SHADER_PARAMETER(float, PathThroughputThreshold)

		SHADER_PARAMETER(int32, MaxPrimaryHitEvents)
		SHADER_PARAMETER(int32, MaxSecondaryHitEvents)
		SHADER_PARAMETER(uint32, SampleTranslucentReflectionInReflections)

		// Reflection-specific includes (includes output targets)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTracingParameters, ReflectionTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenReflectionTileParameters, ReflectionTileParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
	END_SHADER_PARAMETER_STRUCT()

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (RemapPermutation(PermutationVector) != PermutationVector)
		{
			return false;
		}

		if (ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline)
		{
			return false;
		}

		// Does platform support SER?
		if (PermutationVector.Get<FUseShaderExecutionReordering>() && !FDataDrivenShaderPlatformInfo::GetSupportsShaderExecutionReordering(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform)
			&& FLumenHardwareRayTracingShaderBase::ShouldCompilePermutation(Parameters, ShaderDispatchType);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType, FShaderCompilerEnvironment& OutEnvironment)
	{
		FLumenHardwareRayTracingShaderBase::ModifyCompilationEnvironment(Parameters, ShaderDispatchType, Lumen::ESurfaceCacheSampling::HighResPages, OutEnvironment);
	}

	static ERayTracingPayloadType GetRayTracingPayloadType(const int32 PermutationId)
	{
		return ERayTracingPayloadType::RayTracingMaterial;
	}
};

IMPLEMENT_LUMEN_RAYGEN_RAYTRACING_SHADER(FRayTracedTranslucencyHardwareRayTracing)

IMPLEMENT_GLOBAL_SHADER(FRayTracedTranslucencyHardwareRayTracingRGS, "/Engine/Private/Lumen/RayTracedTranslucency.usf", "RayTracedTranslucencyHardwareRayTracingRGS", SF_RayGen);

void FDeferredShadingSceneRenderer::PrepareHardwareRayTracingTranslucency(const FViewInfo& View, TArray<FRHIRayTracingShader*>& OutRayGenShaders)
{
	if (RayTracedTranslucency::IsEnabled(View))
	{
		FRayTracedTranslucencyHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracedTranslucencyHardwareRayTracingRGS::FUseRayTracedRefraction>(RayTracedTranslucency::UseRayTracedRefraction(Views));
		PermutationVector.Set<FRayTracedTranslucencyHardwareRayTracingRGS::FUseShaderExecutionReordering>(LumenHardwareRayTracing::UseShaderExecutionReordering());
		PermutationVector = FRayTracedTranslucencyHardwareRayTracingRGS::RemapPermutation(PermutationVector);
		auto RayGenerationShader = View.ShaderMap->GetShader<FRayTracedTranslucencyHardwareRayTracingRGS>(PermutationVector);
		OutRayGenShaders.Add(RayGenerationShader.GetRayTracingShader());
	}
}

namespace RayTracedTranslucency
{
	void DispatchRayGenOrComputeShader(
		FRDGBuilder& GraphBuilder,
		const FSceneTextures& SceneTextures,
		const FSceneTextureParameters& SceneTextureParameters,
		const FScene* Scene,
		const FViewInfo& View,
		const FLumenCardTracingParameters& TracingParameters,
		const FLumenReflectionTracingParameters& ReflectionTracingParameters,
		const FLumenReflectionTileParameters& ReflectionTileParameters,
		const FCompactedReflectionTraceParameters& CompactedTraceParameters,
		const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
		const FRayTracedTranslucencyHardwareRayTracingRGS::FPermutationDomain& PermutationVector,
		EDiffuseIndirectMethod DiffuseIndirectMethod,
		uint32 RayCount,
		bool bUseRadianceCache,
		bool bSampleSceneColorAtHit,
		bool bNeedTraceHairVoxel,
		ERDGPassFlags ComputePassFlags)
	{
		FRDGBufferRef CompactedTraceTexelAllocator = CompactedTraceParameters.CompactedTraceTexelAllocator->Desc.Buffer;
		FRDGBufferRef CompactedTraceTexelData = CompactedTraceParameters.CompactedTraceTexelData->Desc.Buffer;

		FRDGBufferRef HardwareRayTracingIndirectArgsBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.RTTranslucency.CompactTracingIndirectArgs"));
		FIntPoint OutputThreadGroupSize = FRayTracedTranslucencyHardwareRayTracingRGS::GetThreadGroupSize();
		DispatchLumenReflectionHardwareRayTracingIndirectArgs(GraphBuilder, View, HardwareRayTracingIndirectArgsBuffer, CompactedTraceTexelAllocator, OutputThreadGroupSize, ComputePassFlags);

		FRayTracedTranslucencyHardwareRayTracingRGS::FParameters* Parameters = GraphBuilder.AllocParameters<FRayTracedTranslucencyHardwareRayTracingRGS::FParameters>();
		{
			SetLumenHardwareRayTracingSharedParameters(
				GraphBuilder,
				SceneTextureParameters,
				View,
				TracingParameters,
				&Parameters->SharedParameters);

			Parameters->HardwareRayTracingIndirectArgs = HardwareRayTracingIndirectArgsBuffer;
			Parameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(CompactedTraceTexelAllocator, PF_R32_UINT);
			Parameters->CompactedTraceTexelData = GraphBuilder.CreateSRV(CompactedTraceTexelData, PF_R32_UINT);

			Parameters->TranslucencyForceOpaque = RayTracedTranslucency::UseForceOpaque();
			Parameters->HitLightingShadowMode = LumenHardwareRayTracing::GetHitLightingShadowMode();
			Parameters->HitLightingShadowTranslucencyMode = LumenHardwareRayTracing::GetHitLightingShadowTranslucencyMode();
			Parameters->HitLightingDirectLighting = LumenHardwareRayTracing::UseHitLightingDirectLighting() ? 1 : 0;
			Parameters->HitLightingSkylight = LumenHardwareRayTracing::UseHitLightingSkylight(DiffuseIndirectMethod) ? 1 : 0;
			Parameters->UseReflectionCaptures = LumenHardwareRayTracing::UseReflectionCapturesForHitLighting();

			Parameters->SecondaryPathStartBias = FMath::Max(CVarLumenReflectionsHardwareRayTracingBias.GetValueOnRenderThread(), 0.f);
			Parameters->SecondaryPathStartNormalBias = FMath::Max(CVarLumenReflectionsHardwareRayTracingNormalBias.GetValueOnRenderThread(), 0.f);
			Parameters->PathThroughputThreshold = RayTracedTranslucency::GetPathThroughputThreshold();

			Parameters->MaxPrimaryHitEvents = RayTracedTranslucency::GetMaxPrimaryHitEvents(View);
			Parameters->MaxSecondaryHitEvents = RayTracedTranslucency::GetMaxSecondaryHitEvents(View);
			Parameters->SampleTranslucentReflectionInReflections = RayTracedTranslucency::AllowTranslucentReflectionInReflections();

			// Reflection-specific
			Parameters->ReflectionTracingParameters = ReflectionTracingParameters;
			Parameters->ReflectionTileParameters = ReflectionTileParameters;
			Parameters->RadianceCacheParameters = RadianceCacheParameters;

			if (bNeedTraceHairVoxel)
			{
				Parameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
			}
		}

		FRayTracedTranslucencyHardwareRayTracingRGS::AddLumenRayTracingDispatchIndirect(
			GraphBuilder,
			RDG_EVENT_NAME("RayTracedTranslucencyHardwareRayTracingRGS"),
			View,
			PermutationVector,
			Parameters,
			Parameters->HardwareRayTracingIndirectArgs,
			/*IndirectArgsOffset*/ 0,
			/*bUseMinimalPayload*/ false,
			ComputePassFlags);
	}
}

#endif // RHI_RAYTRACING

void RenderHardwareRayTracingTranslucency(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	const FSceneTextureParameters& SceneTextureParameters,
	const FScene* Scene,
	const FViewInfo& View,
	const FLumenCardTracingParameters& TracingParameters,
	const FLumenReflectionTracingParameters& ReflectionTracingParameters,
	const FLumenReflectionTileParameters& ReflectionTileParameters,
	float MaxTraceDistance,
	EDiffuseIndirectMethod DiffuseIndirectMethod,
	ERDGPassFlags ComputePassFlags,
	bool bUseRayTracedRefraction
)
{
#if RHI_RAYTRACING
	const bool bNeedTraceHairVoxel = false;
	const bool bUseRadianceCache = false;
	const bool bSampleSceneColorAtHit = false;

	const LumenRadianceCache::FRadianceCacheInterpolationParameters RadianceCacheParameters;

	checkf(ComputePassFlags != ERDGPassFlags::AsyncCompute, TEXT("Async Lumen HWRT is only supported for inline ray tracing"));

	const FIntPoint BufferSize = ReflectionTracingParameters.ReflectionTracingBufferSize;
	const int32 RayCount = BufferSize.X * BufferSize.Y;

	FCompactedReflectionTraceParameters CompactedTraceParameters = LumenReflections::CompactTraces(
		GraphBuilder,
		View,
		TracingParameters,
		ReflectionTracingParameters,
		ReflectionTileParameters,
		/*bCullByDistanceFromCamera*/ false,
		/*CompactionTracingEndDistanceFromCamera*/ 0.0f,
		MaxTraceDistance,
		ComputePassFlags);

	// Trace with hit-lighting
	{
		FRayTracedTranslucencyHardwareRayTracingRGS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FRayTracedTranslucencyHardwareRayTracingRGS::FUseRayTracedRefraction>(bUseRayTracedRefraction);
		PermutationVector.Set<FRayTracedTranslucencyHardwareRayTracingRGS::FUseShaderExecutionReordering>(LumenHardwareRayTracing::UseShaderExecutionReordering());
		PermutationVector = FRayTracedTranslucencyHardwareRayTracingRGS::RemapPermutation(PermutationVector);

		RayTracedTranslucency::DispatchRayGenOrComputeShader(
			GraphBuilder,
			SceneTextures,
			SceneTextureParameters,
			Scene,
			View,
			TracingParameters,
			ReflectionTracingParameters,
			ReflectionTileParameters,
			CompactedTraceParameters,
			RadianceCacheParameters,
			PermutationVector,
			DiffuseIndirectMethod,
			RayCount,
			bUseRadianceCache,
			bSampleSceneColorAtHit,
			bNeedTraceHairVoxel,
			ComputePassFlags);
	}
#endif
}
