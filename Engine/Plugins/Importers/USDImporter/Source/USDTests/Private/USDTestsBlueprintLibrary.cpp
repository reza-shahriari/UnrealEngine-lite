// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDTestsBlueprintLibrary.h"

#include "Objects/USDInfoCache.h"
#include "USDErrorUtils.h"
#include "USDStageActor.h"

#include "CoreMinimal.h"
#include "Editor.h"
#include "Editor/Transactor.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"

bool USDTestsBlueprintLibrary::RecompileBlueprintStageActor(AUsdStageActor* BlueprintDerivedStageActor)
{
#if WITH_EDITOR
	if (!BlueprintDerivedStageActor)
	{
		return false;
	}

	UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(BlueprintDerivedStageActor->GetClass());
	if (!BPClass)
	{
		return false;
	}

	UBlueprint* BP = Cast<UBlueprint>(BPClass->ClassGeneratedBy);
	if (!BP)
	{
		return false;
	}

	// Compile blueprint. Copied from BlueprintEditorTests.cpp
	FBlueprintEditorUtils::RefreshAllNodes(BP);
	FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection);
	if (BP->Status == EBlueprintStatus::BS_UpToDate)
	{
		USD_LOG_INFO(TEXT("Blueprint compiled successfully (%s)"), *BP->GetName());
		return true;
	}
	else if (BP->Status == EBlueprintStatus::BS_UpToDateWithWarnings)
	{
		USD_LOG_WARNING(TEXT("Blueprint compiled successfully with warnings(%s)"), *BP->GetName());
		return true;
	}
	else if (BP->Status == EBlueprintStatus::BS_Error)
	{
		USD_LOG_ERROR(TEXT("Blueprint failed to compile (%s)"), *BP->GetName());
		return false;
	}

	USD_LOG_ERROR(TEXT("Blueprint is in an unexpected state after compiling (%s)"), *BP->GetName());
#endif	  // WITH_EDITOR
	return false;
}

void USDTestsBlueprintLibrary::DirtyStageActorBlueprint(AUsdStageActor* BlueprintDerivedStageActor)
{
#if WITH_EDITOR
	if (!BlueprintDerivedStageActor)
	{
		return;
	}

	UBlueprintGeneratedClass* BPClass = Cast<UBlueprintGeneratedClass>(BlueprintDerivedStageActor->GetClass());
	if (!BPClass)
	{
		return;
	}

	UBlueprint* BP = Cast<UBlueprint>(BPClass->ClassGeneratedBy);
	if (!BP)
	{
		return;
	}

	// We need to add something that potentially modifies the blueprint code. Just dirtying the blueprint or changing it status doesn't trigger
	// a recompile when going into PIE
	FName VarName = FBlueprintEditorUtils::FindUniqueKismetName(BP, TEXT("NewVar"));
	FEdGraphPinType StringPinType(UEdGraphSchema_K2::PC_String, NAME_None, nullptr, EPinContainerType::None, false, FEdGraphTerminalType());
	bool bSuccess = FBlueprintEditorUtils::AddMemberVariable(BP, VarName, StringPinType);
	if (!bSuccess)
	{
		USD_LOG_ERROR(TEXT("Failed to add new variable to blueprint (%s)"), *BP->GetName());
	}
#endif	  // WITH_EDITOR
}

int64 USDTestsBlueprintLibrary::GetSubtreeVertexCount(AUsdStageActor* StageActor, const FString& PrimPath)
{
	if (StageActor)
	{
		if (UUsdInfoCache* Cache = StageActor->UsdInfoCache)
		{
			TOptional<uint64> Result = Cache->GetInner().GetSubtreeVertexCount(UE::FSdfPath{*PrimPath});
			if (Result.IsSet())
			{
				// Narrowing conversion here, but we're only using this for our test scenes, which have at most a few thousand vertices
				return static_cast<int64>(Result.GetValue());
			}
			else
			{
				return -1;
			}
		}
	}

	return -1;
}

int64 USDTestsBlueprintLibrary::GetSubtreeMaterialSlotCount(AUsdStageActor* StageActor, const FString& PrimPath)
{
	if (StageActor)
	{
		if (UUsdInfoCache* Cache = StageActor->UsdInfoCache)
		{
			TOptional<uint64> Result = Cache->GetInner().GetSubtreeMaterialSlotCount(UE::FSdfPath{*PrimPath});
			if (Result.IsSet())
			{
				return static_cast<int64>(Result.GetValue());
			}
			else
			{
				return -1;
			}
		}
	}

	return -1;
}

void USDTestsBlueprintLibrary::SetUsdStageCpp(AUsdStageActor* StageActor, const FString& NewStageRootLayer)
{
	if (!StageActor)
	{
		return;
	}

	UE::FUsdStage NewStage = UnrealUSDWrapper::OpenStage(*NewStageRootLayer, EUsdInitialLoadSet::LoadAll);
	StageActor->SetUsdStage(NewStage);
}

void USDTestsBlueprintLibrary::ClearTransactionHistory()
{
	if (GEditor)
	{
		if (UTransactor* Transactor = GEditor->Trans)
		{
			Transactor->Reset(NSLOCTEXT("USDTests", "ClearTransactionHistoryReason", "USDTestsBlueprintLibrary::ClearTransactionHistory was called"));
		}
	}
}
