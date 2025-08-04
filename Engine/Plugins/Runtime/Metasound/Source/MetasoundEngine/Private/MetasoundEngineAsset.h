// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Metasound.h"
#include "MetasoundAssetManager.h"
#include "MetasoundAssetSubsystem.h"
#include "MetasoundDocumentBuilderRegistry.h"
#include "MetasoundEngineModule.h"
#include "MetasoundFrontendDocumentIdGenerator.h"
#include "MetasoundFrontendQuery.h"
#include "MetasoundFrontendRegistryKey.h"
#include "MetasoundGlobals.h"
#include "MetasoundSettings.h"
#include "MetasoundUObjectRegistry.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Serialization/Archive.h"
#include "UObject/AssetRegistryTagsContext.h"

#if WITH_EDITORONLY_DATA
#include "Algo/Transform.h"
#include "Interfaces/ITargetPlatform.h"
#include "MetasoundFrontendRegistryContainer.h"
#include "Misc/DataValidation.h"
#include "Serialization/JsonWriter.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "UObject/StrongObjectPtrTemplates.h"
#endif // WITH_EDITORONLY_DATA

#define LOCTEXT_NAMESPACE "MetasoundEngine"


namespace Metasound::Engine
{
	/** MetaSound Engine Asset helper provides routines for UObject based MetaSound assets. 
	 * Any UObject deriving from FMetaSoundAssetBase should use these helper functions
	 * in their UObject overrides. 
	 */
	struct FAssetHelper
	{
		static bool SerializationRequiresDeterminism(bool bIsCooking)
		{
			return bIsCooking || IsRunningCookCommandlet();
		}

#if WITH_EDITOR
		static void PreDuplicate(TScriptInterface<IMetaSoundDocumentInterface> MetaSound, FObjectDuplicationParameters& DupParams)
		{
			FDocumentBuilderRegistry::GetChecked().SetEventLogVerbosity(FDocumentBuilderRegistry::ELogEvent::DuplicateEntries, ELogVerbosity::NoLogging);
		}

		static void PostDuplicate(TScriptInterface<IMetaSoundDocumentInterface> MetaSound, EDuplicateMode::Type InDuplicateMode)
		{
			using namespace Engine;
			using namespace Frontend;

			if (InDuplicateMode == EDuplicateMode::Normal)
			{
				UObject* MetaSoundObject = MetaSound.GetObject();
				check(MetaSoundObject);

				FDocumentBuilderRegistry& BuilderRegistry = FDocumentBuilderRegistry::GetChecked();
				UMetaSoundBuilderBase& DuplicateBuilder = BuilderRegistry.FindOrBeginBuilding(*MetaSoundObject);

				FMetaSoundFrontendDocumentBuilder& DocBuilder = DuplicateBuilder.GetBuilder();
				const FMetasoundFrontendClassName DuplicateName = DocBuilder.GetConstDocumentChecked().RootGraph.Metadata.GetClassName();
				DocBuilder.GenerateNewClassName();

				constexpr bool bForceUnregisterNodeClass = true;
				BuilderRegistry.FinishBuilding(DuplicateName, MetaSound->GetAssetPathChecked(), bForceUnregisterNodeClass);
				BuilderRegistry.SetEventLogVerbosity(FDocumentBuilderRegistry::ELogEvent::DuplicateEntries, ELogVerbosity::All);
			}
		}

		template <typename TMetaSoundObject>
		static void PostEditUndo(TMetaSoundObject& InMetaSound)
		{
			InMetaSound.GetModifyContext().SetForceRefreshViews();

			const FMetasoundFrontendClassName& ClassName = InMetaSound.GetConstDocument().RootGraph.Metadata.GetClassName();
			Frontend::IDocumentBuilderRegistry::GetChecked().ReloadBuilder(ClassName);

			if (UMetasoundEditorGraphBase* Graph = Cast<UMetasoundEditorGraphBase>(InMetaSound.GetGraph()))
			{
				Graph->RegisterGraphWithFrontend();
			}
		}

