// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	Renderer.cpp: Renderer module implementation.
=============================================================================*/

#include "CoreMinimal.h"
#include "Misc/CoreMisc.h"
#include "Stats/Stats.h"
#include "Modules/ModuleManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "EngineDefines.h"
#include "EngineGlobals.h"
#include "RenderingThread.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "RenderTargetPool.h"
#include "PostProcess/SceneRenderTargets.h"
#include "VisualizeTexture.h"
#include "SceneCore.h"
#include "SceneHitProxyRendering.h"
#include "SceneRendering.h"
#include "BasePassRendering.h"
#include "MobileBasePassRendering.h"
#include "TranslucentRendering.h"
#include "RendererModule.h"
#include "GPUBenchmark.h"
#include "SystemSettings.h"
#include "VisualizeTexturePresent.h"
#include "MeshPassProcessor.inl"
#include "DebugViewModeRendering.h"
#include "EditorPrimitivesRendering.h"
#include "VisualizeTexturePresent.h"
#include "ScreenSpaceDenoise.h"
#include "VT/VirtualTextureFeedbackResource.h"
#include "VT/VirtualTextureSystem.h"
#include "PostProcess/TemporalAA.h"
#include "CanvasRender.h"
#include "RendererOnScreenNotification.h"
#include "Nanite/NaniteRayTracing.h"
#include "Lumen/Lumen.h"
#include "ScenePrivate.h"
#include "SceneUniformBuffer.h"
#include "SceneRenderTargetParameters.h"
#include "EngineModule.h"
#include "RendererInterface.h"
#include "PrimitiveSceneShaderData.h"
#include "MeshDrawCommandStats.h"
#include "LocalFogVolumeRendering.h"
#include "Rendering/RayTracingGeometryManager.h"
#include "PathTracing.h"
#include "LightFunctionAtlas.h"
#include "SceneRenderBuilder.h"

DEFINE_LOG_CATEGORY(LogRenderer);

IMPLEMENT_MODULE(FRendererModule, Renderer);

#if !IS_MONOLITHIC
	// visual studio cannot find cross dll data for visualizers
	// thus as a workaround for now, copy and paste this into every module
	// where we need to visualize SystemSettings
	FSystemSettings* GSystemSettingsForVisualizers = &GSystemSettings;
#endif

static int32 bFlushRenderTargetsOnWorldCleanup = 1;
FAutoConsoleVariableRef CVarFlushRenderTargetsOnWorldCleanup(TEXT("r.bFlushRenderTargetsOnWorldCleanup"), bFlushRenderTargetsOnWorldCleanup, TEXT(""));

static int32 bBindTileMeshDrawingDummyRenderTarget = 0;
static FAutoConsoleVariableRef CVarBindTileMeshDrawingDummyRenderTarget(
	TEXT("r.DrawTileMesh.DummyRT"),
	bBindTileMeshDrawingDummyRenderTarget,
	TEXT("Enable binding of a dummy render target for tile mesh drawing to workaround driver bugs.")
);

void FRendererModule::StartupModule()
{
#if MESH_DRAW_COMMAND_STATS
	FMeshDrawCommandStatsManager::CreateInstance();
#endif

	GScreenSpaceDenoiser = IScreenSpaceDenoiser::GetDefaultDenoiser();

	FRendererOnScreenNotification::Get();
	FVirtualTextureSystem::Initialize();

#if RHI_RAYTRACING
	GRayTracingGeometryManager = new FRayTracingGeometryManager();

	Nanite::GRayTracingManager.Initialize();
#endif

	StopRenderingThreadDelegate = RegisterStopRenderingThreadDelegate(FStopRenderingThreadDelegate::CreateLambda([this]
	{
		ENQUEUE_RENDER_COMMAND(FSceneRendererCleanUp)(
			[](FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder::WaitForAsyncDeleteTask();
			FSceneRenderBuilder::WaitForAsyncDeleteTask();
		});
	}));

	// Needs to run on startup, after static init.
	GIdentityPrimitiveUniformBuffer.InitContents();
	GDistanceCullFadedInUniformBuffer.InitContents();
	GDitherFadedInUniformBuffer.InitContents();

#if RHI_RAYTRACING && WITH_EDITOR
	if (FApp::CanEverRender() && !FApp::IsUnattended())
	{
		FCoreDelegates::OnPostEngineInit.AddLambda([]() {
			// We add this step via the PostEngineInit delegate so that it can run after PostInitRHI has run,
			// and the rendering thread has been started so that we are able to create RTPSOs.
			// For now, we only attempt to create the PathTracer RTPSO as it is the most expensive to compile by far.
			// See UE-190955 for example timings.
			PreparePathTracingRTPSO();
		});
	}
#endif
}

