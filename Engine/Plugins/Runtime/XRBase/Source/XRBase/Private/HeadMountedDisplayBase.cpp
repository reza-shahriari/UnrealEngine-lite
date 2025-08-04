// Copyright Epic Games, Inc. All Rights Reserved.

#include "HeadMountedDisplayBase.h"

#include "DefaultStereoLayers.h"
#include "EngineAnalytics.h"
#include "Interfaces/IAnalyticsProvider.h"
#include "AnalyticsEventAttribute.h"
#include "Misc/CoreDelegates.h"
#include "RenderingThread.h"
#include "Engine/Texture.h"
#include "DefaultSpectatorScreenController.h"
#include "DefaultXRCamera.h"
#include "Engine/Engine.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "XRCopyTexture.h"
#if WITH_EDITOR
#include "Editor/EditorEngine.h" // for UEditorEngine::IsHMDTrackingAllowed()
#endif

// including interface headers without their own implementation file, so that 
// functions (default ctors, etc.) get compiled into this module
#include "IXRSystemAssets.h"


constexpr float FHeadMountedDisplayBase::PixelDensityMin;
constexpr float FHeadMountedDisplayBase::PixelDensityMax;

FHeadMountedDisplayBase::FHeadMountedDisplayBase(IARSystemSupport* InARImplementation)
	: FXRTrackingSystemBase(InARImplementation)
	, bHeadTrackingEnforced(false)
{
}


void FHeadMountedDisplayBase::RecordAnalytics()
{
	TArray<FAnalyticsEventAttribute> EventAttributes;
	if (FEngineAnalytics::IsAvailable() && PopulateAnalyticsAttributes(EventAttributes))
	{
		// send analytics data
		FString OutStr(TEXT("Editor.VR.DeviceInitialised"));
		FEngineAnalytics::GetProvider().RecordEvent(OutStr, EventAttributes);
	}
}

bool FHeadMountedDisplayBase::PopulateAnalyticsAttributes(TArray<FAnalyticsEventAttribute>& EventAttributes)
{
	IHeadMountedDisplay::MonitorInfo MonitorInfo;
	if (!GetHMDMonitorInfo(MonitorInfo))
	{
		// still send the event but fill it with predictable values
		MonitorInfo = IHeadMountedDisplay::MonitorInfo();
		MonitorInfo.MonitorId = -1;
		MonitorInfo.MonitorName = TEXT("FailedToGetHMDMonitorInfo");
	}

	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("DeviceName"), GetSystemName().ToString()));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("VersionString"), UHeadMountedDisplayFunctionLibrary::GetVersionString()));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("DisplayDeviceName"), *MonitorInfo.MonitorName));
	// duplicating the metric because DisplayDeviceName has been sent garbage values before and cannot be trusted.
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("HMDMonitorName"), *MonitorInfo.MonitorName));
#if PLATFORM_WINDOWS
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("DisplayId"), MonitorInfo.MonitorId));
#else // Other platforms need some help in formatting size_t as text
	FString DisplayId(FString::Printf(TEXT("%llu"), (uint64)MonitorInfo.MonitorId));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("DisplayId"), DisplayId));
#endif
	FString MonResolution(FString::Printf(TEXT("(%d, %d)"), MonitorInfo.ResolutionX, MonitorInfo.ResolutionY));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("Resolution"), MonResolution));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("InterpupillaryDistance"), GetInterpupillaryDistance()));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("ChromaAbCorrectionEnabled"), IsChromaAbCorrectionEnabled()));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("MirrorToWindow"), IsSpectatorScreenActive()));
	EventAttributes.Add(FAnalyticsEventAttribute(TEXT("XRSecondaryScreenPercentage"), UHeadMountedDisplayFunctionLibrary::GetXRSecondaryScreenPercentage()));

	return true;
}

bool FHeadMountedDisplayBase::IsHeadTrackingEnforced() const
{
	return bHeadTrackingEnforced;
}

void FHeadMountedDisplayBase::SetHeadTrackingEnforced(bool bEnabled)
{
	bHeadTrackingEnforced = bEnabled;
}

bool FHeadMountedDisplayBase::IsHeadTrackingAllowed() const
{
	const bool bTrackingEnabled = IsStereoEnabled() || IsHeadTrackingEnforced();
#if WITH_EDITOR
	if (GIsEditor)
	{
		// @todo vreditor: We need to do a pass over VREditor code and make sure we are handling the VR modes correctly.  HeadTracking can be enabled without Stereo3D, for example
		UEditorEngine* EdEngine = Cast<UEditorEngine>(GEngine);
		return (!EdEngine || EdEngine->IsHMDTrackingAllowed()) && bTrackingEnabled;
	}
#endif // WITH_EDITOR
	return bTrackingEnabled;
}

IStereoLayers* FHeadMountedDisplayBase::GetStereoLayers()
{
	if (!DefaultStereoLayers.IsValid())
	{
		DefaultStereoLayers = FSceneViewExtensions::NewExtension<FDefaultStereoLayers>(this);
	}
	return DefaultStereoLayers.Get();
}

bool FHeadMountedDisplayBase::GetHMDDistortionEnabled(EShadingPath /* ShadingPath */) const
{
	return true;
}

