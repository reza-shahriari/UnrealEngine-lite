// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosVDGTAccelerationStructuresDataComponent.h"

#include "ChaosVDRecording.h"
#include "Algo/Copy.h"

UChaosVDGTAccelerationStructuresDataComponent::UChaosVDGTAccelerationStructuresDataComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	
	SetCanEverAffectNavigation(false);
	bNavigationRelevant = false;
}

void UChaosVDGTAccelerationStructuresDataComponent::UpdateAABBTreeData(TConstArrayView<TSharedPtr<FChaosVDAABBTreeDataWrapper>> AABBTreeDataView)
{
	RecordedABBTreeData.Reset(AABBTreeDataView.Num());
	Algo::Copy(AABBTreeDataView, RecordedABBTreeData);
}

void UChaosVDGTAccelerationStructuresDataComponent::UpdateFromNewGameFrameData(const FChaosVDGameFrameData& InGameFrameData)
{
	if (TSharedPtr<FChaosVDAccelerationStructureContainer> AABBTreeDataContainer = InGameFrameData.GetCustomDataHandler().GetData<FChaosVDAccelerationStructureContainer>())
	{
		if (const TArray<TSharedPtr<FChaosVDAABBTreeDataWrapper>>* RecordedAABBTreesData = AABBTreeDataContainer->RecordedAABBTreesBySolverID.Find(SolverID))
		{
			UpdateAABBTreeData(*RecordedAABBTreesData);
		}
	}
}

void UChaosVDGTAccelerationStructuresDataComponent::ClearData()
{
	RecordedABBTreeData.Reset();
}
