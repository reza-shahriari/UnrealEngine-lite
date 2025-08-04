// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Animation/AnimInstance.h"
#include "DrawDebugHelpers.h"
#include "IObjectChooser.h"
#include "PoseSearch/PoseSearchDefines.h"
#include "PoseSearch/PoseSearchEvent.h"
#include "PoseSearch/PoseSearchFeatureChannel.h"
#include "PoseSearch/PoseSearchIndex.h"
#include "PoseSearch/PoseSearchResult.h"

#define UE_API POSESEARCH_API

struct FTransformTrajectory;
class UPoseSearchDatabase;
class UPoseSearchFeatureChannel_Position;

namespace UE::PoseSearch
{
	

// Utils functions
const FTransform& GetContextTransform(const UObject* AnimContext);
const USkeleton* GetContextSkeleton(const UObject* AnimContext);
const USkeleton* GetContextSkeleton(FChooserEvaluationContext& AnimContext);
const AActor* GetContextOwningActor(const UObject* AnimContext);
FVector GetContextLocation(const UObject* AnimContext);

// Experimental, this feature might be removed without warning, not for production use
const USkeletalMeshComponent* GetContextSkeletalMeshComponent(const UObject* AnimContext);
// Experimental, this feature might be removed without warning, not for production use
const FBoneContainer GetBoneContainer(const UObject* AnimContext);

struct FPoseIndicesHistory;
struct IPoseHistory;

enum class UE_DEPRECATED(5.6, "EDebugDrawFlags is no longer used") EDebugDrawFlags : uint32
{
	None = 0,

	// used to differenciate channels debug drawing of the query
	DrawQuery = 1 << 0,
};
PRAGMA_DISABLE_DEPRECATION_WARNINGS
ENUM_CLASS_FLAGS(EDebugDrawFlags);
PRAGMA_ENABLE_DEPRECATION_WARNINGS

enum class EPoseCandidateFlags : uint32
{
	None = 0,

	Valid_Pose = 1 << 0,
	Valid_ContinuingPose = 1 << 1,
	Valid_CurrentPose = 1 << 2,

	AnyValidMask = Valid_Pose | Valid_ContinuingPose | Valid_CurrentPose,

	DiscardedBy_PoseJumpThresholdTime = 1 << 3,
	DiscardedBy_PoseReselectHistory = 1 << 4,
	DiscardedBy_BlockTransition = 1 << 5,
	DiscardedBy_PoseFilter = 1 << 6,
	DiscardedBy_AssetIdxFilter = 1 << 7,
	DiscardedBy_Search = 1 << 8,
	DiscardedBy_AssetReselection = 1 << 9,

	AnyDiscardedMask = DiscardedBy_PoseJumpThresholdTime | DiscardedBy_PoseReselectHistory | DiscardedBy_BlockTransition | DiscardedBy_PoseFilter | DiscardedBy_AssetIdxFilter | DiscardedBy_Search | DiscardedBy_AssetReselection,
};
ENUM_CLASS_FLAGS(EPoseCandidateFlags);

#if ENABLE_DRAW_DEBUG
struct FDebugDrawParams
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	UE_DEPRECATED(5.6, "Use other FDebugDrawParams signatures instead")
	UE_API FDebugDrawParams(TArrayView<FAnimInstanceProxy*> InAnimInstanceProxies, TConstArrayView<const IPoseHistory*> InPoseHistories, const FRoleToIndex& InRoleToIndex, const UPoseSearchDatabase* InDatabase, EDebugDrawFlags InFlags = EDebugDrawFlags::None);
	UE_DEPRECATED(5.6, "Use other FDebugDrawParams signatures instead")
	UE_API FDebugDrawParams(TArrayView<const USkinnedMeshComponent*> InMeshes, TConstArrayView<const IPoseHistory*> InPoseHistories, const FRoleToIndex& InRoleToIndex, const UPoseSearchDatabase* InDatabase, EDebugDrawFlags InFlags = EDebugDrawFlags::None);
	UE_API PRAGMA_ENABLE_DEPRECATION_WARNINGS
	FDebugDrawParams(TConstArrayView<FChooserEvaluationContext*> InAnimContexts, TConstArrayView<const IPoseHistory*> InPoseHistories, const FRoleToIndex& InRoleToIndex, const UPoseSearchDatabase* InDatabase);
	
