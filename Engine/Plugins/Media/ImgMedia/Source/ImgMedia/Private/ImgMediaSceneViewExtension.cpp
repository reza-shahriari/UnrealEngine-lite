// Copyright Epic Games, Inc. All Rights Reserved.

#include "ImgMediaSceneViewExtension.h"
#include "ImgMediaPrivate.h"
#include "DynamicResolutionState.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "SceneView.h"

#define LOCTEXT_NAMESPACE "ImgMediaSceneViewExtension"

static TAutoConsoleVariable<float> CVarImgMediaFieldOfViewMultiplier(
	TEXT("ImgMedia.FieldOfViewMultiplier"),
	1.0f,
	TEXT("Multiply the field of view for active cameras by this value, generally to increase the frustum overall sizes to mitigate missing tile artifacts.\n"),
	ECVF_Default);

static TAutoConsoleVariable<bool> CVarImgMediaProcessTilesInnerOnly(
	TEXT("ImgMedia.ICVFX.InnerOnlyTiles"),
	false,
	TEXT("This CVar will ignore tile calculation for all viewports except for Display Cluster inner viewports. User should enable upscaling on Media plate to display lower quality mips instead, otherwise ")
	TEXT("other viewports will only display tiles loaded specifically for inner viewport and nothing else. \n"),
#if WITH_EDITOR
	FConsoleVariableDelegate::CreateLambda([](IConsoleVariable* CVar)
	{
		if (CVar->GetBool())
		{
			FNotificationInfo Info(LOCTEXT("EnableUpscalingNotification", "Tile calculation enabled for Display Cluster Inner Viewports exclusively.\nUse Mip Upscaling option on Media Plate to fill empty texture areas with lower quality data."));
			// Expire in 5 seconds.
			Info.ExpireDuration = 5.0f;
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}),
#endif
	ECVF_Default);


FImgMediaSceneViewExtension::FImgMediaSceneViewExtension(const FAutoRegister& AutoReg)
	: FSceneViewExtensionBase(AutoReg)
	, CachedViewInfos()
	, DisplayResolutionCachedViewInfos()
{
	OnBeginFrameDelegate = FCoreDelegates::OnBeginFrame.AddRaw(this, &FImgMediaSceneViewExtension::ResetViewInfoCache);
}

FImgMediaSceneViewExtension::~FImgMediaSceneViewExtension()
{
	FCoreDelegates::OnBeginFrame.Remove(OnBeginFrameDelegate);
}

void FImgMediaSceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FImgMediaSceneViewExtension::BeginRenderViewFamily);

	for (const FSceneView* View : InViewFamily.Views)
	{
		if (View != nullptr)
		{
			CacheViewInfo(InViewFamily, *View);
		}
	}
}

int32 FImgMediaSceneViewExtension::GetPriority() const
{
	// Lowest priority value to ensure all other extensions are executed before ours.
	return MIN_int32;
}

