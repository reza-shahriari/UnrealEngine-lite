// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetReferencingPolicySubsystem.h"

#include "AssetReferencingDomains.h"
#include "AssetReferencingPolicySettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DomainAssetReferenceFilter.h"
#include "Editor.h"
#include "Misc/PathViews.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AssetReferencingPolicySubsystem)

#define LOCTEXT_NAMESPACE "AssetReferencingPolicy"

namespace AssetReferencingPolicySubsystem::Private
{
	bool bCheckForMissingRefsToExternalActors = false;
	static FAutoConsoleVariableRef CVarCheckForMissingRefsToExternalActors(
		TEXT("AssetReferencingPolicy.CheckForMissingRefsToExternalActors"),
		bCheckForMissingRefsToExternalActors,
		TEXT("Error on missing dependencies to external actors when validating asset references.")
	);
}

bool UAssetReferencingPolicySubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return true;
}

void UAssetReferencingPolicySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	check(GEditor);
	GEditor->OnMakeAssetReferenceFilter().BindUObject(this, &ThisClass::HandleMakeAssetReferenceFilter);

	DomainDB = MakeShared<FDomainDatabase>();
	DomainDB->Init();

	FEditorDelegates::OnPreAssetValidation.AddUObject(this, &UAssetReferencingPolicySubsystem::UpdateDBIfNecessary);
}

void UAssetReferencingPolicySubsystem::Deinitialize()
{
	check(GEditor);
	GEditor->OnMakeAssetReferenceFilter().Unbind();
	DomainDB.Reset();
}

bool UAssetReferencingPolicySubsystem::ShouldValidateAssetReferences(const FAssetData& Asset) const
{
	TSharedPtr<FDomainData> DomainData = GetDomainDB()->FindDomainFromAssetData(Asset);

	const bool bIsInUnrestrictedFolder = DomainData && DomainData->IsValid() && DomainData->bCanSeeEverything;
	return !bIsInUnrestrictedFolder;
}

TValueOrError<void, TArray<FAssetReferenceError>> UAssetReferencingPolicySubsystem::ValidateAssetReferences(const FAssetData& InAssetData) const
{
	return ValidateAssetReferences(InAssetData, EAssetReferenceFilterRole::None);
}

