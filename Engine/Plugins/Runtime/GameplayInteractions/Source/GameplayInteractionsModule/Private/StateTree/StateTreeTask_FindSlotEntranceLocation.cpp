// Copyright Epic Games, Inc. All Rights Reserved.

#include "StateTree/StateTreeTask_FindSlotEntranceLocation.h"
#include "StateTreeExecutionContext.h"
#include "SmartObjectSubsystem.h"
#include "VisualLogger/VisualLogger.h"
#include "StateTreeLinker.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(StateTreeTask_FindSlotEntranceLocation)

#define LOCTEXT_NAMESPACE "GameplayInteractions"

FStateTreeTask_FindSlotEntranceLocation::FStateTreeTask_FindSlotEntranceLocation()
	: SelectMethod(FSmartObjectSlotEntrySelectionMethod::First)
{
	// No tick needed.
	bShouldCallTick = false;
	// No need to update bound properties after enter state.
	bShouldCopyBoundPropertiesOnTick = false;
	bShouldCopyBoundPropertiesOnExitState = false;
}

bool FStateTreeTask_FindSlotEntranceLocation::Link(FStateTreeLinker& Linker)
{
	Linker.LinkExternalData(SmartObjectSubsystemHandle);
	return true;
}

bool FStateTreeTask_FindSlotEntranceLocation::UpdateResult(const FStateTreeExecutionContext& Context) const
{
	const USmartObjectSubsystem& SmartObjectSubsystem = Context.GetExternalData(SmartObjectSubsystemHandle);
	FInstanceDataType& InstanceData = Context.GetInstanceData(*this);

	if (!InstanceData.ReferenceSlot.IsValid())
	{
		UE_VLOG_UELOG(Context.GetOwner(), LogStateTree, Error, TEXT("[StateTreeTask_FindSlotNavigationLocation] Expected valid ReferenceSlot handle."));
		return false;
	}

	if (!InstanceData.UserActor)
	{
		UE_VLOG_UELOG(Context.GetOwner(), LogStateTree, Error, TEXT("[StateTreeTask_FindSlotNavigationLocation] Expected valid UserActor handle."));
		return false;
	}

	FSmartObjectSlotEntranceLocationRequest Request;
	Request.UserActor = InstanceData.UserActor;
	Request.ValidationFilter = ValidationFilter;
	Request.SelectMethod = SelectMethod;
	Request.bProjectNavigationLocation = bProjectNavigationLocation;
	Request.bTraceGroundLocation = bTraceGroundLocation;
	Request.bCheckEntranceLocationOverlap = bCheckEntranceLocationOverlap;
	Request.bCheckSlotLocationOverlap = bCheckSlotLocationOverlap;
	Request.bCheckTransitionTrajectory = bCheckTransitionTrajectory;
	Request.bUseUpAxisLockedRotation = bUseUpAxisLockedRotation;
	Request.bUseSlotLocationAsFallback = bUseSlotLocationAsFallbackCandidate;

	Request.LocationType = LocationType;
	Request.SearchLocation = InstanceData.UserActor->GetActorLocation();

	FSmartObjectSlotEntranceLocationResult EntryLocation;
	if (SmartObjectSubsystem.FindEntranceLocationForSlot(InstanceData.ReferenceSlot, Request, EntryLocation))
	{
		InstanceData.EntryTransform = FTransform(EntryLocation.Rotation, EntryLocation.Location);
		InstanceData.EntranceTags = EntryLocation.Tags;
		return true;
	}

	return false;
}

EStateTreeRunStatus FStateTreeTask_FindSlotEntranceLocation::EnterState(FStateTreeExecutionContext& Context, const FStateTreeTransitionResult& Transition) const
{
	if (!UpdateResult(Context))
	{
		return EStateTreeRunStatus::Failed;
	}
	
	return EStateTreeRunStatus::Running;
}

#if WITH_EDITOR
FText FStateTreeTask_FindSlotEntranceLocation::GetDescription(const FGuid& ID, FStateTreeDataView InstanceDataView, const IStateTreeBindingLookup& BindingLookup, EStateTreeNodeFormatting Formatting) const
{
	const FInstanceDataType* InstanceData = InstanceDataView.GetPtr<FInstanceDataType>();
	check(InstanceData);

	// Slot
	FText SlotValue = BindingLookup.GetBindingSourceDisplayName(FPropertyBindingPath(ID, GET_MEMBER_NAME_CHECKED(FInstanceDataType, ReferenceSlot)), Formatting);
	if (SlotValue.IsEmpty())
	{
		SlotValue = LOCTEXT("None", "None");
	}

	// Actor
	FText ActorValue = BindingLookup.GetBindingSourceDisplayName(FPropertyBindingPath(ID, GET_MEMBER_NAME_CHECKED(FInstanceDataType, UserActor)), Formatting);
	if (ActorValue.IsEmpty())
	{
		ActorValue = LOCTEXT("None", "None");
	}

	const FText LocationTypeText = UEnum::GetDisplayValueAsText(LocationType);

	const FText Format = (Formatting == EStateTreeNodeFormatting::RichText)
		? LOCTEXT("FindSlotEntranceLocationRich", "<b>Find {EntryOrExit} Location</> <s>for slot</> {Slot} <s>with</> {Actor}")
		: LOCTEXT("FindSlotEntranceLocation", "Find {EntryOrExit} Location for slot {Slot} with {Actor}");

	return FText::FormatNamed(Format,
		TEXT("EntryOrExit"), LocationTypeText,
		TEXT("Slot"), SlotValue,
		TEXT("Actor"), ActorValue);
}
#endif

#undef LOCTEXT_NAMESPACE
