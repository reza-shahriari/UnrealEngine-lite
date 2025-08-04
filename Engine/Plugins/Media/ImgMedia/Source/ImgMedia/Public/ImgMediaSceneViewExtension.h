// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Set.h"
#include "SceneTypes.h"
#include "SceneViewExtension.h"

/**
 * Holds info on a camera which we can use for mipmap calculations.
 */
struct FImgMediaViewInfo
{
	/** Position of camera. */
	FVector Location;
	/** View direction of the camera. */
	FVector ViewDirection;
	/** View-projection matrix of the camera. */
	FMatrix ViewProjectionMatrix;
	/** View-projection matrix of the camera, optionally scaled for overscan frustum calculations. */
	FMatrix OverscanViewProjectionMatrix;
	/** Active viewport size. */
	FIntRect ViewportRect;
	/** View mip bias. */
	float MaterialTextureMipBias;
	/** Hidden or show-only mode for primitive components. */
	bool bPrimitiveHiddenMode;
	/** Hidden or show-only primitive components. */
	TSet<FPrimitiveComponentId> PrimitiveComponentIds;
};

/**
 * Scene view extension used to cache view information (primarily for visible mip/tile calculations).
 */
class FImgMediaSceneViewExtension final : public FSceneViewExtensionBase
{
public:
	FImgMediaSceneViewExtension(const FAutoRegister& AutoReg);
	~FImgMediaSceneViewExtension();

	/**
	 * Get the cached camera information array, updated on the game thread by BeginRenderViewFamily.
	 *
	 * @return Array of info on each camera.
	 */
	const TArray<FImgMediaViewInfo>& GetViewInfos() const { return CachedViewInfos; };

	/**
	 * Get the cached camera information array at display resolution for compositing, updated on the game thread by BeginRenderViewFamily.
	 * Will remain empty if the render resolution matches the display resolution.
	 *
	 * @return Array of info on each camera, at display resolution.
	 */
	const TArray<FImgMediaViewInfo>& GetDisplayResolutionViewInfos() const { return DisplayResolutionCachedViewInfos; };

	void BeginRenderViewFamily(FSceneViewFamily& InViewFamily) override;
	int32 GetPriority() const override;

private:
	/** Cache camera view information for the current frame. */
	void CacheViewInfo(FSceneViewFamily& InViewFamily, const FSceneView& View);

	/** Reset the view info cache. */
	void ResetViewInfoCache();
	
	/** Array of info on each camera used for mipmap calculations. */
	TArray<FImgMediaViewInfo> CachedViewInfos;

	/** Array of info on each camera used for mipmap calculations, at display resolution. */
	TArray<FImgMediaViewInfo> DisplayResolutionCachedViewInfos;

	/** FCoreDelegates::OnBeginFrame delegate. */
	FDelegateHandle OnBeginFrameDelegate;
};