void FRendererModule::ShutdownModule()
{
	UnregisterStopRenderingThreadDelegate(StopRenderingThreadDelegate);

#if RHI_RAYTRACING
	Nanite::GRayTracingManager.Shutdown();

	delete GRayTracingGeometryManager;
	GRayTracingGeometryManager = nullptr;
#endif

	FVirtualTextureSystem::Shutdown();
	FRendererOnScreenNotification::TearDown();

	// Free up the memory of the default denoiser. Responsibility of the plugin to free up theirs.
	delete IScreenSpaceDenoiser::GetDefaultDenoiser();

	// Free up global resources in Lumen
	Lumen::Shutdown();

	void CleanupOcclusionSubmittedFence();
	CleanupOcclusionSubmittedFence();
}

void FRendererModule::OnWorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources, bool bWorldChanged)
{
	FSceneInterface* Scene = World->Scene;
	ENQUEUE_RENDER_COMMAND(OnWorldCleanup)(
	[Scene, bWorldChanged](FRHICommandListImmediate& RHICmdList)
	{
		if(bFlushRenderTargetsOnWorldCleanup > 0)
		{
			GRenderTargetPool.FreeUnusedResources();
		}
		if(bWorldChanged && Scene)
		{
			Scene->OnWorldCleanup();
		}
	});

}

void FRendererModule::InitializeSystemTextures(FRHICommandListImmediate& RHICmdList)
{
	GSystemTextures.InitializeTextures(RHICmdList, GMaxRHIFeatureLevel);
}

BEGIN_SHADER_PARAMETER_STRUCT(FDrawTileMeshPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureShaderData, ReflectionCapture)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FDebugViewModePassUniformParameters, DebugViewMode)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FTranslucentBasePassUniformParameters, TranslucentBasePass)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FOpaqueBasePassUniformParameters, OpaqueBasePass)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMobileBasePassUniformParameters, MobileBasePass)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

FSceneUniformBuffer* FRendererModule::CreateSinglePrimitiveSceneUniformBuffer(FRDGBuilder& GraphBuilder, const FViewInfo& SceneView, FMeshBatch& Mesh)
{
	return CreateSinglePrimitiveSceneUniformBuffer(GraphBuilder, SceneView.FeatureLevel, Mesh);
}