		template<typename TMetaSoundObject>
		static void SetReferencedAssets(TMetaSoundObject& InMetaSound, TSet<Frontend::IMetaSoundAssetManager::FAssetRef>&& InAssetRefs)
		{
			using namespace Frontend;
			
			InMetaSound.ReferencedAssetClassKeys.Reset();
			InMetaSound.ReferencedAssetClassObjects.Reset();

			for (const IMetaSoundAssetManager::FAssetRef& AssetRef : InAssetRefs)
			{
				// Has to be serialized as node class registry key string for back compat
				InMetaSound.ReferencedAssetClassKeys.Add(FNodeClassRegistryKey(AssetRef.Key).ToString());
				if (UObject* Object = FSoftObjectPath(AssetRef.Path).TryLoad())
				{
					InMetaSound.ReferencedAssetClassObjects.Add(Object);
				}
				else
				{
					UE_LOG(LogMetaSound, Error, TEXT("Failed to load referenced asset %s from asset %s"), *AssetRef.Path.ToString(), *InMetaSound.GetPathName());
				}
			}
		}

		static EDataValidationResult IsClassNameUnique(const FMetasoundFrontendDocument& Document, FDataValidationContext& InOutContext)
		{
			using namespace Metasound::Frontend;
			using namespace Metasound::Engine;

			EDataValidationResult Result = EDataValidationResult::Valid;
			IMetaSoundAssetManager& AssetManager = IMetaSoundAssetManager::GetChecked();
			// Validation has added assets to the asset manager
			// and we don't remove them immediately after validation to optimize possible subsequent validation
			// Set this flag to prevent log spam of active assets on shutdown
			AssetManager.SetLogActiveAssetsOnShutdown(false);

			// Add error for multiple assets with the same class name
			const FMetaSoundAssetKey Key(Document.RootGraph.Metadata);
			const TArray<FTopLevelAssetPath> AssetPaths = AssetManager.FindAssetPaths(Key);
			if (AssetPaths.Num() > 1)
			{
				Result = EDataValidationResult::Invalid;

				TArray<FText> PathStrings;
				Algo::Transform(AssetPaths, PathStrings, [](const FTopLevelAssetPath& Path) { return FText::FromString(Path.ToString()); });
				InOutContext.AddError(FText::Format(LOCTEXT("UniqueClassNameValidation",
					"Multiple assets use the same class name which may result in unintended behavior. This may happen when an asset is moved, then the move is reverted in revision control without removing the newly created asset. Please remove the offending asset or duplicate it to automatically generate a new class name." \
					"\nConflicting Asset Paths:\n{0}"), FText::Join(FText::FromString(TEXT("\n")), PathStrings)));
			}

			// Success
			return Result;
		}

		static EDataValidationResult IsDataValid(const UObject& MetaSound, const FMetasoundFrontendDocument& Document, FDataValidationContext& InOutContext)
		{
			using namespace Metasound;

			EDataValidationResult Result = EDataValidationResult::Valid;
			if (Engine::GetEditorAssetValidationEnabled())
			{
				// We cannot rely on the asset registry scan being complete during the call
				// to IsDataValid(...) while running a cook commandlet. The IMetasoundAssetManager
				// will still log errors on duplicate assets which will fail cook. 
				if (!IsRunningCookCommandlet())
				{
					Result = IsClassNameUnique(Document, InOutContext);
				}
			}

			const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>();
			check(Settings);

			TSet<FGuid> ValidPageIDs;
			auto ErrorIfMissing = [&](const FGuid& PageID, const FText& DataDescriptor)
			{
				if (!ValidPageIDs.Contains(PageID))
				{
					if (const FMetaSoundPageSettings* PageSettings = Settings->FindPageSettings(PageID))
					{
						ValidPageIDs.Add(PageSettings->UniqueId);
					}
					else
					{
						Result = EDataValidationResult::Invalid;
						InOutContext.AddMessage(FAssetData(&MetaSound), EMessageSeverity::Error, FText::Format(
							LOCTEXT("InvalidPageDataFormat", "MetaSound contains invalid {0} with page ID '{1}': page not found in Project 'MetaSound' Settings. Remove page data or migrate to existing page identifier."),
							DataDescriptor,
							FText::FromString(PageID.ToString())));
					}
				}
			};

			const TArray<FMetasoundFrontendGraph>& Graphs = Document.RootGraph.GetConstGraphPages();
			for (const FMetasoundFrontendGraph& Graph : Graphs)
			{
				ErrorIfMissing(Graph.PageID, LOCTEXT("GraphPageDescriptor", "graph"));
			}

			for (const FMetasoundFrontendClassInput& ClassInput : Document.RootGraph.GetDefaultInterface().Inputs)
			{
				ClassInput.IterateDefaults([&](const FGuid& PageID, const FMetasoundFrontendLiteral&)
				{
					ErrorIfMissing(PageID, FText::Format(LOCTEXT("InputPageDefaultDescriptorFormat", "input '{0}' default value"), FText::FromName(ClassInput.Name)));
				});
			}
			return Result;
		}

#endif // WITH_EDITOR

