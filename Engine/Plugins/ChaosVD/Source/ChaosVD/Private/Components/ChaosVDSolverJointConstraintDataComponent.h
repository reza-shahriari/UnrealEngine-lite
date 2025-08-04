﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ChaosVDConstraintDataComponent.h"
#include "ChaosVDSolverJointConstraintDataComponent.generated.h"


UCLASS()
class CHAOSVD_API UChaosVDSolverJointConstraintDataComponent : public UChaosVDConstraintDataComponent
{
	GENERATED_BODY()

	virtual void UpdateFromNewSolverStageData(const FChaosVDSolverFrameData& InSolverFrameData, const FChaosVDFrameStageData& InSolverFrameStageData) override;
};