FSceneUniformBuffer* FRendererModule::CreateSinglePrimitiveSceneUniformBuffer(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel, FMeshBatch& Mesh)
{
	FSceneUniformBuffer& SceneUniforms = *GraphBuilder.AllocObject<FSceneUniformBuffer>();

	if (Mesh.VertexFactory->GetPrimitiveIdStreamIndex(FeatureLevel, EVertexInputStreamType::PositionOnly) >= 0)
	{
		FMeshBatchElement& MeshElement = Mesh.Elements[0];

		checkf(Mesh.Elements.Num() == 1, TEXT("Only 1 batch element currently supported by CreateSinglePrimitiveSceneUniformBuffer"));
		checkf(MeshElement.PrimitiveUniformBuffer == nullptr, TEXT("CreateSinglePrimitiveSceneUniformBuffer does not currently support an explicit primitive uniform buffer on vertex factories which manually fetch primitive data.  Use PrimitiveUniformBufferResource instead."));

		if (MeshElement.PrimitiveUniformBufferResource)
		{
			checkf(MeshElement.NumInstances == 1, TEXT("CreateSinglePrimitiveSceneUniformBuffer does not currently support instancing"));
			// Force PrimitiveId to be 0 in the shader
			MeshElement.PrimitiveIdMode = PrimID_ForceZero;

			// Set the LightmapID to 0, since that's where our light map data resides for this primitive
			FPrimitiveUniformShaderParameters PrimitiveParams = *(const FPrimitiveUniformShaderParameters*)MeshElement.PrimitiveUniformBufferResource->GetContents();
			PrimitiveParams.LightmapDataIndex = 0;
			PrimitiveParams.LightmapUVIndex = 0;

			// Set up reference to the single-instance 
			PrimitiveParams.InstanceSceneDataOffset = 0;
			PrimitiveParams.NumInstanceSceneDataEntries = 1;
			PrimitiveParams.InstancePayloadDataOffset = INDEX_NONE;
			PrimitiveParams.InstancePayloadDataStride = 0;
			
			// Now we just need to fill out the first entry of primitive data in a buffer and bind it
			FPrimitiveSceneShaderData PrimitiveSceneData(PrimitiveParams);

			// Also fill out correct single-primitive instance data, derived from the primitive.
			FInstanceSceneShaderData InstanceSceneData{};
			InstanceSceneData.BuildInternal
			(
				0 /* Primitive Id */,
				0 /* Relative Instance Id */,
				0 /* Payload Data Flags */,
				INVALID_LAST_UPDATE_FRAME,
				0 /* Custom Data Count */,
				0.0f /* Random ID */,
				PrimitiveParams.LocalToRelativeWorld,
				true,
				FInstanceSceneShaderData::SupportsCompressedTransforms()
			);

			// Set up the parameters for the LightmapSceneData from the given LCI data 
			FPrecomputedLightingUniformParameters LightmapParams;
			GetPrecomputedLightingParameters(FeatureLevel, LightmapParams, Mesh.LCI);
			FLightmapSceneShaderData LightmapSceneData(LightmapParams);

			FRDGBufferRef PrimitiveSceneDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("PrimitiveSceneDataBuffer"), TConstArrayView<FVector4f>(PrimitiveSceneData.Data));
			FRDGBufferRef LightmapSceneDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("LightmapSceneDataBuffer"), TConstArrayView<FVector4f>(LightmapSceneData.Data));
			FRDGBufferRef InstanceSceneDataBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("InstanceSceneDataBuffer"), TConstArrayView<FVector4f>(InstanceSceneData.Data));
			FRDGBufferRef InstancePayloadDataBuffer = GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FVector4f));
			FRDGBufferRef DummyBufferLight = GSystemTextures.GetDefaultByteAddressBuffer(GraphBuilder, sizeof(FLightSceneData));

			FGPUSceneResourceParameters ShaderParameters;
			ShaderParameters.GPUScenePrimitiveSceneData = GraphBuilder.CreateSRV(PrimitiveSceneDataBuffer);
			ShaderParameters.GPUSceneInstanceSceneData = GraphBuilder.CreateSRV(InstanceSceneDataBuffer);
			ShaderParameters.GPUSceneInstancePayloadData = GraphBuilder.CreateSRV(InstancePayloadDataBuffer);
			ShaderParameters.GPUSceneLightmapData = GraphBuilder.CreateSRV(LightmapSceneDataBuffer);
			ShaderParameters.GPUSceneLightData = GraphBuilder.CreateSRV(DummyBufferLight);
			ShaderParameters.CommonParameters.GPUSceneMaxAllocatedInstanceId = 1;
			ShaderParameters.CommonParameters.GPUSceneMaxPersistentPrimitiveIndex = 1;
			ShaderParameters.CommonParameters.GPUSceneInstanceDataTileSizeLog2 = 0;
			ShaderParameters.CommonParameters.GPUSceneInstanceDataTileSizeMask = 1;
			ShaderParameters.CommonParameters.GPUSceneInstanceDataTileStride = 0;
	
			SceneUniforms.Set(SceneUB::GPUScene, ShaderParameters);
		}
	}

	return &SceneUniforms;
}

TRDGUniformBufferRef<FBatchedPrimitiveParameters> FRendererModule::CreateSinglePrimitiveUniformView(FRDGBuilder& GraphBuilder, const FViewInfo& SceneView, FMeshBatch& Mesh)
{
	return CreateSinglePrimitiveUniformView(GraphBuilder, SceneView.FeatureLevel, SceneView.GetShaderPlatform(), Mesh);
}