		static void GetAssetRegistryTags(TScriptInterface<const IMetaSoundDocumentInterface> DocInterface, FAssetRegistryTagsContext& Context)
		{
			using namespace Frontend;

			const UObject* MetaSound = DocInterface.GetObject();
			check(MetaSound);
			if (MetaSound->GetFlags() & (RF_Transient | RF_ClassDefaultObject))
			{
				return;
			}

			const FMetaSoundAssetClassInfo ClassInfo(*DocInterface);
			ClassInfo.ExportToContext(Context);
		}

		template <typename TMetaSoundObject>
		static FTopLevelAssetPath GetAssetPathChecked(TMetaSoundObject& InMetaSound)
		{
			FTopLevelAssetPath Path;
			ensureAlwaysMsgf(Path.TrySetPath(&InMetaSound), TEXT("Failed to set TopLevelAssetPath from MetaSound '%s'. MetaSound must be highest level object in package."), *InMetaSound.GetPathName());
			ensureAlwaysMsgf(Path.IsValid(), TEXT("Failed to set TopLevelAssetPath from MetaSound '%s'. This may be caused by calling this function when the asset is being destroyed."), *InMetaSound.GetPathName());
			return Path;
		}

		template <typename TMetaSoundObject>
		static TArray<FMetasoundAssetBase*> GetReferencedAssets(TMetaSoundObject& InMetaSound)
		{
			TArray<FMetasoundAssetBase*> ReferencedAssets;

			IMetasoundUObjectRegistry& UObjectRegistry = IMetasoundUObjectRegistry::Get();

			for (TObjectPtr<UObject>& Object : InMetaSound.ReferencedAssetClassObjects)
			{
				if (FMetasoundAssetBase* Asset = UObjectRegistry.GetObjectAsAssetBase(Object))
				{
					ReferencedAssets.Add(Asset);
				}
				else
				{
					UE_LOG(LogMetaSound, Error, TEXT("Referenced asset \"%s\", referenced from \"%s\", is not convertible to FMetasoundAssetBase"), *Object->GetPathName(), *InMetaSound.GetPathName());
				}
			}

			return ReferencedAssets;
		}

