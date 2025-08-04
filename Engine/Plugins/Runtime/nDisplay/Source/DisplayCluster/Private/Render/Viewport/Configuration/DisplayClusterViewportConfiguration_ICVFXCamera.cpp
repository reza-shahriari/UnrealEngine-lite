// Copyright Epic Games, Inc. All Rights Reserved.

#include "DisplayClusterViewportConfiguration_ICVFXCamera.h"

#include "DisplayClusterViewportConfiguration.h"
#include "DisplayClusterViewportConfigurationHelpers.h"
#include "DisplayClusterViewportConfigurationHelpers_ICVFX.h"
#include "DisplayClusterViewportConfigurationHelpers_Visibility.h"

#include "DisplayClusterRootActor.h"
#include "DisplayClusterConfigurationTypes_Viewport.h"
#include "DisplayClusterConfigurationTypes_ICVFX.h"

#include "IDisplayClusterProjection.h"
#include "DisplayClusterProjectionStrings.h"

#include "Containers/DisplayClusterProjectionCameraPolicySettings.h"
#include "Render/Projection/IDisplayClusterProjectionPolicy.h"

#include "Render/Viewport/DisplayClusterViewport.h"
#include "Render/Viewport/DisplayClusterViewportManager.h"
#include "Render/Viewport/DisplayClusterViewportStrings.h"
#include "Render/Viewport/RenderFrame/DisplayClusterRenderFrameSettings.h"
#include "Render/Viewport/LightCard/DisplayClusterViewportLightCardManager.h"

#include "Components/DisplayClusterICVFXCameraComponent.h"

#include "Misc/DisplayClusterLog.h"
#include "Misc/Parse.h"

