// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetDefinition_MetaHumanCharacter.h"

#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterAssetEditor.h"
#include "MetaHumanCharacterEditorLog.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterAnalytics.h"
#include "ThumbnailRendering/SceneThumbnailInfo.h"
#include "ToolMenu.h"
#include "ToolMenus.h"
#include "ContentBrowserMenuContexts.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "Misc/MessageDialog.h"
#include "Logging/StructuredLog.h"

#define LOCTEXT_NAMESPACE "AssetDefinition_MetaHumanCharacter"

namespace MenuExtension_MetaHumanCharacter
{
	static void ExecuteRemoveTexturesAndRigs(const UContentBrowserAssetContextMenuContext* InCBContext)
	{
		if (InCBContext)
		{
			const TArray<UMetaHumanCharacter*> Characters = InCBContext->LoadSelectedObjects<UMetaHumanCharacter>();

			for (UMetaHumanCharacter* Character : Characters)
			{
				UAssetEditorSubsystem* AssetEditorSubsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();

				const bool bFocusIfOpened = false;
				const bool bHasOpenedEditor = AssetEditorSubsystem->FindEditorForAsset(Character, bFocusIfOpened) != nullptr;

				if (bHasOpenedEditor)
				{
					// Get confirmation from the user that its ok to proceed
					const FText Title = FText::Format(LOCTEXT("RemoveTexturesAndRigs_CloseAssetTitle", "Remove Textures and Rigs from '{0}'"),
													  FText::FromString(Character->GetName()));
					const FText Message = FText::Format(LOCTEXT("RemoveTexturesAndRigs_CloseAssetMessage", "'{0}' has its asset editor opened. Removing textures and rigs requires the asset editor to be closed first. Would you like to proceed?"), FText::FromString(Character->GetName()));
					const EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgCategory::Warning, EAppMsgType::YesNo, Message, Title);
					if (Response == EAppReturnType::No)
					{
						UE_LOGFMT(LogMetaHumanCharacterEditor, Display, "Skipping convertion to preset for character '{CharacterName}'", Character->GetName());
						continue;
					}
				}

				if (bHasOpenedEditor)
				{
					AssetEditorSubsystem->CloseAllEditorsForAsset(Character);
				}

				const bool bConverted = UMetaHumanCharacterEditorSubsystem::Get()->RemoveTexturesAndRigs(Character);

				if (!bConverted)
				{
					const FText Message = FText::Format(LOCTEXT("ConvertToPreset_Failed", "Failed to remove texures and rigs from '{0}'"),
														FText::FromString(Character->GetName()));
					FMessageDialog::Open(EAppMsgCategory::Error, EAppMsgType::Ok, Message);
				}
			}
		}
	}

	static void ExtendAssetActions()
	{
		FToolMenuOwnerScoped OwnderScoped(UE_MODULE_NAME);
		{
			UE::ContentBrowser::ExtendToolMenu_AssetContextMenu(UMetaHumanCharacter::StaticClass())
				->AddDynamicSection(
					NAME_None,
					FNewToolMenuDelegate::CreateLambda(
						[](UToolMenu* InMenu)
						{
							const UContentBrowserAssetContextMenuContext* Context = InMenu->FindContext< UContentBrowserAssetContextMenuContext>();
							if (Context && Context->SelectedAssets.Num() > 0)
							{
								FToolMenuSection& Section = InMenu->FindOrAddSection(TEXT("GetAssetActions"));
								{
									const FText Label = LOCTEXT("MetaHumanCharacter_RemoveTexturesAndRigs", "Remove Textures and Rigs");
									const FText Tooltip = LOCTEXT("MetaHumanCharacter_RemoveTexturesAndRigsTooltip", "Remove all textures and rigs from the character.");
									const FSlateIcon Icon{ FAppStyle::GetAppStyleSetName(), "ClassIcon.MetaHumanCharacter" };
									const FUIAction UIAction = FUIAction(
										FExecuteAction::CreateStatic(&ExecuteRemoveTexturesAndRigs, Context)
									);
									Section.AddMenuEntry(TEXT("MetaHumanCharacter_MakePreset"), Label, Tooltip, Icon, UIAction);
								}
							}
						}
					)
				);
		}
	}

	static FDelayedAutoRegisterHelper DelayedAutoRegister(
		EDelayedRegisterRunPhase::EndOfEngineInit,
		[]
		{
			UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateStatic(&ExtendAssetActions));
		}
	);
}

FText UAssetDefinition_MetaHumanCharacter::GetAssetDisplayName() const
{
	return LOCTEXT("MetaHumanCharacterDisplayName", "MetaHuman Character");
}

FLinearColor UAssetDefinition_MetaHumanCharacter::GetAssetColor() const
{
	return FColor::Orange;
}

TSoftClassPtr<UObject> UAssetDefinition_MetaHumanCharacter::GetAssetClass() const
{
	return UMetaHumanCharacter::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_MetaHumanCharacter::GetAssetCategories() const
{
	static const FAssetCategoryPath Categories[] = { FAssetCategoryPath{ LOCTEXT("MetaHumanAssetCategoryPath", "MetaHuman") } };
	return Categories;
}

UThumbnailInfo* UAssetDefinition_MetaHumanCharacter::LoadThumbnailInfo(const FAssetData& InAssetData) const
{
	return UE::Editor::FindOrCreateThumbnailInfo(InAssetData.GetAsset(), USceneThumbnailInfo::StaticClass());
}

EAssetCommandResult UAssetDefinition_MetaHumanCharacter::OpenAssets(const FAssetOpenArgs& InOpenArgs) const
{
	for (UMetaHumanCharacter* MetaHumanCharacter : InOpenArgs.LoadObjects<UMetaHumanCharacter>())
	{
		if (!MetaHumanCharacter->IsCharacterValid())
		{
			UE_LOG(LogMetaHumanCharacterEditor, Error, TEXT("Failed to open character asset editor, as %s is not valid"), *MetaHumanCharacter->GetFullName());
			continue;
		}

		UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>();

		if (!Subsystem->TryAddObjectToEdit(MetaHumanCharacter))
		{
			UE_LOG(LogMetaHumanCharacterEditor, Error, TEXT("Failed to create editing state for %s. The asset may be corrupted."), *MetaHumanCharacter->GetFullName());
			continue;
		}

		UMetaHumanCharacterAssetEditor* MetaHumanCharacterEditor = NewObject<UMetaHumanCharacterAssetEditor>(GetTransientPackage(), NAME_None, RF_Transient);
		MetaHumanCharacterEditor->SetObjectToEdit(MetaHumanCharacter);
		MetaHumanCharacterEditor->Initialize();

		UE::MetaHuman::Analytics::RecordOpenCharacterEditorEvent(MetaHumanCharacter);
	}

	return EAssetCommandResult::Handled;
}

#undef LOCTEXT_NAMESPACE