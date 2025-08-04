﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimInstanceHelpers.h"
#include "Modules/ModuleManager.h"
#include "ObjectTrace.h"
#include "IGameplayProvider.h"
#include "Animation/AnimBlueprint.h"
#include "Animation/AnimBlueprintGeneratedClass.h"
#include "Editor.h"
#include "EdGraph/EdGraph.h"
#include "RewindDebuggerAnimation.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "IAnimationBlueprintEditor.h"
#include "Insights/IUnrealInsightsModule.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "SRewindDebuggerAnimBPTools"

static bool OpenAnimBlueprintAndAttachDebugger(const TraceServices::IAnalysisSession* Session, uint64 ObjectId)
{
	TraceServices::FAnalysisSessionReadScope SessionReadScope(*Session);

	if (const IGameplayProvider* GameplayProvider = Session->ReadProvider<IGameplayProvider>("GameplayProvider"))
	{
		if (const FObjectInfo* ObjectInfo = GameplayProvider->FindObjectInfo(ObjectId))
		{
			if (const FClassInfo* ClassInfo = GameplayProvider->FindClassInfo(ObjectInfo->ClassId))
			{
				if (UAnimBlueprintGeneratedClass* InstanceClass = TSoftObjectPtr<UAnimBlueprintGeneratedClass>(FSoftObjectPath(ClassInfo->PathName)).LoadSynchronous())
				{
					if(UAnimBlueprint* AnimBlueprint = Cast<UAnimBlueprint>(InstanceClass->ClassGeneratedBy))
					{
						GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(AnimBlueprint);
						
						UObject* SelectedInstance = nullptr;
#if OBJECT_TRACE_ENABLED
						SelectedInstance = FObjectTrace::GetObjectFromId(ObjectId);
#endif
						if (SelectedInstance == nullptr)
						{
							if (FRewindDebuggerAnimation* RewindDebuggerAnimation = FRewindDebuggerAnimation::GetInstance())
							{
								SelectedInstance = RewindDebuggerAnimation->GetDebugAnimInstance(ObjectId);
							}
						}

						if(SelectedInstance)
						{
							AnimBlueprint->SetObjectBeingDebugged(SelectedInstance);
						}

						if(IAnimationBlueprintEditor* AnimBlueprintEditor = static_cast<IAnimationBlueprintEditor*>(GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->FindEditorForAsset(AnimBlueprint, true)))
						{
							// navigate the opened editor to the AnimGraph
							if (AnimBlueprint->FunctionGraphs.Num() > 0)
							{
								AnimBlueprintEditor->JumpToHyperlink(AnimBlueprint->FunctionGraphs[0]);
							}
						}

						return true;
					}
				}
			}
		}
	}
	return false;
}

bool FAnimInstanceDoubleClickHandler::HandleDoubleClick(IRewindDebugger* RewindDebugger)
{
	TSharedPtr<FDebugObjectInfo> SelectedObject = RewindDebugger->GetSelectedComponent();
	if (SelectedObject.IsValid())
	{
		return OpenAnimBlueprintAndAttachDebugger(RewindDebugger->GetAnalysisSession(), SelectedObject->ObjectId);
	}
	return false;
}

FName FAnimInstanceDoubleClickHandler::GetTargetTypeName() const
{
	static const FName ObjectTypeName = "AnimInstance";
	return ObjectTypeName;
}

void FAnimInstanceMenu::Register()
{
#if OBJECT_TRACE_ENABLED
	UToolMenu* Menu = UToolMenus::Get()->FindMenu("RewindDebugger.ComponentContextMenu");
	FToolMenuSection& Section = Menu->FindOrAddSection("Blueprint");
	FToolMenuEntry& Entry = Section.AddDynamicEntry("DebugAnimInstanceEntry", FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& InSection)
	{
		UComponentContextMenuContext* Context = InSection.FindContext<UComponentContextMenuContext>();
		if (Context && Context->SelectedObject.IsValid() && Context->TypeHierarchy.Contains("AnimInstance"))
		{
			InSection.AddMenuEntry(NAME_None,
						LOCTEXT("Open AnimBP", "Open/Debug AnimGraph"),
							LOCTEXT("Open AnimBP ToolTip", "Open this Animation Blueprint and attach the debugger to this instance"),
							FSlateIcon(),
							FExecuteAction::CreateLambda([ObjectId = Context->SelectedObject->ObjectId]()
							{
								IUnrealInsightsModule& UnrealInsightsModule = FModuleManager::LoadModuleChecked<IUnrealInsightsModule>("TraceInsights");
								TSharedPtr<const TraceServices::IAnalysisSession> Session = UnrealInsightsModule.GetAnalysisSession();
								OpenAnimBlueprintAndAttachDebugger(Session.Get(), ObjectId);
							}));
		}
	}));
#endif
}

#undef LOCTEXT_NAMESPACE
