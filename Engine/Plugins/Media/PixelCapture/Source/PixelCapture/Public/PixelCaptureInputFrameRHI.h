// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "IPixelCaptureInputFrame.h"
#include "RHI.h"

/**
 * A basic input frame for the Capture system that wraps a RHI texture buffer.
 */
class PIXELCAPTURE_API FPixelCaptureInputFrameRHI : public IPixelCaptureInputFrame
{
public:
	FPixelCaptureInputFrameRHI(FTextureRHIRef InFrameTexture, TSharedPtr<FPixelCaptureUserData> UserData = nullptr);
	virtual ~FPixelCaptureInputFrameRHI() = default;

	virtual int32 GetType() const override;
	virtual int32 GetWidth() const override;
	virtual int32 GetHeight() const override;

	FTextureRHIRef FrameTexture;
};
