// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFeaturePluginStateMachine.h"

#include "AssetRegistry/AssetRegistryState.h"
#include "Engine/StreamableManager.h"
#include "GameFeatureData.h"
#include "GameplayTagsManager.h"
#include "Engine/AssetManager.h"
#include "IPlatformFilePak.h"
#include "InstallBundleUtils.h"
#include "BundlePrereqCombinedStatusHelper.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/PackageLocalizationManager.h"
#include "IO/IoStoreOnDemand.h"
#include "Logging/StructuredLog.h"
#include "Materials/MaterialInterface.h"
#include "Misc/AsciiSet.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigUtilities.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EnumRange.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/ScopedSlowTask.h"
#include "Misc/WildcardString.h"
#include "Algo/Accumulate.h"
#include "Algo/AllOf.h"
#include "Algo/Find.h"
#include "Misc/TVariantMeta.h"
#include "RenderDeferredCleanup.h"
#include "Serialization/MemoryReader.h"
#include "String/ParseTokens.h"
#include "String/LexFromString.h"
#include "Tasks/Pipe.h"
#include "GameFeaturesProjectPolicies.h"
#include "UObject/ObjectRename.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectAllocator.h"
#include "UObject/UObjectIterator.h"
#include "UObject/NoExportTypes.h"
#include "Misc/PathViews.h"
#include "Containers/Queue.h"
#include "ShaderCodeLibrary.h"
#include "DeviceProfiles/DeviceProfileManager.h"
#include "Trace/Trace.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GameFeaturePluginStateMachine)

#define LOCTEXT_NAMESPACE "GameFeatureDataStateMachine"

#if WITH_EDITOR
#include "PluginUtils.h"
#include "Misc/App.h"
#endif //if WITH_EDITOR

namespace UE::GameFeatures
{
	static const FString StateMachineErrorNamespace(TEXT("GameFeaturePlugin.StateMachine."));

	static UE::GameFeatures::FResult CanceledResult = MakeError(StateMachineErrorNamespace + TEXT("Canceled"));

	static int32 ShouldLogMountedFiles = 0;
	static FAutoConsoleVariableRef CVarShouldLogMountedFiles(TEXT("GameFeaturePlugin.ShouldLogMountedFiles"),
		ShouldLogMountedFiles,
		TEXT("Should the newly mounted files be logged."));

	static FString GVerifyPluginSkipList;
	static FAutoConsoleVariableRef CVarVerifyPluginSkipList(TEXT("PluginManager.VerifyUnload.SkipList"),
		GVerifyPluginSkipList,
		TEXT("Comma-separated list of names of plugins for which to skip verification."),
		ECVF_Default);

	static bool bDeferLocalizationDataLoad = true;
	static FAutoConsoleVariableRef CVarDeferLocalizationLoading(TEXT("GameFeaturePlugin.DeferLocalizationDataLoad"),
		bDeferLocalizationDataLoad,
		TEXT("True if we should defer loading the localization data until 'loading' (new behavior), or false to load it on 'mounting' (old behavior)."));

	static TAutoConsoleVariable<bool> CVarAsyncLoad(TEXT("GameFeaturePlugin.AsyncLoad"),
		true,
		TEXT("Enable to use async loading as well async downloading and registering"));

	static TAutoConsoleVariable<bool> CVarForceAsyncLoad(TEXT("GameFeaturePlugin.ForceAsyncLoad"),
		false,
		TEXT("Enable to force use of async loading even if normally not allowed"));

	static TAutoConsoleVariable<bool> CVarAllowForceMonolithicShaderLibrary(TEXT("GameFeaturePlugin.AllowForceMonolithicShaderLibrary"),
		true,
		TEXT("Enable to force only searching for monolithic shader libs when possible"));

	static TAutoConsoleVariable<bool> CVarForceSyncRegisterStartupPlugins(TEXT("GameFeaturePlugin.ForceSyncRegisterStartupPlugins"),
		true,
		TEXT("If true, all plugins loaded during startup will be synchronously registered to ensure things are initialized in time, this only applies if AsyncLoad is enabled"));

	static TAutoConsoleVariable<bool> CVarForceSyncLoadShaderLibrary(TEXT("GameFeaturePlugin.ForceSyncLoadShaderLibrary"),
		true,
		TEXT("Enable to force shaderlibs to be opened on the game thread"));

	static TAutoConsoleVariable<bool> CVarForceSyncAssetRegistryAppend(TEXT("GameFeaturePlugin.ForceSyncAssetRegistryAppend"),
		false,
		TEXT("Enable to force calls to IAssetRegistry::AppendState to happen on the game thread"));

	static TAutoConsoleVariable<bool> CVarWaitForDependencyDeactivation(TEXT("GameFeaturePlugin.WaitForDependencyDeactivation"),
		false,
		TEXT("Enable to make block deactivation until all dependencies are deactivated. Warning - this can lead to failure to unload"));

	static TAutoConsoleVariable<bool> CVarEnableAssetStreaming(TEXT("GameFeaturePlugin.EnableAssetStreaming"),
		true,
		TEXT("Enable experimental asset streaming"));

	static TAutoConsoleVariable<bool> CVarEnableBatchProcessing(TEXT("GameFeaturePlugin.EnableBatchProcessing"),
		false,
		TEXT("Enable batch processing when processing multiple plugins and specified in protocol options."));

	bool ShouldDeferLocalizationDataLoad()
	{
		// Note: We don't defer localization data loading in the editor, as the editor only needs to mount plugins to use them
		return !GIsEditor && bDeferLocalizationDataLoad;
	}

	void MountLocalizationData(UGameFeaturePluginStateMachine* CurrentMachine, FGameFeaturePluginStateMachineProperties& StateProperties)
	{
		check(IsInGameThread());
		check(CurrentMachine && &CurrentMachine->GetProperties() == &StateProperties);

		StateProperties.bIsLoadingLocalizationData = true;
		IPluginManager::Get().MountExplicitlyLoadedPluginLocalizationData(StateProperties.PluginName, [WeakMachine = TWeakObjectPtr<UGameFeaturePluginStateMachine>(CurrentMachine), &StateProperties, bAllowAsyncLoading = StateProperties.AllowAsyncLoading()](bool bLoadedLocalization, const FString& PluginName)
		{
			if (UGameFeaturePluginStateMachine* StateMachine = WeakMachine.Get();
				StateMachine && ensureAlways(&StateMachine->GetProperties() == &StateProperties))
			{
				if (IsInGameThread())
				{
					StateProperties.bIsLoadingLocalizationData = false;
				}
				else if (bAllowAsyncLoading)
				{
					ExecuteOnGameThread(UE_SOURCE_LOCATION, [WeakMachine, &StateProperties]()
					{
						if (UGameFeaturePluginStateMachine* StateMachine = WeakMachine.Get();
							StateMachine && ensureAlways(&StateMachine->GetProperties() == &StateProperties))
						{
							StateProperties.bIsLoadingLocalizationData = false;
							StateProperties.OnRequestUpdateStateMachine.ExecuteIfBound();
						}
					});
				}
			}
		});
	}

	bool ShouldSkipVerify(const FString& PluginName)
	{
		static const FAsciiSet Wildcards("*?");
		bool bSkip = false;
		UE::String::ParseTokens(MakeStringView(GVerifyPluginSkipList), TEXTVIEW(","), [&PluginName, &bSkip](FStringView Item) {
				if (bSkip) { return; }
				if (Item.Equals(PluginName, ESearchCase::IgnoreCase))
				{
					bSkip = true;
				}
				else if (FAsciiSet::HasAny(Item, Wildcards))
				{
					FString Pattern = FString(Item); // Need to copy to null terminate
					if (FWildcardString::IsMatchSubstring(*Pattern, *PluginName, *PluginName + PluginName.Len(), ESearchCase::IgnoreCase))
					{
						bSkip = true;
					}
				}
			});
		return bSkip;
	}

	// Return a higher number for packages which it's more important to include in leak reporting, when the number of leaks we want to report is limited. 
	int32 GetPackageLeakReportingPriority(UPackage* Package)
	{	
		int32 Priority = 0;
		ForEachObjectWithPackage(Package, [&Priority](UObject* Object)
		{
			if (UWorld* World = Cast<UWorld>(Object))
			{
				Priority = 100;
				return true;
			}
			else if (Cast<UMaterialInterface>(Object))
			{
				Priority = FMath::Max(Priority, 50);
				// keep iterating in case we find a world 
			}
			return true;
		}, false);
		return Priority;
	}

	static bool bRealtimeMode = false;

	class FRealtimeMode : public TSharedFromThis<FRealtimeMode>
	{
	public:
		~FRealtimeMode()
		{
			if (TickHandle.IsValid())
			{
				FTSTicker::GetCoreTicker().RemoveTicker(MoveTemp(TickHandle));
			}

			FGameFeaturePluginRequestUpdateStateMachine UpdateRequest;
			while (UpdateRequests.Dequeue(UpdateRequest))
			{
				UpdateRequest.ExecuteIfBound();
			}
		}

		void AddUpdateRequest(FGameFeaturePluginRequestUpdateStateMachine UpdateRequest)
		{
			UpdateRequests.Enqueue(MoveTemp(UpdateRequest));
			EnableTick();
		}

	private:
		void EnableTick()
		{
			if (!TickHandle.IsValid())
			{
				TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FRealtimeMode::Tick));
			}
		}

		bool Tick(float DeltaTime)
		{
			// Self-reference so we don't get destroyed during Tick
			TSharedRef<FRealtimeMode> SelfRef = AsShared();

			{
				constexpr double MaxFrameTime = 0.033; // 30fps
				constexpr double AllottedTime = MaxFrameTime / 2;
				const double StartTime = FPlatformTime::Seconds();

				FGameFeaturePluginRequestUpdateStateMachine UpdateRequest;
				while (UpdateRequests.Dequeue(UpdateRequest))
				{
					UpdateRequest.ExecuteIfBound();

					const double ElapsedTime = FPlatformTime::Seconds() - StartTime;
					if ((ElapsedTime > AllottedTime) || ((DeltaTime + ElapsedTime) > MaxFrameTime))
					{
						break;
					}
				}
			}

			if (UpdateRequests.IsEmpty())
			{
				TickHandle.Reset();
				return false;
			}
			else
			{
				return true;
			}
		}

	private:
		TQueue<FGameFeaturePluginRequestUpdateStateMachine> UpdateRequests;
		FTSTicker::FDelegateHandle TickHandle;
	};

	static TSharedPtr<FRealtimeMode> RealtimeMode;

	static FAutoConsoleVariableRef CVarRealtimeMode(TEXT("GameFeaturePlugin.RealtimeMode"),
		bRealtimeMode,
		TEXT("Sets whether GFS realtime mode is enabled; which distributes plugin state updates over several frames"),
		FConsoleVariableDelegate::CreateLambda(
			[](IConsoleVariable* Var)
			{
				if (Var->GetBool())
				{
					if (!RealtimeMode)
					{
						RealtimeMode = MakeShared<FRealtimeMode>();
					}
				}
				else
				{
					TSharedPtr<FRealtimeMode> Rm = MoveTemp(RealtimeMode);
					Rm.Reset();
				}
			}),
			ECVF_ReadOnly);

#if WITH_EDITOR
	TMap<FString, FGameFeaturePluginRequestUpdateStateMachine> PluginsToUnloadAssets;
	FTSTicker::FDelegateHandle UnloadPluginAssetsHandle;

	bool TickUnloadPluginAssets(float /*DeltaTime*/)
	{
		UnloadPluginAssetsHandle.Reset();
	
		TArray<FString> PluginNames;
		TArray<FGameFeaturePluginRequestUpdateStateMachine> UpdateStateMachineDelegates;
		{
			PluginNames.Reserve(PluginsToUnloadAssets.Num());
			UpdateStateMachineDelegates.Reserve(PluginsToUnloadAssets.Num());
			for (TPair<FString, FGameFeaturePluginRequestUpdateStateMachine>& PluginsToUnloadAsset : PluginsToUnloadAssets)
			{
				PluginNames.Add(PluginsToUnloadAsset.Key);
				UpdateStateMachineDelegates.Add(MoveTemp(PluginsToUnloadAsset.Value));
			}
			PluginsToUnloadAssets.Empty();
		}

		verify(FPluginUtils::UnloadPluginsAssets(PluginNames));

		for (const FGameFeaturePluginRequestUpdateStateMachine& UpdateStateMachineDelegate : UpdateStateMachineDelegates)
		{
			UpdateStateMachineDelegate.ExecuteIfBound();
		}

		return false;
	}

	void ScheduleUnloadPluginAssets(const FString& PluginName, const FGameFeaturePluginRequestUpdateStateMachine& UpdateStateMachineDelegate)
	{
		check(IsInGameThread());
		ensure(!PluginsToUnloadAssets.Contains(PluginName));
		PluginsToUnloadAssets.Add(PluginName, UpdateStateMachineDelegate);
		if (!UnloadPluginAssetsHandle.IsValid())
		{
			UnloadPluginAssetsHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickUnloadPluginAssets));
		}
	}
#endif //if WITH_EDITOR

	enum class EGFPInstallLevel : uint8
	{
		Download = 0,
		Mount,
		AssetStream
	};

	struct FPluginInstallBundleReferencers
	{
		// GFPs using an install bundle and the relevant state of that GFP
		TMap<FStringView, EGFPInstallLevel> GFPs;
	};

	struct FPluginIoStoreOnDemandHandles
	{
		// IoStoreOnDemand assets required for initial download
		UE::IoStore::FOnDemandContentHandle DownloadHandle;
		// IoStoreOnDemand assets from AssetDependencyStreaming
		UE::IoStore::FOnDemandContentHandle StreamInHandle;
	};

	// class to manage GFPs sharing installation data
	class FGFPSharedInstallTracker
	{
	public:
		// The caller should pass all resolved bundle depenencies 
		void AddBundleRefs(const FStringView PluginName, const EGFPInstallLevel Level, const TConstArrayView<FName> Bundles)
		{
			for (const FName BundleName : Bundles)
			{
				FPluginInstallBundleReferencers& PluginRefs = InstallBundleToGFPRefs.FindOrAdd(BundleName);
				PluginRefs.GFPs.Emplace(PluginName, Level);
			}
		}

		// The caller should pass all resolved bundle depenencies 
		UE::IoStore::FOnDemandContentHandle AddOnDemandContentHandle(const FName Bundle, const EGFPInstallLevel Level)
		{
			switch(Level)
			{
			case EGFPInstallLevel::Download:
			{
				FPluginIoStoreOnDemandHandles& Handles = InstallBundleToOnDemandHandles.FindOrAdd(Bundle);
				if (!Handles.DownloadHandle.IsValid())
				{
					Handles.DownloadHandle = UE::IoStore::FOnDemandContentHandle::Create(Bundle.ToString());
				}
				return Handles.DownloadHandle;
			}
			case EGFPInstallLevel::AssetStream:
			{
				FPluginIoStoreOnDemandHandles& Handles = InstallBundleToOnDemandHandles.FindOrAdd(Bundle);
				if (!Handles.StreamInHandle.IsValid())
				{
					Handles.StreamInHandle = UE::IoStore::FOnDemandContentHandle::Create(Bundle.ToString() + TEXTVIEW("/deps"));
				}
				return Handles.StreamInHandle;
			}
			};

			return {};
		}

		// The caller should pass all resolved bundle depenencies 
		TArray<FName> Release(
			const FStringView PluginName, const EGFPInstallLevel InLevel, const TConstArrayView<FName> Bundles)
		{
			TArray<FName> BundlesToRelease;
			for (const FName Bundle : Bundles)
			{
				bool bRelease = true;

				FPluginInstallBundleReferencers* PluginRefs = InstallBundleToGFPRefs.Find(Bundle);
				if (PluginRefs)
				{
					EGFPInstallLevel* Level = PluginRefs->GFPs.FindByHash(GetTypeHash(PluginName), PluginName);
					if (Level && *Level >= InLevel)
					{
						switch(*Level)
						{
						case EGFPInstallLevel::Download:
							PluginRefs->GFPs.RemoveByHash(GetTypeHash(PluginName), PluginName);
							break;
						case EGFPInstallLevel::Mount:
							(*Level) = EGFPInstallLevel::Download;
							break;
						case EGFPInstallLevel::AssetStream:
							(*Level) = EGFPInstallLevel::Mount;
							break;
						}
					}

					for (TPair<FStringView, EGFPInstallLevel>& GFPRef : PluginRefs->GFPs)
					{
						if (GFPRef.Value >= InLevel)
						{
							bRelease = false;
							break;
						}
					}

					if (PluginRefs->GFPs.IsEmpty())
					{
						InstallBundleToGFPRefs.Remove(Bundle);
					}
				}

				if (bRelease)
				{
					BundlesToRelease.Add(Bundle);
				}
			}

			switch(InLevel)
			{
			case EGFPInstallLevel::Download:
				for (const FName Bundle : BundlesToRelease)
				{
					InstallBundleToOnDemandHandles.Remove(Bundle);
				}
				break;
			case EGFPInstallLevel::AssetStream:
				for (const FName& Bundle : BundlesToRelease)
				{
					FPluginIoStoreOnDemandHandles* Handles = InstallBundleToOnDemandHandles.Find(Bundle);
					if (Handles)
					{
						Handles->StreamInHandle.Reset();
					}
				}
				break;
			}

			return BundlesToRelease;
		}

	private:
		TMap<FName, FPluginInstallBundleReferencers> InstallBundleToGFPRefs;
		TMap<FName, FPluginIoStoreOnDemandHandles> InstallBundleToOnDemandHandles;
	};

	static FGFPSharedInstallTracker GFPSharedInstallTracker;

	// Callback delegates are moved to the stack before broadcasting.
	// This class tracks callback delegates on the stack to handle removing callbacks from them
	// for state machines that are also on the stack.
	template<typename TCallbackMultiDelegate> 
		requires 
			std::is_same_v<TCallbackMultiDelegate, FDestinationGameFeaturePluginState::FOnDestinationStateReached> ||
			std::is_same_v<TCallbackMultiDelegate, FGameFeaturePluginStateMachineProperties::FOnTransitionCanceled>
	class TBroadcastingCallback
	{
	public:
		TCallbackMultiDelegate CallbackDelegate;

	private:
		TBroadcastingCallback<TCallbackMultiDelegate>* Next = nullptr;
		inline static TBroadcastingCallback<TCallbackMultiDelegate>* Head = nullptr;

	public:
		TBroadcastingCallback(TCallbackMultiDelegate&& InCallbackDelegate)
			: CallbackDelegate(MoveTemp(InCallbackDelegate))
			, Next(Head)
		{
			Head = this;
		}

		~TBroadcastingCallback()
		{
			Head = Next;
		}

		static void RemovePendingCallback(FDelegateHandle InHandle)
		{
			TBroadcastingCallback<TCallbackMultiDelegate>* Current = Head;
			while (Current)
			{
				Current->CallbackDelegate.Remove(InHandle);
				Current = Current->Next;
			}
		}

		static void RemovePendingCallback(FDelegateUserObject InObject)
		{
			TBroadcastingCallback<TCallbackMultiDelegate>* Current = Head;
			while (Current)
			{
				Current->CallbackDelegate.RemoveAll(InObject);
				Current = Current->Next;
			}
		}
	};

	using FBroadcastingOnDestinationStateReached = TBroadcastingCallback<FDestinationGameFeaturePluginState::FOnDestinationStateReached>;
	using FBroadcastingOnTransitionCanceled = TBroadcastingCallback<FGameFeaturePluginStateMachineProperties::FOnTransitionCanceled>;
}

void FGameFeaturePluginStateStatus::SetTransition(EGameFeaturePluginState InTransitionToState)
{
	TransitionToState = InTransitionToState;
	TransitionResult.ErrorCode = MakeValue();
	TransitionResult.OptionalErrorText = FText();
}

void FGameFeaturePluginStateStatus::SetTransitionError(EGameFeaturePluginState TransitionToErrorState, UE::GameFeatures::FResult TransitionResultIn, bool bInSuppressErrorLog /*= false*/)
{
	if (ensureAlwaysMsgf(TransitionResultIn.HasError(), TEXT("Invalid call to SetTransitionError with an FResult that isn't an error! TransitionToErrorState: %s"), *UE::GameFeatures::ToString(TransitionToErrorState)))
	{
		TransitionResult = MoveTemp(TransitionResultIn);
	}
	else
	{
		//Logic error using a non-error FResult, so just generate a general error to keep the SetTransitionError intent
		TransitionResult = MakeError(TEXT("Invalid_Transition_Error"));
	}

	TransitionToState = TransitionToErrorState;
	bSuppressErrorLog = bInSuppressErrorLog;
}

UE::GameFeatures::FResult FGameFeaturePluginState::GetErrorResult(const FString& ErrorCode, const FText OptionalErrorText/*= FText()*/) const
{
	return GetErrorResult(TEXT(""), ErrorCode, OptionalErrorText);
}

UE::GameFeatures::FResult FGameFeaturePluginState::GetErrorResult(const FString& ErrorNamespaceAddition, const FString& ErrorCode, const FText OptionalErrorText/*= FText()*/) const
{
	const FString StateName = UE::GameFeatures::ToString(UGameFeaturesSubsystem::Get().GetPluginState(StateProperties.PluginIdentifier));
	const FString ErrorCodeEnding = ErrorNamespaceAddition.IsEmpty() ? ErrorCode : ErrorNamespaceAddition + ErrorCode;
	const FString CompleteErrorCode = FString::Printf(TEXT("%s%s.%s"), *UE::GameFeatures::StateMachineErrorNamespace, *StateName, *ErrorCodeEnding);
	return UE::GameFeatures::FResult(MakeError(CompleteErrorCode), OptionalErrorText);
}