	UE_API const FSearchIndex* GetSearchIndex() const;
	UE_API const UPoseSearchSchema* GetSchema() const;

	UE_API float ExtractPermutationTime(TConstArrayView<float> PoseVector) const;
	UE_API FVector ExtractPosition(TConstArrayView<float> PoseVector, float SampleTimeOffset, int8 SchemaBoneIdx, const FRole& Role, EPermutationTimeType PermutationTimeType = EPermutationTimeType::UseSampleTime, int32 SamplingAttributeId = INDEX_NONE, float PermutationSampleTimeOffset = 0.f) const;
	UE_API FQuat ExtractRotation(TConstArrayView<float> PoseVector, float SampleTimeOffset, int8 SchemaBoneIdx, const FRole& Role, EPermutationTimeType PermutationTimeType = EPermutationTimeType::UseSampleTime, int32 SamplingAttributeId = INDEX_NONE, float PermutationSampleTimeOffset = 0.f) const;
	UE_API FTransform GetRootBoneTransform(const FRole& Role, float SampleTimeOffset = 0.f) const;

	UE_API void DrawLine(const FVector& LineStart, const FVector& LineEnd, const FColor& Color, float Thickness = 0.f) const;
	UE_API void DrawPoint(const FVector& Position, const FColor& Color, float Thickness = 6.f) const;
	UE_API void DrawCircle(const FVector& Center, const FVector& UpVector, float Radius, int32 Segments, const FColor& Color, float Thickness = 0.f) const;
	UE_API void DrawCircle(const FMatrix& TransformMatrix, float Radius, int32 Segments, const FColor& Color, float Thickness = 0.f) const;
	UE_API void DrawWedge(const FVector& Origin, const FVector& Direction, float InnerRadius, float OuterRadius, float Width, int32 Segments, const FColor& Color, float Thickness = 0.f) const;
	UE_API void DrawSphere(const FVector& Center, float Radius, int32 Segments, const FColor& Color, float Thickness = 0.f) const;
	UE_API void DrawCentripetalCatmullRomSpline(TConstArrayView<FVector> Points, TConstArrayView<FColor> Colors, float Alpha, int32 NumSamplesPerSegment, float Thickness = 1.f) const;
	
	UE_API void DrawFeatureVector(TConstArrayView<float> PoseVector);
	UE_API void DrawFeatureVector(int32 PoseIdx);

	UE_API bool IsAnyWeightRelevant(const UPoseSearchFeatureChannel* Channel) const;

private:
	bool CanDraw() const;

	const TConstArrayView<FChooserEvaluationContext*> AnimContexts;
	const TConstArrayView<const IPoseHistory*> PoseHistories;

	// NoTe: mapping Role to the index of the associated asset that this FDebugDrawParams is drawing.
	// NOT the index of the UPoseSearchSchema::Skeletons! Use UPoseSearchSchema::GetRoledSkeleton API to resolve that Role to FPoseSearchRoledSkeleton 
	const FRoleToIndex RoleToIndex;

	const UPoseSearchDatabase* Database = nullptr;
	TAlignedArray<float> DynamicWeightsSqrtBuffer;
	TConstArrayView<float> DynamicWeightsSqrt;
};

#endif // ENABLE_DRAW_DEBUG

// float buffer of features according to a UPoseSearchSchema layout. Used to build search queries at runtime
struct FCachedQuery
{
public:
	explicit FCachedQuery(const UPoseSearchSchema* InSchema);
	const UPoseSearchSchema* GetSchema() const { return Schema; }
	TArrayView<float> EditValues() { return Values; }
	TConstArrayView<float> GetValues() const { return Values; }

private:
	TStackAlignedArray<float> Values;
	
	// no need for a TWeakObjectPtr since it doesn't persist across multiple frames (same lifespan as FSearchContext)
	const UPoseSearchSchema* Schema;
};

// CachedChannels uses hashed unique identifiers to determine channels that can share feature vector data during the building of the query
struct FCachedChannel
{
	// no need for a TWeakObjectPtr since it doesn't persist across multiple frames (same lifespan as FSearchContext)
	const UPoseSearchFeatureChannel* Channel = nullptr;

