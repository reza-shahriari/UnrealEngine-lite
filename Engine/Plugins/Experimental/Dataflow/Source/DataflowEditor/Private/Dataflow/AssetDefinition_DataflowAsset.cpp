// Copyright Epic Games, Inc. All Rights Reserved.

#include "Dataflow/AssetDefinition_DataflowAsset.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "ContentBrowserModule.h"
#include "Dataflow/DataflowEditor.h"
#include "Dataflow/DataflowEditorModule.h"
#include "Dataflow/DataflowObject.h"
#include "Dialog/SMessageDialog.h"
#include "IContentBrowserSingleton.h"
#include "Math/Color.h"
#include "Misc/FileHelper.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"


#define LOCTEXT_NAMESPACE "AssetActions_DataflowAsset"


bool bCanEditDataflow = false;
FAutoConsoleVariableRef CVarDataflowIsEditable(TEXT("p.Dataflow.IsEditable"), bCanEditDataflow, TEXT("Whether to allow edits of the dataflow [def:true]"));

namespace UE::DataflowAssetDefinitionHelpers
{
	// Return true if we should proceed, false if we should re-open the dialog
	bool CreateNewDataflowAsset(const UObject* Asset, UObject*& OutDataflowAsset)
	{
		const UClass* const DataflowClass = UDataflow::StaticClass();

		FSaveAssetDialogConfig NewDataflowAssetDialogConfig;
		{
			const FString PackageName = Asset->GetOutermost()->GetName();
			NewDataflowAssetDialogConfig.DefaultPath = FPackageName::GetLongPackagePath(PackageName);
			const FString AssetName = Asset->GetName();
			NewDataflowAssetDialogConfig.DefaultAssetName = AssetName + "_Dataflow";
			NewDataflowAssetDialogConfig.AssetClassNames.Add(DataflowClass->GetClassPathName());
			NewDataflowAssetDialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::Disallow;
			NewDataflowAssetDialogConfig.DialogTitleOverride = LOCTEXT("NewDataflowAssetDialogTitle", "Save Dataflow Asset As");
		}

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");

		FString NewPackageName;
		FText OutError;
		for (bool bFilenameValid = false; !bFilenameValid; bFilenameValid = FFileHelper::IsFilenameValidForSaving(NewPackageName, OutError))
		{
			const FString AssetSavePath = ContentBrowserModule.Get().CreateModalSaveAssetDialog(NewDataflowAssetDialogConfig);
			if (AssetSavePath.IsEmpty())
			{
				OutDataflowAsset = nullptr;
				return false;
			}
			NewPackageName = FPackageName::ObjectPathToPackageName(AssetSavePath);
		}

		const FName NewAssetName(FPackageName::GetLongPackageAssetName(NewPackageName));
		UPackage* const NewPackage = CreatePackage(*NewPackageName);
		UObject* const NewAsset = NewObject<UObject>(NewPackage, DataflowClass, NewAssetName, RF_Public | RF_Standalone | RF_Transactional);

		NewAsset->MarkPackageDirty();

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(NewAsset);

		OutDataflowAsset = NewAsset;
		return true;
	}


	// Return true if we should proceed, false if we should re-open the dialog
	bool OpenDataflowAsset(const UObject* Asset, UObject*& OutDataflowAsset)
	{
		const UClass* const DataflowClass = UDataflow::StaticClass();

		FOpenAssetDialogConfig NewDataflowAssetDialogConfig;
		{
			const FString PackageName = Asset->GetOutermost()->GetName();
			NewDataflowAssetDialogConfig.DefaultPath = FPackageName::GetLongPackagePath(PackageName);
			NewDataflowAssetDialogConfig.AssetClassNames.Add(DataflowClass->GetClassPathName());
			NewDataflowAssetDialogConfig.bAllowMultipleSelection = false;
			NewDataflowAssetDialogConfig.DialogTitleOverride = LOCTEXT("OpenDataflowAssetDialogTitle", "Open Dataflow Asset");
		}

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		TArray<FAssetData> AssetData = ContentBrowserModule.Get().CreateModalOpenAssetDialog(NewDataflowAssetDialogConfig);

		if (AssetData.Num() == 1)
		{
			OutDataflowAsset = AssetData[0].GetAsset();
			return true;
		}

		return false;
	}

	// Return true if we should proceed, false if we should re-open the dialog
	bool NewOrOpenDialog(const UObject* Asset, UObject*& OutDataflowAsset)
	{
		TSharedRef<SMessageDialog> ConfirmDialog = SNew(SMessageDialog)
			.Title(FText(LOCTEXT("Dataflow_WindowTitle", "Create or Open Dataflow graph?")))
			.Message(LOCTEXT("Dataflow_WindowText", "This Asset currently has no Dataflow graph"))
			.Buttons({
				SMessageDialog::FButton(LOCTEXT("Dataflow_NewText", "Create new Dataflow")),
				SMessageDialog::FButton(LOCTEXT("Dataflow_OpenText", "Open existing Dataflow")),
				SMessageDialog::FButton(LOCTEXT("Dataflow_ContinueText", "Continue without Dataflow")),
					 });

		const int32 ResultButtonIdx = ConfirmDialog->ShowModal();
		switch (ResultButtonIdx)
		{
		case 0:
			return CreateNewDataflowAsset(Asset, OutDataflowAsset);
		case 1:
			return OpenDataflowAsset(Asset, OutDataflowAsset);
		default:
			break;
		}

		return true;
	}