UE::GameFeatures::FResult FGameFeaturePluginState::GetErrorResult(const FString& ErrorNamespaceAddition, const EInstallBundleResult ErrorResult) const
{
	UE::GameFeatures::FResult BaseResult = GetErrorResult(ErrorNamespaceAddition, LexToString(ErrorResult));
	BaseResult.OptionalErrorText = UE::GameFeatures::CommonErrorCodes::GetErrorTextForBundleResult(ErrorResult);
	return MoveTemp(BaseResult);
}

UE::GameFeatures::FResult FGameFeaturePluginState::GetErrorResult(const FString& ErrorNamespaceAddition, const EInstallBundleReleaseResult ErrorResult) const
{
	UE::GameFeatures::FResult BaseResult = GetErrorResult(ErrorNamespaceAddition, LexToString(ErrorResult));
	BaseResult.OptionalErrorText = UE::GameFeatures::CommonErrorCodes::GetErrorTextForReleaseResult(ErrorResult);
	return MoveTemp(BaseResult);
}


FGameFeaturePluginState::~FGameFeaturePluginState()
{
	CleanupDeferredUpdateCallbacks();
}

UE::GameFeatures::FResult FGameFeaturePluginState::TryUpdateProtocolOptions(const FGameFeatureProtocolOptions& NewOptions)
{
	UE::GameFeatures::FResult Result = StateProperties.ValidateProtocolOptionsUpdate(NewOptions);

	if (!Result.HasError())
	{
		StateProperties.ProtocolOptions = NewOptions;
	}
	return Result;
}

FDestinationGameFeaturePluginState* FGameFeaturePluginState::AsDestinationState()
{
	FDestinationGameFeaturePluginState* Ret = nullptr;

	EGameFeaturePluginStateType Type = GetStateType();
	if (Type == EGameFeaturePluginStateType::Destination || Type == EGameFeaturePluginStateType::Error)
	{
		Ret = static_cast<FDestinationGameFeaturePluginState*>(this);
	}

	return Ret;
}

FErrorGameFeaturePluginState* FGameFeaturePluginState::AsErrorState()
{
	FErrorGameFeaturePluginState* Ret = nullptr;

	if (GetStateType() == EGameFeaturePluginStateType::Error)
	{
		Ret = static_cast<FErrorGameFeaturePluginState*>(this);
	}

	return Ret;
}

void FGameFeaturePluginState::UpdateStateMachineDeferred(float Delay /*= 0.0f*/) const
{
	CleanupDeferredUpdateCallbacks();

	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([this](float dts) mutable
	{
		// @note Release FGameFeaturePluginState::TickHandle first in case the termination callback triggers a GC and destroys the state machine
		TickHandle.Reset();
		StateProperties.OnRequestUpdateStateMachine.ExecuteIfBound();
		return false;
	}), Delay);
}

void FGameFeaturePluginState::UpdateStateMachineImmediate() const
{
	StateProperties.OnRequestUpdateStateMachine.ExecuteIfBound();
}

void FGameFeaturePluginState::UpdateProgress(float Progress) const
{
	StateProperties.OnFeatureStateProgressUpdate.ExecuteIfBound(Progress);
}

bool FGameFeaturePluginState::CanBatchProcess() const
{
	// Batch processing is tick driven so is technically "async". 
	// Hence if we are sync loading, avoid using batch processing since it could impact order of operations.
	return UseAsyncLoading();
}

bool FGameFeaturePluginState::IsWaitingForBatchProcessing() const
{
	return StateProperties.IsWaitingForBatchProcessing();
}

bool FGameFeaturePluginState::WasBatchProcessed() const
{
	return StateProperties.WasBatchProcessed();
}

void FGameFeaturePluginState::CleanupDeferredUpdateCallbacks() const
{
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
}

bool FGameFeaturePluginState::ShouldVisitUninstallStateBeforeTerminal() const
{
	switch (StateProperties.GetPluginProtocol())
	{
		case (EGameFeaturePluginProtocol::InstallBundle):
		{
			//InstallBundleProtocol's have a MetaData that controlls if they uninstall currently
			return StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>().bUninstallBeforeTerminate;
		}

		//Default behavior is to just Terminate
		default:
		{
			return false;
		}
	}
}

bool FGameFeaturePluginState::AllowIniLoading() const
{
	switch (StateProperties.GetPluginProtocol())
	{
	case (EGameFeaturePluginProtocol::InstallBundle):
	{
		//InstallBundleProtocol's have a MetaData that controlls if INI loading is allowed
		//The protocol default is not to allow INI loading since the source is likely untrusted
		return StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>().bAllowIniLoading;
	}

	//Default behavior is to allow INI loading
	default:
	{
		return true;
	}
	}
}

bool FGameFeaturePluginState::AllowAsyncLoading() const
{
	return StateProperties.AllowAsyncLoading();
}

bool FGameFeaturePluginState::UseAsyncLoading() const
{
	return AllowAsyncLoading() && UE::GameFeatures::CVarAsyncLoad.GetValueOnGameThread();
}

/*
=========================================================
  States
=========================================================
*/

template<typename TransitionPolicy>
struct FTransitionDependenciesGameFeaturePluginState : public FGameFeaturePluginState
{
	FTransitionDependenciesGameFeaturePluginState(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FGameFeaturePluginState(InStateProperties)
	{}

	virtual ~FTransitionDependenciesGameFeaturePluginState()
	{
		ClearDependencies();
	}

	virtual void BeginState() override
	{
		ClearDependencies();
		bCheckedRealtimeMode = false;
	}

	virtual void EndState() override
	{
		ClearDependencies();
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!bCheckedRealtimeMode)
		{
			bCheckedRealtimeMode = true;
			if (UE::GameFeatures::RealtimeMode)
			{
				UE::GameFeatures::RealtimeMode->AddUpdateRequest(StateProperties.OnRequestUpdateStateMachine);
				return;
			}
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_TransitionDependencies);
		checkf(!StateProperties.PluginInstalledFilename.IsEmpty(), TEXT("PluginInstalledFilename must be set by the loading dependencies phase. PluginURL: %s"), *StateProperties.PluginIdentifier.GetFullPluginURL());
		checkf(FPaths::GetExtension(StateProperties.PluginInstalledFilename) == TEXT("uplugin"), TEXT("PluginInstalledFilename must have a uplugin extension. PluginURL: %s"), *StateProperties.PluginIdentifier.GetFullPluginURL());

		UGameFeaturesSubsystem& GameFeaturesSubsystem = UGameFeaturesSubsystem::Get();
		if (!bRequestedDependencies)
		{
			TArray<UGameFeaturePluginStateMachine*> Dependencies;
			if (!TransitionPolicy::GetPluginDependencyStateMachines(StateProperties, Dependencies))
			{
				// Failed to query dependencies
				StateStatus.SetTransitionError(TransitionPolicy::GetErrorState(), GetErrorResult(TEXT("Failed_Dependency_Query")));
				return;
			}

			bRequestedDependencies = true;

			UE_CLOG(Dependencies.Num() > 0, LogGameFeatures, Verbose, TEXT("Found %i dependencies for %s"), Dependencies.Num(), *StateProperties.PluginName);

			const bool bAllowAsyncLoading = AllowAsyncLoading();
			RemainingDependencies.Reserve(Dependencies.Num());
			for (UGameFeaturePluginStateMachine* Dependency : Dependencies)
			{
				ensureMsgf(bAllowAsyncLoading || !Dependency->AllowAsyncLoading(), 
					TEXT("FGameFeaturePluginState::AllowAsyncLoading is false for %s but true for dependency being waited on %s"), 
					*StateProperties.PluginName, *Dependency->GetPluginURL());

				const EGameFeaturePluginState DependencyState = Dependency->GetCurrentState();
				const bool bIsInErrorState = DependencyState == EGameFeaturePluginState::ErrorRegistering || DependencyState == EGameFeaturePluginState::ErrorWaitingForDependencies;
				if (TransitionPolicy::ShouldWaitForDependencies() && bIsInErrorState && Dependency->IsErrorStateUnrecoverable())
				{
					UE_LOG(LogGameFeatures, Error, TEXT("Dependency %s failed and is in %s"), *Dependency->GetPluginURL(), *UE::GameFeatures::ToString(DependencyState));
					StateStatus.SetTransitionError(TransitionPolicy::GetErrorState(), GetErrorResult(TEXT("Failed_Dependency_Transition")));
					if (UGameFeaturePluginStateMachine* CurrentMachine = GameFeaturesSubsystem.FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier))
					{
						UE_LOG(LogGameFeatures, Error, TEXT("Setting %s to be in unrecoverable error as one of their dependencies is in unrecoverable error"), *StateProperties.PluginIdentifier.GetFullPluginURL());
						CurrentMachine->SetUnrecoverableError();
					}
					return;
				}
				else
				{
					RemainingDependencies.Emplace(Dependency, MakeValue());
					TransitionDependency(Dependency);
				}
			}
		}

		for (FDepResultPair& Pair : RemainingDependencies)
		{
			UGameFeaturePluginStateMachine* RemainingDependency = Pair.Key.Get();
			if (!RemainingDependency)
			{
				// One of the dependency state machines was destroyed before finishing
				StateStatus.SetTransitionError(TransitionPolicy::GetErrorState(), GetErrorResult(TEXT("Dependency_Destroyed_Before_Finish")));
				return;
			}

			if (Pair.Value.HasError())
			{
				UE_LOG(LogGameFeatures, Error, TEXT("Dependency %s failed to transition with error %s"), *RemainingDependency->GetPluginURL(), *Pair.Value.GetError());
				StateStatus.SetTransitionError(TransitionPolicy::GetErrorState(), GetErrorResult(TEXT("Failed_Dependency_Transition")));
				if (RemainingDependency->IsErrorStateUnrecoverable())
				{
					if (UGameFeaturePluginStateMachine* CurrentMachine = GameFeaturesSubsystem.FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier))
					{
						UE_LOG(LogGameFeatures, Error, TEXT("Setting %s to be in unrecoverable error as one of their dependencies is in unrecoverable error"), *StateProperties.PluginIdentifier.GetFullPluginURL());
						CurrentMachine->SetUnrecoverableError();
					}
				}
				return;
			}
		}

		if (RemainingDependencies.Num() == 0)
		{
			StateStatus.SetTransition(TransitionPolicy::GetTransitionState());
		}
	}

	void TransitionDependency(UGameFeaturePluginStateMachine* Dependency)
	{
		bool bSetDestination = false;

		if (TransitionPolicy::ExcludeDepedenciesFromBatchProcessing())
		{
			Dependency->ExcludeFromBatchProcessing();
		}
		
		if (TransitionPolicy::ShouldWaitForDependencies())
		{
			bSetDestination = Dependency->SetDestination(TransitionPolicy::GetDependencyStateRange(),
				FGameFeatureStateTransitionComplete::CreateRaw(this, &FTransitionDependenciesGameFeaturePluginState::OnDependencyTransitionComplete));
		}
		else
		{
			bSetDestination = Dependency->SetDestination(TransitionPolicy::GetDependencyStateRange(), 
				FGameFeatureStateTransitionComplete::CreateStatic(&FTransitionDependenciesGameFeaturePluginState::OnDependencyTransitionCompleteNoWait));
			if (bSetDestination)
			{
				OnDependencyTransitionComplete(Dependency, MakeValue());
			}
		}

		if (!bSetDestination)
		{
			const bool bCancelPending = Dependency->TryCancel(
				FGameFeatureStateTransitionCanceled::CreateRaw(this, &FTransitionDependenciesGameFeaturePluginState::OnDependencyTransitionCanceled));
			if (!ensure(bCancelPending))
			{
				OnDependencyTransitionComplete(Dependency, GetErrorResult(TEXT("Failed_Dependency_Transition")));
			}
		}
	}

	void OnDependencyTransitionCanceled(UGameFeaturePluginStateMachine* Dependency)
	{
		// Special case for terminal state since it cannot be exited, we need to make a new machine
		if (Dependency->GetCurrentState() == EGameFeaturePluginState::Terminal)
		{
			// Inherit dep protocol options if possible
			FGameFeatureProtocolOptions DepProtocolOptions;
			EGameFeaturePluginProtocol DepProtocol = Dependency->GetPluginIdentifier().GetPluginProtocol();
			if (DepProtocol == EGameFeaturePluginProtocol::InstallBundle && StateProperties.ProtocolOptions.HasSubtype<FInstallBundlePluginProtocolOptions>())
			{
				DepProtocolOptions = StateProperties.RecycleProtocolOptions();
			}

			UGameFeaturePluginStateMachine* NewMachine = UGameFeaturesSubsystem::Get().FindOrCreateGameFeaturePluginStateMachine(Dependency->GetPluginURL(), DepProtocolOptions);
			checkf(NewMachine != Dependency, TEXT("Game Feature Plugin %s should have already been removed from subsystem!"), *Dependency->GetPluginURL());

			const int32 Index = RemainingDependencies.IndexOfByPredicate([Dependency](const FDepResultPair& Pair)
				{
					return Pair.Key == Dependency;
				});

			check(Index != INDEX_NONE);
			FDepResultPair& FoundDep = RemainingDependencies[Index];
			FoundDep.Key = NewMachine;

			Dependency->RemovePendingTransitionCallback(this);
			Dependency->RemovePendingCancelCallback(this);

			Dependency = NewMachine;
		}

		// Now that the transition has been canceled, retry reaching the desired destination
		bool bSetDestination = false;
		if (TransitionPolicy::ShouldWaitForDependencies())
		{
			bSetDestination = Dependency->SetDestination(TransitionPolicy::GetDependencyStateRange(),
				FGameFeatureStateTransitionComplete::CreateRaw(this, &FTransitionDependenciesGameFeaturePluginState::OnDependencyTransitionComplete));
		}
		else
		{
			bSetDestination = Dependency->SetDestination(TransitionPolicy::GetDependencyStateRange(), 
				FGameFeatureStateTransitionComplete::CreateStatic(&FTransitionDependenciesGameFeaturePluginState::OnDependencyTransitionCompleteNoWait));
			if (bSetDestination)
			{
				OnDependencyTransitionComplete(Dependency, MakeValue());
			}
		}

		if (!ensure(bSetDestination))
		{
			OnDependencyTransitionComplete(Dependency, GetErrorResult(TEXT("Failed_Dependency_Transition")));
		}
	}

	void OnDependencyTransitionComplete(UGameFeaturePluginStateMachine* Dependency, const UE::GameFeatures::FResult& Result)
	{
		const int32 Index = RemainingDependencies.IndexOfByPredicate([Dependency](const FDepResultPair& Pair)
			{
				return Pair.Key == Dependency;
			});

		if (ensure(Index != INDEX_NONE))
		{
			FDepResultPair& FoundDep = RemainingDependencies[Index];

			if (Result.HasError())
			{
				FoundDep.Value = Result;
			}
			else
			{
				RemainingDependencies.RemoveAtSwap(Index, EAllowShrinking::No);
			}

			UpdateStateMachineImmediate();
		}
	}

	static void OnDependencyTransitionCompleteNoWait(UGameFeaturePluginStateMachine* Dependency, const UE::GameFeatures::FResult& Result)
	{
		if (Result.HasError())
		{
			if (Result.GetError() == UE::GameFeatures::CanceledResult.GetError())
			{
				UE_LOGFMT(LogGameFeatures, Warning, "Dependency {Dep} failed to transition because it was cancelled by another request {Error}",
					("Dep", Dependency->GetPluginIdentifier().GetIdentifyingString()),
					("Error", Result.GetError()));
			}
			else
			{

				UE_LOGFMT(LogGameFeatures, Error, "Dependency {Dep} failed to transition with error {Error}",
					("Dep", Dependency->GetPluginIdentifier().GetIdentifyingString()),
					("Error", Result.GetError()));
			}
		}
	}

	void ClearDependencies()
	{
		if (RemainingDependencies.Num() > 0)
		{
			for (FDepResultPair& Pair : RemainingDependencies)
			{
				UGameFeaturePluginStateMachine* RemainingDependency = Pair.Key.Get();
				if (RemainingDependency)
				{
					RemainingDependency->RemovePendingTransitionCallback(this);
					RemainingDependency->RemovePendingCancelCallback(this);
				}
			}

			// Also need to cleanup callbacks from any delegates currently on the stack
			UE::GameFeatures::FBroadcastingOnDestinationStateReached::RemovePendingCallback(this);
			UE::GameFeatures::FBroadcastingOnTransitionCanceled::RemovePendingCallback(this);

			RemainingDependencies.Empty();
		}

		bRequestedDependencies = false;
	}

	using FDepResultPair = TPair<TWeakObjectPtr<UGameFeaturePluginStateMachine>, UE::GameFeatures::FResult>;
	TArray<FDepResultPair> RemainingDependencies;
	bool bRequestedDependencies = false;
	bool bCheckedRealtimeMode = false;
};

struct FGameFeaturePluginState_Uninitialized : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Uninitialized(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		checkf(false, TEXT("UpdateState can not be called while uninitialized"));
	}
};

struct FGameFeaturePluginState_Terminal : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Terminal(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	bool bEnteredTerminalState = false;

	virtual UE::GameFeatures::FResult TryUpdateProtocolOptions(const FGameFeatureProtocolOptions& NewOptions) override
	{
		//Should never update our options during Terminal
		return GetErrorResult(TEXT("ProtocolOptions."), TEXT("Terminal"));
	}

	virtual void BeginState() override
	{
		checkf(!bEnteredTerminalState, TEXT("Plugin entered terminal state more than once! %s"), *StateProperties.PluginIdentifier.GetFullPluginURL());
		bEnteredTerminalState = true;

		UGameFeaturesSubsystem::Get().OnGameFeatureTerminating(StateProperties.PluginName, StateProperties.PluginIdentifier);
	}
};

struct FGameFeaturePluginState_UnknownStatus : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_UnknownStatus(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::UnknownStatus)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::UnknownStatus)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::CheckingStatus);

			UGameFeaturesSubsystem::Get().OnGameFeatureCheckingStatus(StateProperties.PluginIdentifier);
		}
	}
};

struct FGameFeaturePluginState_CheckingStatus : public FGameFeaturePluginState
{
	FGameFeaturePluginState_CheckingStatus(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	bool bParsedURL = false;
	bool bIsAvailable = false;

	virtual void BeginState() override
	{
		bParsedURL = false;
		bIsAvailable = false;
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!bParsedURL)
		{
			TValueOrError<void, FString> ParseUrlResult = StateProperties.ParseURL();
			bParsedURL = !ParseUrlResult.HasError();
			if (!bParsedURL)
			{
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorCheckingStatus, GetErrorResult(ParseUrlResult.GetError()));
				return;
			}
		}

		if (StateProperties.GetPluginProtocol() == EGameFeaturePluginProtocol::File)
		{
			bIsAvailable = FPaths::FileExists(StateProperties.PluginInstalledFilename);
		}
		else if (StateProperties.GetPluginProtocol() == EGameFeaturePluginProtocol::InstallBundle)
		{
			TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
			if (BundleManager == nullptr)
			{
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorCheckingStatus, GetErrorResult(TEXT("BundleManager_Null")));
				return;
			}

			if (BundleManager->GetInitState() == EInstallBundleManagerInitState::Failed)
			{
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorCheckingStatus, GetErrorResult(TEXT("BundleManager_Failed_Init")));
				return;
			}

			if (BundleManager->GetInitState() == EInstallBundleManagerInitState::NotInitialized)
			{
				// Just wait for any pending init
				UpdateStateMachineDeferred(1.0f);
				return;
			}

			FInstallBundlePluginProtocolMetaData& ProtocolMetadata = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();

			const bool bAddDependencies = true;
			TValueOrError<FInstallBundleCombinedInstallState, EInstallBundleResult> MaybeInstallState = BundleManager->GetInstallStateSynchronous(
				ProtocolMetadata.InstallBundles, bAddDependencies);
			if (MaybeInstallState.HasError())
			{
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorCheckingStatus, GetErrorResult(TEXT("BundleManager_Failed_GetInstallState")));
				return;
			}

			const FInstallBundleCombinedInstallState& InstallState = MaybeInstallState.GetValue();
			bIsAvailable = Algo::AllOf(ProtocolMetadata.InstallBundles, 
				[&InstallState](FName BundleName) { return InstallState.IndividualBundleStates.Contains(BundleName); });

			if (bIsAvailable)
			{
				// Update metadata with fully expanded dependency list. This can only be done after all bundles are known to be available,
				// otherwise unavailable bundles in the URL could be stripped from the list.
				ProtocolMetadata.InstallBundles.Empty(InstallState.IndividualBundleStates.Num());
				InstallState.IndividualBundleStates.GetKeys(ProtocolMetadata.InstallBundles);
				ProtocolMetadata.InstallBundlesWithAssetDependencies = InstallState.BundlesWithIoStoreOnDemand.Array();
			}
		}
		else
		{
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorCheckingStatus, GetErrorResult(TEXT("Unknown_Protocol")));
			return;
		}

		if (!bIsAvailable)
		{
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorUnavailable, GetErrorResult(TEXT("Plugin_Unavailable")));
			return;
		}

		UGameFeaturesSubsystem::Get().OnGameFeatureStatusKnown(StateProperties.PluginName, StateProperties.PluginIdentifier);
		StateStatus.SetTransition(EGameFeaturePluginState::StatusKnown);
	}
};

struct FGameFeaturePluginState_ErrorCheckingStatus : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorCheckingStatus(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorCheckingStatus)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorCheckingStatus)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::CheckingStatus);
		}
	}
};

struct FGameFeaturePluginState_ErrorUnavailable : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorUnavailable(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorUnavailable)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorUnavailable)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::CheckingStatus);
		}
	}
};

struct FGameFeaturePluginState_StatusKnown : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_StatusKnown(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::StatusKnown)
		{
			if (ShouldVisitUninstallStateBeforeTerminal())
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Uninstalling);
			}
			else
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
			}
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::StatusKnown)
		{
			if (StateProperties.GetPluginProtocol() != EGameFeaturePluginProtocol::File)
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Downloading);
			}
			else
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Installed);
			}
		}
	}
};

