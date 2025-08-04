// Copyright Epic Games, Inc. All Rights Reserved.

#include "DNAIndexMapping.h"

#include "HAL/LowLevelMemTracker.h"
#include "DNAReader.h"
#include "Animation/Skeleton.h"
#include "Animation/SmartName.h"
#include "Engine/SkeletalMesh.h"
#include "Animation/AnimCurveTypes.h"


/** Constructs curve name from nameToSplit using formatString of form x<obj>y<attr>z **/
static FString CreateCurveName(const FString& NameToSplit, const FString& FormatString)
{
	// constructs curve name from NameToSplit (always in form <obj>.<attr>)
	// using FormatString of form x<obj>y<attr>z
	// where x, y and z are arbitrary strings
	// example:
	// FormatString="mesh_<obj>_<attr>"
	// 'head.blink_L' becomes 'mesh_head_blink_L'
	FString ObjectName, AttributeName;
	if (!NameToSplit.Split(".", &ObjectName, &AttributeName))
	{
		return TEXT("");
	}
	FString CurveName = FormatString;
	CurveName = CurveName.Replace(TEXT("<obj>"), *ObjectName);
	CurveName = CurveName.Replace(TEXT("<attr>"), *AttributeName);
	return CurveName;
}

void FDNAIndexMapping::MapControlCurves(const IDNAReader* DNAReader, const USkeleton* Skeleton)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const uint32 ControlCount = DNAReader->GetRawControlCount();

	ControlAttributeCurves.Empty();
	ControlAttributeCurves.Reserve(ControlCount);

	for (uint32_t ControlIndex = 0; ControlIndex < ControlCount; ++ControlIndex)
	{
		const FString DNAControlName = DNAReader->GetRawControlName(ControlIndex);
		const FString AnimatedControlName = CreateCurveName(DNAControlName, TEXT("<obj>_<attr>"));
		if (AnimatedControlName == TEXT(""))
		{
			return;
		}
		ControlAttributeCurves.Add(*AnimatedControlName, ControlIndex);
	}

	// This has the side-effect of sorting the NamedValueArray, which is necessary to avoid it happening at
	// runtime, which would cause concurrent mutation of the data structure, not guarded by any locking mechanism
	UE::Anim::FNamedValueArrayUtils::RemoveByPredicate(ControlAttributeCurves, FCachedIndexedCurve{}, [](const UE::Anim::FCurveElementIndexed&, const UE::Anim::FCurveElementIndexed&) { return true; });
}

void FDNAIndexMapping::MapNeuralNetworkMaskCurves(const IDNAReader* DNAReader, const USkeleton* Skeleton)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const uint16 NeuralNetworkCount = DNAReader->GetNeuralNetworkCount();

	NeuralNetworkMaskCurves.Empty();
	NeuralNetworkMaskCurves.Reserve(NeuralNetworkCount);

	const uint16 MeshCount = DNAReader->GetMeshCount();
	for (uint16 MeshIndex = 0; MeshIndex < MeshCount; ++MeshIndex)
	{
		const uint16 MeshRegionCount = DNAReader->GetMeshRegionCount(MeshIndex);
		for (uint16 RegionIndex = 0; RegionIndex < MeshRegionCount; ++RegionIndex)
		{
			const FString& MeshRegionName = DNAReader->GetMeshRegionName(MeshIndex, RegionIndex);
			TArrayView<const uint16> NeuralNetworkIndices = DNAReader->GetNeuralNetworkIndicesForMeshRegion(MeshIndex, RegionIndex);
			const FString MaskCurveName = TEXT("CTRL_ML_") + MeshRegionName;
			for (const auto NeuralNetworkIndex : NeuralNetworkIndices)
			{
				NeuralNetworkMaskCurves.Add(*MaskCurveName, NeuralNetworkIndex);
			}
		}
	}

	// This has the side-effect of sorting the NamedValueArray, which is necessary to avoid it happening at
	// runtime, which would cause concurrent mutation of the data structure, not guarded by any locking mechanism
	UE::Anim::FNamedValueArrayUtils::RemoveByPredicate(NeuralNetworkMaskCurves, FCachedIndexedCurve{}, [](const UE::Anim::FCurveElementIndexed&, const UE::Anim::FCurveElementIndexed&) { return true; });
}