	// index of the associated query in FSearchContext::CachedQueries
	int32 CachedQueryIndex = INDEX_NONE;
};

struct FSearchContext
{
	UE_DEPRECATED(5.6, "Use other signaure instead")
	UE_API FSearchContext(float InDesiredPermutationTimeOffset = 0.f, const FPoseIndicesHistory* InPoseIndicesHistory = nullptr,
		const FSearchResult& InCurrentResult = FSearchResult(), const FFloatInterval& InPoseJumpThresholdTime = FFloatInterval(0.f, 0.f), bool bInUseCachedChannelData = false);

	UE_API FSearchContext(float InDesiredPermutationTimeOffset, const FPoseIndicesHistory* InPoseIndicesHistory,
		const FSearchResult& InCurrentResult, const FFloatInterval& InPoseJumpThresholdTime, const FPoseSearchEvent& InEventToSearch);

	// deleting copies and moves since members could reference other members (e.g.: CurrentResultPoseVector could point to CurrentResultPoseVectorData, so it'll require proper copies and movers implementations)
	FSearchContext(const FSearchContext& Other) = delete;
	FSearchContext(FSearchContext&& Other) = delete;
	FSearchContext& operator=(const FSearchContext& Other) = delete;
	FSearchContext& operator=(FSearchContext&& Other) = delete;

	UE_API void AddRole(const FRole& Role, FChooserEvaluationContext* AnimContext, const IPoseHistory* PoseHistory);

	// Returns the curve value of name CurveName at an offset time of SampleTimeOffset.
	// If the curve is not found, assume value of 0, which is consistent with curve behavior in the animation update.
	UE_API float GetSampleCurveValue(float SampleTimeOffset, const FName& CurveName, const FRole& SampleRole);

	// Returns the rotation of the bone Schema.BoneReferences[SchemaSampleBoneIdx] at an offset time of SampleTimeOffset relative to the
	// transform of the bone Schema.BoneReferences[SchemaOriginBoneIdx] at an offset time of time OriginTimeOffset 
	// Times will be processed by GetPermutationTimeOffsets(PermutationTimeType, ...)
	UE_API FQuat GetSampleRotation(float SampleTimeOffset, float OriginTimeOffset, int8 SchemaSampleBoneIdx, int8 SchemaOriginBoneIdx, const FRole& SampleRole, const FRole& OriginRole, EPermutationTimeType PermutationTimeType = EPermutationTimeType::UseSampleTime, const FQuat* SampleBoneRotationWorldOverride = nullptr);
	
	// Returns the position of the bone Schema.BoneReferences[SchemaSampleBoneIdx] at an offset time of SampleTimeOffset relative to the
	// transform of the bone Schema.BoneReferences[SchemaOriginBoneIdx] at an offset time of time OriginTimeOffset 
	// Times will be processed by GetPermutationTimeOffsets(PermutationTimeType, ...)
	UE_API FVector GetSamplePosition(float SampleTimeOffset, float OriginTimeOffset, int8 SchemaSampleBoneIdx, int8 SchemaOriginBoneIdx, const FRole& SampleRole, const FRole& OriginRole, EPermutationTimeType PermutationTimeType = EPermutationTimeType::UseSampleTime, const FVector* SampleBonePositionWorldOverride = nullptr);
	
	// Returns the delta velocity of the velocity of the bone Schema.BoneReferences[SchemaSampleBoneIdx] at an offset time of SampleTimeOffset minus
	// the velocity of the bone Schema.BoneReferences[SchemaOriginBoneIdx] at an offset time of time OriginTimeOffset 
	// Times will be processed by GetPermutationTimeOffsets(PermutationTimeType, ...)
	// if bUseCharacterSpaceVelocities is true, velocities will be computed in root bone space, rather than world space
	UE_API FVector GetSampleVelocity(float SampleTimeOffset, float OriginTimeOffset, int8 SchemaSampleBoneIdx, int8 SchemaOriginBoneIdx, const FRole& SampleRole, const FRole& OriginRole, bool bUseCharacterSpaceVelocities = true, EPermutationTimeType PermutationTimeType = EPermutationTimeType::UseSampleTime, const FVector* SampleBoneVelocityWorldOverride = nullptr);