struct FGameFeaturePluginState_ErrorManagingData : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorManagingData(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorManagingData)
		{			
			StateStatus.SetTransition(EGameFeaturePluginState::Releasing);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorManagingData)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Downloading);
		}
	}
};

struct FGameFeaturePluginState_ErrorUninstalling : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorUninstalling(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorUninstalling)
		{
			if (ShouldVisitUninstallStateBeforeTerminal())
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Uninstalling);
			}
			else
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
			}
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorUninstalling)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::StatusKnown);
		}
	}
};

//Base class for our states that want to request to release data from InstallBundleManager
struct FBaseDataReleaseGameFeaturePluginState : public FGameFeaturePluginState
{
	FBaseDataReleaseGameFeaturePluginState(FGameFeaturePluginStateMachineProperties& InStateProperties) 
		: FGameFeaturePluginState(InStateProperties) 
		, Result(MakeValue())
	{}

	virtual ~FBaseDataReleaseGameFeaturePluginState()
	{}

	UE::GameFeatures::FResult Result;
	bool bWasDeleted = false;
	TArray<FName> PendingBundles;

	void CleanUp()
	{
		PendingBundles.Empty();
		IInstallBundleManager::ReleasedDelegate.RemoveAll(this);
	}

	void OnContentRemoved(FInstallBundleReleaseRequestResultInfo BundleResult)
	{
		if (!PendingBundles.Contains(BundleResult.BundleName))
		{
			return;
		}

		PendingBundles.Remove(BundleResult.BundleName);

		if (!Result.HasError() && BundleResult.Result != EInstallBundleReleaseResult::OK)
		{
			Result = GetErrorResult(TEXT("BundleManager.OnRemove_Failed."), BundleResult.Result);
		}

		if (PendingBundles.Num() > 0)
		{
			return;
		}

		if (Result.HasValue())
		{
			bWasDeleted = true;
		}

		UpdateStateMachineImmediate();
	}

	void BeginRemoveRequest()
	{
		CleanUp();

		Result = MakeValue();
		bWasDeleted = false;

		if (!ShouldReleaseContent())
		{
			bWasDeleted = true;
			return;
		}

		TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
		check(BundleManager.IsValid());

		TArray<FName> InstallBundlesToRelease = UE::GameFeatures::GFPSharedInstallTracker.Release(
			StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::Download, GetInstallBundles());

		// Always set ExplicitRemoveList, GFPSharedInstallTracker has filtered out shared dependencies
		EInstallBundleReleaseRequestFlags ReleaseFlags = GetReleaseRequestFlags() | EInstallBundleReleaseRequestFlags::ExplicitRemoveList;
		TValueOrError<FInstallBundleReleaseRequestInfo, EInstallBundleResult> MaybeRequestInfo = BundleManager->RequestReleaseContent(
			InstallBundlesToRelease, ReleaseFlags);

		if (MaybeRequestInfo.HasError())
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue uninstall for the PluginURL(%.*s) because %s"), ShortUrl.Len(), ShortUrl.GetData(), LexToString(MaybeRequestInfo.GetError()));
			Result = GetErrorResult(TEXT("BundleManager.Begin."), MaybeRequestInfo.GetError());

			return;
		}

		FInstallBundleReleaseRequestInfo RequestInfo = MaybeRequestInfo.StealValue();

		if (EnumHasAnyFlags(RequestInfo.InfoFlags, EInstallBundleRequestInfoFlags::SkippedUnknownBundles))
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue uninstall for the PluginURL(%.*s) because failed to resolve install bundles!"), ShortUrl.Len(), ShortUrl.GetData());
			Result = GetErrorResult(TEXT("BundleManager.Begin."), TEXT("Resolve_Failed"), UE::GameFeatures::CommonErrorCodes::GetGenericReleaseResult());

			return;
		}

		if (RequestInfo.BundlesEnqueued.Num() == 0)
		{
			bWasDeleted = true;
		}
		else
		{
			PendingBundles = MoveTemp(RequestInfo.BundlesEnqueued);
			IInstallBundleManager::ReleasedDelegate.AddRaw(this, &FBaseDataReleaseGameFeaturePluginState::OnContentRemoved);
		}
	}

	virtual void BeginState() override
	{
		BeginRemoveRequest();
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!Result.HasValue())
		{
			StateStatus.SetTransitionError(GetFailureTransitionState(), Result);
			return;
		}

		if (!bWasDeleted)
		{
			return;
		}

		StateStatus.SetTransition(GetSuccessTransitionState());
	}

	virtual void EndState() override
	{
		CleanUp();
	}

	/** Controls what check is done to determine if this state should run or not */
	bool ShouldReleaseContent() const
	{
		switch (StateProperties.GetPluginProtocol())
		{
			case (EGameFeaturePluginProtocol::InstallBundle):
			{
				return true;
			}

			default:
			{
				return false;
			}
		}
	}

	virtual TConstArrayView<FName> GetInstallBundles()
	{
		TConstArrayView<FName> Ret;
		if (ShouldReleaseContent())
		{
			Ret = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>().InstallBundles;
		}
		return Ret;
	}

	/** Determine what kind of release request flags we submit */
	virtual EInstallBundleReleaseRequestFlags GetReleaseRequestFlags() const
	{
		// Always set ExplicitRemoveList, GFPSharedInstallTracker has filtered out shared dependencies
		EInstallBundleReleaseRequestFlags ReleaseFlags = EInstallBundleReleaseRequestFlags::ExplicitRemoveList;
		return ReleaseFlags;
	}

	/** Determines what state you transition to in the event of a success or failure to release content */
	virtual EGameFeaturePluginState GetSuccessTransitionState() const = 0;
	virtual EGameFeaturePluginState GetFailureTransitionState() const = 0;
};

struct FGameFeaturePluginState_Uninstalled : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Uninstalled(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::Uninstalled)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Terminal);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::Uninstalled)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::CheckingStatus);
		}
	}
};

struct FGameFeaturePluginState_Uninstalling : public FBaseDataReleaseGameFeaturePluginState
{
	FGameFeaturePluginState_Uninstalling(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FBaseDataReleaseGameFeaturePluginState(InStateProperties)
	{}

	virtual EGameFeaturePluginState GetSuccessTransitionState() const override
	{
		return EGameFeaturePluginState::Uninstalled;
	}
	
	virtual EGameFeaturePluginState GetFailureTransitionState() const override
	{
		return EGameFeaturePluginState::ErrorUninstalling;
	}

	//Must be overridden to determine what kind of release request we submit
	virtual EInstallBundleReleaseRequestFlags GetReleaseRequestFlags() const override
	{
		const EInstallBundleReleaseRequestFlags BaseFlags = FBaseDataReleaseGameFeaturePluginState::GetReleaseRequestFlags();
		return (BaseFlags | EInstallBundleReleaseRequestFlags::RemoveFilesIfPossible);
	}

	virtual UE::GameFeatures::FResult TryUpdateProtocolOptions(const FGameFeatureProtocolOptions& NewOptions) override
	{
		//Use base functionality to update our metadata
		UE::GameFeatures::FResult LocalResult = FGameFeaturePluginState::TryUpdateProtocolOptions(NewOptions);

		if (LocalResult.HasError())
		{
			return LocalResult;
		}

		//If we are no longer uninstalling before terminate, just exit now as a success immediately
		if (!ShouldVisitUninstallStateBeforeTerminal())
		{
			FBaseDataReleaseGameFeaturePluginState::CleanUp();

			Result = MakeValue();
			bWasDeleted = true;
			UpdateStateMachineImmediate();

			return LocalResult;
		}		
		
		//Restart our remove request to handle other changes
		BeginRemoveRequest();

		return LocalResult;
	}
};

struct FGameFeaturePluginState_Releasing : public FBaseDataReleaseGameFeaturePluginState
{
	FGameFeaturePluginState_Releasing(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FBaseDataReleaseGameFeaturePluginState(InStateProperties)
	{}

	virtual void BeginState() override
	{
		if (ShouldReleaseContent())
		{
			UGameFeaturesSubsystem::Get().OnGameFeatureReleasing(StateProperties.PluginName, StateProperties.PluginIdentifier);
		}

		FBaseDataReleaseGameFeaturePluginState::BeginState();
	}

	virtual EGameFeaturePluginState GetSuccessTransitionState() const override
	{
		return EGameFeaturePluginState::StatusKnown;
	}

	virtual EGameFeaturePluginState GetFailureTransitionState() const override
	{
		return EGameFeaturePluginState::ErrorManagingData;
	}
};

struct FGameFeaturePluginState_Downloading : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Downloading(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FGameFeaturePluginState(InStateProperties)
		, Result(MakeValue())
	{}

	virtual ~FGameFeaturePluginState_Downloading()
	{
		Cleanup();
	}

	UE::GameFeatures::FResult Result;
	bool bSuppressResultErrorLog = false;
	bool bPluginDownloaded = false;

	TArray<FName> PendingBundleDownloads;
	TUniquePtr<FInstallBundleCombinedProgressTracker> ProgressTracker;
	FTSTicker::FDelegateHandle ProgressUpdateHandle;
	FDelegateHandle GotContentStateHandle;

	// Required for callback lifetime safety
	struct FIoStoreOnDemandContext
	{
		TArray<UE::IoStore::FOnDemandInstallRequest> InstallRequests;
		int32 PendingInstalls = 0;
		bool bStateValid = true;

		void Cancel()
		{
			for (UE::IoStore::FOnDemandInstallRequest& Request : InstallRequests)
			{
				Request.Cancel();
			}
		}
	};
	TSharedPtr<FIoStoreOnDemandContext> IoStoreOnDemandContext;

	void EnsureAllowAsyncLoading() const
	{
		ensureMsgf(AllowAsyncLoading(), TEXT("FGameFeaturePluginState::AllowAsyncLoading is false while attempting to download GFP data for %s"), *StateProperties.PluginName);
	}

	void Cleanup()
	{
		if (ProgressUpdateHandle.IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(ProgressUpdateHandle);
			ProgressUpdateHandle.Reset();
		}

		if (GotContentStateHandle.IsValid())
		{
			TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
			if (BundleManager)
			{
				BundleManager->CancelAllGetContentStateRequests(GotContentStateHandle);
			}
			GotContentStateHandle.Reset();
		}

		IInstallBundleManager::InstallBundleCompleteDelegate.RemoveAll(this);
		IInstallBundleManager::PausedBundleDelegate.RemoveAll(this);

		Result = MakeValue();
		bSuppressResultErrorLog = false;
		bPluginDownloaded = false;
		PendingBundleDownloads.Empty();
		ProgressTracker = nullptr;

		if (IoStoreOnDemandContext)
		{
			IoStoreOnDemandContext->Cancel();
			IoStoreOnDemandContext->bStateValid = false;
			IoStoreOnDemandContext = nullptr;
		}
	}

	void OnGotContentState(FInstallBundleCombinedContentState BundleContentState)
	{
		GotContentStateHandle.Reset();

		TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();

		if (StateProperties.bTryCancel)
		{
			Result = UE::GameFeatures::CanceledResult;
			UpdateStateMachineImmediate();
			return;
		}

		const FInstallBundlePluginProtocolMetaData& MetaData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();

		UE::GameFeatures::GFPSharedInstallTracker.AddBundleRefs(StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::Download, MetaData.InstallBundles);

		const TConstArrayView<FName> InstallBundles = GetInstallBundles();
		const EInstallBundleRequestFlags InstallFlags = GetRequestFlags();
		TValueOrError<FInstallBundleRequestInfo, EInstallBundleResult> MaybeRequestInfo = BundleManager->RequestUpdateContent(InstallBundles, InstallFlags);

		if (MaybeRequestInfo.HasError())
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue download for the PluginURL(%.*s) because %s"), ShortUrl.Len(), ShortUrl.GetData(), LexToString(MaybeRequestInfo.GetError()));
			Result = GetErrorResult(TEXT("BundleManager.GotState."), LexToString(MaybeRequestInfo.GetError()));
			UpdateStateMachineImmediate();
			return;
		}

		FInstallBundleRequestInfo RequestInfo = MaybeRequestInfo.StealValue();

		if (EnumHasAnyFlags(RequestInfo.InfoFlags, EInstallBundleRequestInfoFlags::SkippedUnknownBundles))
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue download for the PluginURL(%.*s) because failed to resolve install bundles!"), ShortUrl.Len(), ShortUrl.GetData());
			Result = GetErrorResult(TEXT("BundleManager.GotState."), TEXT("Resolve_Failed"), UE::GameFeatures::CommonErrorCodes::GetGenericConnectionError());

			UpdateStateMachineImmediate();
			return;
		}

		if (RequestInfo.BundlesEnqueued.Num() == 0)
		{
			UpdateProgress(1.0f);
			InstallIoStoreOnDemandContent();
		}
		else
		{
			PendingBundleDownloads = MoveTemp(RequestInfo.BundlesEnqueued);
			IInstallBundleManager::InstallBundleCompleteDelegate.AddRaw(this, &FGameFeaturePluginState_Downloading::OnInstallBundleCompleted);
			IInstallBundleManager::PausedBundleDelegate.AddRaw(this, &FGameFeaturePluginState_Downloading::OnInstallBundlePaused);

			ProgressTracker = MakeUnique<FInstallBundleCombinedProgressTracker>(false);
			ProgressTracker->SetBundlesToTrackFromContentState(BundleContentState, PendingBundleDownloads);

			ProgressUpdateHandle = FTSTicker::GetCoreTicker().AddTicker(
				FTickerDelegate::CreateRaw(this, &FGameFeaturePluginState_Downloading::OnUpdateProgress)/*, 0.1f*/);

			//If this setting is flipped then we should immediately request to pause downloads.
			//We still generate the downloads so that we have an accurate PendingBundleDownloads list
			const FInstallBundlePluginProtocolOptions& Options = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();
			if (Options.bUserPauseDownload)
			{
				ChangePauseState(true);
			}
		}
	}

	void OnInstallBundleCompleted(FInstallBundleRequestResultInfo BundleResult)
	{
		if (!PendingBundleDownloads.Contains(BundleResult.BundleName))
		{
			return;
		}

		PendingBundleDownloads.Remove(BundleResult.BundleName);

		if (!Result.HasError() && BundleResult.Result != EInstallBundleResult::OK)
		{
			//Use OptionalErrorCode and/or OptionalErrorText if available
			const FString ErrorCodeEnding = (BundleResult.OptionalErrorCode.IsEmpty()) ? LexToString(BundleResult.Result) : BundleResult.OptionalErrorCode;
			const FText ErrorText = BundleResult.OptionalErrorCode.IsEmpty() ? UE::GameFeatures::CommonErrorCodes::GetErrorTextForBundleResult(BundleResult.Result) : BundleResult.OptionalErrorText;

			Result = GetErrorResult(TEXT("BundleManager.OnComplete."), ErrorCodeEnding, ErrorText);

			if (BundleResult.Result != EInstallBundleResult::UserCancelledError)
			{
				TryCancelState();
			}
		}

		if (PendingBundleDownloads.Num() > 0)
		{
			return;
		}

		OnUpdateProgress(0.0f);

		InstallIoStoreOnDemandContent();
	}

	void InstallIoStoreOnDemandContent()
	{
		const FInstallBundlePluginProtocolMetaData& MetaData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();
		if (MetaData.InstallBundlesWithAssetDependencies.IsEmpty())
		{
			bPluginDownloaded = Result.HasValue();
			UpdateStateMachineImmediate();
			return;
		}

		UE::IoStore::IOnDemandIoStore* IoStore = UE::IoStore::TryGetOnDemandIoStore();
		if (!IoStore && !Result.HasError())
		{
			Result = GetErrorResult(TEXT("IoStoreOnDemand.ModuleNotFound"));
			UpdateStateMachineImmediate();
			return;
		}

		IoStoreOnDemandContext = MakeShared<FIoStoreOnDemandContext>();
		IoStoreOnDemandContext->PendingInstalls = MetaData.InstallBundlesWithAssetDependencies.Num();

		for (const FName InstallBundle : MetaData.InstallBundlesWithAssetDependencies)
		{
			UE::IoStore::FOnDemandInstallArgs InstallArgs;
			InstallArgs.MountId = InstallBundle.ToString();
			InstallArgs.TagSets.Emplace(TEXTVIEW("mount")); // May not be a real tagset, but this will install all the mandatory untagged chunks
			InstallArgs.Options |= UE::IoStore::EOnDemandInstallOptions::InstallSoftReferences;
			InstallArgs.Options |= UE::IoStore::EOnDemandInstallOptions::CallbackOnGameThread;
			
			InstallArgs.ContentHandle = UE::GameFeatures::GFPSharedInstallTracker.AddOnDemandContentHandle(
				InstallBundle, UE::GameFeatures::EGFPInstallLevel::Download);
			check(InstallArgs.ContentHandle.IsValid());

			// This should be pretty small, so not going to worry about progress here.
			IoStoreOnDemandContext->InstallRequests.Add(
				IoStore->Install(MoveTemp(InstallArgs),
				[this, LambdaOnDemandContext = IoStoreOnDemandContext](const UE::IoStore::FOnDemandInstallResult& OnDemandInstallResult)
				{
					if (!LambdaOnDemandContext->bStateValid)
					{
						// Owning state got cleaned up, bail
						return;
					}

					if (!OnDemandInstallResult.Status.IsOk() && !Result.HasError())
					{
						const FString StatusString = OnDemandInstallResult.Status.ToString();
						FString ErrorCode = GetIoErrorText(OnDemandInstallResult.Status.GetErrorCode());
						ErrorCode.ReplaceCharInline(TEXT(' '), TEXT('_'), ESearchCase::CaseSensitive);

						// IOStore errors aren't localized 
						Result = GetErrorResult(TEXT("IoStoreOnDemand.OnComplete."), ErrorCode, FText::AsCultureInvariant(StatusString));
						TryCancelState();
					}

					--IoStoreOnDemandContext->PendingInstalls;
					if (IoStoreOnDemandContext->PendingInstalls == 0)
					{
						bPluginDownloaded = Result.HasValue();
						UpdateStateMachineImmediate();
					}
				}));
		}
	}

	bool OnUpdateProgress(float dts)
	{
		if (ProgressTracker)
		{
			ProgressTracker->ForceTick();

			float Progress = ProgressTracker->GetCurrentCombinedProgress().ProgressPercent;
			UpdateProgress(Progress);

			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			UE_LOG(LogGameFeatures, VeryVerbose, TEXT("Download Progress: %f for PluginURL(%.*s)"), Progress, ShortUrl.Len(), ShortUrl.GetData());
		}

		return true;
	}

	void ChangePauseState(bool bPause)
	{
		if (PendingBundleDownloads.Num() > 0)
		{
			TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();

			if (bPause)
			{
				BundleManager->PauseUpdateContent(PendingBundleDownloads);
			}
			else
			{
				BundleManager->ResumeUpdateContent(PendingBundleDownloads);
			}
			BundleManager->RequestPausedBundleCallback();

			//Use same text we use for InstallBundleManager's UserPaused as this was also a user pause
			const TCHAR* PauseReason = InstallBundleUtil::GetInstallBundlePauseReason(EInstallBundlePauseFlags::UserPaused);

			NotifyPauseChange(bPause, PauseReason);
		}
	}

	void OnInstallBundlePaused(FInstallBundlePauseInfo InPauseBundleInfo)
	{
		if (PendingBundleDownloads.Contains(InPauseBundleInfo.BundleName))
		{
			const bool bIsPaused = (InPauseBundleInfo.PauseFlags != EInstallBundlePauseFlags::None);
			const TCHAR* PauseReason = InstallBundleUtil::GetInstallBundlePauseReason(InPauseBundleInfo.PauseFlags);

			NotifyPauseChange(bIsPaused, PauseReason);
		}
	}

	void NotifyPauseChange(bool bIsPaused, FString PauseReason)
	{
		FGameFeaturePauseStateChangeContext Context(UE::GameFeatures::ToString(EGameFeaturePluginState::Downloading), PauseReason, bIsPaused);
		UGameFeaturesSubsystem::Get().OnGameFeaturePauseChange(StateProperties.PluginIdentifier, StateProperties.PluginName, Context);
	}