	// Create a new UDataflow if one doesn't already exist for the Asset
	UObject* NewOrOpenDataflowAsset(const UObject* Asset)
	{
		UObject* DataflowAsset = nullptr;
		bool bDialogDone = false;
		while (!bDialogDone)
		{
			bDialogDone = NewOrOpenDialog(Asset, DataflowAsset);
		}

		return DataflowAsset;
	}
}



namespace UE::Dataflow::DataflowAsset
{
	struct FColorScheme
	{
		static inline const FLinearColor Asset = FColor(180, 120, 110);
		static inline const FLinearColor NodeHeader = FColor(180, 120, 110);
		static inline const FLinearColor NodeBody = FColor(18, 12, 11, 127);
	};
}

FText UAssetDefinition_DataflowAsset::GetAssetDisplayName() const
{
	return NSLOCTEXT("AssetTypeActions", "AssetTypeActions_DataflowAsset", "DataflowAsset");
}

TSoftClassPtr<UObject> UAssetDefinition_DataflowAsset::GetAssetClass() const
{
	return UDataflow::StaticClass();
}

FLinearColor UAssetDefinition_DataflowAsset::GetAssetColor() const
{
	return UE::Dataflow::DataflowAsset::FColorScheme::Asset;
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_DataflowAsset::GetAssetCategories() const
{
	static const auto Categories = { EAssetCategoryPaths::Physics };
	return Categories;
}

UThumbnailInfo* UAssetDefinition_DataflowAsset::LoadThumbnailInfo(const FAssetData& InAsset) const
{
	return UE::Editor::FindOrCreateThumbnailInfo(InAsset.GetAsset(), USceneThumbnailInfo::StaticClass());
}

FAssetOpenSupport UAssetDefinition_DataflowAsset::GetAssetOpenSupport(const FAssetOpenSupportArgs& OpenSupportArgs) const
{
	return Super::GetAssetOpenSupport(OpenSupportArgs);
}


EAssetCommandResult UAssetDefinition_DataflowAsset::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	TArray<UDataflow*> DataflowObjects = OpenArgs.LoadObjects<UDataflow>();

	// For now the dataflow editor only works on one asset at a time
	ensure(DataflowObjects.Num() == 0 || DataflowObjects.Num() == 1);

	if (DataflowObjects.Num() == 1)
	{
		if (bCanEditDataflow || DataflowObjects[0]->Type == EDataflowType::Simulation)
		{
			UAssetEditorSubsystem* const AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
			UDataflowEditor* const AssetEditor = NewObject<UDataflowEditor>(AssetEditorSubsystem, NAME_None, RF_Transient);
			AssetEditor->RegisterToolCategories({"General"});

			// Validate the asset
			if (UDataflow* const DataflowAsset = CastChecked<UDataflow>(DataflowObjects[0]))
			{
				AssetEditor->Initialize({ DataflowAsset });
				return EAssetCommandResult::Handled;
			}
		}
		else
		{
			TSharedRef<SMessageDialog> MessageDialog = SNew(SMessageDialog)
				.Title(FText(LOCTEXT("Dataflow_OpenAssetDialog_Title", "Dataflow Asset")))
				.Message(LOCTEXT("Dataflow_OpenAssetDialog_Text", "Dataflow assets can only be changed while editing assets using them (Cloth, Flesh, Geometry Collection, ...)"))
				.Buttons({
					SMessageDialog::FButton(LOCTEXT("Ok", "Ok"))
					.SetPrimary(true)
				});
			MessageDialog->ShowModal();
			return EAssetCommandResult::Handled;
		}
	}
	return EAssetCommandResult::Unhandled;
}

void FDataflowConnectionData::Set(const FDataflowOutput& Output, const FDataflowInput& Input)
{
	static constexpr TCHAR Format[] = TEXT("/{0}:{1}|{2}");

	const FDataflowNode* OutputNode = Output.GetOwningNode();
	const FDataflowNode* InputNode = Input.GetOwningNode();

	Out = FString::Format(Format, { OutputNode ? OutputNode->GetName().ToString() : FString(), Output.GetName().ToString(), Output.GetType().ToString() });
	In = FString::Format(Format, { InputNode ? InputNode->GetName().ToString() : FString(), Input.GetName().ToString(), Input.GetType().ToString() });
}

FString FDataflowConnectionData::GetNode(const FString InConnection)
{
	FString Left, Right;

	if (InConnection.Split(TEXT(":"), &Left, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		if (Left.Split(TEXT("/"), nullptr, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return Right;
		}
	}

	return {};
}

FString FDataflowConnectionData::GetProperty(const FString InConnection)
{
	FString Left, Right;

	if (InConnection.Split(TEXT(":"), nullptr, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		if (Right.Split(TEXT("|"), &Left, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return Left;
		}
		else
		{
			return Right; // no types
		}
	}

	return {};
}

void FDataflowConnectionData::GetNodePropertyAndType(const FString InConnection, FString& OutNode, FString& OutProperty, FString& OutType)
{
	FString Left, Right;

	// String should look like "/NodeName:Property|Type"
	if (InConnection.Split(TEXT(":"), &Left, &Right, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		// get the node name from "/NodeName"
		Left.Split(TEXT("/"), nullptr, &OutNode, ESearchCase::IgnoreCase, ESearchDir::FromEnd);

		// get the property and type from "Property|Type"
		const bool bHasType = Right.Split(TEXT("|"), &OutProperty, &OutType, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		if (!bHasType)
		{
			OutProperty = Right;
			OutType = {};
		}
	}
}

#undef LOCTEXT_NAMESPACE
