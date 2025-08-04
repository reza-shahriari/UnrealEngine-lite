// Copyright Epic Games, Inc. All Rights Reserved.

#include "LandscapeEditorUtils.h"
#include "LandscapeSettings.h"

#include "LandscapeEditorModule.h"
#include "LandscapeEdit.h"
#include "LandscapeEdMode.h"
#include "LandscapeEditorObject.h"
#include "LandscapeTiledImage.h"
#include "LandscapeStreamingProxy.h"

#include "DesktopPlatformModule.h"
#include "EditorModeManager.h"
#include "EditorModes.h"

#include "Algo/Transform.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionHelpers.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/MessageDialog.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Algo/LevenshteinDistance.h"

namespace LandscapeEditorUtils
{
	int32 GetMaxSizeInComponents()
	{
		const ULandscapeSettings* Settings = GetDefault<ULandscapeSettings>();
		return Settings->MaxComponents;
	}


	TOptional<FString> GetImportExportFilename(const FString& InDialogTitle, const FString& InStartPath, const FString& InDialogTypeString, bool bInImporting)
	{
		FString Filename;
		IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();

		if (DesktopPlatform == nullptr)
		{
			return TOptional<FString>();
		}

		TArray<FString> Filenames;
		ILandscapeEditorModule& LandscapeEditorModule = FModuleManager::GetModuleChecked<ILandscapeEditorModule>("LandscapeEditor");
		FEdModeLandscape* LandscapeEdMode = static_cast<FEdModeLandscape*>(GLevelEditorModeTools().GetActiveMode(FBuiltinEditorModes::EM_Landscape));

		bool bSuccess;
		
		if (bInImporting)
		{
			bSuccess = DesktopPlatform->OpenFileDialog(
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
				InDialogTitle,
				InStartPath,
				TEXT(""),
				InDialogTypeString,
				EFileDialogFlags::None,
				Filenames);
		}
		else
		{
			bSuccess = DesktopPlatform->SaveFileDialog(
				FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
				InDialogTitle,
				InStartPath,
				TEXT(""),
				InDialogTypeString,
				EFileDialogFlags::None,
				Filenames);
		}

		if (bSuccess)
		{
			FString TiledFileNamePattern;

			if (bInImporting && FLandscapeTiledImage::CheckTiledNamePath(Filenames[0], TiledFileNamePattern) && FMessageDialog::Open(EAppMsgType::YesNo, FText::FromString(FString::Format(TEXT("Use '{0}' Tiled Image?"), { TiledFileNamePattern }))) == EAppReturnType::Yes)
			{
				Filename = TiledFileNamePattern;
			}
			else
			{
				Filename = Filenames[0];
			}

		}

		return TOptional<FString>(Filename);

	}

	void SaveLandscapeProxies(UWorld* World, TArrayView<ALandscapeProxy*> Proxies)
	{
		// Save the proxies
		TRACE_CPUPROFILER_EVENT_SCOPE(SaveCreatedActors);
		UWorldPartition::FDisableNonDirtyActorTrackingScope Scope(World->GetWorldPartition(), true);
		LandscapeEditorUtils::SaveObjects(Proxies);
	}

	const TArray<FAssetData> GetLandscapeTargetLayerInfoAssets()
	{
		TArray<FAssetData> LayerInfoAssets;

		const UClass* AssetClass = ULandscapeLayerInfoObject::StaticClass();
		const FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
		FARFilter Filter;
		const FName PackageName = *AssetClass->GetPackage()->GetName();
		const FName AssetName = AssetClass->GetFName();

		Filter.ClassPaths.Add(FTopLevelAssetPath(PackageName, AssetName));
		AssetRegistryModule.Get().GetAssets(Filter, LayerInfoAssets);

		return LayerInfoAssets;
	}

	const FAssetData* FindLandscapeTargetLayerInfoAsset(const FName& LayerName, const FString& TargetLayerAssetPackagePath)
	{
		const TArray<FAssetData>& LayerInfoAssets = GetLandscapeTargetLayerInfoAssets();

		int32 BestScore = -1.0f;
		const FAssetData* BestLayerInfoObj = nullptr;

		for (const FAssetData& LayerInfoAsset : LayerInfoAssets)
		{
			const ULandscapeLayerInfoObject* LayerInfo = CastChecked<ULandscapeLayerInfoObject>(LayerInfoAsset.GetAsset());
			FString CurrentPackagePath = LayerInfoAsset.PackagePath.ToString() + "/";

			// Only include assets in the package or its sub folders
			if (LayerInfo && LayerInfo->LayerName == LayerName && CurrentPackagePath.Contains(TargetLayerAssetPackagePath))
			{
				float WorstCase = static_cast<float>(TargetLayerAssetPackagePath.Len() + CurrentPackagePath.Len());
				WorstCase = FMath::Max(WorstCase, 1.f);

				const float Score = 1.0f - (static_cast<float>(Algo::LevenshteinDistance(TargetLayerAssetPackagePath, CurrentPackagePath)) / WorstCase);
				if (Score > BestScore)
				{
					BestScore = Score;
					BestLayerInfoObj = &LayerInfoAsset;
				}
			}
		}

		return BestLayerInfoObj;
	}

}


