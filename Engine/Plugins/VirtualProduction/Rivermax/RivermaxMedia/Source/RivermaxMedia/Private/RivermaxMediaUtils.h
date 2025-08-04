// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "RivermaxFormats.h"
#include "RivermaxMediaOutput.h"
#include "RivermaxMediaSource.h"
#include "RivermaxTypes.h"

struct FGuid;

namespace UE::RivermaxMediaUtils::Private
{
	struct FSourceBufferDesc
	{
		uint32 BytesPerElement = 0;
		uint32 NumberOfElements = 0;
	};

	UE::RivermaxCore::ESamplingType MediaOutputPixelFormatToRivermaxSamplingType(ERivermaxMediaOutputPixelFormat InPixelFormat);
	UE::RivermaxCore::ESamplingType MediaSourcePixelFormatToRivermaxSamplingType(ERivermaxMediaSourcePixelFormat InPixelFormat);
	ERivermaxMediaSourcePixelFormat RivermaxPixelFormatToMediaSourcePixelFormat(UE::RivermaxCore::ESamplingType InSamplingType);
	ERivermaxMediaOutputPixelFormat RivermaxPixelFormatToMediaOutputPixelFormat(UE::RivermaxCore::ESamplingType InSamplingType);
	UE::RivermaxCore::ERivermaxAlignmentMode MediaOutputAlignmentToRivermaxAlignment(ERivermaxMediaAlignmentMode InAlignmentMode);
	UE::RivermaxCore::EFrameLockingMode MediaOutputFrameLockingToRivermax(ERivermaxFrameLockingMode InFrameLockingMode);
	FSourceBufferDesc GetBufferDescription(const FIntPoint& Resolution, ERivermaxMediaSourcePixelFormat InPixelFormat);

	/** Gets a resolution aligned to a pixel group specified for the provided video format as per 2110-20 requirements. */
	FIntPoint GetAlignedResolution(const UE::RivermaxCore::FVideoFormatInfo& InFormatInfo, const FIntPoint& ResolutionToAlign);

	/** 
	* Convert a set of streaming option to its SDP description. Currently only support video type. 
	* Returns true if SDP could successfully be created from rivermax options.
	*/
	bool StreamOptionsToSDPDescription(const UE::RivermaxCore::FRivermaxOutputOptions& Options, TArray<char>& OutSDPDescription);
}

// Custom version to keep track of and restore depricated properties.
struct RIVERMAXMEDIA_API FRivermaxMediaVersion
{
	enum Type
	{
		BeforeCustomVersionAdded = 0,

		// Add new versions above this comment.
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1


	};

	// Rivermax Guild
	const static FGuid GUID;

private:
	FRivermaxMediaVersion() {}
};