	virtual void BeginState() override
	{
		Cleanup();

		if (!ensure(ShouldDownloadContent()))
		{
			bPluginDownloaded = true;
			UpdateProgress(1.0f);
			return;
		}

		TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
		const TConstArrayView<FName> InstallBundles = GetInstallBundles();

		const FInstallBundlePluginProtocolOptions& Options = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();

		const bool bAddDependencies = false; // We already got all dependencies in the CheckingStatus state
		if (InstallBundles.Num() > 1)
		{
			// Handle bDoNotDownload flag before doing any async ops
			if (Options.bDoNotDownload)
			{
				TValueOrError<FInstallBundleCombinedInstallState, EInstallBundleResult> MaybeInstallState = 
					BundleManager->GetInstallStateSynchronous(InstallBundles, bAddDependencies);
				check(MaybeInstallState.HasValue());
				FInstallBundleCombinedInstallState& InstallState = MaybeInstallState.GetValue();

				// if not up to date, check to see if we allow downloading
				if (!InstallState.GetAllBundlesHaveState(EInstallBundleInstallState::UpToDate))
				{
					Result = GetErrorResult(TEXT("GFPStateMachine.DownloadNotAllowed"));
					bSuppressResultErrorLog = true; // Don't log an error if the user disallowed the download
					UpdateStateMachineImmediate();
					return;
				}
			}

			UGameFeaturesSubsystem::Get().OnGameFeatureDownloading(StateProperties.PluginName, StateProperties.PluginIdentifier);
			EnsureAllowAsyncLoading();

			GotContentStateHandle = BundleManager->GetContentState(InstallBundles, EInstallBundleGetContentStateFlags::None, bAddDependencies,
				FInstallBundleGetContentStateDelegate::CreateRaw(this, &FGameFeaturePluginState_Downloading::OnGotContentState));
		}
		else
		{
			TValueOrError<FInstallBundleCombinedInstallState, EInstallBundleResult> MaybeInstallState = 
				BundleManager->GetInstallStateSynchronous(InstallBundles, bAddDependencies);
			check(MaybeInstallState.HasValue());
			FInstallBundleCombinedInstallState& InstallState = MaybeInstallState.GetValue();

			// Handle bDoNotDownload flag before doing any async ops
			// if not up to date, check to see if we allow downloading
			if (Options.bDoNotDownload && !InstallState.GetAllBundlesHaveState(EInstallBundleInstallState::UpToDate))
			{
				Result = GetErrorResult(TEXT("GFPStateMachine.DownloadNotAllowed"));
				bSuppressResultErrorLog = true; // Don't log an error if the user disallowed the download
				UpdateStateMachineImmediate();
				return;
			}

			UGameFeaturesSubsystem::Get().OnGameFeatureDownloading(StateProperties.PluginName, StateProperties.PluginIdentifier);
			EnsureAllowAsyncLoading();

			// We usualy only have a use case of one bundle per GFP and we only care about relative weighting here, so we don't need 
			// any of the other content state metadata. We can just assume the weight is 1.0 and not have to wait for the full 
			// async call to get the rest of the metadata.
			FInstallBundleCombinedContentState HackContentState;
			HackContentState.IndividualBundleStates.Reserve(InstallState.IndividualBundleStates.Num());
			for (const TPair<FName, EInstallBundleInstallState>& Pair : InstallState.IndividualBundleStates)
			{
				FInstallBundleContentState& BundleContentState = HackContentState.IndividualBundleStates.Emplace(Pair.Key);
				BundleContentState.State = Pair.Value;
				BundleContentState.Weight = 1.0f;
			}
			HackContentState.BundlesWithIoStoreOnDemand = MoveTemp(InstallState.BundlesWithIoStoreOnDemand);
			OnGotContentState(MoveTemp(HackContentState));
		}
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!Result.HasValue())
		{
			const EGameFeaturePluginState FailState = GetFailureTransitionState();
			StateStatus.SetTransitionError(FailState, Result, bSuppressResultErrorLog);
			return;
		}

		if (!bPluginDownloaded)
		{
			return;
		}

		const EGameFeaturePluginState SuccessState = GetSuccessTransitionState();
		StateStatus.SetTransition(SuccessState);
	}

	virtual void TryCancelState() override
	{
		if (PendingBundleDownloads.Num() > 0)
		{
			TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
			BundleManager->CancelUpdateContent(PendingBundleDownloads);
			if (IoStoreOnDemandContext)
			{
				IoStoreOnDemandContext->Cancel();
			}
		}
	}

	virtual UE::GameFeatures::FResult TryUpdateProtocolOptions(const FGameFeatureProtocolOptions& NewOptions) override
	{
		//Need to update our BundleFlags for any bundles we are downloading
		EInstallBundleRequestFlags OldRequestFlags;
		bool OldUserPausedFlag;
		{
			const FInstallBundlePluginProtocolOptions& OldOptions = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();
			OldRequestFlags = OldOptions.InstallBundleFlags;
			OldUserPausedFlag = OldOptions.bUserPauseDownload;
		}

		UE::GameFeatures::FResult OptionsResult = FGameFeaturePluginState::TryUpdateProtocolOptions(NewOptions);
		if (OptionsResult.HasError())
		{
			return OptionsResult;
		}

		//if we don't have any in-progress downloads the default behavior is all we need
		if (PendingBundleDownloads.Num() == 0)
		{
			return OptionsResult;
		}

		const FInstallBundlePluginProtocolOptions& Options = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();

		//Update our InstallBundleRequestFlags
		{
			EInstallBundleRequestFlags UpdatedRequestFlags = Options.InstallBundleFlags;

			EInstallBundleRequestFlags AddFlags = (UpdatedRequestFlags & (~OldRequestFlags));
			EInstallBundleRequestFlags RemoveFlags = ((~UpdatedRequestFlags) & OldRequestFlags);

			if ((AddFlags != EInstallBundleRequestFlags::None) || (RemoveFlags != EInstallBundleRequestFlags::None))
			{
				TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
				BundleManager->UpdateContentRequestFlags(PendingBundleDownloads, AddFlags, RemoveFlags);
			}
		}

		//Handle pausing or resuming the download if the bUserPauseDownload flag has changed
		{
			if (Options.bUserPauseDownload != OldUserPausedFlag)
			{
				ChangePauseState(Options.bUserPauseDownload);
			}
		}

		return OptionsResult;
	}

	virtual void EndState() override
	{
		Cleanup();
	}

	/** Controls what check is done to determine if this state should run or not */
	bool ShouldDownloadContent() const
	{
		switch (StateProperties.GetPluginProtocol())
		{
		case (EGameFeaturePluginProtocol::InstallBundle):
			return true;
		default:
			return false;
		}
	}

	virtual TConstArrayView<FName> GetInstallBundles()
	{
		TConstArrayView<FName> Ret;
		if (ShouldDownloadContent())
		{
			Ret = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>().InstallBundles;
		}
		return Ret;
	}

	/** Determine what kind of request flags we submit */
	EInstallBundleRequestFlags GetRequestFlags() const
	{
		//Pull our InstallFlags from the Options, but also make sure SkipMount is set as there is a separate mounting step that will re-request this
		//without SkipMount and then mount the data, this allows us to pre-download data without mounting it
		EInstallBundleRequestFlags InstallFlags = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>().InstallBundleFlags;
		InstallFlags |= EInstallBundleRequestFlags::SkipMount;
		return InstallFlags;
	}

	/** Determines what state you transition to in the event of a success or failure to release content */
	EGameFeaturePluginState GetSuccessTransitionState() const
	{
		return EGameFeaturePluginState::Installed;
	}

	EGameFeaturePluginState GetFailureTransitionState() const
	{
		return EGameFeaturePluginState::ErrorManagingData;
	}
};

struct FGameFeaturePluginState_Installed : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Installed(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination > EGameFeaturePluginState::Installed)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Mounting);
		}
		else if (StateProperties.Destination < EGameFeaturePluginState::Installed)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Releasing);
		}
	}
};

struct FGameFeaturePluginState_ErrorMounting : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorMounting(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorMounting)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Unmounting);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorMounting)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Mounting);
		}
	}
};

struct FGameFeaturePluginState_ErrorWaitingForDependencies : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorWaitingForDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorWaitingForDependencies)
		{
			// There is no cleaup state equivalent to EGameFeaturePluginState::WaitingForDependencies so just go back to unmounting
			StateStatus.SetTransition(EGameFeaturePluginState::Unmounting);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorWaitingForDependencies)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::WaitingForDependencies);
		}
	}
};

struct FGameFeaturePluginState_ErrorRegistering : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorRegistering(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorRegistering)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Unregistering);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorRegistering)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Registering);
		}
	}
};

struct FGameFeaturePluginState_Unmounting : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Unmounting(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	UE::GameFeatures::FResult Result = MakeValue();
	TArray<FName> PendingBundles;
	bool bUnmounting = false;
	bool bUnmounted = false;
	bool bCheckedRealtimeMode = false;

	void Unmount()
	{
		if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
			Plugin && Plugin->GetDescriptor().bExplicitlyLoaded)
		{
			if (!UE::GameFeatures::ShouldDeferLocalizationDataLoad())
			{
				IPluginManager::Get().UnmountExplicitlyLoadedPluginLocalizationData(StateProperties.PluginName);
			}

#if UE_MERGED_MODULES
			// We normally do not allow unloading code because it's difficult to make that safe.
			// For example, destructors can be called much later than we'd expect because of garbage collection or async work.
			// We allow this behavior for merged modular builds because the have a smaller scope (intended for console clients) and
			// unloading code to save memory is the main goal for that feature.
			static constexpr bool bAllowUnloadCode = true;
#else
			static constexpr bool bAllowUnloadCode = false;
#endif // UE_MERGED_MODULES

			// The asset registry listens to FPackageName::OnContentPathDismounted() and 
			// will automatically cleanup the asset registry state we added for this plugin.
			// This will also cause any assets we added to the asset manager to be removed.
			// Scan paths added to the asset manager should have already been cleaned up.
			FText FailureReason;
			if (!IPluginManager::Get().UnmountExplicitlyLoadedPlugin(StateProperties.PluginName, &FailureReason, bAllowUnloadCode))
			{
				const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
				ensureMsgf(false, TEXT("Failed to explicitly unmount the PluginURL(%.*s) because %s"), ShortUrl.Len(), ShortUrl.GetData(), *FailureReason.ToString());
				Result = GetErrorResult(TEXT("Plugin_Cannot_Explicitly_Unmount"));
				return;
			}
		}

		if (StateProperties.bAddedPluginToManager)
		{
			verify(IPluginManager::Get().RemoveFromPluginsList(StateProperties.PluginInstalledFilename));
			StateProperties.bAddedPluginToManager = false;
		}

		if (StateProperties.GetPluginProtocol() != EGameFeaturePluginProtocol::InstallBundle)
		{
			bUnmounted = true;
			return;
		}

		TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();
		check(BundleManager.IsValid());

		const TArray<FName>& InstallBundles = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>().InstallBundles;

		TArray<FName> InstallBundlesToRelease = UE::GameFeatures::GFPSharedInstallTracker.Release(
			StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::Mount, InstallBundles);

		EInstallBundleReleaseRequestFlags ReleaseFlags = 
			EInstallBundleReleaseRequestFlags::SkipReleaseUnmountOnly |
			EInstallBundleReleaseRequestFlags::ExplicitRemoveList; // Always set ExplicitRemoveList, GFPSharedInstallTracker has filtered out shared dependencies
		TValueOrError<FInstallBundleReleaseRequestInfo, EInstallBundleResult> MaybeRequestInfo = BundleManager->RequestReleaseContent(
			InstallBundlesToRelease, ReleaseFlags);

		if (MaybeRequestInfo.HasError())
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue unmount for the PluginURL(%.*s) because %s"), ShortUrl.Len(), ShortUrl.GetData(), LexToString(MaybeRequestInfo.GetError()));
			Result = GetErrorResult(TEXT("BundleManager.Begin."), MaybeRequestInfo.GetError());
			return;
		}

		FInstallBundleReleaseRequestInfo RequestInfo = MaybeRequestInfo.StealValue();

		if (EnumHasAnyFlags(RequestInfo.InfoFlags, EInstallBundleRequestInfoFlags::SkippedUnknownBundles))
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue unmount for the PluginURL(%.*s) because failed to resolve install bundles!"), ShortUrl.Len(), ShortUrl.GetData());
			Result = GetErrorResult(TEXT("BundleManager.Begin."), TEXT("Cannot_Resolve"), UE::GameFeatures::CommonErrorCodes::GetGenericConnectionError());
			return;
		}

		if (RequestInfo.BundlesEnqueued.Num() == 0)
		{
			bUnmounted = true;
		}
		else
		{
			PendingBundles = MoveTemp(RequestInfo.BundlesEnqueued);
			IInstallBundleManager::ReleasedDelegate.AddRaw(this, &FGameFeaturePluginState_Unmounting::OnContentReleased);
		}
	}

	void OnContentReleased(FInstallBundleReleaseRequestResultInfo BundleResult)
	{
		if (!PendingBundles.Contains(BundleResult.BundleName))
		{
			return;
		}

		PendingBundles.Remove(BundleResult.BundleName);

		if (!Result.HasError() && BundleResult.Result != EInstallBundleReleaseResult::OK)
		{
			Result = GetErrorResult(TEXT("BundleManager.OnReleased."), BundleResult.Result);
		}

		if (PendingBundles.Num() > 0)
		{
			return;
		}

		if (Result.HasValue())
		{
			bUnmounted = true;
		}

		UpdateStateMachineImmediate();
	}

	virtual void BeginState() override
	{
		Result = MakeValue();
		PendingBundles.Empty();
		bUnmounting = false;
		bUnmounted = false;
		bCheckedRealtimeMode = false;
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!bCheckedRealtimeMode)
		{
			bCheckedRealtimeMode = true;
			if (UE::GameFeatures::RealtimeMode)
			{
				UE::GameFeatures::RealtimeMode->AddUpdateRequest(StateProperties.OnRequestUpdateStateMachine);
				return;
			}
		}

		if (!bUnmounting)
		{
			bUnmounting = true;
			Unmount();
		}

		if (!Result.HasValue())
		{
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorMounting, Result);
			return;
		}

		if (!bUnmounted)
		{
			return;
		}

		StateStatus.SetTransition(EGameFeaturePluginState::Installed);
	}

	virtual void EndState() override
	{
		IInstallBundleManager::ReleasedDelegate.RemoveAll(this);
	}
};