FVector2D FHeadMountedDisplayBase::GetEyeCenterPoint_RenderThread(const int32 ViewIndex) const
{
	check(IsInRenderingThread());

	// Note: IsHeadTrackingAllowed() can only be called from the game thread.
	// IsStereoEnabled() and IsHeadTrackingEnforced() can be called from both the render and game threads, however.
	if (!(IsStereoEnabled() || IsHeadTrackingEnforced()))
	{
		return FVector2D(0.5f, 0.5f);
	}

	const FMatrix StereoProjectionMatrix = GetStereoProjectionMatrix(ViewIndex);
	//0,0,1 is the straight ahead point, wherever it maps to is the center of the projection plane in -1..1 coordinates.  -1,-1 is bottom left.
	const FVector4 ScreenCenter = StereoProjectionMatrix.TransformPosition(FVector(0.0f, 0.0f, 1.0f));
	//transform into 0-1 screen coordinates 0,0 is top left.  
	const FVector2D CenterPoint(0.5f + (ScreenCenter.X / 2.0f), 0.5f - (ScreenCenter.Y / 2.0f));
	return CenterPoint;
}

void FHeadMountedDisplayBase::OnLateUpdateApplied_RenderThread(FRDGBuilder& GraphBuilder, const FTransform& NewRelativeTransform)
{
	if (DefaultStereoLayers.IsValid())
	{
		DefaultStereoLayers->UpdateHmdTransform(NewRelativeTransform);
	}
}

void FHeadMountedDisplayBase::CalculateStereoViewOffset(const int32 ViewIndex, FRotator& ViewRotation, const float WorldToMeters, FVector& ViewLocation)
{
	TSharedPtr<class IXRCamera, ESPMode::ThreadSafe> HMDCamera = GetXRCamera();
	if (HMDCamera.IsValid())
	{
		HMDCamera->CalculateStereoCameraOffset(ViewIndex, ViewRotation, ViewLocation);
	}
}

void FHeadMountedDisplayBase::InitCanvasFromView(FSceneView* InView, UCanvas* Canvas)
{
}

bool FHeadMountedDisplayBase::IsSpectatorScreenActive() const
{
	ISpectatorScreenController const * Controller = GetSpectatorScreenController();
	return (Controller && Controller->GetSpectatorScreenMode() != ESpectatorScreenMode::Disabled);
}

ISpectatorScreenController* FHeadMountedDisplayBase::GetSpectatorScreenController()
{
	return SpectatorScreenController.Get();
}

class ISpectatorScreenController const * FHeadMountedDisplayBase::GetSpectatorScreenController() const
{
	return SpectatorScreenController.Get();
}

void FHeadMountedDisplayBase::CVarSinkHandler()
{
	check(IsInGameThread());

	if (GEngine && GEngine->XRSystem.IsValid())
	{
		static const auto SecondaryScreenPercentageHMDCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("xr.SecondaryScreenPercentage.HMDRenderTarget"));
		IHeadMountedDisplay* const HMDDevice = GEngine->XRSystem->GetHMDDevice();
		if (HMDDevice && SecondaryScreenPercentageHMDCVar)
		{
			float NewPixelDensity = SecondaryScreenPercentageHMDCVar->GetFloat() / 100.0f;
			if (NewPixelDensity < PixelDensityMin || NewPixelDensity > PixelDensityMax)
			{
				UE_LOG(LogHMD, Warning, TEXT("Invalid secondary screen percentage. Valid values must be within the range: [%f, %f]."), PixelDensityMin * 100, PixelDensityMax * 100);
				NewPixelDensity = FMath::Clamp(NewPixelDensity, PixelDensityMin, PixelDensityMax);
			}
			HMDDevice->SetPixelDensity(NewPixelDensity);
		}

		// If vr.PixelDensity is defined in a config file or set manually somewhere, trigger an ensure.
		static const auto DeprecatedCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"));
		if (DeprecatedCVar && DeprecatedCVar->GetFloat() != 1.0f)
		{
			ensureMsgf(false, TEXT("vr.PixelDensity is deprecated in UE 5.5 and will not affect the resolution. Use xr.SecondaryScreenPercentage.HMDRenderTarget instead, where 100.0f = ideal resolution."));
		}
	}
}

FAutoConsoleVariableSink FHeadMountedDisplayBase::CVarSink(FConsoleCommandDelegate::CreateStatic(&FHeadMountedDisplayBase::CVarSinkHandler));

void FHeadMountedDisplayBase::CopyTexture_RenderThread(FRHICommandListImmediate& RHICmdList, FRHITexture* SrcTexture, FIntRect SrcRect,
	FRHITexture* DstTexture, FIntRect DstRect, bool bClearBlack, bool bNoAlpha) const
{
	check(IsInRenderingThread());

	RHICmdList.Transition(FRHITransitionInfo(DstTexture, ERHIAccess::Unknown, ERHIAccess::RTV));
	RHICmdList.Transition(FRHITransitionInfo(SrcTexture, ERHIAccess::Unknown, ERHIAccess::SRVMask));

	FXRCopyTextureOptions Options(GMaxRHIFeatureLevel);
	Options.LoadAction = ERenderTargetLoadAction::ELoad;
	Options.bClearBlack = bClearBlack;
	// This call only comes from the spectator screen so we expect alpha to be premultiplied.
	Options.BlendMod = bNoAlpha ?
		EXRCopyTextureBlendModifier::Opaque :
		EXRCopyTextureBlendModifier::PremultipliedAlphaBlend;
	Options.SetDisplayMappingOptions(const_cast<FHeadMountedDisplayBase*>(this)->GetRenderTargetManager());

	FRHIRenderPassInfo RenderPassInfo(DstTexture, ERenderTargetActions::Load_Store);
	RHICmdList.BeginRenderPass(RenderPassInfo, TEXT("OpenXRHMD_CopyTexture"));
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	XRCopyTexture_InRenderPass(RHICmdList, SrcTexture, SrcRect, DstTexture, DstRect, Options);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	RHICmdList.EndRenderPass();

	RHICmdList.Transition(FRHITransitionInfo(DstTexture, ERHIAccess::RTV, ERHIAccess::Present));
}
