// Copyright Epic Games, Inc. All Rights Reserved.

#include "LumenHardwareRayTracingCommon.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "ComponentRecreateRenderStateContext.h"
#include "LumenReflections.h"
#include "RayTracedTranslucency.h"
#include "LumenScreenProbeGather.h"
#include "LumenRadianceCache.h"
#include "LumenVisualize.h"
#include "RayTracing/RayTracing.h"
#include "Nanite/NaniteRayTracing.h"

static TAutoConsoleVariable<int32> CVarLumenUseHardwareRayTracing(
	TEXT("r.Lumen.HardwareRayTracing"),
	1,
	TEXT("Uses Hardware Ray Tracing for Lumen features, when available.\n")
	TEXT("Lumen will fall back to Software Ray Tracing otherwise.\n")
	TEXT("Note: Hardware ray tracing has significant scene update costs for\n")
	TEXT("scenes with more than 100k instances."),
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* InVariable)
	{
		// Recreate proxies so that FPrimitiveSceneProxy::UpdateVisibleInLumenScene() can pick up any changed state
		FGlobalComponentRecreateRenderStateContext Context;
	}),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

// Note: Driven by URendererSettings and must match the enum exposed there
static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingLightingMode(
	TEXT("r.Lumen.HardwareRayTracing.LightingMode"),
	0,
	TEXT("Determines the ray hit lighting mode:\n")
	TEXT("0 - Use Lumen Surface Cache for ray hit lighting. This method gives the best GI and reflection performance, but quality will be limited by how well surface cache represents given scene.\n")
	TEXT("1 - Calculate lighting at a ray hit point for GI and reflections. This will improve both GI and reflection quality, but greatly increases GPU cost, as full material and lighting will be evaluated at every hit point. Lumen Surface Cache will still be used for secondary bounces.\n")
	TEXT("2 - Calculate lighting at a ray hit point for reflections. This will improve reflection quality, but increases GPU cost, as full material needs to be evaluated and shadow rays traced. Lumen Surface Cache will still be used for GI and secondary bounces, including GI seen in reflections."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingHitLightingDirectLighting(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.DirectLighting"),
	1,
	TEXT("Whether to calculate direct lighting when doing Hit Lighting or sample it from the Surface Cache."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingHitLightingShadowMode(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.ShadowMode"),
	RAY_TRACING_SHADOWS_TYPE_SOFT,
	TEXT("Which shadow mode to use for calculating direct lighting in ray hits:\n")
	TEXT("0 - Disabled shadows\n")
	TEXT("1 - Hard shadows, but less noise\n")
	TEXT("2 - Area shadows, but more noise"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingHitLightingSkylight(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.Skylight"),
	2,
	TEXT("Whether to calculate unshadowed skylight when doing Hit Lighting or sample shadowed skylight from the Surface Cache.\n")
	TEXT("0 - Disabled\n")
	TEXT("1 - Enabled\n")
	TEXT("2 - Enabled only for standalone Lumen Reflections"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingHitLightingReflectionCaptures(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.ReflectionCaptures"),
	0,
	TEXT("Whether to apply Reflection Captures to ray hits when using Hit Lighting."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<bool> CVarLumenHardwareRayTracingHitLightingForceOpaque(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.ForceOpaque"),
	false,
	TEXT("Allow forcing hit lighting rays to be marked as opaque so they do not execute the Any Hit Shader:\n")
	TEXT("0 - Rays will execute the any hit shader, allowing masked materials to be seen correctly (default) \n")
	TEXT("1 - Rays are forced to be marked opaque which improves performance but may incorrectly deal with masked materials."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingHitLightingShadowTranslucencyMode(
	TEXT("r.Lumen.HardwareRayTracing.HitLighting.ShadowTranslucencyMode"),
	RAY_TRACING_SHADOWS_TRANSLUCENCY_TYPE_MASKED,
	TEXT("Controls how opacity is handled for shadow rays in hit lighting:\n")
	TEXT("0 - Rays will treat all geometry as opaque (even masked geometry). Meshes with multiple segments with different shadow casting settings won't be supported.\n")
	TEXT("1 - Rays will execute any-hit shaders on masked geometry and support shadow casting settings on mesh segments (default)\n")
	TEXT("2 - Rays will execute any-hit shaders on masked and translucent geometry, supporting fractional visiblity"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<bool> CVarLumenHardwareRayTracingShaderExecutionReordering(
	TEXT("r.Lumen.HardwareRayTracing.ShaderExecutionReordering"),
	true,
	TEXT("When true, use Shader Execution Reordering (SER) to improve coherence of material evaluation. This may improve performance for scenes with many materials. This has no effect if the hardware does not support SER."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenUseHardwareRayTracingInline(
	TEXT("r.Lumen.HardwareRayTracing.Inline"),
	1,
	TEXT("Uses Hardware Inline Ray Tracing for selected Lumen passes, when available.\n"),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<float> CVarLumenHardwareRayTracingPullbackBias(
	TEXT("r.Lumen.HardwareRayTracing.PullbackBias"),
	8.0,
	TEXT("Determines the pull-back bias when resuming a screen-trace ray (default = 8.0)"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenHardwareRayTracingFarFieldBias(
	TEXT("r.Lumen.HardwareRayTracing.FarFieldBias"),
	200.0f,
	TEXT("Determines bias for the far field traces. Default = 200"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingMaxIterations(
	TEXT("r.Lumen.HardwareRayTracing.MaxIterations"),
	8192,
	TEXT("Limit number of ray tracing traversal iterations on supported platfoms.\n"
		"Incomplete misses will be treated as hitting a black surface (can cause overocculsion).\n"
		"Incomplete hits will be treated as a hit (can cause leaking)."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenRadiosityHardwareRayTracingAvoidSelfIntersections(
	TEXT("r.Lumen.HardwareRayTracing.AvoidSelfIntersections"),
	3,
	TEXT("Whether to skip back face hits for a small distance in order to avoid self-intersections when BLAS mismatches rasterized geometry.\n")
	TEXT("0 - Disabled. May have extra leaking, but it's the fastest mode.\n")
	TEXT("1 - Enabled. This mode retraces to skip first backface hit up to r.Lumen.HardwareRayTracing.SkipBackFaceHitDistance. Good default on most platforms.\n")
	TEXT("2 - Enabled. This mode uses AHS to skip any backface hits up to r.Lumen.HardwareRayTracing.SkipBackFaceHitDistance. Faster on platforms with inline AHS support.\n")
	TEXT("3 - Enabled. Automatically chooses between mode 1 and 2 depending on platform for best performance."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingSurfaceCacheAlphaMasking(
	TEXT("r.Lumen.HardwareRayTracing.SurfaceCacheAlphaMasking"),
	0,
	TEXT("Whether to support alpha masking based on the surface cache alpha channel. Disabled by default, as it slows down ray tracing performance."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

static TAutoConsoleVariable<int32> CVarLumenHardwareRayTracingMeshSectionVisibilityTest(
	TEXT("r.Lumen.HardwareRayTracing.MeshSectionVisibilityTest"),
	1,
	TEXT("Whether to test mesh section visibility at runtime.\n")
	TEXT("When enabled translucent mesh sections are automatically hidden based on the material, but it slows down performance due to extra visibility tests per intersection.\n")
	TEXT("When disabled translucent meshes can be hidden only if they are fully translucent. Individual mesh sections need to be hidden upfront inside the static mesh editor."),
	ECVF_RenderThreadSafe | ECVF_Scalability
);

TAutoConsoleVariable<float> CVarLumenHardwareRayTracingMinTraceDistanceToSampleSurfaceCache(
	TEXT("r.Lumen.HardwareRayTracing.MinTraceDistanceToSampleSurfaceCache"),
	10.0f,
	TEXT("Ray hit distance from which we can start sampling surface cache in order to fix feedback loop where surface cache texel hits itself and propagates lighting."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<float> CVarLumenHardwareRayTracingSurfaceCacheSamplingDepthBias(
	TEXT("r.Lumen.HardwareRayTracing.SurfaceCacheSampling.DepthBias"),
	10.0f,
	TEXT("Max distance to project a texel from a mesh card onto a hit point. Higher values will fix issues of mismatch between ray tracing geometry and rasterization, but will also increase leaking."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

bool Lumen::UseHardwareRayTracing(const FSceneViewFamily& ViewFamily)
{
#if RHI_RAYTRACING
	return IsRayTracingEnabled(ViewFamily.GetShaderPlatform())
		&& (LumenHardwareRayTracing::IsInlineSupported() || LumenHardwareRayTracing::IsRayGenSupported())
		&& CVarLumenUseHardwareRayTracing.GetValueOnAnyThread() != 0
		// HWRT does not support multiple views yet due to TLAS, but stereo views can be allowed as they reuse TLAS for View[0]
		&& (ViewFamily.Views.Num() == 1 || (ViewFamily.Views.Num() == 2 && IStereoRendering::IsStereoEyeView(*ViewFamily.Views[0])))
		&& ViewFamily.Views[0]->IsRayTracingAllowedForView();
#else
	return false;
#endif
}

bool LumenHardwareRayTracing::IsInlineSupported()
{
	return GRHISupportsInlineRayTracing;
}

bool LumenHardwareRayTracing::IsRayGenSupported()
{
	// Indirect RayGen dispatch is required for Lumen RayGen shaders
	return GRHISupportsRayTracingShaders && GRHISupportsRayTracingDispatchIndirect;
}

LumenHardwareRayTracing::EAvoidSelfIntersectionsMode LumenHardwareRayTracing::GetAvoidSelfIntersectionsMode()
{
	int32 Mode = CVarLumenRadiosityHardwareRayTracingAvoidSelfIntersections.GetValueOnRenderThread();

	if (Mode == 3)
	{
		return GRHIGlobals.RayTracing.SupportsInlinedCallbacks ? LumenHardwareRayTracing::EAvoidSelfIntersectionsMode::AHS : LumenHardwareRayTracing::EAvoidSelfIntersectionsMode::Retrace;
	}
	else
	{
		return (LumenHardwareRayTracing::EAvoidSelfIntersectionsMode)FMath::Clamp(Mode, 0, (uint32)LumenHardwareRayTracing::EAvoidSelfIntersectionsMode::MAX - 1);
	}
}

bool LumenHardwareRayTracing::UseSurfaceCacheAlphaMasking()
{
	return CVarLumenHardwareRayTracingSurfaceCacheAlphaMasking.GetValueOnRenderThread() != 0;
}

bool Lumen::IsUsingRayTracingLightingGrid(const FSceneViewFamily& ViewFamily, const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod)
{
	if (UseHardwareRayTracing(ViewFamily) 
		&& (LumenReflections::UseHitLighting(View, DiffuseIndirectMethod)
			|| LumenVisualize::UseHitLighting(View, DiffuseIndirectMethod)
			|| LumenScreenProbeGather::UseHitLighting(View, DiffuseIndirectMethod)
			|| LumenRadianceCache::UseHitLighting(View, DiffuseIndirectMethod)
			|| RayTracedTranslucency::IsEnabled(View)))
	{
		return true;
	}

	return false;
}

void LumenHardwareRayTracing::SetRayTracingSceneOptions(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod, EReflectionsMethod ReflectionsMethod, RayTracing::FSceneOptions& SceneOptions)
{
	if (ReflectionsMethod == EReflectionsMethod::Lumen
		&& LumenReflections::UseHitLighting(View, DiffuseIndirectMethod) 
		&& LumenReflections::UseTranslucentRayTracing(View))
	{
		SceneOptions.bTranslucentGeometry = true;
	}

	if (RayTracedTranslucency::IsEnabled(View))
	{
		SceneOptions.bTranslucentGeometry = true;
	}
}

LumenHardwareRayTracing::EHitLightingMode LumenHardwareRayTracing::GetHitLightingMode(const FViewInfo& View, EDiffuseIndirectMethod DiffuseIndirectMethod)
{
#if RHI_RAYTRACING
	if (!LumenHardwareRayTracing::IsRayGenSupported())
	{
		return LumenHardwareRayTracing::EHitLightingMode::SurfaceCache;
	}
	
	if (DiffuseIndirectMethod != EDiffuseIndirectMethod::Lumen)
	{
		// Force HitLightingForReflections when using standalone Lumen Reflections
		return LumenHardwareRayTracing::EHitLightingMode::HitLightingForReflections;
	}

	int32 LightingModeInt = CVarLumenHardwareRayTracingLightingMode.GetValueOnAnyThread();

	// Without ray tracing shaders (RayGen) support we can only use Surface Cache mode.
	if (View.FinalPostProcessSettings.LumenRayLightingMode == ELumenRayLightingModeOverride::SurfaceCache || !LumenHardwareRayTracing::IsRayGenSupported())
	{
		LightingModeInt = static_cast<int32>(LumenHardwareRayTracing::EHitLightingMode::SurfaceCache);
	}
	else if (View.FinalPostProcessSettings.LumenRayLightingMode == ELumenRayLightingModeOverride::HitLightingForReflections)
	{
		LightingModeInt = static_cast<int32>(LumenHardwareRayTracing::EHitLightingMode::HitLightingForReflections);
	}
	else if (View.FinalPostProcessSettings.LumenRayLightingMode == ELumenRayLightingModeOverride::HitLighting)
	{
		LightingModeInt = static_cast<int32>(LumenHardwareRayTracing::EHitLightingMode::HitLighting);
	}

	LightingModeInt = FMath::Clamp<int32>(LightingModeInt, 0, (int32)LumenHardwareRayTracing::EHitLightingMode::MAX - 1);
	return static_cast<LumenHardwareRayTracing::EHitLightingMode>(LightingModeInt);
#else
	return LumenHardwareRayTracing::EHitLightingMode::SurfaceCache;
#endif
}

uint32 LumenHardwareRayTracing::GetHitLightingShadowMode()
{
	return FMath::Clamp(CVarLumenHardwareRayTracingHitLightingShadowMode.GetValueOnRenderThread(), RAY_TRACING_SHADOWS_TYPE_OFF, RAY_TRACING_SHADOWS_TYPE_SOFT);
}

uint32 LumenHardwareRayTracing::GetHitLightingShadowTranslucencyMode()
{
	return FMath::Clamp(CVarLumenHardwareRayTracingHitLightingShadowTranslucencyMode.GetValueOnRenderThread(), RAY_TRACING_SHADOWS_TRANSLUCENCY_TYPE_OPAQUE, RAY_TRACING_SHADOWS_TRANSLUCENCY_TYPE_FRACTIONAL_VISIBILITY);
}

bool LumenHardwareRayTracing::UseHitLightingForceOpaque()
{
	return CVarLumenHardwareRayTracingHitLightingForceOpaque.GetValueOnRenderThread() != 0;
}

bool LumenHardwareRayTracing::UseHitLightingDirectLighting()
{
	return CVarLumenHardwareRayTracingHitLightingDirectLighting.GetValueOnRenderThread() != 0;
}

bool LumenHardwareRayTracing::UseHitLightingSkylight(EDiffuseIndirectMethod DiffuseIndirectMethod)
{
	if (CVarLumenHardwareRayTracingHitLightingSkylight.GetValueOnRenderThread() == 2)
	{
		// Standalone Lumen Reflections enabled sky light by default in mode 2
		return DiffuseIndirectMethod != EDiffuseIndirectMethod::Lumen;
	}

	return CVarLumenHardwareRayTracingHitLightingSkylight.GetValueOnRenderThread() != 0;
}

bool LumenHardwareRayTracing::UseReflectionCapturesForHitLighting()
{
	int32 UseReflectionCaptures = CVarLumenHardwareRayTracingHitLightingReflectionCaptures.GetValueOnRenderThread();
	return UseReflectionCaptures != 0;
}

bool Lumen::UseHardwareInlineRayTracing(const FSceneViewFamily& ViewFamily)
{
#if RHI_RAYTRACING
	if (Lumen::UseHardwareRayTracing(ViewFamily)
		&& LumenHardwareRayTracing::IsInlineSupported()
		// Can't disable inline tracing if RayGen isn't supported
		&& (CVarLumenUseHardwareRayTracingInline.GetValueOnRenderThread() != 0 || !LumenHardwareRayTracing::IsRayGenSupported()))
	{
		return true;
	}
#endif

	return false;
}

bool LumenHardwareRayTracing::UseShaderExecutionReordering()
{
	// If current hardware suports it and user asked for it to be enabled
	return GRHIGlobals.SupportsShaderExecutionReordering && CVarLumenHardwareRayTracingShaderExecutionReordering.GetValueOnRenderThread();
}

float LumenHardwareRayTracing::GetFarFieldBias()
{
	return FMath::Max(CVarLumenHardwareRayTracingFarFieldBias.GetValueOnRenderThread(), 0.0f);
}

#if RHI_RAYTRACING

FLumenHardwareRayTracingShaderBase::FLumenHardwareRayTracingShaderBase() = default;
FLumenHardwareRayTracingShaderBase::FLumenHardwareRayTracingShaderBase(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
	: FGlobalShader(Initializer)
{
}

void FLumenHardwareRayTracingShaderBase::ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType, Lumen::ESurfaceCacheSampling SurfaceCacheSampling, FShaderCompilerEnvironment& OutEnvironment)
{
	OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_FEEDBACK"), SurfaceCacheSampling == Lumen::ESurfaceCacheSampling::AlwaysResidentPagesWithoutFeedback ? 0 : 1);
	OutEnvironment.SetDefine(TEXT("SURFACE_CACHE_HIGH_RES_PAGES"), SurfaceCacheSampling == Lumen::ESurfaceCacheSampling::HighResPages ? 1 : 0);
	OutEnvironment.SetDefine(TEXT("LUMEN_HARDWARE_RAYTRACING"), 1);

	// GPU Scene definitions
	OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);

	// Inline
	const bool bInlineRayTracing = ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline;
	if (bInlineRayTracing)
	{
		OutEnvironment.SetDefine(TEXT("LUMEN_HARDWARE_INLINE_RAYTRACING"), 1);
		OutEnvironment.CompilerFlags.Add(CFLAG_InlineRayTracing);
	}
}

void FLumenHardwareRayTracingShaderBase::ModifyCompilationEnvironmentInternal(Lumen::ERayTracingShaderDispatchType ShaderDispatchType, bool UseThreadGroupSize64, FShaderCompilerEnvironment& OutEnvironment)
{
	const bool bInlineRayTracing = ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline;
	if (bInlineRayTracing && !UseThreadGroupSize64)
	{
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
}

FIntPoint FLumenHardwareRayTracingShaderBase::GetThreadGroupSizeInternal(Lumen::ERayTracingShaderDispatchType ShaderDispatchType, bool UseThreadGroupSize64)
{
	// Current inline ray tracing implementation requires 1:1 mapping between thread groups and waves.
	const bool bInlineRayTracing = ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline;
	if (bInlineRayTracing)
	{
		return UseThreadGroupSize64 ? FIntPoint(64, 1) : FIntPoint(32, 1);
	}

	return FIntPoint(1, 1);
}

bool FLumenHardwareRayTracingShaderBase::ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters, Lumen::ERayTracingShaderDispatchType ShaderDispatchType)
{
	const bool bInlineRayTracing = ShaderDispatchType == Lumen::ERayTracingShaderDispatchType::Inline;
	if (bInlineRayTracing)
	{
		return IsRayTracingEnabledForProject(Parameters.Platform) && RHISupportsRayTracing(Parameters.Platform) && RHISupportsInlineRayTracing(Parameters.Platform);
	}
	else
	{
		return ShouldCompileRayTracingShadersForProject(Parameters.Platform);
	}
}

bool FLumenHardwareRayTracingShaderBase::UseThreadGroupSize64(EShaderPlatform ShaderPlatform)
{
	return !Lumen::UseThreadGroupSize32() && RHISupportsWaveSize64(ShaderPlatform);
}

namespace Lumen
{
	const TCHAR* GetRayTracedNormalModeName(int NormalMode)
	{
		if (NormalMode == 0)
		{
			return TEXT("SDF");
		}

		return TEXT("Geometry");
	}

	float GetHardwareRayTracingPullbackBias()
	{
		return CVarLumenHardwareRayTracingPullbackBias.GetValueOnRenderThread();
	}
}

void SetLumenHardwareRayTracingSharedParameters(
	FRDGBuilder& GraphBuilder,
	const FSceneTextureParameters& SceneTextures,
	const FViewInfo& View,
	const FLumenCardTracingParameters& TracingParameters,
	FLumenHardwareRayTracingShaderBase::FSharedParameters* SharedParameters)
{
	SharedParameters->SceneTextures = SceneTextures;
	SharedParameters->SceneTexturesStruct = View.GetSceneTextures().UniformBuffer;
	SharedParameters->Substrate = Substrate::BindSubstrateGlobalUniformParameters(View);

	//SharedParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	checkf(View.HasRayTracingScene(), TEXT("TLAS does not exist. Verify that the current pass is represented in Lumen::AnyLumenHardwareRayTracingPassEnabled()."));
	SharedParameters->TLAS = View.GetRayTracingSceneLayerViewChecked(ERayTracingSceneLayer::Base);
	SharedParameters->FarFieldTLAS = View.GetRayTracingSceneLayerViewChecked(ERayTracingSceneLayer::FarField);

	// Lighting data
	SharedParameters->LightGridParameters = View.RayTracingLightGridUniformBuffer;
	SharedParameters->ReflectionCapture = View.ReflectionCaptureUniformBuffer;
	SharedParameters->ForwardLightStruct = View.ForwardLightingResources.ForwardLightUniformBuffer;

	// Inline
	// TODO: Do we still need to use View.GetPrimaryView()? And if so should it also be done when initializing RayTracingSceneMetadata?
	checkf(View.VisibleRayTracingShaderBindingsFinalizeTask.IsCompleted(), TEXT("VisibleRayTracingShaderBindingsFinalizeTask must be completed before accessing Lumen HWRT data."));
	SharedParameters->HitGroupData = View.GetPrimaryView()->LumenHardwareRayTracingHitDataBuffer ? GraphBuilder.CreateSRV(View.GetPrimaryView()->LumenHardwareRayTracingHitDataBuffer) : nullptr;
	SharedParameters->LumenHardwareRayTracingUniformBuffer = View.GetPrimaryView()->LumenHardwareRayTracingUniformBuffer;
	SharedParameters->RayTracingSceneMetadata = View.InlineRayTracingBindingDataBuffer ? GraphBuilder.CreateSRV(View.InlineRayTracingBindingDataBuffer) : nullptr;
	SharedParameters->RWInstanceHitCountBuffer = View.GetRayTracingInstanceHitCountUAV(GraphBuilder);
	SharedParameters->NaniteRayTracing = Nanite::GRayTracingManager.GetUniformBuffer();
	
	// Lumen
	SharedParameters->TracingParameters = TracingParameters;
	SharedParameters->MaxTraversalIterations = FMath::Max(CVarLumenHardwareRayTracingMaxIterations.GetValueOnRenderThread(), 1);
	SharedParameters->MinTraceDistanceToSampleSurfaceCache = CVarLumenHardwareRayTracingMinTraceDistanceToSampleSurfaceCache.GetValueOnRenderThread();
	SharedParameters->SurfaceCacheSamplingDepthBias = CVarLumenHardwareRayTracingSurfaceCacheSamplingDepthBias.GetValueOnRenderThread();
	SharedParameters->MeshSectionVisibilityTest = CVarLumenHardwareRayTracingMeshSectionVisibilityTest.GetValueOnRenderThread();
}

#endif // RHI_RAYTRACING