struct FGameFeaturePluginState_Mounting : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Mounting(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FGameFeaturePluginState(InStateProperties)
		, Result(MakeValue())
	{}

	enum class ESubState : uint8
	{
		None = 0,
		MountPlugin,
		LoadAssetRegistry
	};
	FRIEND_ENUM_CLASS_FLAGS(ESubState);

	int32 NumObservedPostMountPausers = 0;
	int32 NumExpectedPostMountPausers = 0;
	TArray<FName> PendingBundles;
	FDelegateHandle PakFileMountedDelegateHandle;
	UE::GameFeatures::FResult Result;
	ESubState StartedSubStates = ESubState::None;
	ESubState CompletedSubStates = ESubState::None;
	bool bCheckedRealtimeMode = false;
	bool bForceMonolithicShaderLibrary = true;	// use monolithic unless a DLC plugin is chunked

	static UE::Tasks::FPipe ShaderlibPipe;

	void OnInstallBundleCompleted(FInstallBundleRequestResultInfo BundleResult)
	{
		if (!PendingBundles.Contains(BundleResult.BundleName))
		{
			return;
		}

		PendingBundles.Remove(BundleResult.BundleName);

		if (!Result.HasError() && BundleResult.Result != EInstallBundleResult::OK)
		{
			if (BundleResult.OptionalErrorCode.IsEmpty())
			{
				Result = GetErrorResult(TEXT("BundleManager.OnComplete."), BundleResult.Result);
			}
			else
			{
				Result = GetErrorResult(TEXT("BundleManager.OnComplete."), BundleResult.OptionalErrorCode, BundleResult.OptionalErrorText);
			}
		}

		if (bForceMonolithicShaderLibrary && BundleResult.bContainsChunks)
		{
			bForceMonolithicShaderLibrary = false;
		}

		if (PendingBundles.IsEmpty())
		{
			IInstallBundleManager::InstallBundleCompleteDelegate.RemoveAll(this);
			if (PakFileMountedDelegateHandle.IsValid())
			{
				FCoreDelegates::GetOnPakFileMounted2().Remove(PakFileMountedDelegateHandle);
				PakFileMountedDelegateHandle.Reset();
			}
			
			UpdateStateMachineImmediate();
		}
	}

	void OnPakFileMounted(const IPakFile& PakFile)
	{
		if (FPakFile* Pak = (FPakFile*)(&PakFile))
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			UE_LOG(LogGameFeatures, Display, TEXT("Mounted Pak File for (%.*s) with following files:"), ShortUrl.Len(), ShortUrl.GetData());
			TArray<FString> OutFileList;
			Pak->GetPrunedFilenames(OutFileList);
			for (const FString& FileName : OutFileList)
			{
				UE_LOG(LogGameFeatures, Display, TEXT("(%s)"), *FileName);
			}
		}
	}

	void OnPostMountPauserCompleted(FStringView InPauserTag)
	{
		check(IsInGameThread());
		ensure(NumExpectedPostMountPausers != INDEX_NONE);
		++NumObservedPostMountPausers;

		UE_LOG(LogGameFeatures, Display, TEXT("Post-mount of %s resumed by %.*s"), *StateProperties.PluginName, InPauserTag.Len(), InPauserTag.GetData());

		if (NumObservedPostMountPausers == NumExpectedPostMountPausers)
		{
			UpdateStateMachineImmediate();
		}
	}

	virtual bool UseAsyncLoading() const override
	{
		if (UE::GameFeatures::CVarForceSyncRegisterStartupPlugins.GetValueOnGameThread())
		{
			if (UGameFeaturesSubsystem::Get().GetPolicy().IsLoadingStartupPlugins())
			{
				return false;
			}
		}

		return FGameFeaturePluginState::UseAsyncLoading();
	}

	virtual void BeginState() override
	{
		NumObservedPostMountPausers = 0;
		NumExpectedPostMountPausers = 0;
		PendingBundles.Empty();
		PakFileMountedDelegateHandle.Reset();
		Result = MakeValue();
		StartedSubStates = ESubState::None;
		CompletedSubStates = ESubState::None;
		bCheckedRealtimeMode = false;
		bForceMonolithicShaderLibrary = false;

		if (StateProperties.GetPluginProtocol() != EGameFeaturePluginProtocol::InstallBundle)
		{
			return;
		}

		// Assume monolithic shader, will be set to false if chunks are detected
		bForceMonolithicShaderLibrary = UE::GameFeatures::CVarAllowForceMonolithicShaderLibrary.GetValueOnGameThread();
		
		TSharedPtr<IInstallBundleManager> BundleManager = IInstallBundleManager::GetPlatformInstallBundleManager();

		const FInstallBundlePluginProtocolMetaData& MetaData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();
		const TArray<FName>& InstallBundles = MetaData.InstallBundles;

		UE::GameFeatures::GFPSharedInstallTracker.AddBundleRefs(StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::Mount, InstallBundles);

		const FInstallBundlePluginProtocolOptions& Options = StateProperties.ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();
		const EInstallBundleRequestFlags InstallFlags = UseAsyncLoading() ?
			(Options.InstallBundleFlags | EInstallBundleRequestFlags::AsyncMount) : Options.InstallBundleFlags;

		// Make bundle manager use verbose log level for most logs.
		// We are already done with downloading, so we don't care about logging too much here unless mounting fails.
		const ELogVerbosity::Type InstallBundleManagerVerbosityOverride = ELogVerbosity::Verbose;
		TValueOrError<FInstallBundleRequestInfo, EInstallBundleResult> MaybeRequestInfo = BundleManager->RequestUpdateContent(InstallBundles, InstallFlags, InstallBundleManagerVerbosityOverride);

		if (MaybeRequestInfo.HasError())
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue mount for the PluginURL(%.*s) because %s"), ShortUrl.Len(), ShortUrl.GetData(), LexToString(MaybeRequestInfo.GetError()));
			Result = GetErrorResult(TEXT("BundleManager.Begin.CannotStart."), MaybeRequestInfo.GetError());
			return;
		}

		FInstallBundleRequestInfo RequestInfo = MaybeRequestInfo.StealValue();

		if (EnumHasAnyFlags(RequestInfo.InfoFlags, EInstallBundleRequestInfoFlags::SkippedUnknownBundles))
		{
			const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
			ensureMsgf(false, TEXT("Unable to enqueue mount for the PluginURL(%.*s) because failed to resolve install bundles!"), ShortUrl.Len(), ShortUrl.GetData());
			Result = GetErrorResult(TEXT("BundleManager.Begin."), TEXT("Resolve_Failed"));
			return;
		}

		if (RequestInfo.BundlesEnqueued.Num() > 0)
		{
			PendingBundles = MoveTemp(RequestInfo.BundlesEnqueued);
			IInstallBundleManager::InstallBundleCompleteDelegate.AddRaw(this, &FGameFeaturePluginState_Mounting::OnInstallBundleCompleted);
			if (UE::GameFeatures::ShouldLogMountedFiles)
			{
				// Track with a delegate handle to avoid unbinding if we don't use this. Unbinding this causes an occaisonal perf spike.
				PakFileMountedDelegateHandle = FCoreDelegates::GetOnPakFileMounted2().AddRaw(this, &FGameFeaturePluginState_Mounting::OnPakFileMounted);
			}
		}

		for (const FInstallBundleRequestResultInfo& BundleResult : RequestInfo.BundleResults)
		{
			if (bForceMonolithicShaderLibrary && BundleResult.bContainsChunks)
			{
				bForceMonolithicShaderLibrary = false;
			}
		}
	}

	void UpdateState_MountPlugin(bool bLoadPluginIniHierarchy)
	{
		if (EnumHasAnyFlags(StartedSubStates, ESubState::MountPlugin))
		{
			return;
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Mounting_Plugin);

		StartedSubStates |= ESubState::MountPlugin;

		if (Result.HasError())
		{
			CompletedSubStates |= ESubState::MountPlugin;
			return;
		}

		// Pre-mount
		// Normally the shader library itself listens to a "New Plugin mounted" (and "New Pakfile mounted") callback and the library is opened automatically. This switch governs whether the manual behavior is wanted.
		bool bManuallyOpenPluginShaderLibrary = true;
		{
			FGameFeaturePreMountingContext Context;
			UGameFeaturesSubsystem::Get().OnGameFeaturePreMounting(StateProperties.PluginName, StateProperties.PluginIdentifier, Context);
			bManuallyOpenPluginShaderLibrary = Context.bOpenPluginShaderLibrary;
		}

		checkf(!StateProperties.PluginInstalledFilename.IsEmpty(), TEXT("PluginInstalledFilename must be set by the Mounting. PluginURL: %s"), *StateProperties.PluginIdentifier.GetFullPluginURL());
		checkf(FPaths::GetExtension(StateProperties.PluginInstalledFilename) == TEXT("uplugin"), TEXT("PluginInstalledFilename must have a uplugin extension. PluginURL: %s"), *StateProperties.PluginIdentifier.GetFullPluginURL());

		// refresh the plugins list to let the plugin manager know about it
		const TSharedPtr<IPlugin> MaybePlugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
		const bool bNeedsPluginMount = (MaybePlugin == nullptr || MaybePlugin->GetDescriptor().bExplicitlyLoaded);

		if (MaybePlugin)
		{
			if (!FPaths::IsSamePath(MaybePlugin->GetDescriptorFileName(), StateProperties.PluginInstalledFilename))
			{
				Result = GetErrorResult(TEXT("Plugin_Name_Already_In_Use"));
			}
		}
		else
		{
			const bool bAddedPlugin = IPluginManager::Get().AddToPluginsList(StateProperties.PluginInstalledFilename);
			if (bAddedPlugin)
			{
				StateProperties.bAddedPluginToManager = true;
			}
			else
			{
				Result = GetErrorResult(TEXT("Failed_To_Register_Plugin"));
			}
		}

		// now load ini files if desired, now that we know the plugin has been loaded
		if (bLoadPluginIniHierarchy)
		{
			UGameFeatureData::InitializeBasePluginIniFile(StateProperties.PluginInstalledFilename);
		}


		if (Result.HasError() || !bNeedsPluginMount)
		{
			CompletedSubStates |= ESubState::MountPlugin;
			return;
		}

		if (bManuallyOpenPluginShaderLibrary)
		{
			// We want to control opening the shader lib
			FShaderCodeLibrary::DontOpenPluginShaderLibraryOnMount(StateProperties.PluginName);
		}

		if (!UseAsyncLoading() || UE::GameFeatures::CVarForceSyncLoadShaderLibrary.GetValueOnGameThread())
		{
			verify(IPluginManager::Get().MountExplicitlyLoadedPlugin(StateProperties.PluginName));
			if (!UE::GameFeatures::ShouldDeferLocalizationDataLoad())
			{
				UGameFeaturePluginStateMachine* CurrentMachine = UGameFeaturesSubsystem::Get().FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier);
				UE::GameFeatures::MountLocalizationData(CurrentMachine, StateProperties);
			}
			if (bManuallyOpenPluginShaderLibrary)
			{
				TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
				FShaderCodeLibrary::OpenPluginShaderLibrary(*Plugin, bForceMonolithicShaderLibrary);
			}
			CompletedSubStates |= ESubState::MountPlugin;
			return;
		}

		verify(IPluginManager::Get().MountExplicitlyLoadedPlugin(StateProperties.PluginName));
		if (!UE::GameFeatures::ShouldDeferLocalizationDataLoad())
		{
			UGameFeaturePluginStateMachine* CurrentMachine = UGameFeaturesSubsystem::Get().FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier);
			UE::GameFeatures::MountLocalizationData(CurrentMachine, StateProperties);
		}

		// Now load the shader lib in the background
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
		if (bManuallyOpenPluginShaderLibrary && Plugin->CanContainContent() && Plugin->IsEnabled())
		{
			// TEMP HACK - use a pipe because if this goes too wide we can end up blocking all available tasks.
			ShaderlibPipe.Launch(UE_SOURCE_LOCATION, [this, Plugin]
			{
				FShaderCodeLibrary::OpenPluginShaderLibrary(*Plugin, bForceMonolithicShaderLibrary);

				ExecuteOnGameThread(UE_SOURCE_LOCATION, [this]
				{
					CompletedSubStates |= ESubState::MountPlugin;
					UpdateStateMachineImmediate();
				});

			}, UE::Tasks::ETaskPriority::BackgroundHigh);

			return;
		}

		CompletedSubStates |= ESubState::MountPlugin;
	}

	void UpdateState_LoadAssetRegistry()
	{
		if (EnumHasAnyFlags(StartedSubStates, ESubState::LoadAssetRegistry))
		{
			return;
		}

		StartedSubStates |= ESubState::LoadAssetRegistry;

		if (Result.HasError())
		{
			CompletedSubStates |= ESubState::LoadAssetRegistry;
			return;
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Mounting_AR);

		// After the new plugin is mounted add the asset registry for that plugin.
		TSharedPtr<IPlugin> NewlyMountedPlugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
		if (!NewlyMountedPlugin || !NewlyMountedPlugin->CanContainContent())
		{
			CompletedSubStates |= ESubState::LoadAssetRegistry;
			return;
		}

		FString PluginAssetRegistry;
		{
			const FString PluginFolder = FPaths::GetPath(StateProperties.PluginInstalledFilename);
			TArray<FString> PluginAssetRegistrySearchPaths;
			// For GFPs cooked as DLC
			PluginAssetRegistrySearchPaths.Add(PluginFolder / TEXT("AssetRegistry.bin"));
			// For GFPs with a unique chunk
			PluginAssetRegistrySearchPaths.Add(FPaths::ProjectDir() / FString::Printf(TEXT("AssetRegistry_GFP_%s.bin"), *StateProperties.PluginName));
			for (FString& Path : PluginAssetRegistrySearchPaths)
			{
				if (IFileManager::Get().FileExists(*Path))
				{
					PluginAssetRegistry = MoveTemp(Path);
					break;
				}
			}

			if (PluginAssetRegistry.IsEmpty())
			{
				CompletedSubStates |= ESubState::LoadAssetRegistry;
				return;
			}
		}

		auto RefreshPackageLocalizationCacheForPlugin = [NewlyMountedPlugin]()
		{
			// We need to refresh the package localization cache for a GFP if it loaded cooked asset registry state, 
			// as we need the asset registry data to correctly build the package localization cache for the GFP
			if (NewlyMountedPlugin && NewlyMountedPlugin->CanContainContent())
			{
				FPackageLocalizationManager::Get().InvalidateRootSourcePath(NewlyMountedPlugin->GetMountedAssetPath());
			}
		};

		if (!UseAsyncLoading())
		{
			FAssetRegistryState PluginAssetRegistryState;
			if (FAssetRegistryState::LoadFromDisk(*PluginAssetRegistry, FAssetRegistryLoadOptions(), PluginAssetRegistryState))
			{
				IAssetRegistry& AssetRegistry = UAssetManager::Get().GetAssetRegistry();
				AssetRegistry.AppendState(PluginAssetRegistryState);
				RefreshPackageLocalizationCacheForPlugin();
			}
			else
			{
				Result = GetErrorResult(TEXT("Failed_To_Load_Plugin_AssetRegistry"));
			}

			CompletedSubStates |= ESubState::LoadAssetRegistry;
			return;
		}

		const bool bForceSyncAssetRegistryAppend = UE::GameFeatures::CVarForceSyncAssetRegistryAppend.GetValueOnGameThread();
		UE::Tasks::Launch(UE_SOURCE_LOCATION, [this, PluginAssetRegistry=MoveTemp(PluginAssetRegistry), bForceSyncAssetRegistryAppend, RefreshPackageLocalizationCacheForPlugin]
		{
			bool bSuccess = false;
			TSharedPtr<FAssetRegistryState> PluginAssetRegistryState = MakeShared<FAssetRegistryState>();
			if (FAssetRegistryState::LoadFromDisk(*PluginAssetRegistry, FAssetRegistryLoadOptions(), *PluginAssetRegistryState))
			{
				IAssetRegistry& AssetRegistry = UAssetManager::Get().GetAssetRegistry();
				if (!bForceSyncAssetRegistryAppend)
				{
					AssetRegistry.AppendState(*PluginAssetRegistryState);
					RefreshPackageLocalizationCacheForPlugin();
				}
				bSuccess = true;
			}

			ExecuteOnGameThread(UE_SOURCE_LOCATION, [this, PluginAssetRegistryState, bSuccess, bForceSyncAssetRegistryAppend, RefreshPackageLocalizationCacheForPlugin]
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Mounting_ARComplete);

				if (!bSuccess)
				{
					Result = GetErrorResult(TEXT("Failed_To_Load_Plugin_AssetRegistry"));
				}
				else if (bForceSyncAssetRegistryAppend)
				{
					IAssetRegistry& AssetRegistry = UAssetManager::Get().GetAssetRegistry();
					AssetRegistry.AppendState(*PluginAssetRegistryState);
					RefreshPackageLocalizationCacheForPlugin();
				}

				CompletedSubStates |= ESubState::LoadAssetRegistry;
				UpdateStateMachineImmediate();
			});

		}, UE::Tasks::ETaskPriority::BackgroundHigh);
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		// Check if waiting for install bundles
		if (PendingBundles.Num() > 0)
		{
			return;
		}

		// Check if post-mount is paused
		if (NumExpectedPostMountPausers > 0)
		{
			// Check if post-mount unpaused
			if (NumExpectedPostMountPausers == NumObservedPostMountPausers)
			{
				NumExpectedPostMountPausers = INDEX_NONE;
				TransitionOut(StateStatus);
			}
			return;
		}

		if (!bCheckedRealtimeMode)
		{
			bCheckedRealtimeMode = true;
			if (UE::GameFeatures::RealtimeMode)
			{
				UE::GameFeatures::RealtimeMode->AddUpdateRequest(StateProperties.OnRequestUpdateStateMachine);
				return;
			}
		}

		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Mounting);
		
		UpdateState_MountPlugin(AllowIniLoading());
		UpdateState_LoadAssetRegistry();

		const bool bComplete = EnumHasAllFlags(CompletedSubStates, ESubState::MountPlugin | ESubState::LoadAssetRegistry);

		// Post-mount
		if (bComplete)
		{
			FGameFeaturePostMountingContext Context(StateProperties.PluginName, [this](FStringView InPauserTag) { OnPostMountPauserCompleted(InPauserTag); });
			NumExpectedPostMountPausers = INDEX_NONE;
			UGameFeaturesSubsystem::Get().OnGameFeaturePostMounting(StateProperties.PluginName, StateProperties.PluginIdentifier, Context);
			NumExpectedPostMountPausers = Context.NumPausers;

			// Check if we got post-mount paused
			if (NumExpectedPostMountPausers <= 0)
			{
				TransitionOut(StateStatus);
			}
		}
	}

	void TransitionOut(FGameFeaturePluginStateStatus& StateStatus)
	{
		if (Result.HasError())
		{
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorMounting, Result);
		}
		else
		{
			StateStatus.SetTransition(EGameFeaturePluginState::WaitingForDependencies);
		}
	}

	virtual void EndState() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Mounting_EndState);
		IInstallBundleManager::InstallBundleCompleteDelegate.RemoveAll(this);
		if (PakFileMountedDelegateHandle.IsValid())
		{
			FCoreDelegates::GetOnPakFileMounted2().RemoveAll(this);
			PakFileMountedDelegateHandle.Reset();
		}
	}
};
ENUM_CLASS_FLAGS(FGameFeaturePluginState_Mounting::ESubState);
UE::Tasks::FPipe FGameFeaturePluginState_Mounting::ShaderlibPipe(TEXT("FGameFeaturePluginState_Mounting::ShaderlibPipe"));

struct FWaitingForDependenciesTransitionPolicy
{
	static bool GetPluginDependencyStateMachines(const FGameFeaturePluginStateMachineProperties& InStateProperties, TArray<UGameFeaturePluginStateMachine*>& OutDependencyMachines)
	{
		UGameFeaturesSubsystem& GameFeaturesSubsystem = UGameFeaturesSubsystem::Get();

		return GameFeaturesSubsystem.FindOrCreatePluginDependencyStateMachines(
			InStateProperties.PluginIdentifier.GetFullPluginURL(), InStateProperties, OutDependencyMachines);
	}

	static FGameFeaturePluginStateRange GetDependencyStateRange()
	{
		return FGameFeaturePluginStateRange(EGameFeaturePluginState::Registered, EGameFeaturePluginState::Active);
	}

	static EGameFeaturePluginState GetTransitionState()
	{
		return UE::GameFeatures::CVarEnableAssetStreaming.GetValueOnGameThread() ? EGameFeaturePluginState::AssetDependencyStreaming : EGameFeaturePluginState::Registering;
	}

	static EGameFeaturePluginState GetErrorState()
	{
		return EGameFeaturePluginState::ErrorWaitingForDependencies;
	}

	static bool ExcludeDepedenciesFromBatchProcessing()
	{
		return false;
	}

	static bool ShouldWaitForDependencies()
	{
		return true;
	}
};

struct FGameFeaturePluginState_WaitingForDependencies : public FTransitionDependenciesGameFeaturePluginState<FWaitingForDependenciesTransitionPolicy>
{
	FGameFeaturePluginState_WaitingForDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FTransitionDependenciesGameFeaturePluginState(InStateProperties)
	{
	}
};

struct FGameFeaturePluginState_AssetDependencyStreamOut : public FGameFeaturePluginState
{
	FGameFeaturePluginState_AssetDependencyStreamOut(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FGameFeaturePluginState(InStateProperties)
	{}

	virtual void BeginState() override
	{
		if (StateProperties.GetPluginProtocol() != EGameFeaturePluginProtocol::InstallBundle)
		{
			return;
		}

		const FInstallBundlePluginProtocolMetaData& MetaData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();

		UE::GameFeatures::GFPSharedInstallTracker.Release(
			StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::AssetStream, MetaData.InstallBundlesWithAssetDependencies);
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		StateStatus.SetTransition(EGameFeaturePluginState::Unmounting);
	}
};

struct FGameFeaturePluginState_ErrorAssetDependencyStreaming : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorAssetDependencyStreaming(FGameFeaturePluginStateMachineProperties& InStateProperties) 
		: FErrorGameFeaturePluginState(InStateProperties) 
	{}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorAssetDependencyStreaming)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::AssetDependencyStreamOut);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorAssetDependencyStreaming)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::AssetDependencyStreaming);
		}
	}
};

struct FGameFeaturePluginState_AssetDependencyStreaming : public FGameFeaturePluginState
{
	FGameFeaturePluginState_AssetDependencyStreaming(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FGameFeaturePluginState(InStateProperties)
	{}

	virtual ~FGameFeaturePluginState_AssetDependencyStreaming()
	{
		Cleanup();
	}

	struct FIoStoreOnDemandProgress
	{
		FName InstallBundle;
		UE::IoStore::FOnDemandInstallProgress Progress;
	};

	// Required for callback lifetime safety
	struct FIoStoreOnDemandContext
	{
		TArray<UE::IoStore::FOnDemandInstallRequest> InstallRequests;
		TArray<FIoStoreOnDemandProgress> Progress;
		int32 PendingInstalls = 0;
		bool bStateValid = true;
	};

	TSharedPtr<FIoStoreOnDemandContext> IoStoreOnDemandContext;

	UE::GameFeatures::FResult Result{ MakeValue() };
	bool bComplete = false;

	void Cleanup()
	{
		Result = MakeValue();
		bComplete = false;

		if (IoStoreOnDemandContext)
		{
			for (UE::IoStore::FOnDemandInstallRequest& Request : IoStoreOnDemandContext->InstallRequests)
			{
				Request.Cancel();
			}
			IoStoreOnDemandContext->bStateValid = false;
			IoStoreOnDemandContext = nullptr;
		}
	}

	virtual void BeginState() override
	{
		Cleanup();

		if (StateProperties.GetPluginProtocol() != EGameFeaturePluginProtocol::InstallBundle)
		{
			bComplete = true;
			return;
		}

		const FInstallBundlePluginProtocolMetaData& MetaData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();

		if (MetaData.InstallBundlesWithAssetDependencies.IsEmpty())
		{
			bComplete = true;
			return;
		}

		UE::IoStore::IOnDemandIoStore* IoStore = UE::IoStore::TryGetOnDemandIoStore();
		if (!IoStore)
		{
			Result = GetErrorResult(TEXT("IoStoreOnDemand.ModuleNotFound"));
			return;
		}

		TValueOrError<TArray<EStreamingAssetInstallMode>, FString> MaybeInstallModes = 
			UGameFeaturesSubsystem::Get().GetPolicy().GetStreamingAssetInstallModes(
				StateProperties.PluginIdentifier.GetFullPluginURL(), MetaData.InstallBundlesWithAssetDependencies);

		if (MaybeInstallModes.HasError())
		{
			Result = GetErrorResult(TEXT("IoStoreOnDemand.InstallMode"), MaybeInstallModes.GetError());
			return;
		}

		UE::GameFeatures::GFPSharedInstallTracker.AddBundleRefs(
			StateProperties.PluginName, UE::GameFeatures::EGFPInstallLevel::AssetStream, MetaData.InstallBundlesWithAssetDependencies);

		IoStoreOnDemandContext = MakeShared<FIoStoreOnDemandContext>();
		IoStoreOnDemandContext->PendingInstalls = MetaData.InstallBundlesWithAssetDependencies.Num();
		IoStoreOnDemandContext->Progress.Reserve(MetaData.InstallBundlesWithAssetDependencies.Num());

		const TArray<EStreamingAssetInstallMode>& InstallModes = MaybeInstallModes.GetValue();
		for (int i = 0; const FName InstallBundle : MetaData.InstallBundlesWithAssetDependencies)
		{
			IoStoreOnDemandContext->Progress.Emplace(FIoStoreOnDemandProgress{InstallBundle});

			const EStreamingAssetInstallMode InstallMode = InstallModes[i++];

			UE::IoStore::FOnDemandInstallArgs InstallArgs;
			InstallArgs.MountId = InstallBundle.ToString();
			if (InstallMode == EStreamingAssetInstallMode::GfpRequiredOnly)
			{
				InstallArgs.TagSets.Emplace(TEXTVIEW("required"));
			}
			InstallArgs.Options |= UE::IoStore::EOnDemandInstallOptions::InstallSoftReferences;
			InstallArgs.Options |= UE::IoStore::EOnDemandInstallOptions::CallbackOnGameThread;

			InstallArgs.ContentHandle = UE::GameFeatures::GFPSharedInstallTracker.AddOnDemandContentHandle(
				InstallBundle, UE::GameFeatures::EGFPInstallLevel::AssetStream);

			IoStoreOnDemandContext->InstallRequests.Add(IoStore->Install(MoveTemp(InstallArgs),
				// On Complete
				[this, LambdaOnDemandContext = IoStoreOnDemandContext](const UE::IoStore::FOnDemandInstallResult& OnDemandInstallResult)
				{
					if (!LambdaOnDemandContext->bStateValid)
					{
						// Owning state got cleaned up, bail
						return;
					}

					if (!OnDemandInstallResult.Status.IsOk() && !Result.HasError())
					{
						const FString StatusString = OnDemandInstallResult.Status.ToString();
						FString ErrorCode = GetIoErrorText(OnDemandInstallResult.Status.GetErrorCode());
						ErrorCode.ReplaceCharInline(TEXT(' '), TEXT('_'), ESearchCase::CaseSensitive);

						// IOStore errors aren't localized 
						Result = GetErrorResult(TEXT("IoStoreOnDemand.OnComplete."), ErrorCode, FText::AsCultureInvariant(StatusString));
						TryCancelState();
					}

					--IoStoreOnDemandContext->PendingInstalls;
					if (IoStoreOnDemandContext->PendingInstalls == 0)
					{
						bComplete = true;
						UpdateStateMachineImmediate();
					}

				},
				// On Progress
				[this, LambdaOnDemandContext = IoStoreOnDemandContext, InstallBundle](const UE::IoStore::FOnDemandInstallProgress& Progress)
				{
					if (!LambdaOnDemandContext->bStateValid)
					{
						// Owning state got cleaned up, bail
						return;
					}

					FIoStoreOnDemandProgress* MyProgress = Algo::FindBy(
						LambdaOnDemandContext->Progress, InstallBundle, &FIoStoreOnDemandProgress::InstallBundle);
					check(MyProgress);
					MyProgress->Progress = Progress;

					const UE::IoStore::FOnDemandInstallProgress SumProgress = Algo::TransformAccumulate(
						LambdaOnDemandContext->Progress,
						&FIoStoreOnDemandProgress::Progress,
						UE::IoStore::FOnDemandInstallProgress(),
						&UE::IoStore::FOnDemandInstallProgress::Combine);
					const float OverallProgress = SumProgress.GetRelativeProgress();

					StateProperties.OnFeatureStateProgressUpdate.ExecuteIfBound(OverallProgress);
				}));
		}
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (!Result.HasValue())
		{
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorAssetDependencyStreaming, Result);
			return;
		}

		if (!bComplete)
		{
			return;
		}

		StateStatus.SetTransition(EGameFeaturePluginState::Registering);
	}

	virtual void EndState() override
	{
		Cleanup();
	}

	virtual void TryCancelState() override
	{
		if (IoStoreOnDemandContext)
		{
			for (UE::IoStore::FOnDemandInstallRequest& InstallRequest : IoStoreOnDemandContext->InstallRequests)
			{
				InstallRequest.Cancel();
			}
		}
	}
};

struct FGameFeaturePluginState_Unregistering : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Unregistering(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	bool bHasUnloaded = false;
#if WITH_EDITOR
	bool bRequestedUnloadPluginAssets = false;
#endif //if WITH_EDITOR

	virtual void BeginState() override
	{
		bHasUnloaded = false;
#if WITH_EDITOR
		bRequestedUnloadPluginAssets = false;
#endif //if WITH_EDITOR
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (bHasUnloaded)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::AssetDependencyStreamOut);
			return;
		}

#if WITH_EDITOR
		if (bRequestedUnloadPluginAssets)
		{
			bHasUnloaded = true;
			UpdateStateMachineDeferred();
			return;
		}
#endif //if WITH_EDITOR

		if (StateProperties.GameFeatureData)
		{
			UGameFeaturesSubsystem::Get().OnGameFeatureUnregistering(StateProperties.GameFeatureData, StateProperties.PluginName, StateProperties.PluginIdentifier);

			UGameFeaturesSubsystem::RemoveGameFeatureFromAssetManager(StateProperties.GameFeatureData, StateProperties.PluginName, StateProperties.AddedPrimaryAssetTypes);
			StateProperties.AddedPrimaryAssetTypes.Empty();

			UGameFeaturesSubsystem::UnloadGameFeatureData(StateProperties.GameFeatureData);
		}
		StateProperties.GameFeatureData = nullptr;

		// Try to remove the gameplay tags, this might be ignored depending on project settings
		const FString PluginFolder = FPaths::GetPath(StateProperties.PluginInstalledFilename);
		UGameplayTagsManager::Get().RemoveTagIniSearchPath(PluginFolder / TEXT("Config") / TEXT("Tags"));