	UE_API void ResetCurrentBestCost();
	UE_API void UpdateCurrentBestCost(const FPoseSearchCost& PoseSearchCost);
	float GetCurrentBestTotalCost() const { return CurrentBestTotalCost; }

	UE_API TConstArrayView<float> GetOrBuildQuery(const UPoseSearchSchema* Schema);
	UE_API TConstArrayView<float> GetCachedQuery(const UPoseSearchSchema* Schema) const;

	UE_API bool IsCurrentResultFromDatabase(const UPoseSearchDatabase* Database) const;
	UE_API bool CanUseCurrentResult() const;

	TConstArrayView<float> GetCurrentResultPoseVector() const { return CurrentResultPoseVector; }

	UE_API void UpdateCurrentResultPoseVector();
	const FSearchResult& GetCurrentResult() const { return CurrentResult; }
	const FFloatInterval& GetPoseJumpThresholdTime() const { return PoseJumpThresholdTime; }
	const FPoseIndicesHistory* GetPoseIndicesHistory() const { return PoseIndicesHistory; }
	
	UE_API bool ArePoseHistoriesValid() const;
	const TArray<const IPoseHistory*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>>& GetPoseHistories() const { return PoseHistories; }
	UE_API const IPoseHistory* GetPoseHistory(const FRole& Role) const;

	float GetDesiredPermutationTimeOffset() const { return DesiredPermutationTimeOffset; }
	
	UE_DEPRECATED(5.6, "Use GetContext instead")
	UE_API const UAnimInstance* GetAnimInstance(const FRole& Role) const;
	UE_DEPRECATED(5.6, "Use GetContext instead")
	UE_API const UObject* GetAnimContext(const FRole& Role) const;

	UE_API const FChooserEvaluationContext* GetContext(const FRole& Role) const;
	UE_API FChooserEvaluationContext* GetContext(const FRole& Role);
	UE_DEPRECATED(5.6, "Use GetContexts instead")
	const TArray<const UAnimInstance*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> GetAnimInstances() const 
	{
	 	TArray<const UAnimInstance*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> AnimInstances;
	 	for (const FChooserEvaluationContext* AnimContext : AnimContexts)
	 	{
	 		AnimInstances.Add(Cast<UAnimInstance>(AnimContext->GetFirstObjectParam()));
	 	}
	 	return AnimInstances;
	}

	UE_DEPRECATED(5.6, "Use GetContexts instead")
	const TArray<const UAnimInstance*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> GetAnimContexts() const 
	{
		TArray<const UAnimInstance*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> AnimInstances;
		for (const FChooserEvaluationContext* AnimContext : AnimContexts)
		{
			AnimInstances.Add(Cast<UAnimInstance>(AnimContext->GetFirstObjectParam()));
		}
		return AnimInstances;
	}
	
	TArrayView<FChooserEvaluationContext*> GetContexts() { return AnimContexts; }

	const FRoleToIndex& GetRoleToIndex() const { return RoleToIndex; }

	void SetAssetsToConsider(TConstArrayView<const UObject*> InAssetsToConsider) { AssetsToConsider = InAssetsToConsider; }
	TConstArrayView<const UObject*> GetAssetsToConsider() const { return AssetsToConsider; }
	// Experimental, this feature might be removed without warning, not for production use
	const FPoseSearchEvent& GetEventToSearch() const { return EventToSearch; }
	
	// returns the world space transform of the bone SchemaBoneIdx at time SampleTime
	UE_API FTransform GetWorldBoneTransformAtTime(float SampleTime, const FRole& SampleRole, int8 SchemaBoneIdx);

#if WITH_EDITOR
	void SetAsyncBuildIndexInProgress() { bAsyncBuildIndexInProgress = true; }
	void ResetAsyncBuildIndexInProgress() { bAsyncBuildIndexInProgress = false; }
	bool IsAsyncBuildIndexInProgress() const { return bAsyncBuildIndexInProgress; }
#endif // WITH_EDITOR

	bool AnyCachedQuery() const { return !CachedQueries.IsEmpty(); }
	void AddNewFeatureVectorBuilder(const UPoseSearchSchema* Schema) { CachedQueries.Emplace(Schema); }
	UE_API TArrayView<float> EditFeatureVector();
	
