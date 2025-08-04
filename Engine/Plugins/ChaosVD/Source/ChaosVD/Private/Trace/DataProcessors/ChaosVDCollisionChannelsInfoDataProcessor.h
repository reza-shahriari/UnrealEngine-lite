﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "Trace/DataProcessors/ChaosVDDataProcessorBase.h"

struct FChaosVDSolverFrameData;

/**
 * Data processor implementation that read any recorded collision channels info
 */
class FChaosVDCollisionChannelsInfoDataProcessor final : public FChaosVDDataProcessorBase
{
public:
	explicit FChaosVDCollisionChannelsInfoDataProcessor();
	
	virtual bool ProcessRawData(const TArray<uint8>& InData) override;

private:

	bool ProcessPendingCollisionChannelData(const TArray<uint8>& InData);
	
	TArray<uint8> PendingCollisionChannelDataToProcess;
};