		static void PreSaveAsset(FMetasoundAssetBase& InMetaSound, FObjectPreSaveContext InSaveContext)
		{
#if WITH_EDITORONLY_DATA
			using namespace Frontend;

			if (IMetaSoundAssetManager* AssetManager = IMetaSoundAssetManager::Get())
			{
				AssetManager->WaitUntilAsyncLoadReferencedAssetsComplete(InMetaSound);
			}

			const bool bIsCooking = InSaveContext.IsCooking();
			const bool bCanEverExecute = Metasound::CanEverExecuteGraph(bIsCooking);
			if (!bCanEverExecute)
			{
				FName PlatformName;
				if (const ITargetPlatform* TargetPlatform = InSaveContext.GetTargetPlatform())
				{
					PlatformName = *TargetPlatform->IniPlatformName();
				}
				const bool bIsDeterministic = SerializationRequiresDeterminism(bIsCooking);
				FDocumentIDGenerator::FScopeDeterminism DeterminismScope(bIsDeterministic);
				InMetaSound.UpdateAndRegisterForSerialization(PlatformName);
			}
 			else if (FApp::CanEverRenderAudio())
			{
				if (UMetasoundEditorGraphBase* MetaSoundGraph = Cast<UMetasoundEditorGraphBase>(InMetaSound.GetGraph()))
				{
					// Uses graph flavor of register with frontend to update editor systems/asset editors in case editor is enabled.
					MetaSoundGraph->RegisterGraphWithFrontend();
					InMetaSound.GetModifyContext().SetForceRefreshViews();
				}
			}
			else
			{
				UE_LOG(LogMetaSound, Warning, TEXT("PreSaveAsset for MetaSound: (%s) is doing nothing because InSaveContext.IsCooking, IsRunningCommandlet, and FApp::CanEverRenderAudio were all false")
					, *InMetaSound.GetOwningAssetName());
			}
#endif // WITH_EDITORONLY_DATA
		}

		template <typename TMetaSoundObject>
		static void SerializeToArchive(TMetaSoundObject& InMetaSound, FArchive& InArchive)
		{
#if WITH_EDITORONLY_DATA
			using namespace Frontend;

			bool bVersionedAsset = false;

			if (InArchive.IsLoading())
			{
				const bool bIsTransacting = InArchive.IsTransacting();
				TStrongObjectPtr<UMetaSoundBuilderBase> Builder;
				{
					FGCScopeGuard ScopeGuard;
					Builder.Reset(&FDocumentBuilderRegistry::GetChecked().FindOrBeginBuilding(InMetaSound, bIsTransacting));
				}

				{
					const bool bIsCooking = InArchive.IsCooking();
					const bool bIsDeterministic = SerializationRequiresDeterminism(bIsCooking);
					FDocumentIDGenerator::FScopeDeterminism DeterminismScope(bIsDeterministic);
					check(Builder.IsValid());
					bVersionedAsset = InMetaSound.VersionAsset(Builder->GetBuilder());
				}

				Builder->ClearInternalFlags(EInternalObjectFlags::Async);
			}

			if (bVersionedAsset)
			{
				InMetaSound.SetVersionedOnLoad();
			}
#endif // WITH_EDITORONLY_DATA
		}

		template<typename TMetaSoundObject>
		static void PostLoad(TMetaSoundObject& InMetaSound)
		{
			using namespace Frontend;
			// Do not call asset manager on CDO objects which may be loaded before asset 
			// manager is set.
			const bool bIsCDO = InMetaSound.HasAnyFlags(RF_ClassDefaultObject);
			if (!bIsCDO)
			{
				if (InMetaSound.GetAsyncReferencedAssetClassPaths().Num() > 0)
				{
					IMetaSoundAssetManager::GetChecked().RequestAsyncLoadReferencedAssets(InMetaSound);
				}
			}
		}

		template<typename TMetaSoundObject>
		static void OnAsyncReferencedAssetsLoaded(TMetaSoundObject& InMetaSound, const TArray<FMetasoundAssetBase*>& InAsyncReferences)
		{
			for (FMetasoundAssetBase* AssetBase : InAsyncReferences)
			{
				if (AssetBase)
				{
					if (UObject* OwningAsset = AssetBase->GetOwningAsset())
					{
						InMetaSound.ReferencedAssetClassObjects.Add(OwningAsset);
						InMetaSound.ReferenceAssetClassCache.Remove(FSoftObjectPath(OwningAsset));
					}
				}
			}
		}
	};
} // namespace Metasound::Engine
#undef LOCTEXT_NAMESPACE // MetasoundEngine