#if WITH_EDITOR
		// This will properly unload any plugin asset that could be opened in the editor
		// and ensure standalone packages get unloaded as well
		if (FApp::IsGame())
		{
			verify(FPluginUtils::UnloadPluginAssets(StateProperties.PluginName));

			bHasUnloaded = true;
			UpdateStateMachineDeferred();
		}
		else
		{
			bRequestedUnloadPluginAssets = true;
			UE::GameFeatures::ScheduleUnloadPluginAssets(StateProperties.PluginName, StateProperties.OnRequestUpdateStateMachine);
		}
#else
		bHasUnloaded = true;
		UpdateStateMachineDeferred();
#endif
	}
};

struct FGameFeaturePluginState_Registering : public FGameFeaturePluginState
{
	enum class ELoadGFDState : uint8
	{
		Pending = 0,
		Success,
		Cancelled,
		Failed
	};

	TSharedPtr<FStreamableHandle> GameFeatureDataHandle;
	TArray<FString, TInlineAllocator<2>> GameFeatureDataSearchPaths;
	ELoadGFDState LoadGFDState = ELoadGFDState::Pending;
	bool bCheckedRealtimeMode = false;

	FGameFeaturePluginState_Registering(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}
	
	void TryAsyncLoadGameFeatureData(int32 Attempt = 0)
	{
		if (!GameFeatureDataSearchPaths.IsValidIndex(Attempt))
		{
			LoadGFDState = ELoadGFDState::Failed;
			UpdateStateMachineDeferred();
			return;
		}

		check(LoadGFDState == ELoadGFDState::Pending);

		bool bIsLoading = false;

		const FString& Path = GameFeatureDataSearchPaths[Attempt];
		if (FPackageName::DoesPackageExist(Path))
		{
			GameFeatureDataHandle = UGameFeaturesSubsystem::LoadGameFeatureData(Path, true /*bStartStalled*/);
			if (GameFeatureDataHandle && GameFeatureDataHandle->IsLoadingInProgress())
			{
				GameFeatureDataHandle->BindCancelDelegate(FStreamableDelegate::CreateLambda([this]
				{
					const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
					UE_LOG(LogGameFeatures, Error, TEXT("Game Feature Data loading was cancelled for URL %.*s"), ShortUrl.Len(), ShortUrl.GetData());

					LoadGFDState = ELoadGFDState::Cancelled;
					UpdateStateMachineDeferred();
				}));

				GameFeatureDataHandle->BindCompleteDelegate(FStreamableDelegate::CreateLambda([this, Attempt]
				{
					StateProperties.GameFeatureData = Cast<UGameFeatureData>(GameFeatureDataHandle->GetLoadedAsset());
					if (!StateProperties.GameFeatureData)
					{
						TryAsyncLoadGameFeatureData(Attempt + 1);
						return;
					}

					LoadGFDState = ELoadGFDState::Success;
					UpdateStateMachineDeferred();
				}));

				bIsLoading = true;
				GameFeatureDataHandle->StartStalledHandle();
			}
		}

		if (!bIsLoading)
		{
			TryAsyncLoadGameFeatureData(Attempt + 1);
		}
	}

	virtual bool UseAsyncLoading() const override
	{
		if (UE::GameFeatures::CVarForceSyncRegisterStartupPlugins.GetValueOnGameThread())
		{
			if (UGameFeaturesSubsystem::Get().GetPolicy().IsLoadingStartupPlugins())
			{
				return false;
			}
		}
		return FGameFeaturePluginState::UseAsyncLoading();
	}

	virtual void BeginState() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Registering_Begin);

		bCheckedRealtimeMode = false;

		const FString PluginFolder = FPaths::GetPath(StateProperties.PluginInstalledFilename);

		if (AllowIniLoading())
		{
			UGameplayTagsManager::Get().AddTagIniSearchPath(PluginFolder / TEXT("Config") / TEXT("Tags"), GConfig->GetStagedPluginConfigCache(FName(*StateProperties.PluginName)));
		}

		LoadGFDState = ELoadGFDState::Pending;

		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(StateProperties.PluginName);
		ensure(Plugin.IsValid());

		// If the plugin contains content then load the GameFeatureData otherwise procedurally create one that is transient.
		if (!Plugin->GetDescriptor().bCanContainContent)
		{
			StateProperties.GameFeatureData = NewObject<UGameFeatureData>(GetTransientPackage(), FName(*StateProperties.PluginName), RF_Transient);
			LoadGFDState = ELoadGFDState::Success;
			return;
		}

		FString BackupGameFeatureDataPath = FString::Printf(TEXT("/%s/%s.%s"), *StateProperties.PluginName, *StateProperties.PluginName, *StateProperties.PluginName);

		FString PreferredGameFeatureDataPath = TEXT("/") + StateProperties.PluginName + TEXT("/GameFeatureData.GameFeatureData");

		if (AllowIniLoading())
		{
			// Allow game feature location to be overriden globally and from within the plugin
			FString OverrideIniPathName = StateProperties.PluginName + TEXT("_Override");
			FString OverridePath = GConfig->GetStr(TEXT("GameFeatureData"), *OverrideIniPathName, GGameIni);
			if (OverridePath.IsEmpty())
			{
				const FString SettingsOverride = PluginFolder / TEXT("Config") / TEXT("Settings.ini");
				if (FPaths::FileExists(SettingsOverride))
				{
					GConfig->LoadFile(SettingsOverride);
					OverridePath = GConfig->GetStr(TEXT("GameFeatureData"), TEXT("Override"), SettingsOverride);
					GConfig->UnloadFile(SettingsOverride);
				}
			}
			if (!OverridePath.IsEmpty())
			{
				PreferredGameFeatureDataPath = MoveTemp(OverridePath);
			}
		}

		GameFeatureDataSearchPaths.Empty();
		GameFeatureDataSearchPaths.Emplace(MoveTemp(PreferredGameFeatureDataPath));
		GameFeatureDataSearchPaths.Emplace(MoveTemp(BackupGameFeatureDataPath));

		if (UseAsyncLoading())
		{
			TryAsyncLoadGameFeatureData();
		}
		else
		{
			FScopedSlowTask LoadingGameFeatureData(1.0f, 
				FText::Format(
					LOCTEXT("LoadingGameFeatureData", "Loading Game Feature Data for Plugin: {0}"), 
					FText::FromString(StateProperties.PluginName)));
			LoadingGameFeatureData.Visibility = ESlowTaskVisibility::Important;
			for (const FString& Path : GameFeatureDataSearchPaths)
			{
				if (FPackageName::DoesPackageExist(Path))
				{
					GameFeatureDataHandle = UGameFeaturesSubsystem::LoadGameFeatureData(Path);
					if (GameFeatureDataHandle)
					{
						GameFeatureDataHandle->WaitUntilComplete(0.0f, false);
						StateProperties.GameFeatureData = Cast<UGameFeatureData>(GameFeatureDataHandle->GetLoadedAsset());
					}
				}

				if (StateProperties.GameFeatureData)
				{
					break;
				}
			}

			LoadGFDState = StateProperties.GameFeatureData ? ELoadGFDState::Success : ELoadGFDState::Failed;
		}
	}

	virtual void EndState() override
	{
		GameFeatureDataHandle = nullptr;
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Registering_Update);

		if (!bCheckedRealtimeMode)
		{
			bCheckedRealtimeMode = true;
			if (UE::GameFeatures::RealtimeMode)
			{
				UE::GameFeatures::RealtimeMode->AddUpdateRequest(StateProperties.OnRequestUpdateStateMachine);
				return;
			}
		}

		if (!StateProperties.GameFeatureData)
		{
			check(LoadGFDState != ELoadGFDState::Success);

			if (LoadGFDState == ELoadGFDState::Pending)
			{
				return;
			}

			if (LoadGFDState == ELoadGFDState::Cancelled)
			{
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorRegistering, GetErrorResult(TEXT("Load_Cancelled_GameFeatureData")));
				return;
			}
		}

		if (StateProperties.GameFeatureData)
		{
			check(LoadGFDState == ELoadGFDState::Success);

			StateStatus.SetTransition(EGameFeaturePluginState::Registered);

			check(StateProperties.AddedPrimaryAssetTypes.Num() == 0);
			UGameFeaturesSubsystem::Get().AddGameFeatureToAssetManager(StateProperties.GameFeatureData, StateProperties.PluginName, StateProperties.AddedPrimaryAssetTypes);

			UGameFeaturesSubsystem::Get().OnGameFeatureRegistering(StateProperties.GameFeatureData, StateProperties.PluginName, StateProperties.PluginIdentifier);
		}
		else
		{
			check(LoadGFDState == ELoadGFDState::Failed);

			// The gamefeaturedata does not exist. The pak file may not be openable or this is a builtin plugin where the pak file does not exist.
			StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorRegistering, GetErrorResult(TEXT("Plugin_Missing_GameFeatureData")));
			if (UGameFeaturePluginStateMachine* CurrentMachine = UGameFeaturesSubsystem::Get().FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier))
			{
				UE_LOG(LogGameFeatures, Error, TEXT("Setting %s to be in unrecoverable error as GameFeatureData is missing"), *StateProperties.PluginIdentifier.GetFullPluginURL());
				CurrentMachine->SetUnrecoverableError();
			}
		}
	}
};

struct FGameFeaturePluginState_Registered : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Registered(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination > EGameFeaturePluginState::Registered)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Loading);
		}
		else if (StateProperties.Destination < EGameFeaturePluginState::Registered)
		{
			StateStatus.SetTransition( EGameFeaturePluginState::Unregistering);
		}
	}
};

struct FGameFeaturePluginState_ErrorLoading : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorLoading(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorLoading)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Unloading);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorLoading)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Loading);
		}
	}
};

struct FGameFeaturePluginState_Unloading : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Unloading(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	virtual void BeginState() override
	{
		if (UE::GameFeatures::ShouldDeferLocalizationDataLoad())
		{
			IPluginManager::Get().UnmountExplicitlyLoadedPluginLocalizationData(StateProperties.PluginName);
		}
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		UnloadGameFeatureBundles(StateProperties.GameFeatureData);
		UGameFeaturesSubsystem::Get().OnGameFeatureUnloading(StateProperties.GameFeatureData, StateProperties.PluginIdentifier);

		StateStatus.SetTransition(EGameFeaturePluginState::Registered);
	}


	void UnloadGameFeatureBundles(const UGameFeatureData* GameFeatureToLoad)
	{
		if (GameFeatureToLoad == nullptr)
		{
			return;
		}

		const UGameFeaturesProjectPolicies& Policy = UGameFeaturesSubsystem::Get().GetPolicy();

		// Remove all bundles from feature data and completely unload everything else
		FPrimaryAssetId GameFeatureAssetId = GameFeatureToLoad->GetPrimaryAssetId();
		TSharedPtr<FStreamableHandle> Handle = UAssetManager::Get().ChangeBundleStateForPrimaryAssets({ GameFeatureAssetId }, {}, {}, /*bRemoveAllBundles=*/ true);
		ensureAlways(Handle == nullptr || Handle->HasLoadCompleted()); // Should be no handle since nothing is being loaded

		TArray<FPrimaryAssetId> AssetIds = Policy.GetPreloadAssetListForGameFeature(GameFeatureToLoad, /*bIncludeLoadedAssets=*/true);

		// Don't unload game feature data asset yet, that will happen in FGameFeaturePluginState_Unregistering
		ensureAlways(AssetIds.RemoveSwap(GameFeatureAssetId, EAllowShrinking::No) == 0);

		if (AssetIds.Num() > 0)
		{
			UAssetManager::Get().UnloadPrimaryAssets(AssetIds);
		}
	}
};

struct FGameFeaturePluginState_Loading : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Loading(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	TSharedPtr<FStreamableHandle> BundleHandle;

	virtual void BeginState() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Loading_Begin);
		check(StateProperties.GameFeatureData);

		if (UE::GameFeatures::ShouldDeferLocalizationDataLoad())
		{
			UGameFeaturePluginStateMachine* CurrentMachine = UGameFeaturesSubsystem::Get().FindGameFeaturePluginStateMachine(StateProperties.PluginIdentifier);
			UE::GameFeatures::MountLocalizationData(CurrentMachine, StateProperties);
		}

		BundleHandle = LoadGameFeatureBundles(StateProperties.GameFeatureData);
	}

	virtual void EndState() override
	{
		BundleHandle = nullptr;
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Loading_Update);
		check(StateProperties.GameFeatureData);

		if (BundleHandle)
		{
			if (!UseAsyncLoading())
			{
				BundleHandle->WaitUntilComplete(0.0f, false);
			}

			if (BundleHandle->IsLoadingInProgress())
			{
				return;
			}

			if (BundleHandle->WasCanceled())
			{
				BundleHandle.Reset();
				StateStatus.SetTransitionError(EGameFeaturePluginState::ErrorLoading, GetErrorResult(TEXT("Load_Cancelled_Preload")));
				return;
			}
		}

		UGameFeaturesSubsystem::Get().OnGameFeatureLoading(StateProperties.GameFeatureData, StateProperties.PluginIdentifier);

		StateStatus.SetTransition(EGameFeaturePluginState::Loaded);
	}

	/** Loads primary assets and bundles for the specified game feature */
	TSharedPtr<FStreamableHandle> LoadGameFeatureBundles(const UGameFeatureData* GameFeatureToLoad)
	{
		check(GameFeatureToLoad);

		const UGameFeaturesProjectPolicies& Policy = UGameFeaturesSubsystem::Get().GetPolicy<UGameFeaturesProjectPolicies>();

		TArray<FPrimaryAssetId> AssetIdsToLoad = Policy.GetPreloadAssetListForGameFeature(GameFeatureToLoad);

		FPrimaryAssetId GameFeatureAssetId = GameFeatureToLoad->GetPrimaryAssetId();
		if (GameFeatureAssetId.IsValid())
		{
			AssetIdsToLoad.Add(GameFeatureAssetId);
		}

		TSharedPtr<FStreamableHandle> RetHandle;
		if (AssetIdsToLoad.Num() > 0)
		{
			FAssetManagerLoadParams LoadParams;
			LoadParams.OnCancel = FStreamableDelegateWithHandle::CreateLambda([this](const TSharedPtr<FStreamableHandle>&)
			{
				const FStringView ShortUrl = StateProperties.PluginIdentifier.GetIdentifyingString();
				UE_LOG(LogGameFeatures, Error, TEXT("Game Feature preloading was cancelled for URL %.*s"), ShortUrl.Len(), ShortUrl.GetData());
				UpdateStateMachineDeferred();
			});

			// This can't be bound to the handle after its created, AM may bind it internally
			LoadParams.OnComplete = FStreamableDelegateWithHandle::CreateLambda([this](const TSharedPtr<FStreamableHandle>&)
			{
				UpdateStateMachineDeferred();
			});

			RetHandle = UAssetManager::Get().LoadPrimaryAssets(AssetIdsToLoad, Policy.GetPreloadBundleStateForGameFeature(), MoveTemp(LoadParams));
		}

		return RetHandle;
	}
};

struct FGameFeaturePluginState_Loaded : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Loaded(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination > EGameFeaturePluginState::Loaded)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::ActivatingDependencies);
		}
		else if (StateProperties.Destination < EGameFeaturePluginState::Loaded)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::Unloading);
		}
	}
};

struct FGameFeaturePluginState_ErrorDeactivatingDependencies : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorDeactivatingDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorDeactivatingDependencies)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::DeactivatingDependencies);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorDeactivatingDependencies)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::DeactivatingDependencies);
		}
	}
};

struct FDeactivatingDependenciesTransitionPolicy
{
	static bool GetPluginDependencyStateMachines(const FGameFeaturePluginStateMachineProperties& InStateProperties, TArray<UGameFeaturePluginStateMachine*>& OutDependencyMachines)
	{
		UGameFeaturesSubsystem& GameFeaturesSubsystem = UGameFeaturesSubsystem::Get();

		return GameFeaturesSubsystem.FindPluginDependencyStateMachinesToDeactivate(
			InStateProperties.PluginIdentifier.GetFullPluginURL(), InStateProperties.PluginInstalledFilename, OutDependencyMachines);
	}

	static FGameFeaturePluginStateRange GetDependencyStateRange()
	{
		return FGameFeaturePluginStateRange(EGameFeaturePluginState::Terminal, EGameFeaturePluginState::Loaded);
	}

	static EGameFeaturePluginState GetTransitionState()
	{
		return EGameFeaturePluginState::Deactivating;
	}

	static EGameFeaturePluginState GetErrorState()
	{
		return EGameFeaturePluginState::ErrorDeactivatingDependencies;
	}

	static bool ExcludeDepedenciesFromBatchProcessing()
	{
		return false;
	}

	static bool ShouldWaitForDependencies()
	{
		return UE::GameFeatures::CVarWaitForDependencyDeactivation.GetValueOnGameThread();
	}
};

struct FGameFeaturePluginState_DeactivatingDependencies : public FTransitionDependenciesGameFeaturePluginState<FDeactivatingDependenciesTransitionPolicy>
{
	FGameFeaturePluginState_DeactivatingDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FTransitionDependenciesGameFeaturePluginState(InStateProperties)
	{
	}
};

struct FGameFeaturePluginState_Deactivating : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Deactivating(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	int32 NumObservedPausers = 0;
	int32 NumExpectedPausers = 0;
	bool bInProcessOfDeactivating = false;
	bool bHasUnloaded = false;

	virtual void BeginState() override
	{
		NumObservedPausers = 0;
		NumExpectedPausers = 0;
		bInProcessOfDeactivating = false;
		bHasUnloaded = false;

		static bool bUseNewDynamicLayers = IConsoleManager::Get().FindConsoleVariable(TEXT("ini.UseNewDynamicLayers"))->GetInt() != 0;
		if (bUseNewDynamicLayers)
		{
			FName Tag = *StateProperties.PluginName;
			UE::DynamicConfig::PerformDynamicConfig(Tag, [Tag](FConfigModificationTracker* ChangeTracker)
			{
				FConfigCacheIni::RemoveTagFromAllBranches(Tag, ChangeTracker);
				IConsoleManager::Get().UnsetAllConsoleVariablesWithTag(Tag);
			});
		}
	}

	void OnPauserCompleted(FStringView InPauserTag)
	{
		check(IsInGameThread());
		ensure(NumExpectedPausers != INDEX_NONE);
		++NumObservedPausers;

		UE_LOG(LogGameFeatures, Display, TEXT("Deactivation of %s resumed by %.*s"), *StateProperties.PluginName, InPauserTag.Len(), InPauserTag.GetData());

		if (NumObservedPausers == NumExpectedPausers)
		{
			UpdateStateMachineImmediate();
		}
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (bHasUnloaded)
		{
			check(NumExpectedPausers == NumObservedPausers);
			StateStatus.SetTransition(EGameFeaturePluginState::Loaded);
			return;
		}

		if (!bInProcessOfDeactivating)
		{
			// Make sure we won't complete the transition prematurely if someone registers as a pauser but fires immediately
			bInProcessOfDeactivating = true;
			NumExpectedPausers = INDEX_NONE;
			NumObservedPausers = 0;

			// Deactivate
			FGameFeatureDeactivatingContext Context(StateProperties.PluginName, [this](FStringView InPauserTag) { OnPauserCompleted(InPauserTag); });
			UGameFeaturesSubsystem::Get().OnGameFeatureDeactivating(StateProperties.GameFeatureData, StateProperties.PluginName, Context, StateProperties.PluginIdentifier);
			NumExpectedPausers = Context.NumPausers;

			// Since we are pausing work during this deactivation, also notify the OnGameFeaturePauseChange delegate
			if (NumExpectedPausers > 0)
			{
				FGameFeaturePauseStateChangeContext PauseContext(UE::GameFeatures::ToString(EGameFeaturePluginState::Deactivating), TEXT("PendingDeactivationCallbacks"), true);
				UGameFeaturesSubsystem::Get().OnGameFeaturePauseChange(StateProperties.PluginIdentifier, StateProperties.PluginName, PauseContext);
			}
		}

		if (NumExpectedPausers == NumObservedPausers)
		{
			//If we previously sent an OnGameFeaturePauseChange delegate we need to send that work is now unpaused
			if (NumExpectedPausers > 0)
			{
				FGameFeaturePauseStateChangeContext PauseContext(UE::GameFeatures::ToString(EGameFeaturePluginState::Deactivating), TEXT(""), false);
				UGameFeaturesSubsystem::Get().OnGameFeaturePauseChange(StateProperties.PluginIdentifier, StateProperties.PluginName, PauseContext);
			}

			if (!bHasUnloaded && StateProperties.Destination.MaxState == EGameFeaturePluginState::Loaded)
			{
				// If we aren't going farther than Loaded, GC now
				// otherwise we will defer until closer to our destination state
				bHasUnloaded = true;
				UpdateStateMachineDeferred();
			}
			else
			{
				StateStatus.SetTransition(EGameFeaturePluginState::Loaded);
			}
		}
		else
		{
			UE_LOG(LogGameFeatures, Log, TEXT("Game feature %s deactivation paused until %d observer tasks complete their deactivation"), *GetPathNameSafe(StateProperties.GameFeatureData), NumExpectedPausers - NumObservedPausers);
		}
	}
};

