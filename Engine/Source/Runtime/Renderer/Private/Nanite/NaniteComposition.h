// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NaniteShared.h"

extern bool UseComputeDepthExport();

struct FCustomDepthTextures;

namespace Nanite
{

struct FRasterContext;
struct FRasterResults;

struct FCustomDepthContext
{
	FRDGTextureRef InputDepth = nullptr;
	FRDGTextureSRVRef InputStencilSRV = nullptr;
	FRDGTextureRef DepthTarget = nullptr;
	FRDGTextureRef StencilTarget = nullptr;
	bool bComputeExport = true;
};

void EmitDepthTargets(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& View,
	bool bDrawSceneViewsInOneNanitePass,
	FRasterResults& RasterResults,
	FRDGTextureRef SceneDepth,
	FRDGTextureRef VelocityBuffer,
	FRDGTextureRef FirstStageDepthBuffer
);

FCustomDepthContext InitCustomDepthStencilContext(
	FRDGBuilder& GraphBuilder,
	const FCustomDepthTextures& CustomDepthTextures,
	bool bWriteCustomStencil
);

void EmitCustomDepthStencilTargets(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& View,
	bool bDrawSceneViewsInOneNanitePass,
	const FIntVector4& PageConstants,
	FRDGBufferRef VisibleClustersSWHW,
	FRDGBufferRef ViewsBuffer,
	FRDGTextureRef VisBuffer64,
	const FCustomDepthContext& CustomDepthContext
);

void FinalizeCustomDepthStencil(
	FRDGBuilder& GraphBuilder,
	const FCustomDepthContext& CustomDepthContext,
	FCustomDepthTextures& OutTextures
);

void MarkSceneStencilRects(
	FRDGBuilder& GraphBuilder,
	const FRasterContext& RasterContext,
	FScene& Scene,
	FViewInfo* SharedView,
	FIntPoint ViewportSize,
	uint32 NumRects,
	FRDGBufferSRVRef RectMinMaxBufferSRV,
	FRDGTextureRef DepthAtlasTexture
);

void EmitSceneDepthRects(
	FRDGBuilder& GraphBuilder,
	const FRasterContext& RasterContext,
	FScene& Scene,
	FViewInfo* SharedView,
	FIntPoint ViewportSize,
	uint32 NumRects,
	FRDGBufferSRVRef RectMinMaxBufferSRV,
	FRDGTextureRef DepthAtlasTexture
);

}