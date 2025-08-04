// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/Async.h"
#include "CoreMinimal.h"
#include "EpicRtcVideoCommon.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Video/CodecUtils/CodecUtilsH264.h"
#include "Video/CodecUtils/CodecUtilsVP9.h"

namespace UE::PixelStreaming2
{
	static const FString SFU_PLAYER_ID_PREFIX = FString(TEXT("SFU_"));

	inline bool IsSFU(const FString& InPlayerId)
	{
		return InPlayerId.StartsWith(SFU_PLAYER_ID_PREFIX);
	}
} // namespace UE::PixelStreaming2