struct FGameFeaturePluginState_ErrorActivatingDependencies : public FErrorGameFeaturePluginState
{
	FGameFeaturePluginState_ErrorActivatingDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties) : FErrorGameFeaturePluginState(InStateProperties) {}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::ErrorActivatingDependencies)
		{
			// There is no cleaup state equivalent to EGameFeaturePluginState::ErrorActivatingDependencies so just go back to Unloading
			StateStatus.SetTransition(EGameFeaturePluginState::Unloading);
		}
		else if (StateProperties.Destination > EGameFeaturePluginState::ErrorActivatingDependencies)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::ActivatingDependencies);
		}
	}
};

struct FActivatingDependenciesTransitionPolicy
{
	static bool GetPluginDependencyStateMachines(const FGameFeaturePluginStateMachineProperties& InStateProperties, TArray<UGameFeaturePluginStateMachine*>& OutDependencyMachines)
	{
		UGameFeaturesSubsystem& GameFeaturesSubsystem = UGameFeaturesSubsystem::Get();

		return GameFeaturesSubsystem.FindPluginDependencyStateMachinesToActivate(
			InStateProperties.PluginIdentifier.GetFullPluginURL(), InStateProperties.PluginInstalledFilename, OutDependencyMachines);
	}

	static FGameFeaturePluginStateRange GetDependencyStateRange()
	{
		return FGameFeaturePluginStateRange(EGameFeaturePluginState::Active, EGameFeaturePluginState::Active);
	}

	static EGameFeaturePluginState GetTransitionState()
	{
		return EGameFeaturePluginState::Activating;
	}

	static EGameFeaturePluginState GetErrorState()
	{
		return EGameFeaturePluginState::ErrorActivatingDependencies;
	}

	static bool ExcludeDepedenciesFromBatchProcessing()
	{
		return true;
	}

	static bool ShouldWaitForDependencies()
	{
		return true;
	}
};

struct FGameFeaturePluginState_ActivatingDependencies : public FTransitionDependenciesGameFeaturePluginState<FActivatingDependenciesTransitionPolicy>
{
	FGameFeaturePluginState_ActivatingDependencies(FGameFeaturePluginStateMachineProperties& InStateProperties)
		: FTransitionDependenciesGameFeaturePluginState(InStateProperties)
	{
	}
};

struct FGameFeaturePluginState_Activating : public FGameFeaturePluginState
{
	FGameFeaturePluginState_Activating(FGameFeaturePluginStateMachineProperties& InStateProperties) : FGameFeaturePluginState(InStateProperties) {}

	bool CanBatchProcess() const override
	{
		return FGameFeaturePluginState::CanBatchProcess() && AllowIniLoading();
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Activating);
		check(GEngine);
		check(StateProperties.GameFeatureData);

		// If this plugin caused localization data to load, we need that load to finish before marking it as active
		if (StateProperties.bIsLoadingLocalizationData)
		{
			if (AllowAsyncLoading())
			{
				return;
			}

			FTextLocalizationManager::Get().WaitForAsyncTasks();
			StateProperties.bIsLoadingLocalizationData = false;
		}

		if (IsWaitingForBatchProcessing())
		{
			return;
		}

		if(!WasBatchProcessed())
		{
			if (AllowIniLoading())
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Activating_InitIni);
				StateProperties.GameFeatureData->InitializeHierarchicalPluginIniFiles(StateProperties.PluginInstalledFilename);
			}
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Activating_SendEvents);
			FGameFeatureActivatingContext Context;
			UGameFeaturesSubsystem::Get().OnGameFeatureActivating(StateProperties.GameFeatureData, StateProperties.PluginName, Context, StateProperties.PluginIdentifier);
		}

		StateStatus.SetTransition(EGameFeaturePluginState::Active);
	}

	static void BatchProcess(TConstArrayView<UGameFeaturePluginStateMachine*> GFPs)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_BatchProcess_OnFenceCompleteActivating);
		UGameFeaturesSubsystem& GFPSubSys = UGameFeaturesSubsystem::Get();

		TArray<FString> PluginInstalledFilenames;
		PluginInstalledFilenames.Reserve(GFPs.Num());

		for (const UGameFeaturePluginStateMachine* GFPSM : GFPs)
		{
			PluginInstalledFilenames.Emplace(GFPSM->GetProperties().PluginInstalledFilename);
		}

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GFP_BatchActivating_InitIni);
			UGameFeatureData::InitializeHierarchicalPluginIniFiles(PluginInstalledFilenames);
		}
	}
};

struct FGameFeaturePluginState_Active : public FDestinationGameFeaturePluginState
{
	FGameFeaturePluginState_Active(FGameFeaturePluginStateMachineProperties& InStateProperties) : FDestinationGameFeaturePluginState(InStateProperties) {}

	virtual void BeginState() override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Active);
		check(GEngine);

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GFP_Active_SendEvents);
			UGameFeaturesSubsystem::Get().OnGameFeatureActivated(StateProperties.GameFeatureData, StateProperties.PluginName, StateProperties.PluginIdentifier);
		}
	}

	virtual void UpdateState(FGameFeaturePluginStateStatus& StateStatus) override
	{
		if (StateProperties.Destination < EGameFeaturePluginState::Active)
		{
			StateStatus.SetTransition(EGameFeaturePluginState::DeactivatingDependencies);
		}
	}
};

/*
=========================================================
  State Machine
=========================================================
*/
namespace UE::GameFeatures
{
	/** Helper classes that look for existence of batch processing functions on states so we can identify these compile time */
	template<class T, class = void>
	struct TBatchProcessHelperWrapper
	{
		static bool ImplementsBatchProcess()
		{
			return false;
		}
		static void BatchProcess(TConstArrayView<UGameFeaturePluginStateMachine*>)
		{
			check(!"Not implemented");
		}
	};

	template<class T>
	struct TBatchProcessHelperWrapper<T, std::enable_if_t<std::is_invocable_r<void, decltype(T::BatchProcess), TConstArrayView<UGameFeaturePluginStateMachine*>>::value>>
	{
		static bool ImplementsBatchProcess()
		{
			return true;
		}

		static void BatchProcess(TConstArrayView<UGameFeaturePluginStateMachine*> GFPSMs)
		{
			T::BatchProcess(GFPSMs);
		}
	};

	struct FBatchProcessHelperFunctors
	{
		using FImplementsBatchProcessPtr = bool(*)();
		using FBatchProcessPtr = void (*)(TConstArrayView<UGameFeaturePluginStateMachine*>);

		FBatchProcessHelperFunctors() = default;

		FBatchProcessHelperFunctors(FImplementsBatchProcessPtr InImplementsBatchProcessPtr, FBatchProcessPtr InBatchProcessPtr)
			: ImplementsBatchProcess(InImplementsBatchProcessPtr)
			, BatchProcess(InBatchProcessPtr)
		{

		}
		
		FImplementsBatchProcessPtr ImplementsBatchProcess = nullptr;
		FBatchProcessPtr BatchProcess = nullptr;
	};

	#define GAME_FEATURE_PLUGIN_STATE_MAKE_BATCH_PROCESS_FN(inEnum, inText) FBatchProcessHelperFunctors(&TBatchProcessHelperWrapper<FGameFeaturePluginState_##inEnum>::ImplementsBatchProcess, &TBatchProcessHelperWrapper<FGameFeaturePluginState_##inEnum>::BatchProcess), 
	static TStaticArray<FBatchProcessHelperFunctors, EGameFeaturePluginState::MAX> BatchProcessingHelperFunctors = {
		GAME_FEATURE_PLUGIN_STATE_LIST(GAME_FEATURE_PLUGIN_STATE_MAKE_BATCH_PROCESS_FN)
	};
	#undef GAME_FEATURE_PLUGIN_STATE_MAKE_BATCH_PROCESS_FN
}

UGameFeaturePluginStateMachine::UGameFeaturePluginStateMachine(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, CurrentStateInfo(EGameFeaturePluginState::Uninitialized)
	, bInUpdateStateMachine(false)
	, bRegisteredAsTransitioningGFPSM(false)
{

}

void UGameFeaturePluginStateMachine::InitStateMachine(FGameFeaturePluginIdentifier InPluginIdentifier, const FGameFeatureProtocolOptions& InProtocolOptions)
{
	check(GetCurrentState() == EGameFeaturePluginState::Uninitialized);
	CurrentStateInfo.State = EGameFeaturePluginState::UnknownStatus;
	StateProperties = FGameFeaturePluginStateMachineProperties(
		MoveTemp(InPluginIdentifier),
		FGameFeaturePluginStateRange(CurrentStateInfo.State),
		FGameFeaturePluginRequestUpdateStateMachine::CreateUObject(this, &ThisClass::UpdateStateMachine),
		FGameFeatureStateProgressUpdate::CreateUObject(this, &ThisClass::UpdateCurrentStateProgress));

	StateProperties.ProtocolOptions = InProtocolOptions;

#define GAME_FEATURE_PLUGIN_STATE_MAKE_STATE(inEnum, inText) AllStates[EGameFeaturePluginState::inEnum] = MakeUnique<FGameFeaturePluginState_##inEnum>(StateProperties);
	GAME_FEATURE_PLUGIN_STATE_LIST(GAME_FEATURE_PLUGIN_STATE_MAKE_STATE)
#undef GAME_FEATURE_PLUGIN_STATE_MAKE_STATE

	CheckAddBatchingRequestForCurrentState();
	AllStates[CurrentStateInfo.State]->BeginState();
}

bool UGameFeaturePluginStateMachine::SetDestination(FGameFeaturePluginStateRange InDestination, FGameFeatureStateTransitionComplete OnFeatureStateTransitionComplete, FDelegateHandle* OutCallbackHandle /*= nullptr*/)
{
	check(IsValidDestinationState(InDestination.MinState));
	check(IsValidDestinationState(InDestination.MaxState));

	bool bDestinationSet = false;
	bool bDestinationChanged = false;

	if (!InDestination.IsValid())
	{
		// Invalid range
	}
	else if (CurrentStateInfo.State == EGameFeaturePluginState::Terminal && !InDestination.Contains(EGameFeaturePluginState::Terminal))
	{
		// Can't tranistion away from terminal state
	}
	else if (!IsRunning())
	{
		// Not running so any new range is acceptable

		if (OutCallbackHandle)
		{
			OutCallbackHandle->Reset();
		}

		FDestinationGameFeaturePluginState* CurrState = AllStates[CurrentStateInfo.State]->AsDestinationState();

		if (InDestination.Contains(CurrentStateInfo.State))
		{
			OnFeatureStateTransitionComplete.ExecuteIfBound(this, MakeValue());
		}
		else
		{
			if (CurrentStateInfo.State < InDestination)
			{
				FDestinationGameFeaturePluginState* MinDestState = AllStates[InDestination.MinState]->AsDestinationState();
				FDelegateHandle CallbackHandle = MinDestState->OnDestinationStateReached.Add(MoveTemp(OnFeatureStateTransitionComplete));
				if (OutCallbackHandle)
				{
					*OutCallbackHandle = CallbackHandle;
				}
			}
			else if (CurrentStateInfo.State > InDestination)
			{
				FDestinationGameFeaturePluginState* MaxDestState = AllStates[InDestination.MaxState]->AsDestinationState();
				FDelegateHandle CallbackHandle = MaxDestState->OnDestinationStateReached.Add(MoveTemp(OnFeatureStateTransitionComplete));
				if (OutCallbackHandle)
				{
					*OutCallbackHandle = CallbackHandle;
				}
			}

			StateProperties.Destination = InDestination;
			UpdateStateMachine();
			bDestinationChanged = true;
		}

		bDestinationSet = true;
	}
	else if (TOptional<FGameFeaturePluginStateRange> NewDestination = StateProperties.Destination.Intersect(InDestination))
	{
		// The machine is already running so we can only transition to this range if it overlaps with our current range.
		// We can satisfy both ranges in this case.

		if (OutCallbackHandle)
		{
			OutCallbackHandle->Reset();
		}

		if (CurrentStateInfo.State < StateProperties.Destination)
		{
			StateProperties.Destination = *NewDestination;

			if (InDestination.Contains(CurrentStateInfo.State))
			{
				OnFeatureStateTransitionComplete.ExecuteIfBound(this, MakeValue());
			}
			else
			{
				FDestinationGameFeaturePluginState* MinDestState = AllStates[InDestination.MinState]->AsDestinationState();
				FDelegateHandle CallbackHandle = MinDestState->OnDestinationStateReached.Add(MoveTemp(OnFeatureStateTransitionComplete));
				if (OutCallbackHandle)
				{
					*OutCallbackHandle = CallbackHandle;
				}
				bDestinationChanged = true;
			}
		}
		else if(CurrentStateInfo.State > StateProperties.Destination)
		{
			StateProperties.Destination = *NewDestination;

			if (InDestination.Contains(CurrentStateInfo.State))
			{
				OnFeatureStateTransitionComplete.ExecuteIfBound(this, MakeValue());
			}
			else
			{
				FDestinationGameFeaturePluginState* MaxDestState = AllStates[InDestination.MaxState]->AsDestinationState();
				FDelegateHandle CallbackHandle = MaxDestState->OnDestinationStateReached.Add(MoveTemp(OnFeatureStateTransitionComplete));
				if (OutCallbackHandle)
				{
					*OutCallbackHandle = CallbackHandle;
				}
				bDestinationChanged = true;
			}
		}
		else
		{
			checkf(false, TEXT("IsRunning() returned true but state machine has reached destination!"));
		}

		bDestinationSet = true;
	}
	else
	{
		// The requested state range is completely outside the the current state range so reject the request
	}

#if !UE_BUILD_SHIPPING
	if (bDestinationChanged && UGameFeaturesSubsystem::Get().GetPluginDebugStateEnabled(GetPluginURL()))
	{
		PLATFORM_BREAK();
	}
#endif

	return bDestinationSet;
}

bool UGameFeaturePluginStateMachine::TryCancel(FGameFeatureStateTransitionCanceled OnFeatureStateTransitionCanceled, FDelegateHandle* OutCallbackHandle /*= nullptr*/)
{
	if (!IsRunning())
	{
		return false;
	}

	StateProperties.bTryCancel = true;
	FDelegateHandle CallbackHandle = StateProperties.OnTransitionCanceled.Add(MoveTemp(OnFeatureStateTransitionCanceled));
	if(OutCallbackHandle)
	{
		*OutCallbackHandle = CallbackHandle;
	}

	const EGameFeaturePluginState CurrentState = GetCurrentState();
	AllStates[CurrentState]->TryCancelState();

	return true;
}

UE::GameFeatures::FResult UGameFeaturePluginStateMachine::TryUpdatePluginProtocolOptions(const FGameFeatureProtocolOptions& InOptions, bool& bOutDidUpdate)
{
	bOutDidUpdate = false;

	if (StateProperties.ProtocolOptions == InOptions)
	{
		return MakeValue();
	}

	const EGameFeaturePluginState CurrentState = GetCurrentState();
	UE::GameFeatures::FResult Result = AllStates[CurrentState]->TryUpdateProtocolOptions(InOptions);
	bOutDidUpdate = Result.HasValue();

	return Result;
}

void UGameFeaturePluginStateMachine::RemovePendingTransitionCallback(FDelegateHandle InHandle)
{
	for (std::underlying_type<EGameFeaturePluginState>::type iState = 0;
		iState < EGameFeaturePluginState::MAX;
		++iState)
	{
		if (FDestinationGameFeaturePluginState* DestState = AllStates[iState]->AsDestinationState())
		{
			if (DestState->OnDestinationStateReached.Remove(InHandle))
			{
				break;
			}
		}
	}
}

void UGameFeaturePluginStateMachine::RemovePendingTransitionCallback(FDelegateUserObject DelegateObject)
{
	for (std::underlying_type<EGameFeaturePluginState>::type iState = 0;
		iState < EGameFeaturePluginState::MAX;
		++iState)
	{
		if (FDestinationGameFeaturePluginState* DestState = AllStates[iState]->AsDestinationState())
		{
			if (DestState->OnDestinationStateReached.RemoveAll(DelegateObject))
			{
				break;
			}
		}
	}
}

void UGameFeaturePluginStateMachine::RemovePendingCancelCallback(FDelegateHandle InHandle)
{
	StateProperties.OnTransitionCanceled.Remove(InHandle);
}

void UGameFeaturePluginStateMachine::RemovePendingCancelCallback(FDelegateUserObject DelegateObject)
{
	StateProperties.OnTransitionCanceled.RemoveAll(DelegateObject);
}

const FString& UGameFeaturePluginStateMachine::GetGameFeatureName() const
{
	FString PluginFilename;
	if (!StateProperties.PluginName.IsEmpty())
	{
		return StateProperties.PluginName;
	}
	else
	{
		return StateProperties.PluginIdentifier.GetFullPluginURL();
	}
}

const FGameFeaturePluginIdentifier& UGameFeaturePluginStateMachine::GetPluginIdentifier() const
{
	return StateProperties.PluginIdentifier;
}

const FString& UGameFeaturePluginStateMachine::GetPluginURL() const
{
	return StateProperties.PluginIdentifier.GetFullPluginURL();
}

const FGameFeatureProtocolMetadata& UGameFeaturePluginStateMachine::GetProtocolMetadata() const
{
	return StateProperties.ProtocolMetadata;
}

const FGameFeatureProtocolOptions& UGameFeaturePluginStateMachine::GetProtocolOptions() const
{
	return StateProperties.ProtocolOptions;
}

FGameFeatureProtocolOptions UGameFeaturePluginStateMachine::RecycleProtocolOptions() const
{
	return StateProperties.RecycleProtocolOptions();
}

const FString& UGameFeaturePluginStateMachine::GetPluginName() const
{
	return StateProperties.PluginName;
}

bool UGameFeaturePluginStateMachine::GetPluginFilename(FString& OutPluginFilename) const
{
	OutPluginFilename = StateProperties.PluginInstalledFilename;
	return !OutPluginFilename.IsEmpty();
}

EGameFeaturePluginState UGameFeaturePluginStateMachine::GetCurrentState() const
{
	return GetCurrentStateInfo().State;
}

FGameFeaturePluginStateRange UGameFeaturePluginStateMachine::GetDestination() const
{
	return StateProperties.Destination;
}

const FGameFeaturePluginStateInfo& UGameFeaturePluginStateMachine::GetCurrentStateInfo() const
{
	return CurrentStateInfo;
}

bool UGameFeaturePluginStateMachine::IsRunning() const
{
	return !StateProperties.Destination.Contains(CurrentStateInfo.State);
}

bool UGameFeaturePluginStateMachine::IsStatusKnown() const
{
	return GetCurrentState() == EGameFeaturePluginState::ErrorUnavailable || 
		   GetCurrentState() == EGameFeaturePluginState::Uninstalling || 
		   GetCurrentState() == EGameFeaturePluginState::ErrorUninstalling ||
		   GetCurrentState() >= EGameFeaturePluginState::StatusKnown;
}

bool UGameFeaturePluginStateMachine::IsAvailable() const
{
	ensure(IsStatusKnown());
	return GetCurrentState() >= EGameFeaturePluginState::StatusKnown;
}

bool UGameFeaturePluginStateMachine::AllowAsyncLoading() const
{
	return StateProperties.AllowAsyncLoading();
}

bool UGameFeaturePluginStateMachine::HasAssetStreamingDependencies() const
{
	ensure(IsStatusKnown());
	if (StateProperties.ProtocolMetadata.HasSubtype<FInstallBundlePluginProtocolMetaData>())
	{
		const FInstallBundlePluginProtocolMetaData& ProtocolData = StateProperties.ProtocolMetadata.GetSubtype<FInstallBundlePluginProtocolMetaData>();
		return !ProtocolData.InstallBundlesWithAssetDependencies.IsEmpty();
	}
	return false;
}

void UGameFeaturePluginStateMachine::SetWasLoadedAsBuiltIn()
{
	StateProperties.bWasLoadedAsBuiltInGameFeaturePlugin = true;
}

bool UGameFeaturePluginStateMachine::WasLoadedAsBuiltIn() const
{
	return StateProperties.bWasLoadedAsBuiltInGameFeaturePlugin;
}

UGameFeatureData* UGameFeaturePluginStateMachine::GetGameFeatureDataForActivePlugin()
{
	if (GetCurrentState() == EGameFeaturePluginState::Active)
	{
		return StateProperties.GameFeatureData;
	}

	return nullptr;
}

UGameFeatureData* UGameFeaturePluginStateMachine::GetGameFeatureDataForRegisteredPlugin(bool bCheckForRegistering /*= false*/)
{
	const EGameFeaturePluginState CurrentState = GetCurrentState();

	if (CurrentState >= EGameFeaturePluginState::Registered || (bCheckForRegistering && (CurrentState == EGameFeaturePluginState::Registering)))
	{
		return StateProperties.GameFeatureData;
	}

	return nullptr;
}

const FGameFeaturePluginStateMachineProperties& UGameFeaturePluginStateMachine::GetProperties() const
{
	return StateProperties;
}

bool UGameFeaturePluginStateMachine::IsErrorStateUnrecoverable() const
{
	return bIsInUnrecoverableError;
}

void UGameFeaturePluginStateMachine::SetUnrecoverableError()
{
	bIsInUnrecoverableError = true;
}

bool UGameFeaturePluginStateMachine::IsValidTransitionState(EGameFeaturePluginState InState) const
{
	check(InState != EGameFeaturePluginState::MAX);
	return AllStates[InState]->GetStateType() == EGameFeaturePluginStateType::Transition;
}

bool UGameFeaturePluginStateMachine::IsValidDestinationState(EGameFeaturePluginState InDestinationState) const
{
	check(InDestinationState != EGameFeaturePluginState::MAX);
	return AllStates[InDestinationState]->GetStateType() == EGameFeaturePluginStateType::Destination;
}

bool UGameFeaturePluginStateMachine::IsValidErrorState(EGameFeaturePluginState InDestinationState) const
{
	check(InDestinationState != EGameFeaturePluginState::MAX);
	return AllStates[InDestinationState]->GetStateType() == EGameFeaturePluginStateType::Error;
}

UE_TRACE_EVENT_BEGIN(Cpu, GFP_UpdateStateMachine, NoSync)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, PluginName)
UE_TRACE_EVENT_END()