TRDGUniformBufferRef<FBatchedPrimitiveParameters> FRendererModule::CreateSinglePrimitiveUniformView(FRDGBuilder& GraphBuilder, ERHIFeatureLevel::Type FeatureLevel, EShaderPlatform ShaderPlatform, FMeshBatch& Mesh)
{
	check(PlatformGPUSceneUsesUniformBufferView(ShaderPlatform));

	FBatchedPrimitiveParameters* BatchedPrimitiveParameters = GraphBuilder.AllocParameters<FBatchedPrimitiveParameters>();

	FRDGBufferDesc PrimitiveDataBufferDesc = FRDGBufferDesc::CreateStructuredUploadDesc(16u, (PLATFORM_MAX_UNIFORM_BUFFER_RANGE / 16u));
	PrimitiveDataBufferDesc.Usage |= EBufferUsageFlags::UniformBuffer;
	FRDGBufferRef PrimitiveDataBuffer = nullptr;
	
	if (Mesh.VertexFactory->GetPrimitiveIdStreamIndex(FeatureLevel, EVertexInputStreamType::PositionOnly) >= 0)
	{
		FMeshBatchElement& MeshElement = Mesh.Elements[0];
		checkf(Mesh.Elements.Num() == 1, TEXT("Only 1 batch element currently supported by CreateSinglePrimitiveUniformView"));
		checkf(MeshElement.PrimitiveUniformBuffer == nullptr, TEXT("CreateSinglePrimitiveUniformView does not currently support an explicit primitive uniform buffer on vertex factories which manually fetch primitive data.  Use PrimitiveUniformBufferResource instead."));

		if (MeshElement.PrimitiveUniformBufferResource)
		{
			checkf(MeshElement.NumInstances == 1, TEXT("CreateSinglePrimitiveUniformView does not currently support instancing"));
			// Force PrimitiveId to be 0 in the shader
			MeshElement.PrimitiveIdMode = PrimID_ForceZero;
			FPrimitiveUniformShaderParameters PrimitiveParams = *(const FPrimitiveUniformShaderParameters*)MeshElement.PrimitiveUniformBufferResource->GetContents();
			// Now we just need to fill out the first entry of a batched primitive data in a buffer
			FBatchedPrimitiveShaderData ShaderData(PrimitiveParams);
			PrimitiveDataBuffer = GraphBuilder.CreateBuffer(PrimitiveDataBufferDesc, TEXT("SinglePrimitiveUniformView"));
			GraphBuilder.QueueBufferUpload(PrimitiveDataBuffer, ShaderData.Data.GetData(), ShaderData.Data.Num() * sizeof(FVector4f));
		}
	}

	if (PrimitiveDataBuffer == nullptr)
	{
		// Upload Identity parameters
		FBatchedPrimitiveShaderData ShaderData{};
		PrimitiveDataBuffer = GraphBuilder.CreateBuffer(PrimitiveDataBufferDesc, TEXT("SinglePrimitiveUniformView"));
		GraphBuilder.QueueBufferUpload(PrimitiveDataBuffer, ShaderData.Data.GetData(), ShaderData.Data.Num() * sizeof(FVector4f));
	}

	BatchedPrimitiveParameters->Data = GraphBuilder.CreateSRV(PrimitiveDataBuffer);
	return GraphBuilder.CreateUniformBuffer(BatchedPrimitiveParameters);
}

static float GetEmissiveMaxValueForPixelFormat(EPixelFormat PixelFormat)
{
	switch (PixelFormat)
	{
	// R11G11B10
	case PF_FloatR11G11B10:
	case PF_FloatRGB:
		return 64512.0f; // Max10BitsFloat

	// FP16
	case PF_FloatRGBA:
	case PF_G16R16F:
	case PF_G16R16F_FILTER:
	case PF_R16F:
	case PF_R16F_FILTER:
		return FFloat16::MaxF16Float;

	// FP32
	//case PF_R32_FLOAT:
	//case PF_G32R32F:
	//case PF_A32B32G32R32F:
	//default:
		// fall through
	}

	return UE_MAX_FLT; // default for FP32 and all other formats for now
}

