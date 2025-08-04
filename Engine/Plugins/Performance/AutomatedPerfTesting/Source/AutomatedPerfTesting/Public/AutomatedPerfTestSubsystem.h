﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "AutomatedPerfTestSubsystem.generated.h"

/**
 * 
 */
UCLASS(MinimalAPI)
class UAutomatedPerfTestSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

	UFUNCTION(BlueprintCallable, Category="Automated Perf Testing")
	static FString GetTestID();
	
};
