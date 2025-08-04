// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"

enum EShaderPlatform : uint16;

namespace UE::StereoRenderUtils
{

/*
	* Detect the single-draw stereo shader variant, in order to support usage across different platforms
	*/
class FStereoShaderAspects
{
public:
	/**
	* Determines the stereo aspects of the shader pipeline based on the input shader platform
	* @param Platform	Target shader platform used to determine stereo shader variant
	*/
	RENDERCORE_API FStereoShaderAspects(EShaderPlatform Platform);

	/**  
	* Default empty constructor for object in FSceneView. Do not use! 
	*/
	RENDERCORE_API FStereoShaderAspects();

	/**
	* Whether instanced stereo rendering is enabled - i.e. using a single instanced drawcall to render to both stereo views.
	* The output is redirected via the viewport index.
	*/
	inline bool IsInstancedStereoEnabled() const { return bInstancedStereoEnabled; }

	/**
	* Whether mobile multiview is enabled - i.e. using VK_KHR_multiview. Another drawcall reduction technique, independent of instanced stereo.
	* Mobile multiview generates view indices to index into texture arrays.
	* Can be internally emulated using instanced stereo if native support is unavailable, by using ISR-generated view indices to index into texture arrays.
	*/
	inline bool IsMobileMultiViewEnabled() const { return bMobileMultiViewEnabled; };

	/**
	* Whether multiviewport rendering is enabled - i.e. using ViewportIndex to index into viewport.
	* Relies on instanced stereo rendering being enabled.
	*/
	inline bool IsInstancedMultiViewportEnabled() const { return bInstancedMultiViewportEnabled; };

	/**
	* Whether MMV fallback was requested - i.e. using ISR-generated view indices to index into texture arrays.
	* True when on a mobile shader platform and vr.MobileMultiView=1, but ISR is supported by the RHI and MMV is not (e.g. D3D12 mobile preview).
	*/
	UE_DEPRECATED(5.6, "The MMV fallback path is deprecated and will be disabled via UE_SUPPORT_MMV_FALLBACK by default even if this function returns true.")
	inline bool IsMobileMultiViewFallbackEnabled() const { return bMobileMultiViewFallback; };

private:
	bool bInstancedStereoEnabled : 1;
	bool bMobileMultiViewEnabled : 1;
	bool bInstancedMultiViewportEnabled : 1;

	bool bInstancedStereoNative : 1;
	bool bMobileMultiViewNative : 1;
	bool bMobileMultiViewFallback : 1;

};

RENDERCORE_API void LogISRInit(const UE::StereoRenderUtils::FStereoShaderAspects& Aspects);

RENDERCORE_API void VerifyISRConfig(const UE::StereoRenderUtils::FStereoShaderAspects& Aspects, EShaderPlatform ShaderPlatform);

} // namespace UE::StereoRenderUtils
