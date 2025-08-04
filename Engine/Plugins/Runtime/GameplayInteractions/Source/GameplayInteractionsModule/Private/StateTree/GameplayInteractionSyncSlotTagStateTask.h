// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "GameplayInteractionsTypes.h"
#include "GameplayInteractionSyncSlotTagStateTask.generated.h"

enum class EDataValidationResult : uint8;

class USmartObjectSubsystem;

USTRUCT()
struct FGameplayInteractionSyncSlotTagStateInstanceData
{
	GENERATED_BODY()

	/** The target slot to monitor */
	UPROPERTY(EditAnywhere, Category="Input")
	FSmartObjectSlotHandle TargetSlot;

	FDelegateHandle OnEventHandle;
	bool bBreakSignalled = false;
};

/**
 * Task to monitor existence of a Gameplay Tag on the specified Smart Object slot.
 * If the monitored Gameplay Tag does not exists on the target slot, or this task completes, a BreakEventTag is sent to the the target slot as well as on the running State Tree.
 * This allows to the task to be used to sync State Tree execution between State Tree instances via a Smart Object slot.
 */
USTRUCT(meta = (DisplayName = "Sync Slot Tag State", Category="Gameplay Interactions|Smart Object"))
struct FGameplayInteractionSyncSlotTagStateTask : public FGameplayInteractionStateTreeTask
{
	GENERATED_BODY()

	FGameplayInteractionSyncSlotTagStateTask();
	
	using FInstanceDataType = FGameplayInteractionSyncSlotTagStateInstanceData;

protected:
	virtual const UStruct* GetInstanceDataType() const override { return FInstanceDataType::StaticStruct(); }

	virtual bool Link(FStateTreeLinker& Linker) override;
	virtual EStateTreeRunStatus EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
	virtual void ExitState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const override;
#if WITH_EDITOR
	virtual EDataValidationResult Compile(UE::StateTree::ICompileNodeContext& Context) override;
	virtual FText GetDescription(const FGuid& ID, FStateTreeDataView InstanceDataView, const IStateTreeBindingLookup& BindingLookup, EStateTreeNodeFormatting Formatting = EStateTreeNodeFormatting::Text) const override;
	virtual FName GetIconName() const override
	{
		return FName("StateTreeEditorStyle|Node.Sync");
	}
	virtual FColor GetIconColor() const override
	{
		return UE::StateTree::Colors::Blue;
	}
#endif
	
	/** The tag to monitor on the target slot. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FGameplayTag TagToMonitor;

	/** Event to send when the monitored tag is not present anymore, or when this task becomes inactive. */
	UPROPERTY(EditAnywhere, Category = "Parameter")
	FGameplayTag BreakEventTag;

	/** Handle to retrieve USmartObjectSubsystem. */
	TStateTreeExternalDataHandle<USmartObjectSubsystem> SmartObjectSubsystemHandle;
};