	UE_API const UPoseSearchFeatureChannel* GetCachedChannelData(uint32 ChannelUniqueIdentifier, const UPoseSearchFeatureChannel* Channel, TConstArrayView<float>& CachedChannelData);
	bool IsUseCachedChannelData() const { return bUseCachedChannelData; }
	void SetUseCachedChannelData(bool bInUseCachedChannelData) { bUseCachedChannelData = bInUseCachedChannelData; }
	
	// Experimental, this feature might be removed without warning, not for production use
	bool IsContinuingInteraction() const { return bIsContinuingInteraction; }
	// Experimental, this feature might be removed without warning, not for production use
	void SetIsContinuingInteraction(bool bInIsContinuingInteraction) { bIsContinuingInteraction = bInIsContinuingInteraction; }

private:
	UE_API float GetSampleCurveValueInternal(float SampleTime, const FName& CurveName, const FRole& SampleRole);
	UE_API FVector GetSamplePositionInternal(float SampleTime, float OriginTime, int8 SchemaSampleBoneIdx, int8 SchemaOriginBoneIdx, const FRole& SampleRole, const FRole& OriginRole, const FVector* SampleBonePositionWorldOverride = nullptr);
	UE_API FQuat GetSampleRotationInternal(float SampleTime, float OriginTime, int8 SchemaSampleBoneIdx, int8 SchemaOriginBoneIdx, const FRole& SampleRole, const FRole& OriginRole, const FQuat* SampleBoneRotationWorldOverride = nullptr);
	UE_API FTransform GetWorldRootBoneTransformAtTime(float SampleTime, const FRole& SampleRole) const;
	
	TArray<FChooserEvaluationContext*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> AnimContexts;
	TArray<const IPoseHistory*, TInlineAllocator<PreallocatedRolesNum, TMemStackAllocator<>>> PoseHistories;
	FRoleToIndex RoleToIndex;
	
	// if AssetsToConsider is not empty, we'll search only for poses from UObject(s) that are in the AssetsToConsider
	TConstArrayView<const UObject*> AssetsToConsider;
	const FPoseSearchEvent EventToSearch;

	const float DesiredPermutationTimeOffset = 0.f;
	const FPoseIndicesHistory* PoseIndicesHistory = nullptr;
	const FSearchResult& CurrentResult;
	const FFloatInterval PoseJumpThresholdTime;
	bool bUseCachedChannelData = false;

	TConstArrayView<float> CurrentResultPoseVector;

	TStackAlignedArray<float> CurrentResultPoseVectorData;

	// transforms cached in world space
	TMap<uint32, FTransform, TInlineSetAllocator<64, TMemStackSetAllocator<>>> CachedTransforms;

	TArray<FCachedQuery, TInlineAllocator<PreallocatedCachedQueriesNum, TMemStackAllocator<>>> CachedQueries;

	// mapping channel unique identifier (hash) to FCachedChannel
	TMap<uint32, FCachedChannel, TInlineSetAllocator<PreallocatedCachedChannelDataNum, TMemStackSetAllocator<>>> CachedChannels;

	float CurrentBestTotalCost = MAX_flt;

	// @todo: add FPoseSearchContinuingProperties here and reconstuct the continuing pose FSearchResult for the previous frame from it
	// Experimental, this feature might be removed without warning, not for production use
	bool bIsContinuingInteraction = false;

#if WITH_EDITOR
	bool bAsyncBuildIndexInProgress = false;
#endif // WITH_EDITOR

#if UE_POSE_SEARCH_TRACE_ENABLED

	struct FPoseCandidateIdCost
	{
		int32 PoseIdx = 0;
		FPoseSearchCost Cost;
		bool operator<(const FPoseCandidateIdCost& Other) const { return Other.Cost < Cost; } // Reverse compare because BestCandidates is a max heap
	};
	
public:

	struct FPoseCandidate : public FPoseCandidateIdCost
	{
		EPoseCandidateFlags PoseCandidateFlags = EPoseCandidateFlags::None;
	};

	void Track(const UPoseSearchDatabase* Database, int32 PoseIdx = INDEX_NONE, EPoseCandidateFlags PoseCandidateFlags = EPoseCandidateFlags::None, const FPoseSearchCost& Cost = FPoseSearchCost())
	{
		check(Database);

		FBestPoseCandidates& BestPoseCandidates = BestPoseCandidatesMap.FindOrAdd(Database);
		if (PoseIdx != INDEX_NONE)
		{
			BestPoseCandidates.Add(PoseIdx, PoseCandidateFlags, Cost);
		}
	}