void FDNAIndexMapping::MapDriverJoints(const IDNAReader* DNAReader, const USkeletalMesh* SkeletalMesh)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const uint32 ControlCount = DNAReader->GetRawControlCount();

	DriverJointsToControlAttributesMap.Empty();
	// This is a correct approximation as long as only 4 (rotation) attributes are used as driver joint attributes
	// and no regular raw controls are present in the DNA
	DriverJointsToControlAttributesMap.Reserve(ControlCount / 4);

	for (uint32_t ControlIndex = 0; ControlIndex < ControlCount; ++ControlIndex)
	{
		const FString DriverJointAttrName = DNAReader->GetRawControlName(ControlIndex);
		if (DriverJointAttrName.Len() < 2)
		{
			continue;
		}
		int32 IndexOfLastDot;
		DriverJointAttrName.FindLastChar('.', IndexOfLastDot);
		const FString DriverJointName = DriverJointAttrName.Mid(0, IndexOfLastDot);
		const FName BoneName = FName(*DriverJointName);
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);
		if (BoneIndex == INDEX_NONE)
		{
			// Mixed DNAs will contain both driver joints and normal raw controls in this list, and those will
			// not be found in the joint hierarchy
			continue;
		}

		int32 MappingIndex = DriverJointsToControlAttributesMap.FindLastByPredicate([BoneIndex](const FMeshPoseBoneControlAttributeMapping& Element)
		{
			return Element.MeshPoseBoneIndex.GetInt() == BoneIndex;
		});
		if (MappingIndex == INDEX_NONE)
		{
			FMeshPoseBoneControlAttributeMapping NewMapping{FMeshPoseBoneIndex{BoneIndex}, INDEX_NONE, INDEX_NONE , INDEX_NONE , INDEX_NONE, INDEX_NONE};
			// BoneIndex may be INDEX_NONE, but it's handled properly by the Evaluate method
			MappingIndex = DriverJointsToControlAttributesMap.Add(NewMapping);
		}

		FMeshPoseBoneControlAttributeMapping& Mapping = DriverJointsToControlAttributesMap[MappingIndex];
		Mapping.DNAJointIndex = JointsMapDNAIndicesToMeshPoseBoneIndices.Find(Mapping.MeshPoseBoneIndex);

		if (DriverJointAttrName.EndsWith(TEXT(".x")) || DriverJointAttrName.EndsWith(TEXT(".qx")))
		{
			Mapping.RotationX = ControlIndex;
		}
		else if (DriverJointAttrName.EndsWith(TEXT(".y")) || DriverJointAttrName.EndsWith(TEXT(".qy")))
		{
			Mapping.RotationY = ControlIndex;
		}
		else if (DriverJointAttrName.EndsWith(TEXT(".z")) || DriverJointAttrName.EndsWith(TEXT(".qz")))
		{
			Mapping.RotationZ = ControlIndex;
		}
		else if (DriverJointAttrName.EndsWith(TEXT(".w")) || DriverJointAttrName.EndsWith(TEXT(".qw")))
		{
			Mapping.RotationW = ControlIndex;
		}
	}
}

void FDNAIndexMapping::MapJoints(const IDNAReader* DNAReader, const USkeletalMesh* SkeletalMesh)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const FReferenceSkeleton& RefSkeleton = SkeletalMesh->GetRefSkeleton();
	const uint16 JointCount = DNAReader->GetJointCount();
	JointsMapDNAIndicesToMeshPoseBoneIndices.Reset(JointCount);

	for (uint16 JointIndex = 0; JointIndex < JointCount; ++JointIndex)
	{
		const FString JointName = DNAReader->GetJointName(JointIndex);
		const FName BoneName = FName(*JointName);
		const int32 BoneIndex = RefSkeleton.FindBoneIndex(BoneName);

		// BoneIndex may be INDEX_NONE, but it's handled properly by the Evaluate method
		JointsMapDNAIndicesToMeshPoseBoneIndices.Add(FMeshPoseBoneIndex{ BoneIndex });
	}
}

