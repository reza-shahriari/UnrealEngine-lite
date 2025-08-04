// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayInteractionsTypes.h"
#include "Annotations/SmartObjectSlotEntranceAnnotation.h"
#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_6
#include "SmartObjectSubsystem.h"
#endif
#include "StateTreeTask_FindSlotEntranceLocation.generated.h"

class USmartObjectSubsystem;
class UNavigationQueryFilter;
enum class FSmartObjectSlotEntrySelectionMethod : uint8;

USTRUCT()
struct FStateTreeTask_FindSlotEntranceLocation_InstanceData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "Context")
	TObjectPtr<AActor> UserActor = nullptr;
	
	/** Slot to use as reference to find the result slot. */
	UPROPERTY(EditAnywhere, Category = "Input")
	FSmartObjectSlotHandle ReferenceSlot;

	UPROPERTY(EditAnywhere, Category = "Output")
	FTransform EntryTransform;	// @todo: rename EntranceTransform

	UPROPERTY(EditAnywhere, Category = "Output")
	FGameplayTagContainer EntranceTags;
};

/**
 * Finds entrance location for a Smart Object slot. The query will use slot entrance annotations as candidates.
 * Each candidate is ranked (e.g. based on distance), and optionally validated to be close to a navigable space and without collisions.
 */
USTRUCT(meta = (DisplayName = "Find Slot Entrance Location", Category="Gameplay Interactions|Smart Object"))
struct FStateTreeTask_FindSlotEntranceLocation : public FGameplayInteractionStateTreeTask
{
	GENERATED_BODY()

	GAMEPLAYINTERACTIONSMODULE_API FStateTreeTask_FindSlotEntranceLocation();
	
	using FInstanceDataType = FStateTreeTask_FindSlotEntranceLocation_InstanceData;

protected:
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	GAMEPLAYINTERACTIONSMODULE_API virtual bool Link(FStateTreeLinker& Linker) override;
	GAMEPLAYINTERACTIONSMODULE_API virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
#if WITH_EDITOR
	GAMEPLAYINTERACTIONSMODULE_API virtual FText GetDescription(const FGuid& ID, FStateTreeDataView InstanceDataView, const IStateTreeBindingLookup& BindingLookup, EStateTreeNodeFormatting Formatting = EStateTreeNodeFormatting::Text) const override;
	virtual FName GetIconName() const override
	{
		return FName("StateTreeEditorStyle|Node.Navigation");
	}
	virtual FColor GetIconColor() const override
	{
		return UE::StateTree::Colors::Blue;
	}
#endif
	virtual bool UpdateResult(const FStateTreeExecutionContext& Context) const;

	/** Method to select an entry when multiple entries are present. */
	UPROPERTY(EditAnywhere, Category="Default")
	FSmartObjectSlotEntrySelectionMethod SelectMethod;

	/** If true, the result is required to be in or close to a navigable space. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bProjectNavigationLocation = true;

	/** If true, try to trace the location on ground. If trace fails, an entry is discarded. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bTraceGroundLocation = true;

	/** If true, check collisions between navigation location and slot location. If collisions are found, an entry is discarded. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bCheckTransitionTrajectory = true;

	/** If true, check user capsule collisions at the entrance location. Uses capsule dimensions set in the validation filter. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bCheckEntranceLocationOverlap = true;

	/** If true, check user capsule collisions at the slot location. Uses capsule dimensions set in an annotation on the slot. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bCheckSlotLocationOverlap = true;

	/** If true, the result rotation will only contain rotation around the UP axis (i.e., Yaw only; Pitch and Roll set to 0). */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bUseUpAxisLockedRotation = true;

	/** If true, include slot location as candidate if no entry annotation is present. */
	UPROPERTY(EditAnywhere, Category="Default")
	bool bUseSlotLocationAsFallbackCandidate = false;

	/** Whether we're looking for an entry or exit location. */
	UPROPERTY(EditAnywhere, Category="Default")
	ESmartObjectSlotNavigationLocationType LocationType = ESmartObjectSlotNavigationLocationType::Entry;
	
	/** Validation filter to apply to query. */
	UPROPERTY(EditAnywhere, Category="Default")
	TSubclassOf<USmartObjectSlotValidationFilter> ValidationFilter = nullptr;

	/** Handle to retrieve USmartObjectSubsystem. */
	TStateTreeExternalDataHandle<USmartObjectSubsystem> SmartObjectSubsystemHandle;
};