	struct FBestPoseCandidates
	{
		FBestPoseCandidates()
		{
			// preallocating memory to avoid multiple reallocations / rehashing
			PoseCandidateHeap.Reserve(MaxNumberOfCollectedPoseCandidatesPerDatabase);
			PoseIdxToFlags.Empty(MaxNumberOfCollectedPoseCandidatesPerDatabase);
		}

		void Add(int32 PoseIdx, EPoseCandidateFlags PoseCandidateFlags, const FPoseSearchCost& Cost)
		{
			check(PoseIdx >= 0);
			if (EPoseCandidateFlags* PoseIdxPoseCandidateFlags = PoseIdxToFlags.Find(PoseIdx))
			{
				*PoseIdxPoseCandidateFlags |= PoseCandidateFlags;
			}
			else if (PoseCandidateHeap.Num() < MaxNumberOfCollectedPoseCandidatesPerDatabase || Cost < PoseCandidateHeap.HeapTop().Cost)
			{
				bool bPoppedContinuingPoseCandidate = false;
				FPoseCandidate ContinuingPoseCandidate;
				while (PoseCandidateHeap.Num() >= MaxNumberOfCollectedPoseCandidatesPerDatabase)
				{
					FPoseCandidate PoppedPoseCandidate;
					Pop(PoppedPoseCandidate);

					if (EnumHasAnyFlags(PoppedPoseCandidate.PoseCandidateFlags, EPoseCandidateFlags::Valid_ContinuingPose))
					{
						// we can only have one continuing pose candidate
						check(!bPoppedContinuingPoseCandidate);
						ContinuingPoseCandidate = PoppedPoseCandidate;
						bPoppedContinuingPoseCandidate = true;
					}					
				}

				if (bPoppedContinuingPoseCandidate)
				{
					// if we popped the continuing pose candidate, we make some space for it and push it back
					FPoseCandidate PoppedPoseCandidate;
					Pop(PoppedPoseCandidate);
					PoseCandidateHeap.HeapPush(ContinuingPoseCandidate);
					PoseIdxToFlags.Add(ContinuingPoseCandidate.PoseIdx, ContinuingPoseCandidate.PoseCandidateFlags);
				}

				FPoseCandidate PoseCandidate;
				PoseCandidate.PoseIdx = PoseIdx;
				PoseCandidate.Cost = Cost;
				PoseCandidateHeap.HeapPush(PoseCandidate);
				PoseIdxToFlags.Add(PoseIdx, PoseCandidateFlags);
			}
		}

		int32 Num() const
		{
			return PoseCandidateHeap.Num();
		}

		FPoseCandidate GetUnsortedCandidate(int32 Index) const
		{
			FPoseCandidate PoseCandidate;
			const FPoseCandidateIdCost& PoseCandidateIdCost = PoseCandidateHeap[Index];
			PoseCandidate.PoseIdx = PoseCandidateIdCost.PoseIdx;
			PoseCandidate.Cost = PoseCandidateIdCost.Cost;
			PoseCandidate.PoseCandidateFlags = PoseIdxToFlags[PoseCandidateIdCost.PoseIdx];
			return PoseCandidate;
		}

	private:
		void Pop(FPoseCandidate& OutItem)
		{
			PoseCandidateHeap.HeapPop(OutItem, EAllowShrinking::No);
			OutItem.PoseCandidateFlags = PoseIdxToFlags.FindAndRemoveChecked(OutItem.PoseIdx);
		}

		TArray<FPoseCandidateIdCost> PoseCandidateHeap;
		TMap<int32, EPoseCandidateFlags> PoseIdxToFlags;
	};
	
	const TMap<const UPoseSearchDatabase*, FBestPoseCandidates>& GetBestPoseCandidatesMap() const
	{
		return BestPoseCandidatesMap;
	}

private:
	TMap<const UPoseSearchDatabase*, FBestPoseCandidates> BestPoseCandidatesMap;
#endif // UE_POSE_SEARCH_TRACE_ENABLED
};

} // namespace UE::PoseSearch

#undef UE_API
