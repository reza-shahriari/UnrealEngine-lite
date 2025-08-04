// Copyright Epic Games, Inc. All Rights Reserved.

#include "MetaHumanDepthGeneratorModule.h"
#include "CaptureData.h"
#include "MetaHumanDepthGenerator.h"
#include "ContentBrowserMenuContexts.h"
#include "ToolMenu.h"
#include "ToolMenuDelegates.h"

#if WITH_EDITOR
#include "Settings/EditorLoadingSavingSettings.h"
#include "ContentBrowserModule.h"
#include "ContentBrowserItemPath.h"
#include "IContentBrowserSingleton.h"
#endif

#define LOCTEXT_NAMESPACE "MetaHumanDepthGeneratorModule"

#if WITH_EDITOR

bool operator==(const FAutoReimportDirectoryConfig& InLeft, const FAutoReimportDirectoryConfig& InRight)
{
	return InLeft.SourceDirectory == InRight.SourceDirectory &&
		InLeft.MountPoint == InRight.MountPoint &&
		InLeft.Wildcards == InRight.Wildcards;
}

bool operator==(const FAutoReimportWildcard& InLeft, const FAutoReimportWildcard& InRight)
{
	return InLeft.Wildcard == InRight.Wildcard;
}

namespace UE::MetaHuman::Private
{

FString GetDefaultRootFolder()
{
	static const FString DefaultRelativePath = TEXT("/Game/");
	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	// Default asset creation path is usually the root project folder
	FString DefaultRootFolder =
		ContentBrowserModule.Get().GetInitialPathToSaveAsset(FContentBrowserItemPath(DefaultRelativePath, EContentBrowserPathType::Internal)).GetInternalPathString();

	if (!DefaultRootFolder.EndsWith(TEXT("/")))
	{
		DefaultRootFolder.Append(TEXT("/"));
	}

	return DefaultRootFolder;
}

void AddAutoReimportExemption()
{
	UEditorLoadingSavingSettings* Settings = GetMutableDefault<UEditorLoadingSavingSettings>();

	FAutoReimportDirectoryConfig DirectoryConfig;
	DirectoryConfig.SourceDirectory = GetDefaultRootFolder();

	FAutoReimportWildcard Wildcard;
	Wildcard.Wildcard = FString::Format(TEXT("*/{0}/*.exr"), { UMetaHumanGenerateDepthWindowOptions::ImageSequenceDirectoryName });
	DirectoryConfig.Wildcards.Add(MoveTemp(Wildcard));

	int32 FoundIndex = Settings->AutoReimportDirectorySettings.Find(DirectoryConfig);

	if (FoundIndex == INDEX_NONE)
	{
		// Initialize to false
		DirectoryConfig.Wildcards[0].bInclude = false;

		Settings->AutoReimportDirectorySettings.Add(MoveTemp(DirectoryConfig));
		Settings->SaveConfig();
		Settings->OnSettingChanged().Broadcast(GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings));
	}
}

}
#endif

void FMetaHumanDepthGeneratorModule::StartupModule()
{
	FCoreDelegates::OnPostEngineInit.Add(FSimpleDelegate::CreateRaw(this, &FMetaHumanDepthGeneratorModule::PostEngineInit));

	UToolMenu* Menu = UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UFootageCaptureData::StaticClass());
	FToolMenuSection& Section = Menu->FindOrAddSection("GetAssetActions");
	Section.AddDynamicEntry(NAME_None, FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
	{
		if (const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(InSection))
		{
			const TAttribute<FText> Label = LOCTEXT("GenerateDepth", "Generate Depth");
			const TAttribute<FText> ToolTip = LOCTEXT("GenerateDepth_Tooltip", "Generate depth images using the current stereo views and camera calibration");
			const FSlateIcon Icon = FSlateIcon(TEXT("MetaHumanIdentityStyle"), TEXT("ClassIcon.FootageCaptureData"), TEXT("ClassIcon.FootageCaptureData"));
				
			FToolUIAction UIAction;
			UIAction.ExecuteAction = FToolMenuExecuteAction::CreateLambda([](const FToolMenuContext& InContext)
			{
				const UContentBrowserAssetContextMenuContext* Context = UContentBrowserAssetContextMenuContext::FindContextWithAssets(InContext);

				UFootageCaptureData* FootageCaptureData = Context->LoadFirstSelectedObject<UFootageCaptureData>();
				
				if (FootageCaptureData)
				{
					TStrongObjectPtr<UMetaHumanDepthGenerator> DepthGeneration(NewObject<UMetaHumanDepthGenerator>());
					DepthGeneration->Process(FootageCaptureData);
				}
			});
			InSection.AddMenuEntry("GenerateFootageCaptureDataDepth", Label, ToolTip, Icon, UIAction);
		}
	}));
}

void FMetaHumanDepthGeneratorModule::ShutdownModule()
{
	FCoreDelegates::OnPostEngineInit.RemoveAll(this);
}

void FMetaHumanDepthGeneratorModule::PostEngineInit()
{
#if WITH_EDITOR
	UE::MetaHuman::Private::AddAutoReimportExemption();
#endif
}

IMPLEMENT_MODULE(FMetaHumanDepthGeneratorModule, MetaHumanDepthGenerator)

#undef LOCTEXT_NAMESPACE