void FImgMediaSceneViewExtension::CacheViewInfo(FSceneViewFamily& InViewFamily, const FSceneView& View)
{
	// This relies on DisplayClusterMediaHelpers::GenerateICVFXViewportName to have two strings embedded.
	if (CVarImgMediaProcessTilesInnerOnly.GetValueOnGameThread()
		&& !(InViewFamily.ProfileDescription.Contains("_icvfx_") && InViewFamily.ProfileDescription.Contains("_incamera")))
	{
		return;
	}
	static const auto CVarMinAutomaticViewMipBiasOffset = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.ViewTextureMipBias.Offset"));
	static const auto CVarMinAutomaticViewMipBias = IConsoleManager::Get().FindTConsoleVariableDataFloat(TEXT("r.ViewTextureMipBias.Min"));
	const float FieldOfViewMultiplier = CVarImgMediaFieldOfViewMultiplier.GetValueOnGameThread();

	float ResolutionFraction = InViewFamily.SecondaryViewFraction;

	if (InViewFamily.GetScreenPercentageInterface())
	{
		DynamicRenderScaling::TMap<float> UpperBounds = InViewFamily.GetScreenPercentageInterface()->GetResolutionFractionsUpperBound();
		ResolutionFraction *= UpperBounds[GDynamicPrimaryResolutionFraction];
	}

	FImgMediaViewInfo Info;
	Info.Location = View.ViewMatrices.GetViewOrigin();
	Info.ViewDirection = View.GetViewDirection();
	Info.ViewProjectionMatrix = View.ViewMatrices.GetViewProjectionMatrix();
	
	// This converts a potential narrowing conversion crash to an error. 
	// If FIntRect does 32 to 64 bit widening and then checked narrowing from 64 to 32, which could cause a crash
	// if the value exceedes the 32 bit int limit. 
	{
		UE::Math::TIntRect<int64> UnconstrainedViewportRect64(View.UnconstrainedViewRect);
		UnconstrainedViewportRect64 = UnconstrainedViewportRect64.Scale(ResolutionFraction);

		if (!IntFitsIn<int32>(UnconstrainedViewportRect64.Min.X) ||
			!IntFitsIn<int32>(UnconstrainedViewportRect64.Min.Y) ||
			!IntFitsIn<int32>(UnconstrainedViewportRect64.Max.X) ||
			!IntFitsIn<int32>(UnconstrainedViewportRect64.Max.Y))
		{
			UE_LOG(LogImgMedia, Error, TEXT("Scaled Unconstrained viewport is out of bounds. Original Viewport rect: Min: %d x %d, Max: %d x %d, Screen Percentage: %f"), 
				View.UnconstrainedViewRect.Min.X,
				View.UnconstrainedViewRect.Min.Y,
				View.UnconstrainedViewRect.Max.X,
				View.UnconstrainedViewRect.Max.Y,
				ResolutionFraction);
		}

		Info.ViewportRect = FIntRect(
			FIntPoint((int32)UnconstrainedViewportRect64.Min.X, (int32)UnconstrainedViewportRect64.Min.Y),
			FIntPoint((int32)UnconstrainedViewportRect64.Max.X, (int32)UnconstrainedViewportRect64.Max.Y)
		);
	}


	if (FMath::IsNearlyEqual(FieldOfViewMultiplier, 1.0f))
	{
		Info.OverscanViewProjectionMatrix = Info.ViewProjectionMatrix;
	}
	else
	{
		FMatrix AdjustedProjectionMatrix = View.ViewMatrices.GetProjectionMatrix();

		const double HalfHorizontalFOV = FMath::Atan(1.0 / AdjustedProjectionMatrix.M[0][0]);
		const double HalfVerticalFOV = FMath::Atan(1.0 / AdjustedProjectionMatrix.M[1][1]);

		AdjustedProjectionMatrix.M[0][0] = 1.0 / FMath::Tan(HalfHorizontalFOV * FieldOfViewMultiplier);
		AdjustedProjectionMatrix.M[1][1] = 1.0 / FMath::Tan(HalfVerticalFOV * FieldOfViewMultiplier);

		Info.OverscanViewProjectionMatrix = View.ViewMatrices.GetViewMatrix() * AdjustedProjectionMatrix;
	}

	// We store hidden or show-only ids to later avoid needless calculations when objects are not in view.
	if (View.ShowOnlyPrimitives.IsSet())
	{
		Info.bPrimitiveHiddenMode = false;
		Info.PrimitiveComponentIds = View.ShowOnlyPrimitives.GetValue();
	}
	else
	{
		Info.bPrimitiveHiddenMode = true;
		Info.PrimitiveComponentIds = View.HiddenPrimitives;
	}

	/* View.MaterialTextureMipBias is only set later in rendering so we replicate here the calculations
	 * found in FSceneRenderer::PreVisibilityFrameSetup.*/
	if (View.PrimaryScreenPercentageMethod == EPrimaryScreenPercentageMethod::TemporalUpscale)
	{
		const float EffectivePrimaryResolutionFraction = float(Info.ViewportRect.Width()) / (View.UnscaledViewRect.Width() * InViewFamily.SecondaryViewFraction);
		Info.MaterialTextureMipBias = -(FMath::Max(-FMath::Log2(EffectivePrimaryResolutionFraction), 0.0f)) + CVarMinAutomaticViewMipBiasOffset->GetValueOnGameThread();
		Info.MaterialTextureMipBias = FMath::Max(Info.MaterialTextureMipBias, CVarMinAutomaticViewMipBias->GetValueOnGameThread());

		if (!ensureMsgf(!FMath::IsNaN(Info.MaterialTextureMipBias) && FMath::IsFinite(Info.MaterialTextureMipBias), TEXT("Calculated material texture mip bias is invalid, defaulting to zero.")))
		{
			Info.MaterialTextureMipBias = 0.0f;
		}
	}
	else
	{
		Info.MaterialTextureMipBias = 0.0f;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	static const auto CVarMipMapDebug = IConsoleManager::Get().FindConsoleVariable(TEXT("ImgMedia.MipMapDebug"));

	if (GEngine != nullptr && CVarMipMapDebug != nullptr && CVarMipMapDebug->GetBool())
	{
		const FString ViewName = InViewFamily.ProfileDescription.IsEmpty() ? TEXT("View") : InViewFamily.ProfileDescription;
		GEngine->AddOnScreenDebugMessage(-1, 0.0f, FColor::Cyan, *FString::Printf(TEXT("%s location: [%s], direction: [%s]"), *ViewName, *Info.Location.ToString(), *View.GetViewDirection().ToString()));
	}
#endif

	// We cache the display resolution view info in case it's needed for compositing applications
	const bool bIsDisplayResolutionDifferent = !FMath::IsNearlyEqual(ResolutionFraction, InViewFamily.SecondaryViewFraction);
	if (bIsDisplayResolutionDifferent)
	{
		FImgMediaViewInfo PostUpscaleVirtualInfo = Info;
		PostUpscaleVirtualInfo.MaterialTextureMipBias = 0.0f;
		PostUpscaleVirtualInfo.ViewportRect = FIntRect(0, 0,
			FMath::CeilToInt(View.UnconstrainedViewRect.Width() * InViewFamily.SecondaryViewFraction),
			FMath::CeilToInt(View.UnconstrainedViewRect.Height() * InViewFamily.SecondaryViewFraction)
		);

		DisplayResolutionCachedViewInfos.Add(MoveTemp(PostUpscaleVirtualInfo));
	}

	CachedViewInfos.Add(MoveTemp(Info));
}

void FImgMediaSceneViewExtension::ResetViewInfoCache()
{
	CachedViewInfos.Reset();
	DisplayResolutionCachedViewInfos.Reset();
}

#undef LOCTEXT_NAMESPACE
