// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosVDAdditionalGTDataRouterComponent.h"

#include "ChaosVDRecording.h"
#include "ChaosVDScene.h"
#include "Actors/ChaosVDSolverInfoActor.h"

void UChaosVDAdditionalGTDataRouterComponent::UpdateFromSolverFrameData(const FChaosVDSolverFrameData& InSolverFrameData)
{
	if (!EnumHasAnyFlags(InSolverFrameData.GetAttributes(), EChaosVDSolverFrameAttributes::HasGTDataToReRoute))
	{
		return;
	}

	if (AChaosVDSolverInfoActor* Owner = Cast<AChaosVDSolverInfoActor>(GetOwner()))
	{
		if (TSharedPtr<FChaosVDScene> CVDScene = Owner->GetScene().Pin())
		{
			if (TSharedPtr<FChaosVDGameFrameDataWrapper> GTFrameDataWrapper = InSolverFrameData.GetCustomData().GetData<FChaosVDGameFrameDataWrapper>())
			{
				for (const TObjectPtr<AChaosVDDataContainerBaseActor>& DataContainerActor : CVDScene->GetDataContainerActorsView())
				{
					AChaosVDDataContainerBaseActor::FScopedGameFrameDataReRouting ScopedGTDataUpdate(DataContainerActor.Get());
					DataContainerActor->UpdateFromNewGameFrameData(*GTFrameDataWrapper->FrameData);
				}
			}
		}
	}
}

void UChaosVDAdditionalGTDataRouterComponent::ClearData()
{
}
