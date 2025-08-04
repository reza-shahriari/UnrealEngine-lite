// Copyright Epic Games, Inc. All Rights Reserved.

#include "SharedRigRuntimeContext.h"

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"
#include "DNAReader.h"
#include "RigLogic.h"

void FSharedRigRuntimeContext::CacheVariableJointIndices()
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const uint16 LODCount = BehaviorReader->GetLODCount();
	VariableJointIndicesPerLOD.Reset();
	VariableJointIndicesPerLOD.AddDefaulted(LODCount);
	TSet<uint16> DistinctVariableJointIndices;
	for (uint16 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
	{
		TArrayView<const uint16> VariableAttributeIndices = BehaviorReader->GetJointVariableAttributeIndices(LODIndex);
		DistinctVariableJointIndices.Reset();
		DistinctVariableJointIndices.Reserve(VariableAttributeIndices.Num());
		for (const uint16 AttrIndex : VariableAttributeIndices)
		{
			// In DNA, the number of joint attributes is always 9 (only RigLogic has the ability to switch this)
			// and since the variable indices are queried from the DNA here, we deal with 9 as well, regardless of
			// the state elsewhere where we switched to 10 since the introduction of quaternion outputs from RigLogic.
			static constexpr uint16 NUM_ATTRS_PER_JOINT = 9;
			const uint16 JointIndex = AttrIndex / NUM_ATTRS_PER_JOINT;
			DistinctVariableJointIndices.Add(JointIndex);
		}
		VariableJointIndicesPerLOD[LODIndex].Values = DistinctVariableJointIndices.Array();
	}
}

void FSharedRigRuntimeContext::CacheInverseNeutralJointRotations()
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	static constexpr uint32 JointAttributeCount = 10;
	TArrayView<const float> NeutralJointValues = RigLogic->GetNeutralJointValues();
	ensure(NeutralJointValues.Num() % JointAttributeCount == 0);

	const int32 JointCount = NeutralJointValues.Num() / JointAttributeCount;
	InverseNeutralJointRotations.Reset(JointCount);
	for (int32 JointIndex = 0; JointIndex < JointCount; ++JointIndex)
	{
		const int32 AttrIndex = JointIndex * JointAttributeCount;
		const tdm::fquat NeutralRotation{NeutralJointValues[AttrIndex + 3], NeutralJointValues[AttrIndex + 4], NeutralJointValues[AttrIndex + 5], NeutralJointValues[AttrIndex + 6]};
		const tdm::fquat InverseNeutralRotation = tdm::inverse(NeutralRotation);
		InverseNeutralJointRotations.Add(FQuat(InverseNeutralRotation.x, InverseNeutralRotation.y, InverseNeutralRotation.z, InverseNeutralRotation.w));
	}
}