////////////////////////////////////////////////////////////////////////////////
// FDisplayClusterViewportConfiguration_ICVFXCamera
////////////////////////////////////////////////////////////////////////////////
bool FDisplayClusterViewportConfiguration_ICVFXCamera::CreateAndSetupInnerCameraViewport()
{
	if (FDisplayClusterViewport* NewCameraViewport = FDisplayClusterViewportConfigurationHelpers_ICVFX::GetOrCreateCameraViewport(Configuration, CameraComponent, GetCameraSettings()))
	{
		CameraViewport = NewCameraViewport->AsShared();

		// overlay rendered only for enabled incamera
		check(GetCameraSettings().bEnable);

		// Update camera viewport settings
		FDisplayClusterViewportConfigurationHelpers_ICVFX::UpdateCameraViewportSettings(*CameraViewport, CameraComponent, GetCameraSettings());

		// Support projection policy update
		CameraViewport->UpdateConfiguration_ProjectionPolicy();

		// Reuse for EditorPreview
		FDisplayClusterViewportConfigurationHelpers_ICVFX::PreviewReuseInnerFrustumViewportWithinClusterNodes(*CameraViewport, CameraComponent, GetCameraSettings());

		return true;
	}

	return false;
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::IsCameraProjectionVisibleOnViewport(FDisplayClusterViewport* TargetViewport)
{
	if (TargetViewport && TargetViewport->GetProjectionPolicy().IsValid())
	{
		// Currently, only mono context is supported to check the visibility of the inner camera.
		if (TargetViewport->GetProjectionPolicy()->IsCameraProjectionVisible(CameraContext.ViewRotation, CameraContext.ViewLocation, CameraContext.PrjMatrix))
		{
			return true;
		}
	}

	// do not use camera for this viewport
	return false;
}

void FDisplayClusterViewportConfiguration_ICVFXCamera::Update()
{
	if (CreateAndSetupInnerCameraViewport())
	{
		if (CameraViewport.IsValid())
		{
			// Performance: Do not render InnerFrustum if it is not visible.
			CameraViewport->GetRenderSettingsImpl().bSkipRendering = !EnableInnerFrustumRendering();

			ADisplayClusterRootActor* SceneRootActor = CameraViewport->Configuration->GetRootActor(EDisplayClusterRootActorType::Scene);
			ADisplayClusterRootActor* ConfigurationRootActor = CameraViewport->Configuration->GetRootActor(EDisplayClusterRootActorType::Configuration);
			const FDisplayClusterConfigurationICVFX_StageSettings* StageSettings = CameraViewport->Configuration->GetStageSettings();

			if (SceneRootActor && ConfigurationRootActor && StageSettings)
			{
				const FDisplayClusterConfigurationICVFX_CameraSettings& CameraSettings = GetCameraSettings();

				FDisplayClusterShaderParameters_ICVFX::FCameraSettings ShaderParametersCameraSettings =
					CameraComponent.GetICVFXCameraShaderParameters(*StageSettings, CameraSettings);

				ShaderParametersCameraSettings.Resource.ViewportId = CameraViewport->GetId();

				// Rendering order for camera overlap
				const FString InnerFrustumID = CameraComponent.GetCameraUniqueId();
				const int32 CameraRenderOrder = ConfigurationRootActor->GetInnerFrustumPriority(InnerFrustumID);
				ShaderParametersCameraSettings.RenderOrder = (CameraRenderOrder < 0) ? CameraSettings.RenderSettings.RenderOrder : CameraRenderOrder;

				// Add this camera data to all visible targets:
				for (const FTargetViewport& TargetViewportIt : TargetViewports)
				{
					// Per-viewport CK
					ShaderParametersCameraSettings.ChromakeySource = TargetViewportIt.ChromakeySource;

					// Gain direct access to internal settings of the viewport:
					TargetViewportIt.Viewport->GetRenderSettingsICVFXImpl().ICVFX.Cameras.Add(ShaderParametersCameraSettings);
				}
			}
		}

		// Create and assign chromakey for all targets for this camera
		CreateAndSetupInnerCameraChromakey();
	}
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::Initialize()
{
	// Create new camera projection policy for camera viewport
	TSharedPtr<IDisplayClusterProjectionPolicy, ESPMode::ThreadSafe> CameraProjectionPolicy;
	if (!FDisplayClusterViewportConfigurationHelpers_ICVFX::CreateProjectionPolicyCameraICVFX(Configuration, CameraComponent, GetCameraSettings(), CameraProjectionPolicy))
	{
		return false;
	}

	// Applying the correct sequence of steps to use the projection policy math:
	// SetupProjectionViewPoint()->CalculateView()->GetProjectionMatrix()
	FMinimalViewInfo CameraViewInfo;
	float CustomNearClippingPlane = -1; // a value less than zero means ignoring.
	CameraProjectionPolicy->SetupProjectionViewPoint(nullptr, Configuration.GetRootActorWorldDeltaSeconds(), CameraViewInfo, &CustomNearClippingPlane);

	CameraContext.ViewLocation = CameraViewInfo.Location;
	CameraContext.ViewRotation = CameraViewInfo.Rotation;

	// Todo: Here we need to calculate the correct ViewOffset so that ICVFX can support stereo rendering.
	const FVector ViewOffset = FVector::ZeroVector;

	// Get world scale
	const float WorldToMeters = Configuration.GetWorldToMeters();
	// Supports custom near clipping plane
	const float NCP = (CustomNearClippingPlane >= 0) ? CustomNearClippingPlane : GNearClippingPlane;

	if(CameraProjectionPolicy->CalculateView(nullptr, 0, CameraContext.ViewLocation, CameraContext.ViewRotation, ViewOffset, WorldToMeters, NCP, NCP)
	&& CameraProjectionPolicy->GetProjectionMatrix(nullptr, 0, CameraContext.PrjMatrix))
	{
		return true;
	}

	return false;
}

const FDisplayClusterConfigurationICVFX_CameraSettings& FDisplayClusterViewportConfiguration_ICVFXCamera::GetCameraSettings() const
{
	return ConfigurationCameraComponent.GetCameraSettingsICVFX();
}

FString FDisplayClusterViewportConfiguration_ICVFXCamera::GetCameraUniqueId() const
{
	return CameraComponent.GetCameraUniqueId();
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::EnableChromakeyRendering() const
{
	// Performance: Rnder CK only when it is in use.
	for (const FTargetViewport& TargetViewportIt : TargetViewports)
	{
		if (TargetViewportIt.ChromakeySource == EDisplayClusterShaderParametersICVFX_ChromakeySource::ChromakeyLayers)
		{
			return true;
		}
	}

	return false;
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::EnableInnerFrustumRendering() const
{
	if (TargetViewports.IsEmpty())
	{
		// Headless node
		return true;
	}

	// Performance: If all chromakey sources have a ‘FrameColor’ value for all viewports on the current cluster node,
	// we can skip rendering the InnerFrustum
	for (const FTargetViewport& TargetViewportIt : TargetViewports)
	{
		if (TargetViewportIt.ChromakeySource != EDisplayClusterShaderParametersICVFX_ChromakeySource::FrameColor)
		{
			return true;
		}
	}

	return false;
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::ImplCreateChromakeyViewport()
{
	check(CameraViewport.IsValid());

	const FString ICVFXCameraId = CameraComponent.GetCameraUniqueId();

	// Create new chromakey viewport
	if (FDisplayClusterViewport* NewChromakeyViewport = FDisplayClusterViewportConfigurationHelpers_ICVFX::GetOrCreateChromakeyViewport(Configuration, CameraComponent, GetCameraSettings()))
	{
		ChromakeyViewport = NewChromakeyViewport->AsShared();

		// Update chromakey viewport settings
		FDisplayClusterViewportConfigurationHelpers_ICVFX::UpdateChromakeyViewportSettings(*ChromakeyViewport, *CameraViewport, GetCameraSettings());

		// Support projection policy update
		ChromakeyViewport->UpdateConfiguration_ProjectionPolicy();

		// reuse for EditorPreview
		FDisplayClusterViewportConfigurationHelpers_ICVFX::PreviewReuseChromakeyViewportWithinClusterNodes(*ChromakeyViewport, ICVFXCameraId);

		return true;
	}

	return false;
}

bool FDisplayClusterViewportConfiguration_ICVFXCamera::CreateAndSetupInnerCameraChromakey()
{
	const FDisplayClusterConfigurationICVFX_StageSettings* StageSettings = Configuration.GetStageSettings();
	if (!StageSettings)
	{
		return false;
	}

	// Try create chromakey render on demand
	if (const FDisplayClusterConfigurationICVFX_ChromakeyRenderSettings* ChromakeyRenderSettings = GetCameraSettings().Chromakey.GetChromakeyRenderSettings(*StageSettings))
	{
		if (ChromakeyRenderSettings->ShouldUseChromakeyViewport(*StageSettings) && EnableChromakeyRendering())
		{
			ImplCreateChromakeyViewport();
		}
	}

	// Chromakey viewport name with alpha channel
	const FString ChromakeyViewportId(ChromakeyViewport.IsValid() ? ChromakeyViewport->GetId() : TEXT(""));

	// Assign this chromakey to all supported targets
	for (const FTargetViewport& TargetViewportIt : TargetViewports)
	{
		const bool bEnableChromakey = TargetViewportIt.ChromakeySource != EDisplayClusterShaderParametersICVFX_ChromakeySource::Disabled;
		const bool bEnableChromakeyMarkers = bEnableChromakey && !EnumHasAnyFlags(TargetViewportIt.Viewport->GetRenderSettingsICVFX().Flags, EDisplayClusterViewportICVFXFlags::DisableChromakeyMarkers);

		// Gain direct access to internal settings of the viewport:
		FDisplayClusterViewport_RenderSettingsICVFX& InOutOuterViewportRenderSettingsICVFX = TargetViewportIt.Viewport->GetRenderSettingsICVFXImpl();
		FDisplayClusterShaderParameters_ICVFX::FCameraSettings& DstCameraData = InOutOuterViewportRenderSettingsICVFX.ICVFX.Cameras.Last();

		// Setup chromakey with markers
		FDisplayClusterViewportConfigurationHelpers_ICVFX::UpdateCameraSettings_Chromakey(DstCameraData, *StageSettings, GetCameraSettings(), bEnableChromakey, bEnableChromakeyMarkers, ChromakeyViewportId);

		// Setup overlap chromakey with markers
		FDisplayClusterViewportConfigurationHelpers_ICVFX::UpdateCameraSettings_OverlapChromakey(DstCameraData, *StageSettings, GetCameraSettings(), bEnableChromakeyMarkers);
	}

	return true;
}
