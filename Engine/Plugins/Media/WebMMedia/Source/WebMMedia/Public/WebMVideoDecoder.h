// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_WEBM_LIBS

#include "Templates/SharedPointer.h"
#include "MediaShaders.h"

THIRD_PARTY_INCLUDES_START
#include <vpx/vpx_decoder.h>
#include <vpx/vp8dx.h>
THIRD_PARTY_INCLUDES_END

class IWebMSamplesSink;
class FWebMMediaTextureSample;
class FWebMMediaTextureSamplePool;
struct FWebMFrame;

class FWebMVideoDecoder
{
public:
	WEBMMEDIA_API FWebMVideoDecoder(IWebMSamplesSink& InSamples);
	WEBMMEDIA_API ~FWebMVideoDecoder();

public:
	WEBMMEDIA_API bool Initialize(const char* CodecName);
	WEBMMEDIA_API void DecodeVideoFramesAsync(const TArray<TSharedPtr<FWebMFrame>>& VideoFrames);
	WEBMMEDIA_API bool IsBusy() const;

private:
	struct FConvertParams
	{
		TSharedPtr<FWebMMediaTextureSample, ESPMode::ThreadSafe> VideoSample;
		const vpx_image_t* Image;
	};

	vpx_codec_ctx_t Context;
	TUniquePtr<FWebMMediaTextureSamplePool> VideoSamplePool;
	TRefCountPtr<FRHITexture> DecodedY;
	TRefCountPtr<FRHITexture> DecodedU;
	TRefCountPtr<FRHITexture> DecodedV;
	FGraphEventRef VideoDecodingTask;
	IWebMSamplesSink& Samples;
	bool bTexturesCreated;
	bool bIsInitialized;

	void ConvertYUVToRGBAndSubmit(const FConvertParams& Params);
	void DoDecodeVideoFrames(const TArray<TSharedPtr<FWebMFrame>>& VideoFrames);
	void CreateTextures(const vpx_image_t* Image);
	void Close();
};

#endif // WITH_WEBM_LIBS