void FRendererModule::DrawTileMesh(FCanvasRenderContext& RenderContext, FMeshPassProcessorRenderState& DrawRenderState, const FSceneView& SceneView, FMeshBatch& Mesh, bool bIsHitTesting, const FHitProxyId& HitProxyId, bool bUse128bitRT)
{
	if (!GUsingNullRHI)
	{
		// Create an FViewInfo so we can initialize its RHI resources
		//@todo - reuse this view for multiple tiles, this is going to be slow for each tile
		FViewInfo& View = *RenderContext.Alloc<FViewInfo>(&SceneView);
		View.ViewRect = View.UnscaledViewRect;
		FViewFamilyInfo* ViewFamily = RenderContext.Alloc<FViewFamilyInfo>(*SceneView.Family);
		ViewFamily->Views.Add(&View);
		ViewFamily->AllViews.Add(&View);
		View.Family = ViewFamily;

		// When rendering tiles, this may be to render data in a URenderTargetTexture. In this case we should not clamp so that all the expected values setup by the artists go through.
		if (RenderContext.GetRenderTarget())
		{
			View.MaterialMaxEmissiveValue = GetEmissiveMaxValueForPixelFormat(RenderContext.GetRenderTarget()->Desc.Format);
		}

		// Default init of SceneTexturesConfig will take extents from FSceneTextureExtentState.  We want the view extents, so explicitly
		// set that.  This will bypass scene texture extent caching logic, but this code path doesn't allocate scene textures (it renders
		// directly to RenderContext.GetRenderTarget()), so caching is irrelevant for purposes of avoiding render target pool thrashing.
		InitializeSceneTexturesConfig(ViewFamily->SceneTexturesConfig, *ViewFamily, View.ViewRect.Size());

		const auto FeatureLevel = View.GetFeatureLevel();
		const EShadingPath ShadingPath = GetFeatureLevelShadingPath(FeatureLevel);

		FScene* Scene = ViewFamily->Scene ? ViewFamily->Scene->GetRenderScene() : nullptr;

		Mesh.MaterialRenderProxy->UpdateUniformExpressionCacheIfNeeded(FeatureLevel);
		FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

		FRDGBuilder& GraphBuilder = RenderContext.GraphBuilder;
		
		FSceneUniformBuffer& SceneUniforms = *CreateSinglePrimitiveSceneUniformBuffer(GraphBuilder, FeatureLevel, Mesh);

		if (!FRDGSystemTextures::IsValid(GraphBuilder))
		{
			FRDGSystemTextures::Create(GraphBuilder);
		}

		// Materials sampling VTs need FVirtualTextureSystem to be updated before being rendered.
		// Note that we any VTs dependent on having a SceneRenderer (eg RVT) cannot be warmed up here.
		const FMaterial& MeshMaterial = Mesh.MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel);
		const bool bUseVirtualTexturing = UseVirtualTexturing(View.GetShaderPlatform()) && !MeshMaterial.GetUniformVirtualTextureExpressions().IsEmpty();
		if (bUseVirtualTexturing)
		{
			FVirtualTextureUpdateSettings Settings;
			Settings.EnableThrottling(false);
			FVirtualTextureSystem::Get().Update(GraphBuilder, FeatureLevel, nullptr, Settings);

			VirtualTextureFeedbackBegin(GraphBuilder, TArrayView<const FViewInfo>(&View, 1), RenderContext.GetViewportRect().Size());
		}

		View.InitRHIResources();
		View.ForwardLightingResources.SetUniformBuffer(CreateDummyForwardLightUniformBuffer(GraphBuilder, View.GetShaderPlatform()));
		SetDummyLocalFogVolumeForView(GraphBuilder, View);

		// Create a disabled LightFunctionAtlas to be able to render base pass.
		LightFunctionAtlas::FLightFunctionAtlas LightFunctionAtlas;
		LightFunctionAtlas::FLightFunctionAtlasSceneData LightFunctionAtlasSceneData;
		LightFunctionAtlas.ClearEmptySceneFrame(&View, 0u, &LightFunctionAtlasSceneData);

		TUniformBufferRef<FReflectionCaptureShaderData> EmptyReflectionCaptureUniformBuffer;

		{
			FReflectionCaptureShaderData EmptyData;
			EmptyReflectionCaptureUniformBuffer = TUniformBufferRef<FReflectionCaptureShaderData>::CreateUniformBufferImmediate(EmptyData, UniformBuffer_SingleFrame);
		}

		RDG_EVENT_SCOPE(GraphBuilder, "DrawTileMesh");

		auto* PassParameters = GraphBuilder.AllocParameters<FDrawTileMeshPassParameters>();
		PassParameters->RenderTargets[0] = FRenderTargetBinding(RenderContext.GetRenderTarget(), ERenderTargetLoadAction::ELoad);
		PassParameters->View = View.GetShaderParameters();
		PassParameters->InstanceCullingDrawParams.Scene = SceneUniforms.GetBuffer(GraphBuilder);
		PassParameters->InstanceCullingDrawParams.InstanceCulling = FInstanceCullingContext::CreateDummyInstanceCullingUniformBuffer(GraphBuilder);
		PassParameters->ReflectionCapture = EmptyReflectionCaptureUniformBuffer;

		// FORT-702555, FORT-819709 It is done to workaround a GLES driver bug on old Adreno 6xx drivers.
		if (IsAndroidOpenGLESPlatform(ViewFamily->GetShaderPlatform()) && bBindTileMeshDrawingDummyRenderTarget)
		{
			// Unfortunately, this unused render-target should match the size of actually
			// used one to be GL spec conformant. It states the following:
			// "If the attachment sizes are not all identical, the results of rendering are de-
			// fined only within the largest area that can fit in all of the attachments."
			// Hence, this fix hogs some memory.
			FSceneTexturesConfig& Config = ViewFamily->SceneTexturesConfig;
			FRDGTextureDesc RenderTargetDesc = FRDGTextureDesc::Create2D(Config.Extent, PF_R16F, FClearValueBinding(FLinearColor::Transparent), TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead);
			FRDGTextureRef DummyRenderTarget = GraphBuilder.CreateTexture(RenderTargetDesc, TEXT("DummyTileMeshRT"));
			PassParameters->RenderTargets[1] = FRenderTargetBinding(DummyRenderTarget, ERenderTargetLoadAction::ENoAction);
		}

		// Disable parallel setup tasks since we are only processing one mesh (not worth the task launch cost).
		const bool bForceStereoInstancingOff = false;
		const bool bForceParallelSetupOff = true;

		// handle translucent material blend modes, not relevant in MaterialTexCoordScalesAnalysis since it outputs the scales.
		if (ViewFamily->GetDebugViewShaderMode() == DVSM_OutputMaterialTextureScales)
		{
#if WITH_DEBUG_VIEW_MODES
			// make sure we are doing opaque drawing
			DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
			
			// is this path used on mobile?
			if (ShadingPath == EShadingPath::Deferred)
			{
				PassParameters->DebugViewMode = CreateDebugViewModePassUniformBuffer(GraphBuilder, View, nullptr);

				AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("OutputMaterialTextureScales"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
					[Scene, &View, &Mesh](FMeshPassDrawListContext* InDrawListContext)
				{
					FDebugViewModeMeshProcessor PassMeshProcessor(
						Scene,
						View.GetFeatureLevel(),
						&View,
						false,
						InDrawListContext);
					const uint64 DefaultBatchElementMask = ~0ull;
					PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);

				}, bForceStereoInstancingOff, bForceParallelSetupOff);
			}
#endif // WITH_DEBUG_VIEW_MODES
		}
		else if (IsTranslucentBlendMode(MeshMaterial))
		{
			if (ShadingPath == EShadingPath::Deferred)
			{
				PassParameters->TranslucentBasePass = CreateTranslucentBasePassUniformBuffer(GraphBuilder, Scene, View);

				AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("TranslucentDeferred"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
					[Scene, &View, &Mesh, DrawRenderState, bUse128bitRT](FMeshPassDrawListContext* DynamicMeshPassContext)
				{
					FBasePassMeshProcessor PassMeshProcessor(
						EMeshPass::BasePass,
						Scene,
						View.GetFeatureLevel(),
						&View,
						DrawRenderState,
						DynamicMeshPassContext,
						bUse128bitRT ? FBasePassMeshProcessor::EFlags::bRequires128bitRT : FBasePassMeshProcessor::EFlags::None,
						ETranslucencyPass::TPT_AllTranslucency);

					const uint64 DefaultBatchElementMask = ~0ull;
					PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);
				}, bForceStereoInstancingOff, bForceParallelSetupOff);
			}
			else // Mobile
			{
				PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, EMobileSceneTextureSetupMode::None);

				AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("TranslucentMobile"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
					[Scene, &View, DrawRenderState, &Mesh](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
				{
					FMobileBasePassMeshProcessor PassMeshProcessor(
						EMeshPass::TranslucencyAll,
						Scene,
						&View,
						DrawRenderState,
						DynamicMeshPassContext,
						FMobileBasePassMeshProcessor::EFlags::None,
						ETranslucencyPass::TPT_AllTranslucency);

					const uint64 DefaultBatchElementMask = ~0ull;
					PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);
				}, bForceStereoInstancingOff, bForceParallelSetupOff);
			}
		}
		// handle opaque materials
		else
		{
			// make sure we are doing opaque drawing
			DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());

			// draw the mesh
			if (bIsHitTesting)
			{
				ensureMsgf(HitProxyId == Mesh.BatchHitProxyId, TEXT("Only Mesh.BatchHitProxyId is used for hit testing."));

#if WITH_EDITOR
				AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("HitTesting"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
					[Scene, &View, DrawRenderState, &Mesh](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
				{
					FHitProxyMeshProcessor PassMeshProcessor(
						Scene,
						&View,
						false,
						DrawRenderState,
						DynamicMeshPassContext);

					const uint64 DefaultBatchElementMask = ~0ull;
					PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);
				}, bForceStereoInstancingOff, bForceParallelSetupOff);
#endif
			}
			else
			{
				if (ShadingPath == EShadingPath::Deferred)
				{
					PassParameters->OpaqueBasePass = CreateOpaqueBasePassUniformBuffer(GraphBuilder, View);

					AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("OpaqueDeferred"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
						[Scene, &View, DrawRenderState, &Mesh, bUse128bitRT](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
					{
						FBasePassMeshProcessor PassMeshProcessor(
							EMeshPass::BasePass,
							Scene,
							View.GetFeatureLevel(),
							&View,
							DrawRenderState,
							DynamicMeshPassContext,
							bUse128bitRT ? FBasePassMeshProcessor::EFlags::bRequires128bitRT : FBasePassMeshProcessor::EFlags::None);

						const uint64 DefaultBatchElementMask = ~0ull;
						PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);
					});
				}
				else // Mobile
				{
					PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Opaque, EMobileSceneTextureSetupMode::None);

					AddDrawDynamicMeshPass(GraphBuilder, RDG_EVENT_NAME("OpaqueMobile"), PassParameters, View, RenderContext.GetViewportRect(), RenderContext.GetScissorRect(),
						[Scene, &View, DrawRenderState, &Mesh](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
					{
						FMobileBasePassMeshProcessor PassMeshProcessor(
							EMeshPass::BasePass,
							Scene,
							&View,
							DrawRenderState,
							DynamicMeshPassContext,
							FMobileBasePassMeshProcessor::EFlags::CanReceiveCSM | FMobileBasePassMeshProcessor::EFlags::ForcePassDrawRenderState);

						const uint64 DefaultBatchElementMask = ~0ull;
						PassMeshProcessor.AddMeshBatch(Mesh, DefaultBatchElementMask, nullptr);
					}, bForceStereoInstancingOff, bForceParallelSetupOff);
				}
			}
		}

		if (bUseVirtualTexturing)
		{
			VirtualTexture::EndFeedback(GraphBuilder);
		}
	}
}