void UGameFeaturePluginStateMachine::UpdateStateMachine()
{
	const EGameFeaturePluginState InitialState = GetCurrentState();
	EGameFeaturePluginState CurrentState = InitialState;
	if (bInUpdateStateMachine)
	{
		UE_LOG(LogGameFeatures, Verbose, TEXT("Game feature state machine skipping update for %s in ::UpdateStateMachine. Current State: %s"), *GetGameFeatureName(), *UE::GameFeatures::ToString(CurrentState));
		return;
	}

	UE_TRACE_LOG_SCOPED_T(Cpu, GFP_UpdateStateMachine, CpuChannel)
		<< GFP_UpdateStateMachine.PluginName(*GetGameFeatureName());

	TOptional<TGuardValue<bool>> ScopeGuard(InPlace, bInUpdateStateMachine, true);
	
	using StateIt = std::underlying_type<EGameFeaturePluginState>::type;

	auto DoCallbacks = [this](const UE::GameFeatures::FResult& Result, StateIt Begin, StateIt End)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_DoCallbacks);

		for (StateIt iState = Begin; iState < End; ++iState)
		{
			if (FDestinationGameFeaturePluginState* DestState = AllStates[iState]->AsDestinationState())
			{
				// Use a local callback on the stack. If SetDestination() is called from the callback then we don't want to stomp the callback
				// for the new state transition request.
				// Callback from terminal state could also trigger a GC that would destroy the state machine
				UE::GameFeatures::FBroadcastingOnDestinationStateReached LocalOnDestinationStateReached(MoveTemp(DestState->OnDestinationStateReached));
				DestState->OnDestinationStateReached.Clear();

				LocalOnDestinationStateReached.CallbackDelegate.Broadcast(this, Result);
			}
		}
	};

	auto DoCallback = [&DoCallbacks](const UE::GameFeatures::FResult& Result, StateIt InState)
	{
		DoCallbacks(Result, InState, InState + 1);
	};

	RegisterAsTransitioningStateMachine();

	bool bKeepProcessing = false;
	int32 NumTransitions = 0;
	const int32 MaxTransitions = 10000;
	do
	{
		bKeepProcessing = false;

		FGameFeaturePluginStateStatus StateStatus;
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_UpdateState);
			AllStates[CurrentState]->UpdateState(StateStatus);
		}

		if (StateStatus.TransitionToState == CurrentState)
		{
			UE_LOG(LogGameFeatures, Fatal, TEXT("Game feature state %s transitioning to itself. GameFeature: %s"), *UE::GameFeatures::ToString(CurrentState), *GetGameFeatureName());
		}

		if (StateStatus.TransitionToState != EGameFeaturePluginState::Uninitialized)
		{
			UE_LOG(LogGameFeatures, Verbose, TEXT("Game feature '%s' transitioning state (%s -> %s)"), *GetGameFeatureName(), *UE::GameFeatures::ToString(CurrentState), *UE::GameFeatures::ToString(StateStatus.TransitionToState));
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_EndState);
				AllStates[CurrentState]->EndState();
				CheckAndCancelBatchingRequestForCurrentState();
			}
			CurrentStateInfo = FGameFeaturePluginStateInfo(StateStatus.TransitionToState);
			CurrentState = StateStatus.TransitionToState;
			check(CurrentState != EGameFeaturePluginState::MAX);
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_BeginState);
				CheckAddBatchingRequestForCurrentState();
				AllStates[CurrentState]->BeginState();
			}

			if (CurrentState == EGameFeaturePluginState::Terminal)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_BeginTerm);

				// Remove from gamefeature subsystem before calling back in case this GFP is reloaded on callback,
				// but make sure we don't get destroyed from a GC during a callback
				UGameFeaturesSubsystem::Get().BeginTermination(this);
			}

			if (StateProperties.bTryCancel && AllStates[CurrentState]->GetStateType() != EGameFeaturePluginStateType::Transition)
			{
				StateProperties.Destination = FGameFeaturePluginStateRange(CurrentState);

				StateProperties.bTryCancel = false;
				bKeepProcessing = false;

				// Make sure bInUpdateStateMachine is not set while processing callbacks if we are at our destination
				ScopeGuard.Reset();

				// For all callbacks, return the CanceledResult
				DoCallbacks(UE::GameFeatures::CanceledResult, 0, EGameFeaturePluginState::MAX);

				// Must be called after transtition callbacks, UGameFeaturesSubsystem::ChangeGameFeatureTargetStateComplete may remove the this machine from the subsystem
				UE::GameFeatures::FBroadcastingOnTransitionCanceled LocalOnTransitionCanceled(MoveTemp(StateProperties.OnTransitionCanceled));
				StateProperties.OnTransitionCanceled.Clear();
				LocalOnTransitionCanceled.CallbackDelegate.Broadcast(this);
			}
			else if (const bool bError = !StateStatus.TransitionResult.HasValue(); bError)
			{
				check(IsValidErrorState(CurrentState));
				StateProperties.Destination = FGameFeaturePluginStateRange(CurrentState);

				bKeepProcessing = false;
				
				// Make sure bInUpdateStateMachine is not set while processing callbacks if we are at our destination
				ScopeGuard.Reset();

				// In case of an error, callback all possible callbacks
				DoCallbacks(StateStatus.TransitionResult, 0, EGameFeaturePluginState::MAX);
			}
			else
			{
				bKeepProcessing = AllStates[CurrentState]->GetStateType() == EGameFeaturePluginStateType::Transition || !StateProperties.Destination.Contains(CurrentState);
				if (!bKeepProcessing)
				{
					// Make sure bInUpdateStateMachine is not set while processing callbacks if we are at our destination
					ScopeGuard.Reset();
				}

				DoCallback(StateStatus.TransitionResult, CurrentState);
			}

			if (!bKeepProcessing)
			{
				UnregisterAsTransitioningStateMachine();
			}

			if (CurrentState == EGameFeaturePluginState::Terminal)
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(GFP_UpdateStateMachine_FinishTerm);

				UnregisterAsTransitioningStateMachine();

				check(bKeepProcessing == false);
				// Now that callbacks are done this machine can be cleaned up
				UGameFeaturesSubsystem::Get().FinishTermination(this);
				MarkAsGarbage();
			}
		}
		else if(!IsRunning())
		{
			UnregisterAsTransitioningStateMachine();
		}

		// Log our final state if we've finished transitioning
		if (!bKeepProcessing && InitialState != CurrentState)
		{
			if (!StateStatus.TransitionResult.HasValue())
			{
				constexpr static const TCHAR ErrLogFmt[] = TEXT("Game feature '%s' transition failed. Ending state: %s [%s, %s]. Result: %s");
				if (StateStatus.bSuppressErrorLog)
				{
					UE_LOG(LogGameFeatures, Display, ErrLogFmt,
						*GetGameFeatureName(),
						*UE::GameFeatures::ToString(CurrentState),
						*UE::GameFeatures::ToString(StateProperties.Destination.MinState),
						*UE::GameFeatures::ToString(StateProperties.Destination.MaxState),
						*UE::GameFeatures::ToString(StateStatus.TransitionResult));
				}
				else
				{
					UE_LOG(LogGameFeatures, Error, ErrLogFmt,
						*GetGameFeatureName(),
						*UE::GameFeatures::ToString(CurrentState),
						*UE::GameFeatures::ToString(StateProperties.Destination.MinState),
						*UE::GameFeatures::ToString(StateProperties.Destination.MaxState),
						*UE::GameFeatures::ToString(StateStatus.TransitionResult));
				}
			}
			else if (StateProperties.Destination.Contains(CurrentState))
			{
				UE_LOG(LogGameFeatures, Display, TEXT("Game feature '%s' transitioned successfully. Ending state: %s [%s, %s]"),
					*GetGameFeatureName(),
					*UE::GameFeatures::ToString(CurrentState),
					*UE::GameFeatures::ToString(StateProperties.Destination.MinState),
					*UE::GameFeatures::ToString(StateProperties.Destination.MaxState));
			}
		}

		if (NumTransitions++ > MaxTransitions)
		{
			UE_LOG(LogGameFeatures, Fatal, TEXT("Infinite loop in game feature state machine transitions. Current state %s. GameFeature: %s"), *UE::GameFeatures::ToString(CurrentState), *GetGameFeatureName());
		}
	} while (bKeepProcessing);
}

void UGameFeaturePluginStateMachine::UpdateCurrentStateProgress(float Progress)
{
	CurrentStateInfo.Progress = Progress;
}

void UGameFeaturePluginStateMachine::RegisterAsTransitioningStateMachine()
{
	if (bRegisteredAsTransitioningGFPSM)
	{
		return;
	}

	UGameFeaturesSubsystem::Get().RegisterRunningStateMachine(this);
	bRegisteredAsTransitioningGFPSM = true;
}

void UGameFeaturePluginStateMachine::UnregisterAsTransitioningStateMachine()
{
	if (!bRegisteredAsTransitioningGFPSM)
	{
		return;
	}

	UGameFeaturesSubsystem::Get().UnregisterRunningStateMachine(this);
	bRegisteredAsTransitioningGFPSM = false;
}

void UGameFeaturePluginStateMachine::CheckAddBatchingRequestForCurrentState()
{
	check(!StateProperties.BatchProcessingHandle.IsValid());

	const bool bCanBatchProcess =
		UE::GameFeatures::BatchProcessingHelperFunctors[CurrentStateInfo.State].ImplementsBatchProcess() &&
		StateProperties.CanBatchProcess() &&
		AllStates[CurrentStateInfo.State]->CanBatchProcess();
	
	if (bCanBatchProcess)
	{
		UE_LOG(LogGameFeatures, Verbose, TEXT("Game feature '%s' awaiting batch processing of state (%s)"), *GetGameFeatureName(), *UE::GameFeatures::ToString(CurrentStateInfo.State));
		StateProperties.BatchProcessingHandle = UGameFeaturesSubsystem::Get().AddBatchingRequest(
			CurrentStateInfo.State,
			StateProperties.OnRequestUpdateStateMachine);
	}
}

void UGameFeaturePluginStateMachine::CheckAndCancelBatchingRequestForCurrentState()
{
	// Reset batch processing state.
	StateProperties.bWasBatchProcessed = false;

	// If we are currently awaiting batch processing, cancel and update the state machine.
	if (StateProperties.IsWaitingForBatchProcessing())
	{
		UE_LOG(LogGameFeatures, Verbose, TEXT("Game feature '%s' cancelled batch processing of state (%s)"), *GetGameFeatureName(), *UE::GameFeatures::ToString(CurrentStateInfo.State));
		UGameFeaturesSubsystem::Get().CancelBatchingRequest(
			CurrentStateInfo.State,
			StateProperties.BatchProcessingHandle);

		StateProperties.BatchProcessingHandle.Reset();
		UpdateStateMachine();
	}
}

/* static */ void UGameFeaturePluginStateMachine::BatchProcess(EGameFeaturePluginState State, TConstArrayView<UGameFeaturePluginStateMachine*> GFPSMs)
{
	UE::GameFeatures::BatchProcessingHelperFunctors[State].BatchProcess(GFPSMs);
	for (UGameFeaturePluginStateMachine* GFPSM : GFPSMs)
	{
		GFPSM->StateProperties.BatchProcessingHandle.Reset();
		GFPSM->StateProperties.bWasBatchProcessed = true;
	}
}

void UGameFeaturePluginStateMachine::ExcludeFromBatchProcessing()
{
	// Ensure protocol options are updated to reflect the exclusion of this state machine from batch processing.
	if (StateProperties.ProtocolOptions.bBatchProcess)
	{
		UE_LOG(LogGameFeatures, Verbose, TEXT("Game feature '%s' excluded from batch processing"), *GetGameFeatureName());

		FGameFeatureProtocolOptions NewOptions = StateProperties.ProtocolOptions;
		NewOptions.bBatchProcess = false;

		bool bOutDidUpdate = false;
		TryUpdatePluginProtocolOptions(NewOptions, bOutDidUpdate);
		check(bOutDidUpdate);
		CheckAndCancelBatchingRequestForCurrentState();
	}	
}

FGameFeaturePluginStateMachineProperties::FGameFeaturePluginStateMachineProperties(
	FGameFeaturePluginIdentifier InPluginIdentifier,
	const FGameFeaturePluginStateRange& DesiredDestination,
	const FGameFeaturePluginRequestUpdateStateMachine& RequestUpdateStateMachineDelegate,
	const FGameFeatureStateProgressUpdate& FeatureStateProgressUpdateDelegate)
	: PluginIdentifier(MoveTemp(InPluginIdentifier))
	, Destination(DesiredDestination)
	, OnRequestUpdateStateMachine(RequestUpdateStateMachineDelegate)
	, OnFeatureStateProgressUpdate(FeatureStateProgressUpdateDelegate)
{
}

EGameFeaturePluginProtocol FGameFeaturePluginStateMachineProperties::GetPluginProtocol() const
{
	return PluginIdentifier.GetPluginProtocol();
}

FString FInstallBundlePluginProtocolMetaData::ToString() const
{
	FString ReturnedString;

	//Always encode InstallBundles
	ReturnedString = FString(UE::GameFeatures::PluginURLStructureInfo::OptionSeperator) +
		LexToString(EGameFeatureURLOptions::Bundles) + UE::GameFeatures::PluginURLStructureInfo::OptionAssignOperator;

	FNameBuilder NameBuilder;
	const FString BundlesList = FString::JoinBy(InstallBundles, UE::GameFeatures::PluginURLStructureInfo::OptionListSeperator,
	[&NameBuilder](const FName& BundleName)
	{
		BundleName.ToString(NameBuilder);
		return FStringView(NameBuilder);
	});
	ReturnedString.Append(BundlesList);

	// Only the generic version of CountBits is constexpr...
	static_assert(FGenericPlatformMath::CountBits(static_cast<uint64>(EGameFeatureURLOptions::All)) == 1, "Update this function to handle the newly added EGameFeatureInstallBundleProtocolOptions value!");

	return ReturnedString;
}

TValueOrError<FInstallBundlePluginProtocolMetaData, FString> FInstallBundlePluginProtocolMetaData::FromString(FStringView URLOptionsString)
{
	TArray<FName> InstallBundles;

	bool bParseSuccess = UGameFeaturesSubsystem::ParsePluginURLOptions(URLOptionsString, EGameFeatureURLOptions::Bundles,
	[&InstallBundles](EGameFeatureURLOptions Option, FStringView OptionName, FStringView OptionValue)
	{
		check(Option == EGameFeatureURLOptions::Bundles);
		InstallBundles.Emplace(OptionValue);
	});

	//We require to have InstallBundle names for this URL parse to be correct
	if (!bParseSuccess || InstallBundles.Num() == 0)
	{
		bParseSuccess = false;
		UE_LOG(LogGameFeatures, Error, TEXT("Error parsing InstallBundle protocol options URL %.*s"), URLOptionsString.Len(), URLOptionsString.GetData());
		return MakeError(TEXTVIEW("Bad_PluginURL"));
	}

	FInstallBundlePluginProtocolMetaData Ret;
	Ret.InstallBundles = MoveTemp(InstallBundles);

	return MakeValue<FInstallBundlePluginProtocolMetaData>(MoveTemp(Ret));
}

TValueOrError<void, FString> FGameFeaturePluginStateMachineProperties::ParseURL()
{
	const FStringView BadUrlError = TEXTVIEW("Bad_PluginURL");

	if (!ensureMsgf(!PluginIdentifier.IdentifyingURLSubset.IsEmpty(), TEXT("Unexpected empty IdentifyingURLSubset while parsing URL!")))
	{
		return MakeError(BadUrlError);
	}

	FStringView PluginPathFromURL;
	FStringView URLOptions;
	if (!UGameFeaturesSubsystem::ParsePluginURL(PluginIdentifier.GetFullPluginURL(), nullptr, &PluginPathFromURL, &URLOptions))
	{
		return MakeError(BadUrlError);
	}

	PluginInstalledFilename = PluginPathFromURL;
	PluginName = FPaths::GetBaseFilename(PluginInstalledFilename);

	if (PluginInstalledFilename.IsEmpty() || !PluginInstalledFilename.EndsWith(TEXT(".uplugin")))
	{
		ensureMsgf(false, TEXT("PluginInstalledFilename must have a uplugin extension. PluginInstalledFilename: %s"), *PluginInstalledFilename);
		return MakeError(BadUrlError);
	}

	//Do additional parsing of our Metadata from the options on our remaining URL
	if (GetPluginProtocol() == EGameFeaturePluginProtocol::InstallBundle)
	{
		TValueOrError<FInstallBundlePluginProtocolMetaData, FString> MaybeMetaData = FInstallBundlePluginProtocolMetaData::FromString(URLOptions);
		if (MaybeMetaData.HasError())
		{
			ensureMsgf(false, TEXT("Failure to parse URL %s into a valid FInstallBundlePluginProtocolMetaData"), *PluginIdentifier.GetFullPluginURL());
			return MakeError(MaybeMetaData.StealError());
		}

		FInstallBundlePluginProtocolMetaData& MetaData = *ProtocolMetadata.SetSubtype<FInstallBundlePluginProtocolMetaData>();
		MetaData = MaybeMetaData.StealValue();

		// Add default protocol options if they are not set yet
		if (!ProtocolOptions.HasSubtype<FInstallBundlePluginProtocolOptions>())
		{
			if (ProtocolOptions.HasSubtype<FNull>())
			{
				ProtocolOptions.SetSubtype<FInstallBundlePluginProtocolOptions>();
			}
			else
			{
				ensureMsgf(false, TEXT("Protocol options type is incorrect for URL %s"), *PluginIdentifier.GetFullPluginURL());
				return MakeError(BadUrlError);
			}
		}
	}
	else
	{
		// No protocol options for other (file) protocols right now
		if (!ProtocolOptions.HasSubtype<FNull>())
		{
			ensureMsgf(false, TEXT("Protocol options type is incorrect for URL %s"), *PluginIdentifier.GetFullPluginURL());
			return MakeError(BadUrlError);
		}
	}

	static_assert(static_cast<uint8>(EGameFeaturePluginProtocol::Count) == 3, "Update FGameFeaturePluginStateMachineProperties::ParseURL to handle any new Metadata parsing required for new EGameFeaturePluginProtocol. If no metadata is required just increment this counter.");

	return MakeValue();
}

UE::GameFeatures::FResult FGameFeaturePluginStateMachineProperties::ValidateProtocolOptionsUpdate(const FGameFeatureProtocolOptions& NewProtocolOptions) const
{
	if (GetPluginProtocol() == EGameFeaturePluginProtocol::InstallBundle)
	{
		const FStringView ShortUrl = PluginIdentifier.GetIdentifyingString();

		//Should never change our PluginProtocol
		if (!ensureAlwaysMsgf(NewProtocolOptions.HasSubtype<FInstallBundlePluginProtocolOptions>()
								,TEXT("Error with InstallBundle protocol FGameFeaturePluginStateMachineProperties having an invalid ProtocolOptions. URL: %.*s")
								,ShortUrl.Len(), ShortUrl.GetData()))
		{
			return MakeError(UE::GameFeatures::StateMachineErrorNamespace + TEXT("ProtocolOptions.Invalid_Protocol"));
		}

		if (ProtocolOptions.HasSubtype<FInstallBundlePluginProtocolOptions>())
		{
			const FInstallBundlePluginProtocolOptions& OldOptions = ProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();
			const FInstallBundlePluginProtocolOptions& NewOptions = NewProtocolOptions.GetSubtype<FInstallBundlePluginProtocolOptions>();

			if (!ensureMsgf(OldOptions.bAllowIniLoading == NewOptions.bAllowIniLoading, TEXT("Unexpected change to AllowIniLoading when updating ProtocolOptions. URL: %.*s "), ShortUrl.Len(), ShortUrl.GetData()))
			{
				return MakeError(UE::GameFeatures::StateMachineErrorNamespace + TEXT("ProtocolOptions.Invalid_Update"));
			}
		}

		return MakeValue();
	}

	if (NewProtocolOptions.HasSubtype<FNull>())
	{
		return MakeValue();
	}

	return MakeError(UE::GameFeatures::StateMachineErrorNamespace + TEXT("ProtocolOptions.Unknown_Protocol"));
}

FGameFeatureProtocolOptions FGameFeaturePluginStateMachineProperties::RecycleProtocolOptions() const
{
	FGameFeatureProtocolOptions Result = ProtocolOptions;
	if (Result.HasSubtype<FInstallBundlePluginProtocolOptions>())
	{
		// Don't allow unexpected uninstalls, otherwise respect any flags previously set by the game
		Result.GetSubtype<FInstallBundlePluginProtocolOptions>().bUninstallBeforeTerminate = false;
	}
	return Result;
}

bool FGameFeaturePluginStateMachineProperties::AllowAsyncLoading() const
{
	// Ticking is required for async loading
	// The local bForceSyncLoading should take precedence over UE::GameFeatures::CVarForceAsyncLoad
	return
		!ProtocolOptions.bForceSyncLoading &&
		(!IsRunningCommandlet() || UE::GameFeatures::CVarForceAsyncLoad.GetValueOnGameThread());
}

bool FGameFeaturePluginStateMachineProperties::CanBatchProcess() const
{
	return ProtocolOptions.bBatchProcess && UE::GameFeatures::CVarEnableBatchProcessing.GetValueOnGameThread();
}

bool FGameFeaturePluginStateMachineProperties::IsWaitingForBatchProcessing() const
{
	return BatchProcessingHandle.IsValid();
}

bool FGameFeaturePluginStateMachineProperties::WasBatchProcessed() const
{
	return bWasBatchProcessed;
}

#undef LOCTEXT_NAMESPACE 
