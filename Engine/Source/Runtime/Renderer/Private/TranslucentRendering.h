// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HitProxies.h"
#include "ShaderParameters.h"
#include "GlobalShader.h"
#include "SceneRendering.h"
#include "VolumeRendering.h"
#include "ScreenPass.h"
#include "SystemTextures.h"
#include "RenderGraphDefinitions.h"
#include "PostProcess/SceneRenderTargets.h"

#define DISTORTION_STENCIL_MASK_BIT STENCIL_SANDBOX_MASK

struct FSeparateTranslucencyDimensions
{
	inline FScreenPassTextureViewport GetViewport(FIntRect ViewRect) const
	{
		return FScreenPassTextureViewport(Extent, GetScaledRect(ViewRect, Scale));
	}

	FScreenPassTextureViewport GetInstancedStereoViewport(const FViewInfo& View) const;

	// Extent of the separate translucency targets, if downsampled.
	FIntPoint Extent = FIntPoint::ZeroValue;

	// Amount the view rects should be scaled to match the new separate translucency extent.
	float Scale = 1.0f;

	// The number of MSAA samples to use when creating separate translucency textures.
	uint32 NumSamples = 1;
};

DECLARE_GPU_DRAWCALL_STAT_EXTERN(Translucency);

/** Converts the the translucency pass into the respective mesh pass. */
EMeshPass::Type TranslucencyPassToMeshPass(ETranslucencyPass::Type TranslucencyPass);

/** Returns the translucency views to render for the requested view. */
ETranslucencyView GetTranslucencyView(const FViewInfo& View);

/** Returns the union of all translucency views to render. */
ETranslucencyView GetTranslucencyViews(TArrayView<const FViewInfo> Views);

/** Computes the translucency dimensions. */
FSeparateTranslucencyDimensions UpdateSeparateTranslucencyDimensions(const FSceneRenderer& SceneRenderer);

/** Check if separate translucency pass is needed for given pass and downsample scale */
bool IsSeparateTranslucencyEnabled(ETranslucencyPass::Type TranslucencyPass, float DownsampleScale);

/** Shared function to get the post DOF texture pixel format and creation flags */
const FRDGTextureDesc GetPostDOFTranslucentTextureDesc(ETranslucencyPass::Type TranslucencyPass, const FSeparateTranslucencyDimensions& SeparateTranslucencyDimensions, bool bIsModulate, EShaderPlatform ShaderPlatform);

/** Shared function used to create Post DOF translucent textures */
FRDGTextureMSAA CreatePostDOFTranslucentTexture(FRDGBuilder& GraphBuilder, ETranslucencyPass::Type TranslucencyPass, const FSeparateTranslucencyDimensions& SeparateTranslucencyDimensions, bool bIsModulate, EShaderPlatform ShaderPlatform);


/** Add a pass to compose separate translucency. */
struct FTranslucencyComposition
{
	enum class EOperation
	{
		UpscaleOnly,
		ComposeToExistingSceneColor,
		ComposeToNewSceneColor,
		ComposeToSceneColorAlpha
	};

	EOperation Operation = EOperation::UpscaleOnly;
	bool bApplyModulateOnly = false;

	FScreenPassTextureSlice SceneColor;
	FScreenPassTexture SceneDepth;

	FScreenPassTextureViewport OutputViewport;
	EPixelFormat OutputPixelFormat = PF_Unknown;

	// [Optional] Lens distortion applied on the scene color.
	FLensDistortionLUT LensDistortionLUT;

	FScreenPassTexture AddPass(
		FRDGBuilder& GraphBuilder,
		const FViewInfo& View,
		const FTranslucencyPassResources& TranslucencyTextures) const;
};