void FRendererModule::DebugLogOnCrash()
{
	GVisualizeTexture.DebugLogOnCrash();
	
	GEngine->Exec(NULL, TEXT("rhi.DumpMemory"), *GLog);

	// execute on main thread
	{
		struct FTest
		{
			void Thread()
			{
				GEngine->Exec(NULL, TEXT("Mem FromReport"), *GLog);
			}
		} Test;

		DECLARE_CYCLE_STAT(TEXT("FSimpleDelegateGraphTask.DumpDataAfterCrash"),
			STAT_FSimpleDelegateGraphTask_DumpDataAfterCrash,
			STATGROUP_TaskGraphTasks);

		FSimpleDelegateGraphTask::CreateAndDispatchWhenReady(
			FSimpleDelegateGraphTask::FDelegate::CreateRaw(&Test, &FTest::Thread),
			GET_STATID(STAT_FSimpleDelegateGraphTask_DumpDataAfterCrash), nullptr, ENamedThreads::GameThread
		);
	}
}

void FRendererModule::GPUBenchmark(FSynthBenchmarkResults& InOut, float WorkScale)
{
	check(IsInGameThread());

	FSceneViewInitOptions ViewInitOptions;
	FIntRect ViewRect(0, 0, 1, 1);

	FBox LevelBox(FVector(-UE_OLD_WORLD_MAX), FVector(+UE_OLD_WORLD_MAX));	// LWC_TODO: Scale to renderable world bounds?
	ViewInitOptions.SetViewRectangle(ViewRect);

	// Initialize Projection Matrix and ViewMatrix since FSceneView initialization is doing some math on them.
	// Otherwise it trips NaN checks.
	const FVector ViewPoint = LevelBox.GetCenter();
	ViewInitOptions.ViewOrigin = FVector(ViewPoint.X, ViewPoint.Y, 0.0f);
	ViewInitOptions.ViewRotationMatrix = FMatrix(
		FPlane(1, 0, 0, 0),
		FPlane(0, -1, 0, 0),
		FPlane(0, 0, -1, 0),
		FPlane(0, 0, 0, 1));

	const FVector::FReal ZOffset = UE_OLD_WORLD_MAX;
	ViewInitOptions.ProjectionMatrix = FReversedZOrthoMatrix(
		LevelBox.GetSize().X / 2.f,
		LevelBox.GetSize().Y / 2.f,
		0.5f / ZOffset,
		ZOffset
		);

	FSceneView DummyView(ViewInitOptions);
	FlushRenderingCommands();
	FSynthBenchmarkResults* InOutPtr = &InOut;
	ENQUEUE_RENDER_COMMAND(RendererGPUBenchmarkCommand)(
		[DummyView, WorkScale, InOutPtr](FRHICommandListImmediate& RHICmdList)
		{
			RendererGPUBenchmark(RHICmdList, *InOutPtr, DummyView, WorkScale);
		});
	FlushRenderingCommands();
}