TValueOrError<void, TArray<FAssetReferenceError>> UAssetReferencingPolicySubsystem::ValidateAssetReferences(const FAssetData& InAssetData, const EAssetReferenceFilterRole Role) const
{
	IAssetRegistry& AssetRegistry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	TArray<FAssetReferenceError> Errors;
	static const FName TransientName = GetTransientPackage()->GetFName();

	// Check for missing soft or hard references to cinematic and developers content
	FName PackageFName = InAssetData.PackageName;
	TArray<FName> SoftDependencies;
	TArray<FAssetData> AllDependencyAssets;

	const UAssetReferencingPolicySettings* Settings = GetDefault<UAssetReferencingPolicySettings>();
	UE::AssetRegistry::EDependencyQuery QueryFlags = UE::AssetRegistry::EDependencyQuery::NoRequirements;

	if (Settings->bIgnoreEditorOnlyReferences)
	{
		// If the rules allow ignoring editor-only referencers, restrict the dependencies to game references
		QueryFlags = UE::AssetRegistry::EDependencyQuery::Game;
	}

	AssetRegistry.GetDependencies(PackageFName, SoftDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Soft | QueryFlags);
	for (FName SoftDependency : SoftDependencies)
	{
		FNameBuilder SoftDependencyStr(SoftDependency);
		if (!FPackageName::IsScriptPackage(SoftDependencyStr.ToView()) && !FPackageName::IsVersePackage(SoftDependencyStr.ToView()))
		{
			TArray<FAssetData> DependencyAssets;
			AssetRegistry.GetAssetsByPackageName(SoftDependency, DependencyAssets, true);
			if (DependencyAssets.Num() == 0)
			{
				if (SoftDependency != TransientName)
				{
					Errors.Add(FAssetReferenceError{
						.Type = EAssetReferenceErrorType::DoesNotExist,
						.ReferencedAsset = FAssetData(SoftDependency, FName(FPathViews::GetPath(SoftDependencyStr)), FName(), UObject::StaticClass()->GetClassPathName()),
						.Message = FText::Format(LOCTEXT("IllegalReference_MissingSoftRef", "Soft references {0} which does not exist"), FText::FromName(SoftDependency)),
					});
				}
			}
			else
			{
				AllDependencyAssets.Append(DependencyAssets);
			}
		}
	}

	// Now check hard references to cinematic and developers content
	TArray<FName> HardDependencies;
	AssetRegistry.GetDependencies(PackageFName, HardDependencies, UE::AssetRegistry::EDependencyCategory::Package, UE::AssetRegistry::EDependencyQuery::Hard | QueryFlags);
	for (FName HardDependency : HardDependencies)
	{
		AssetRegistry.GetAssetsByPackageName(HardDependency, AllDependencyAssets, true);
	}

	// Check for missing external actor references which can manifest as LoadErrors and show as either Hard/Soft/Game/EditorOnly.
	if (AssetReferencingPolicySubsystem::Private::bCheckForMissingRefsToExternalActors)
	{
		TArray<FName> PkgDependencies;
		AssetRegistry.GetDependencies(PackageFName, PkgDependencies, UE::AssetRegistry::EDependencyCategory::Package);
		for (FName Dependency : PkgDependencies)
		{
			TArray<FAssetData> DependencyAssets;
			AssetRegistry.GetAssetsByPackageName(Dependency, DependencyAssets, true);
			
			FNameBuilder DependencyStr(Dependency);
			const bool bIsExternalActorPackage = DependencyStr.ToView().Contains(FPackagePath::GetExternalActorsFolderName());
			if (bIsExternalActorPackage && DependencyAssets.Num() == 0)
			{
				Errors.Add(FAssetReferenceError{
					.Type = EAssetReferenceErrorType::DoesNotExist,
					.ReferencedAsset = FAssetData(Dependency, FName(FPathViews::GetPath(DependencyStr)), FName(), UObject::StaticClass()->GetClassPathName()),
					.Message = FText::Format(LOCTEXT("IllegalReference_MissingRefToExternalActor", "references external actor {0} which does not exist"), FText::FromName(Dependency)),
				});
			}
		}
	}

	if (AllDependencyAssets.Num() > 0)
	{
		FAssetReferenceFilterContext AssetReferenceFilterContext;
		AssetReferenceFilterContext.AddRole(Role);
		AssetReferenceFilterContext.AddReferencingAsset(InAssetData);
		TSharedPtr<IAssetReferenceFilter> AssetReferenceFilter = GEditor ? GEditor->MakeAssetReferenceFilter(AssetReferenceFilterContext) : nullptr;
		if (ensure(AssetReferenceFilter.IsValid()))
		{
			const bool bReferenceErrorsAsWarnings = AssetReferenceFilter->DoesAssetDowngradeReferenceErrorsToWarnings(InAssetData);
			for (const FAssetData& Dependency : AllDependencyAssets)
			{
				FText FailureReason;
				if (!AssetReferenceFilter->PassesFilter(Dependency, &FailureReason))
				{
					Errors.Add(FAssetReferenceError{
						.bTreatErrorAsWarning = bReferenceErrorsAsWarnings,
						.Type = EAssetReferenceErrorType::Illegal,
						.ReferencedAsset = Dependency,
						.Message = FText::Format(LOCTEXT("IllegalReference_AssetFilterFail", "Illegal reference: {0}"), FailureReason),
					});
				}
			}
		}
	}
	
	if (Errors.Num())
	{
		return MakeError(MoveTemp(Errors));
	}
	else
	{
		return MakeValue();
	}
}

TSharedPtr<IAssetReferenceFilter> UAssetReferencingPolicySubsystem::HandleMakeAssetReferenceFilter(const FAssetReferenceFilterContext& Context)
{
	return MakeShareable(new FDomainAssetReferenceFilter(Context, GetDomainDB()));
}

void UAssetReferencingPolicySubsystem::UpdateDBIfNecessary() const
{
	DomainDB->UpdateIfNecessary();
}

TSharedPtr<FDomainDatabase> UAssetReferencingPolicySubsystem::GetDomainDB() const
{
	DomainDB->UpdateIfNecessary();
	return DomainDB;
}

FAutoConsoleCommandWithWorldAndArgs GListDomainDatabaseCmd(
	TEXT("Editor.AssetReferenceRestrictions.ListDomainDatabase"),
	TEXT("Lists all of the asset reference domains the AssetReferenceRestrictions plugin knows about"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(
		[](const TArray<FString>& Params, UWorld* World)
		{
			if (UAssetReferencingPolicySubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetReferencingPolicySubsystem>())
			{
				Subsystem->GetDomainDB()->DebugPrintAllDomains();
			}
		}));

#undef LOCTEXT_NAMESPACE