void FDNAIndexMapping::MapMorphTargets(const IDNAReader* DNAReader, const USkeleton* Skeleton, const USkeletalMesh* SkeletalMesh)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const uint16 LODCount = DNAReader->GetLODCount();
	const TMap<FName, int32>& MorphTargetIndexMap = SkeletalMesh->GetMorphTargetIndexMap();
	const TArray<UMorphTarget*>& MorphTargets = SkeletalMesh->GetMorphTargets();

	MorphTargetCurvesPerLOD.Reset(LODCount);
	MorphTargetCurvesPerLOD.AddDefaulted(LODCount);

	for (uint16 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
	{
		TArrayView<const uint16> MappingIndicesForLOD = DNAReader->GetMeshBlendShapeChannelMappingIndicesForLOD(LODIndex);

		MorphTargetCurvesPerLOD[LODIndex].Reserve(MappingIndicesForLOD.Num());

		for (uint16 MappingIndex : MappingIndicesForLOD)
		{
			const FMeshBlendShapeChannelMapping Mapping = DNAReader->GetMeshBlendShapeChannelMapping(MappingIndex);
			const FString MeshName = DNAReader->GetMeshName(Mapping.MeshIndex);
			const FString BlendShapeName = DNAReader->GetBlendShapeChannelName(Mapping.BlendShapeChannelIndex);
			const FString MorphTargetStr = MeshName + TEXT("__") + BlendShapeName;
			const FName MorphTargetName(*MorphTargetStr);
			const int32* MorphTargetIndex = MorphTargetIndexMap.Find(MorphTargetName);
			if ((MorphTargetIndex != nullptr) && (*MorphTargetIndex != INDEX_NONE))
			{
				const UMorphTarget* MorphTarget = MorphTargets[*MorphTargetIndex];
				MorphTargetCurvesPerLOD[LODIndex].Add(MorphTarget->GetFName(), Mapping.BlendShapeChannelIndex);
			}
		}

		// This has the side-effect of sorting the NamedValueArray, which is necessary to avoid it happening at
		// runtime, which would cause concurrent mutation of the data structure, not guarded by any locking mechanism
		UE::Anim::FNamedValueArrayUtils::RemoveByPredicate(MorphTargetCurvesPerLOD[LODIndex], FCachedIndexedCurve{}, [](const UE::Anim::FCurveElementIndexed&, const UE::Anim::FCurveElementIndexed&) { return true; });
	}
}

void FDNAIndexMapping::MapMaskMultipliers(const IDNAReader* DNAReader, const USkeleton* Skeleton)
{
	LLM_SCOPE_BYNAME(TEXT("Animation/RigLogic"));

	const uint16 LODCount = DNAReader->GetLODCount();

	MaskMultiplierCurvesPerLOD.Reset();
	MaskMultiplierCurvesPerLOD.AddDefaulted(LODCount);

	for (uint16 LODIndex = 0; LODIndex < LODCount; ++LODIndex)
	{
		TArrayView<const uint16> IndicesPerLOD = DNAReader->GetAnimatedMapIndicesForLOD(LODIndex);

		MaskMultiplierCurvesPerLOD[LODIndex].Reserve(IndicesPerLOD.Num());

		for (uint16 AnimMapIndex : IndicesPerLOD)
		{
			const FString AnimatedMapName = DNAReader->GetAnimatedMapName(AnimMapIndex);
			const FString MaskMultiplierNameStr = CreateCurveName(AnimatedMapName, TEXT("<obj>_<attr>"));
			if (MaskMultiplierNameStr == "")
			{
				return;
			}

			MaskMultiplierCurvesPerLOD[LODIndex].Add(*MaskMultiplierNameStr, AnimMapIndex);
		}

		// This has the side-effect of sorting the NamedValueArray, which is necessary to avoid it happening at
		// runtime, which would cause concurrent mutation of the data structure, not guarded by any locking mechanism
		UE::Anim::FNamedValueArrayUtils::RemoveByPredicate(MaskMultiplierCurvesPerLOD[LODIndex], FCachedIndexedCurve{}, [](const UE::Anim::FCurveElementIndexed&, const UE::Anim::FCurveElementIndexed&) { return true; });
	}
}

void FDNAIndexMapping::Init(const IDNAReader* DNAReader, const USkeleton* Skeleton, const USkeletalMesh* SkeletalMesh)
{
	SkeletonGuid = Skeleton->GetGuid();
	MapControlCurves(DNAReader, Skeleton);
	MapNeuralNetworkMaskCurves(DNAReader, Skeleton);
	MapJoints(DNAReader, SkeletalMesh);
	MapDriverJoints(DNAReader, SkeletalMesh);
	MapMorphTargets(DNAReader, Skeleton, SkeletalMesh);
	MapMaskMultipliers(DNAReader, Skeleton);
}