void FRendererModule::ResetSceneTextureExtentHistory()
{
	::ResetSceneTextureExtentHistory();
}

static void VisualizeTextureExec( const TCHAR* Cmd, FOutputDevice &Ar )
{
	check(IsInGameThread());
	FlushRenderingCommands();
	GVisualizeTexture.ParseCommands(Cmd, Ar);
}

extern void NaniteStatsFilterExec(const TCHAR* Cmd, FOutputDevice& Ar);

static bool RendererExec( UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar )
{
#if SUPPORTS_VISUALIZE_TEXTURE
	if (FParse::Command(&Cmd, TEXT("VisualizeTexture")) || FParse::Command(&Cmd, TEXT("Vis")))
	{
		VisualizeTextureExec(Cmd, Ar);
		return true;
	}
#endif //SUPPORTS_VISUALIZE_TEXTURE
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	if (FParse::Command(&Cmd, TEXT("DumpUnbuiltLightInteractions")))
	{
		InWorld->Scene->DumpUnbuiltLightInteractions(Ar);
		return true;
	}
	else if (FParse::Command(&Cmd, TEXT("NaniteStats")))
	{
		NaniteStatsFilterExec(Cmd, Ar);
		return true;
	}
	else if(FParse::Command(&Cmd, TEXT("r.RHI.Name")))
	{
		Ar.Logf( TEXT( "Running on the %s RHI" ), GDynamicRHI 
			? (GDynamicRHI->GetName() ? GDynamicRHI->GetName() : TEXT("<NULL Name>"))
			: TEXT("<NULL DynamicRHI>"));
		return true;
	}
	else if (FParse::Command(&Cmd, TEXT("r.ResetRenderTargetsExtent")))
	{
		ResetSceneTextureExtentHistory();
		Ar.Logf(TEXT("Scene texture extent history reset. Next scene render will reallocate textures at the requested size."));
		return true;
	}
#endif

	return false;
}

ICustomCulling* GCustomCullingImpl = nullptr;

void FRendererModule::RegisterCustomCullingImpl(ICustomCulling* impl)
{
	check(GCustomCullingImpl == nullptr);
	GCustomCullingImpl = impl;
}

void FRendererModule::UnregisterCustomCullingImpl(ICustomCulling* impl)
{
	check(GCustomCullingImpl == impl);
	GCustomCullingImpl = nullptr;
}

FStaticSelfRegisteringExec RendererExecRegistration(RendererExec);

void FRendererModule::ExecVisualizeTextureCmd( const FString& Cmd )
{
	// @todo: Find a nicer way to call this
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	VisualizeTextureExec(*Cmd, *GLog);
#endif
}
