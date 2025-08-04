// Copyright Epic Games, Inc. All Rights Reserved.

#include "StateTree.h"
#include "Misc/PackageName.h"
#include "StateTreeLinker.h"
#include "StateTreeTaskBase.h"
#include "StateTreeEvaluatorBase.h"
#include "StateTreeConditionBase.h"
#include "StateTreeConsiderationBase.h"
#include "AssetRegistry/AssetData.h"
#include "Misc/ScopeRWLock.h"
#include "StateTreeDelegates.h"
#include "Logging/LogScopedVerbosityOverride.h"
#include "Misc/DataValidation.h"
#include "Misc/EnumerateRange.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "StateTreePropertyFunctionBase.h"
#include "AutoRTFM.h"

#include <atomic>

#if WITH_EDITOR
#include "Editor.h"
#include "StateTreeModuleImpl.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Templates/GuardValueAccessors.h"
#include "UObject/LinkerLoad.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(StateTree)

const FGuid FStateTreeCustomVersion::GUID(0x28E21331, 0x501F4723, 0x8110FA64, 0xEA10DA1E);
namespace UE::StateTree::Private
{
FCustomVersionRegistration GRegisterStateTreeCustomVersion(FStateTreeCustomVersion::GUID, FStateTreeCustomVersion::LatestVersion, TEXT("StateTreeAsset"));
}

bool UStateTree::IsReadyToRun() const
{
	// Valid tree must have at least one state and valid instance data.
	return States.Num() > 0 && bIsLinked && PropertyBindings.IsValid();
}

FConstStructView UStateTree::GetNode(const int32 NodeIndex) const
{
	return Nodes.IsValidIndex(NodeIndex) ? Nodes[NodeIndex] : FConstStructView();
}

FStateTreeIndex16 UStateTree::GetNodeIndexFromId(const FGuid Id) const
{
	const FStateTreeNodeIdToIndex* Entry = IDToNodeMappings.FindByPredicate([Id](const FStateTreeNodeIdToIndex& Entry){ return Entry.Id == Id; });
	return Entry != nullptr ? Entry->Index : FStateTreeIndex16::Invalid;
}

FGuid UStateTree::GetNodeIdFromIndex(const FStateTreeIndex16 NodeIndex) const
{
	const FStateTreeNodeIdToIndex* Entry = NodeIndex.IsValid()
		? IDToNodeMappings.FindByPredicate([NodeIndex](const FStateTreeNodeIdToIndex& Entry){ return Entry.Index == NodeIndex; })
		: nullptr;
	return Entry != nullptr ? Entry->Id : FGuid();
}

const FCompactStateTreeFrame* UStateTree::GetFrameFromHandle(const FStateTreeStateHandle StateHandle) const
{
	return Frames.FindByPredicate([StateHandle](const FCompactStateTreeFrame& Frame)
		{
			return Frame.RootState == StateHandle;
		});
}

const FCompactStateTreeState* UStateTree::GetStateFromHandle(const FStateTreeStateHandle StateHandle) const
{
	return States.IsValidIndex(StateHandle.Index) ? &States[StateHandle.Index] : nullptr;
}

FStateTreeStateHandle UStateTree::GetStateHandleFromId(const FGuid Id) const
{
	const FStateTreeStateIdToHandle* Entry = IDToStateMappings.FindByPredicate([Id](const FStateTreeStateIdToHandle& Entry){ return Entry.Id == Id; });
	return Entry != nullptr ? Entry->Handle : FStateTreeStateHandle::Invalid;
}

FGuid UStateTree::GetStateIdFromHandle(const FStateTreeStateHandle Handle) const
{
	const FStateTreeStateIdToHandle* Entry = IDToStateMappings.FindByPredicate([Handle](const FStateTreeStateIdToHandle& Entry){ return Entry.Handle == Handle; });
	return Entry != nullptr ? Entry->Id : FGuid();
}

const FCompactStateTransition* UStateTree::GetTransitionFromIndex(const FStateTreeIndex16 TransitionIndex) const
{
	return TransitionIndex.IsValid() && Transitions.IsValidIndex(TransitionIndex.Get()) ? &Transitions[TransitionIndex.Get()] : nullptr;
}

FStateTreeIndex16 UStateTree::GetTransitionIndexFromId(const FGuid Id) const
{
	const FStateTreeTransitionIdToIndex* Entry = IDToTransitionMappings.FindByPredicate([Id](const FStateTreeTransitionIdToIndex& Entry){ return Entry.Id == Id; });
	return Entry != nullptr ? Entry->Index : FStateTreeIndex16::Invalid;
}

FGuid UStateTree::GetTransitionIdFromIndex(const FStateTreeIndex16 Index) const
{
	const FStateTreeTransitionIdToIndex* Entry = IDToTransitionMappings.FindByPredicate([Index](const FStateTreeTransitionIdToIndex& Entry){ return Entry.Index == Index; });
	return Entry != nullptr ? Entry->Id : FGuid();
}

UE_AUTORTFM_ALWAYS_OPEN
static int32 GetThreadIndexForSharedInstanceData()
{
	// Create a unique index for each thread.
	static std::atomic_int ThreadIndexCounter {0};
	static thread_local int32 ThreadIndex = INDEX_NONE; // Cannot init directly on WinRT
	if (ThreadIndex == INDEX_NONE)
	{
		ThreadIndex = ThreadIndexCounter.fetch_add(1);
	}

	return ThreadIndex;
}

TSharedPtr<FStateTreeInstanceData> UStateTree::GetSharedInstanceData() const
{
	int32 ThreadIndex = GetThreadIndexForSharedInstanceData();

	// If shared instance data for this thread exists, return it.
	{
		UE::TReadScopeLock ReadLock(PerThreadSharedInstanceDataLock);
		if (ThreadIndex < PerThreadSharedInstanceData.Num())
		{
			return PerThreadSharedInstanceData[ThreadIndex];
		}
	}

	// Not initialized yet, create new instances up to the index.
	UE::TWriteScopeLock WriteLock(PerThreadSharedInstanceDataLock);

	// It is possible that multiple threads are waiting for the write lock,
	// which means that execution may get here so that 'ThreadIndex' is already in valid range.
	// The loop below is organized to handle that too.
	
	const int32 NewNum = ThreadIndex + 1;
	PerThreadSharedInstanceData.Reserve(NewNum);
	UStateTree* NonConstThis = const_cast<UStateTree*>(this); 
	
	for (int32 Index = PerThreadSharedInstanceData.Num(); Index < NewNum; Index++)
	{
		TSharedPtr<FStateTreeInstanceData> SharedData = MakeShared<FStateTreeInstanceData>();
		SharedData->CopyFrom(*NonConstThis, SharedInstanceData);
		PerThreadSharedInstanceData.Add(SharedData);
	}

	return PerThreadSharedInstanceData[ThreadIndex];
}

bool UStateTree::HasCompatibleContextData(const UStateTree& Other) const
{
	if (ContextDataDescs.Num() != Other.ContextDataDescs.Num())
	{
		return false;
	}

	const int32 Num = ContextDataDescs.Num();
	for (int32 Index = 0; Index < Num; Index++)
	{
		const FStateTreeExternalDataDesc& Desc = ContextDataDescs[Index];
		const FStateTreeExternalDataDesc& OtherDesc = Other.ContextDataDescs[Index];
		
		if (!OtherDesc.Struct 
			|| !OtherDesc.Struct->IsChildOf(Desc.Struct))
		{
			return false;
		}
	}
	
	return true;
}


#if WITH_EDITOR
namespace UE::StateTree::Compiler
{
	void RenameObjectToTransientPackage(UObject* ObjectToRename)
	{
		const ERenameFlags RenFlags = REN_DoNotDirty | REN_DontCreateRedirectors;

		ObjectToRename->SetFlags(RF_Transient);
		ObjectToRename->ClearFlags(RF_Public | RF_Standalone | RF_ArchetypeObject);

		// Rename will remove the renamed object's linker when moving to a new package so invalidate the export beforehand
		FLinkerLoad::InvalidateExport(ObjectToRename);
		ObjectToRename->Rename(nullptr, GetTransientPackage(), RenFlags);
	}
}

void UStateTree::ResetCompiled()
{
	Schema = nullptr;
	Frames.Reset();
	States.Reset();
	Transitions.Reset();
	Nodes.Reset();
	DefaultInstanceData.Reset();
	SharedInstanceData.Reset();
	ContextDataDescs.Reset();
	PropertyBindings.Reset();
	Parameters.Reset();
	ParameterDataType = EStateTreeParameterDataType::GlobalParameterData;
	IDToStateMappings.Reset();
	IDToNodeMappings.Reset();
	IDToTransitionMappings.Reset();
	
	EvaluatorsBegin = 0;
	EvaluatorsNum = 0;

	GlobalTasksBegin = 0;
	GlobalTasksNum = 0;
	bHasGlobalTransitionTasks = false;
	bHasGlobalTickTasks = false;
	bHasGlobalTickTasksOnlyOnEvents = false;
	bCachedRequestGlobalTick = false;
	bCachedRequestGlobalTickOnlyOnEvents = false;
	bScheduledTickAllowed = false;
	
	ResetLinked();

	// Remove objects created from last compilation.
	{
		TArray<UObject*, TInlineAllocator<32>> Children;
		const bool bIncludeNestedObjects = false;
		ForEachObjectWithOuter(this, [&Children, EditorData = EditorData.Get()](UObject* Child)
			{
				if (Child != EditorData)
				{
					Children.Add(Child);
				}
			}, bIncludeNestedObjects);

		for (UObject* Child : Children)
		{
			UE::StateTree::Compiler::RenameObjectToTransientPackage(Child);
		}
	}
}

void UStateTree::OnObjectsReinstanced(const FReplacementObjectMap& ObjectMap)
{
	if (ObjectMap.IsEmpty())
	{
		return;
	}

	bool bShouldRelink = false;

	// Relink if one of the out of date objects got reinstanced.
	if (OutOfDateStructs.Num() > 0)
	{
		for (FObjectKey OutOfDateObjectKey : OutOfDateStructs)
		{
			if (const UObject* OutOfDateObject = OutOfDateObjectKey.ResolveObjectPtr())
			{
				if (ObjectMap.Contains(OutOfDateObject))
				{
					bShouldRelink = true;
					break;
				}
			}
		}
	}

	// If the asset is not linked yet (or has failed), no need to link.
	if (!bShouldRelink && !bIsLinked)
	{
		return;
	}

	// Relink only if the reinstantiated object belongs to this asset,
	// or anything from the property binding refers to the classes of the reinstantiated object.
	if (!bShouldRelink)
	{
		for (TMap<UObject*, UObject*>::TConstIterator It(ObjectMap); It; ++It)
		{
			if (const UObject* ObjectToBeReplaced = It->Value)
			{
				if (ObjectToBeReplaced->IsInOuter(this))
				{
					bShouldRelink = true;
					break;
				}
			}
		}
	}

	if (!bShouldRelink)
	{
		TSet<const UStruct*> Structs;
		for (TMap<UObject*, UObject*>::TConstIterator It(ObjectMap); It; ++It)
		{
			if (const UObject* ObjectToBeReplaced = It->Value)
			{
				// It's a UClass or a UScriptStruct
				if (const UStruct* StructToReplaced = Cast<const UStruct>(ObjectToBeReplaced))
				{
					Structs.Add(StructToReplaced);
				}
				else
				{
					Structs.Add(ObjectToBeReplaced->GetClass());
				}
			}
		}

		bShouldRelink |= PropertyBindings.ContainsAnyStruct(Structs);
	}

	if (bShouldRelink)
	{
		if (!Link())
		{
			UE_LOG(LogStateTree, Error, TEXT("%s failed to link after Object reinstantiation. Take a look at the asset for any errors. Asset will not be usable at runtime."), *GetPathName());
		}
	}
}

void UStateTree::OnUserDefinedStructReinstanced(const UUserDefinedStruct& UserDefinedStruct)
{
	// Struct utils handle reinstancing the struct values (instanced struct, property bag, etc).
	// We will need to update the property binding.
	
	TSet<const UStruct*> Structs;
	Structs.Add(&UserDefinedStruct);
	
	if (PropertyBindings.ContainsAnyStruct(Structs))
	{
		if (!Link())
		{
			UE_LOG(LogStateTree, Error, TEXT("%s failed to link after Struct reinstantiation. Take a look at the asset for any errors. Asset will not be usable at runtime."), *GetPathName());
		}
	}
}

void UStateTree::PostInitProperties()
{
	Super::PostInitProperties();
	
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		OnObjectsReinstancedHandle = FStateTreeModule::OnObjectsReinstanced.AddUObject(this, &UStateTree::OnObjectsReinstanced);
		OnUserDefinedStructReinstancedHandle = FStateTreeModule::OnUserDefinedStructReinstanced.AddUObject(this, &UStateTree::OnUserDefinedStructReinstanced);
		OnPreBeginPIEHandle = FStateTreeModule::OnPreBeginPIE.AddUObject(this, &UStateTree::OnPreBeginPIE);
	}
}

void UStateTree::BeginDestroy()
{
	FStateTreeModule::OnObjectsReinstanced.Remove(OnObjectsReinstancedHandle);
	FStateTreeModule::OnUserDefinedStructReinstanced.Remove(OnUserDefinedStructReinstancedHandle);
	FStateTreeModule::OnPreBeginPIE.Remove(OnPreBeginPIEHandle);

	Super::BeginDestroy();
}

void UStateTree::PostDuplicate(EDuplicateMode::Type DuplicateMode)
{
	Super::PostDuplicate(DuplicateMode);
	ResetCompiled();
}

void UStateTree::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
	const FString SchemaClassName = Schema ? Schema->GetClass()->GetPathName() : TEXT("");
	Context.AddTag(FAssetRegistryTag(UE::StateTree::SchemaTag, SchemaClassName, FAssetRegistryTag::TT_Alphabetical));

	if (Schema)
	{
		Schema->GetAssetRegistryTags(Context);
	}

	Super::GetAssetRegistryTags(Context);
}

void UStateTree::ThreadedPostLoadAssetRegistryTagsOverride(FPostLoadAssetRegistryTagsContext& Context) const
{
	Super::ThreadedPostLoadAssetRegistryTagsOverride(Context);

	static const FName SchemaTag(TEXT("Schema"));
	const FString SchemaTagValue = Context.GetAssetData().GetTagValueRef<FString>(SchemaTag);
	if (!SchemaTagValue.IsEmpty() && FPackageName::IsShortPackageName(SchemaTagValue))
	{
		const FTopLevelAssetPath SchemaTagClassPathName = UClass::TryConvertShortTypeNameToPathName<UStruct>(SchemaTagValue, ELogVerbosity::Warning, TEXT("UStateTree::ThreadedPostLoadAssetRegistryTagsOverride"));
		if (!SchemaTagClassPathName.IsNull())
		{
			Context.AddTagToUpdate(FAssetRegistryTag(SchemaTag, SchemaTagClassPathName.ToString(), FAssetRegistryTag::TT_Alphabetical));
		}
	}
}

EDataValidationResult UStateTree::IsDataValid(FDataValidationContext& Context) const
{
	// Don't warn user that the tree they just saved is not compiled. Only for submit or manual validation
	if (Context.GetValidationUsecase() != EDataValidationUsecase::Save)
	{
		if (UE::StateTree::Delegates::OnRequestEditorHash.IsBound())
		{
			const uint32 CurrentHash = UE::StateTree::Delegates::OnRequestEditorHash.Execute(*this);
			if (CurrentHash != LastCompiledEditorDataHash)
			{
				Context.AddWarning(FText::FromString(FString::Printf(TEXT("%s is not compiled. Please recompile the State Tree."), *GetPathName())));
				return EDataValidationResult::Invalid;
			}
		}
	}

	if (!const_cast<UStateTree*>(this)->Link())
	{
		Context.AddError(FText::FromString(FString::Printf(TEXT("%s failed to link. Please recompile the State Tree for more details errors."), *GetPathName())));
		return EDataValidationResult::Invalid;
	}

	return Super::IsDataValid(Context);
}

#endif // WITH_EDITOR

void UStateTree::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	Super::AddReferencedObjects(InThis, Collector);

	const UStateTree* StateTree = Cast<UStateTree>(InThis);
	check(StateTree);
	
	UE::TReadScopeLock ReadLock(StateTree->PerThreadSharedInstanceDataLock);

	for (const TSharedPtr<FStateTreeInstanceData>& InstanceData : StateTree->PerThreadSharedInstanceData)
	{
		if (InstanceData.IsValid())
		{
			Collector.AddPropertyReferencesWithStructARO(FStateTreeInstanceData::StaticStruct(), InstanceData.Get(), StateTree);
		}
	}
}

void UStateTree::PostLoad()
{
	Super::PostLoad();

	for (int32 NodeIndex = 0; NodeIndex < Nodes.Num(); ++NodeIndex)
	{
		FStructView NodeView = Nodes[NodeIndex];
		if (FStateTreeNodeBase* Node = NodeView.GetPtr<FStateTreeNodeBase>())
		{
			if (Node->InstanceTemplateIndex.IsValid())
			{
				const bool bIsUsingSharedInstanceData = NodeView.GetScriptStruct()->IsChildOf<FStateTreeConditionBase>()
														|| NodeView.GetScriptStruct()->IsChildOf<FStateTreeConsiderationBase>()
														|| NodeView.GetScriptStruct()->IsChildOf<FStateTreePropertyFunctionBase>();
				FStateTreeInstanceData& SourceInstanceData = bIsUsingSharedInstanceData ? SharedInstanceData : DefaultInstanceData;
				if (SourceInstanceData.IsObject(Node->InstanceTemplateIndex.Get()))
				{
					Node->PostLoad(SourceInstanceData.GetMutableObject(Node->InstanceTemplateIndex.Get()));
				}
				else
				{
					Node->PostLoad(SourceInstanceData.GetMutableStruct(Node->InstanceTemplateIndex.Get()));
				}
			}
		}
	}

#if WITH_EDITOR
	if (EditorData)
	{
		// Make sure all the fix up logic in the editor data has had chance to happen.
		EditorData->ConditionalPostLoad();

		TGuardValueAccessors<bool> IsEditorLoadingPackageGuard(UE::GetIsEditorLoadingPackage, UE::SetIsEditorLoadingPackage, true);
		Compile();
	}
#else
	const int32 CurrentVersion = GetLinkerCustomVersion(FStateTreeCustomVersion::GUID);
	if (CurrentVersion < FStateTreeCustomVersion::LatestVersion)
	{		
		UE_LOG(LogStateTree, Error, TEXT("%s: compiled data is in older format. Please recompile the StateTree asset."), *GetPathName());
		return;
	}
#endif

	if (!Link())
	{
		UE_LOG(LogStateTree, Log, TEXT("%s failed to link. Asset will not be usable at runtime."), *GetPathName());
	}
}

#if WITH_EDITORONLY_DATA
void UStateTree::DeclareConstructClasses(TArray<FTopLevelAssetPath>& OutConstructClasses, const UClass* SpecificSubclass)
{
	Super::DeclareConstructClasses(OutConstructClasses, SpecificSubclass);
	TArray<UClass*> SchemaClasses;
	GetDerivedClasses(UStateTreeSchema::StaticClass(), SchemaClasses);
	for (UClass* SchemaClass : SchemaClasses)
	{
		if (!SchemaClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Transient))
		{
			OutConstructClasses.Add(FTopLevelAssetPath(SchemaClass));
		}
	}
}
#endif

void UStateTree::Serialize(const FStructuredArchiveRecord Record)
{
	Super::Serialize(Record);

	Record.GetUnderlyingArchive().UsingCustomVersion(FStateTreeCustomVersion::GUID);
	
	// We need to link and rebind property bindings each time a BP is compiled,
	// because property bindings may get invalid, and instance data potentially needs refreshed.
	if (Record.GetUnderlyingArchive().IsModifyingWeakAndStrongReferences())
	{
		if (!Link() && !HasAnyFlags(RF_ClassDefaultObject))
		{
			UE_LOG(LogStateTree, Log, TEXT("%s failed to link. Asset will not be usable at runtime."), *GetName());	
		}
	}
}

void UStateTree::ResetLinked()
{
	bIsLinked = false;
	ExternalDataDescs.Reset();

#if WITH_EDITOR
	OutOfDateStructs.Reset();
#endif

	UE::TWriteScopeLock WriteLock(PerThreadSharedInstanceDataLock);
	PerThreadSharedInstanceData.Reset();
}

bool UStateTree::ValidateInstanceData()
{
	bool bResult = true;
	for (FConstStructView NodeView : Nodes)
	{
		const FStateTreeNodeBase* Node = NodeView.GetPtr<const FStateTreeNodeBase>();
		if (Node && Node->InstanceTemplateIndex.IsValid())
		{
			const UStruct* CurrentInstanceDataType = nullptr;
			{
				const bool bUseSharedInstanceData = NodeView.GetPtr<const FStateTreeConditionBase>() || NodeView.GetPtr<const FStateTreeConsiderationBase>() || NodeView.GetPtr<const FStateTreePropertyFunctionBase>();
				const FStateTreeInstanceData& SourceInstanceData = bUseSharedInstanceData ? SharedInstanceData : DefaultInstanceData;
				if (SourceInstanceData.IsObject(Node->InstanceTemplateIndex.Get()))
				{
					const UObject* InstanceObject = SourceInstanceData.GetObject(Node->InstanceTemplateIndex.Get());
					CurrentInstanceDataType = InstanceObject ? InstanceObject->GetClass() : nullptr;
				}
				else
				{
					CurrentInstanceDataType = SourceInstanceData.GetStruct(Node->InstanceTemplateIndex.Get()).GetScriptStruct();
				}
			}

			if (CurrentInstanceDataType == nullptr)
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: node '%s' failed. Missing instance value, possibly due to Blueprint class or C++ class/struct template deletion."), *GetPathName(), *WriteToString<64>(Node->StaticStruct()->GetFName()));

				bResult = false;
				continue;
			}

			auto HasNewerVersionExists = [](const UStruct* InstanceDataType)
			{
				// Is the class/scriptstruct a blueprint that got replaced by another class.
				bool bHasNewerVersionExists = InstanceDataType->HasAnyFlags(RF_NewerVersionExists);
				if (!bHasNewerVersionExists)
				{
					if (const UClass* InstanceDataClass = Cast<UClass>(InstanceDataType))
					{
						bHasNewerVersionExists = InstanceDataClass->HasAnyClassFlags(CLASS_NewerVersionExists);
					}
					else if (const UScriptStruct* InstanceDataStruct = Cast<UScriptStruct>(InstanceDataType))
					{
						bHasNewerVersionExists = (InstanceDataStruct->StructFlags & STRUCT_NewerVersionExists) != 0;
					}
				}
				return bHasNewerVersionExists;
			};

			if (HasNewerVersionExists(CurrentInstanceDataType))
			{
				bool bLogError = true;
#if WITH_EDITOR
				OutOfDateStructs.Add(CurrentInstanceDataType);
				bLogError = false;
#endif

				if (bLogError)
				{
					UE_LOG(LogStateTree, Error, TEXT("%s: node '%s' failed. The source Instance Data type '%s' has a newer version."), *GetPathName(), *WriteToString<64>(Node->StaticStruct()->GetFName()), *WriteToString<64>(CurrentInstanceDataType->GetFName()));
				}

				bResult = false;
			}

			{
				const UStruct* DesiredInstanceDataType = Node->GetInstanceDataType();
				// The FMyInstance::StaticStruct doesn't get a notification like the other objects when reinstanced.
				const bool bDesiredHasNewerVersion = HasNewerVersionExists(DesiredInstanceDataType);

				// Use strict testing so that the users will have option to initialize data mismatch if the type changes (even if potentially compatible).
				if (CurrentInstanceDataType != DesiredInstanceDataType
					&& !bDesiredHasNewerVersion)
				{
					bool bLogError = true;
#if WITH_EDITOR
					const UClass* CurrentInstanceDataClass = Cast<UClass>(CurrentInstanceDataType);
					const UClass* DesiredInstanceDataClass = Cast<UClass>(DesiredInstanceDataType);
					if (CurrentInstanceDataClass && DesiredInstanceDataClass)
					{
						// Because of the loading order. It's possible that the OnObjectsReinstanced did not completed.
						if (CurrentInstanceDataClass->ClassGeneratedBy == DesiredInstanceDataClass->ClassGeneratedBy)
						{
							OutOfDateStructs.Add(CurrentInstanceDataType);
							bLogError = false;
						}
					}
#endif
					if (bLogError)
					{
						UE_LOG(LogStateTree, Error, TEXT("%s: node '%s' failed. The source Instance Data type '%s' does not match '%s'"), *GetPathName(), *WriteToString<64>(Node->StaticStruct()->GetFName()), *GetNameSafe(CurrentInstanceDataType), *GetNameSafe(DesiredInstanceDataType));
					}
					bResult = false;
				}
			}
		}
	}

	return bResult;
}

bool UStateTree::Link()
{
	// Initialize the instance data default value.
	// This data will be used to allocate runtime instance on all StateTree users.
	ResetLinked();

	// Validate that all the source instance data types matches the node instance data types
	if (!ValidateInstanceData())
	{
		return false;
	}

	if (States.Num() > 0 && Nodes.Num() > 0)
	{
		// Check that all nodes are valid.
		for (FConstStructView Node : Nodes)
		{
			if (!Node.IsValid())
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: State Tree asset was not properly loaded (missing node). See log for loading failures, or recompile the StateTree asset."), *GetPathName());
				return false;
			}
		}
	}

	// Resolves nodes references to other StateTree data
	{
		FStateTreeLinker Linker(Schema);

		for (int32 Index = 0; Index < Nodes.Num(); Index++)
		{
			FStructView Node = Nodes[Index];
			FStateTreeNodeBase* NodePtr = Node.GetPtr<FStateTreeNodeBase>();
			if (ensure(NodePtr))
			{
				const bool bLinkSucceeded = NodePtr->Link(Linker);
				if (!bLinkSucceeded || Linker.GetStatus() == EStateTreeLinkerStatus::Failed)
				{
					UE_LOG(LogStateTree, Error, TEXT("%s: node '%s' failed to resolve its references."), *GetPathName(), *NodePtr->StaticStruct()->GetName());
					return false;
				}
			}
		}

		if (Schema)
		{
			const bool bSchemaLinkSucceeded = Schema->Link(Linker);
			if (!bSchemaLinkSucceeded || Linker.GetStatus() == EStateTreeLinkerStatus::Failed)
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: schema failed to resolve its references."), *GetPathName());
				return false;
			}
		}

		ExternalDataDescs = Linker.GetExternalDataDescs();
	}

	UpdateRuntimeFlags();

	if (!DefaultInstanceData.AreAllInstancesValid())
	{
		UE_LOG(LogStateTree, Error, TEXT("%s: State Tree asset was not properly loaded (missing instance data). See log for loading failures, or recompile the StateTree asset."), *GetPathName());
		return false;
	}

	if (!SharedInstanceData.AreAllInstancesValid())
	{
		UE_LOG(LogStateTree, Error, TEXT("%s: State Tree asset was not properly loaded (missing shared instance data). See log for loading failures, or recompile the StateTree asset."), *GetPathName());
		return false;
	}
	
	if (!PatchBindings())
	{
		return false;
	}

	// Resolves property paths used by bindings a store property pointers
	if (!PropertyBindings.ResolvePaths())
	{
		return false;
	}

	// Link succeeded, setup tree to be ready to run
	bIsLinked = true;
	
	return true;
}

void UStateTree::UpdateRuntimeFlags()
{
	// Set the tick flags at runtime instead of compilation.
	//This is to support hotfix (when we only modify cpp code).

	for (FCompactStateTreeState& State : States)
	{
		// Update the state task flags.
		State.bHasTickTasks = false;
		State.bHasTickTasksOnlyOnEvents = false;
		State.bHasTransitionTasks = false;
		State.bCachedRequestTick = false;
		State.bCachedRequestTickOnlyOnEvents = false;
		for (int32 TaskIndex = State.TasksBegin; TaskIndex < (State.TasksBegin + State.TasksNum); ++TaskIndex)
		{
			const FStateTreeTaskBase& Task = Nodes[TaskIndex].Get<const FStateTreeTaskBase>();
			if (Task.bTaskEnabled)
			{
				State.bHasTickTasks |= Task.bShouldCallTick;
				State.bHasTickTasksOnlyOnEvents |= Task.bShouldCallTickOnlyOnEvents;
				State.bHasTransitionTasks |= Task.bShouldAffectTransitions;
				if (Task.bConsideredForScheduling)
				{
					State.bCachedRequestTick |= Task.bShouldCallTick || Task.bShouldAffectTransitions;
					State.bCachedRequestTickOnlyOnEvents |= Task.bShouldCallTickOnlyOnEvents;
				}
			}
		}
	}

	// Update the global task flags.
	{
		bHasGlobalTickTasks = false;
		bHasGlobalTickTasksOnlyOnEvents = false;
		bHasGlobalTransitionTasks = false;
		bCachedRequestGlobalTick = false;
		bCachedRequestGlobalTickOnlyOnEvents = false;
		for (int32 TaskIndex = GlobalTasksBegin; TaskIndex < (GlobalTasksBegin + GlobalTasksNum); ++TaskIndex)
		{
			const FStateTreeTaskBase& Task = Nodes[TaskIndex].Get<const FStateTreeTaskBase>();
			if (Task.bTaskEnabled)
			{
				bHasGlobalTickTasks |= Task.bShouldCallTick;
				bHasGlobalTickTasksOnlyOnEvents |= Task.bShouldCallTickOnlyOnEvents;
				bHasGlobalTransitionTasks |= Task.bShouldAffectTransitions;
				if (Task.bConsideredForScheduling)
				{
					bCachedRequestGlobalTick |= Task.bShouldCallTick || Task.bShouldAffectTransitions;
					bCachedRequestGlobalTickOnlyOnEvents |= Task.bShouldCallTickOnlyOnEvents;
				}
			}
		}
	}

	bScheduledTickAllowed = Schema ? Schema->IsScheduledTickAllowed() : false;
}

bool UStateTree::PatchBindings()
{
	const TArrayView<FStateTreeBindableStructDesc> SourceStructs = PropertyBindings.SourceStructs;
	TArrayView<FPropertyBindingCopyInfoBatch> CopyBatches = PropertyBindings.GetMutableCopyBatches();
	const TArrayView<FStateTreePropertyPathBinding> PropertyPathBindings = PropertyBindings.PropertyPathBindings;

	// Make mapping from data handle to source struct.
	TMap<FStateTreeDataHandle, int32> SourceStructByHandle;
	for (TConstEnumerateRef<FStateTreeBindableStructDesc> SourceStruct : EnumerateRange(SourceStructs))
	{
		SourceStructByHandle.Add(SourceStruct->DataHandle, SourceStruct.GetIndex());
	}

	auto GetSourceStructByHandle = [&SourceStructByHandle, &SourceStructs](const FStateTreeDataHandle DataHandle) -> FStateTreeBindableStructDesc*
	{
		if (int32* Index = SourceStructByHandle.Find(DataHandle))
		{
			return &SourceStructs[*Index];
		}
		return nullptr;
	};
	
	// Reconcile out of date classes.
	for (FStateTreeBindableStructDesc& SourceStruct : SourceStructs)
	{
		if (const UClass* SourceClass = Cast<UClass>(SourceStruct.Struct))
		{
			if (SourceClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				SourceStruct.Struct = SourceClass->GetAuthoritativeClass();
			}
		}
	}
	for (FPropertyBindingCopyInfoBatch& CopyBatch : CopyBatches)
	{
		if (const UClass* TargetClass = Cast<UClass>(CopyBatch.TargetStruct.Get().Struct))
		{
			if (TargetClass->HasAnyClassFlags(CLASS_NewerVersionExists))
			{
				CopyBatch.TargetStruct.GetMutable<>().Struct = TargetClass->GetAuthoritativeClass();
			}
		}
	}

	auto PatchPropertyPath = [](FPropertyBindingPath& PropertyPath)
	{
		for (FPropertyBindingPathSegment& Segment : PropertyPath.GetMutableSegments())
		{
			if (const UClass* InstanceStruct = Cast<UClass>(Segment.GetInstanceStruct()))
			{
				if (InstanceStruct->HasAnyClassFlags(CLASS_NewerVersionExists))
				{
					Segment.SetInstanceStruct(InstanceStruct->GetAuthoritativeClass());
				}
			}
		}
	};

	for (FStateTreePropertyPathBinding& PropertyPathBinding : PropertyPathBindings)
	{
		PatchPropertyPath(PropertyPathBinding.GetMutableSourcePath());
		PatchPropertyPath(PropertyPathBinding.GetMutableTargetPath());
	}

	// Update property bag structs before resolving binding.
	const EStateTreeDataSourceType GlobalParameterDataType = UE::StateTree::CastToDataSourceType(ParameterDataType);
	if (FStateTreeBindableStructDesc* RootParamsDesc = GetSourceStructByHandle(FStateTreeDataHandle(GlobalParameterDataType)))
	{
		RootParamsDesc->Struct = Parameters.GetPropertyBagStruct();
	}

	// Refresh state parameter descs and bindings batches.
	for (const FCompactStateTreeState& State : States)
	{
		// For subtrees and linked states, the parameters must exists.
		if (State.Type == EStateTreeStateType::Subtree
			|| State.Type == EStateTreeStateType::Linked
			|| State.Type == EStateTreeStateType::LinkedAsset)
		{
			if (!State.ParameterTemplateIndex.IsValid())
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: Data for state '%s' is malformed. Please recompile the StateTree asset."), *GetPathName(), *State.Name.ToString());
				return false;
			}
		}

		if (State.ParameterTemplateIndex.IsValid())
		{
			// Subtree is a bind source, update bag struct.
			const FCompactStateTreeParameters& Params = DefaultInstanceData.GetMutableStruct(State.ParameterTemplateIndex.Get()).Get<FCompactStateTreeParameters>();
			FStateTreeBindableStructDesc* Desc = GetSourceStructByHandle(State.ParameterDataHandle);
			if (!Desc)
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: Data for state '%s' is malformed. Please recompile the StateTree asset."), *GetPathName(), *State.Name.ToString());
				return false;
			}
			Desc->Struct = Params.Parameters.GetPropertyBagStruct();

			if (State.ParameterBindingsBatch.IsValid())
			{
				FPropertyBindingCopyInfoBatch& Batch = CopyBatches[State.ParameterBindingsBatch.Get()];
				Batch.TargetStruct.GetMutable().Struct = Params.Parameters.GetPropertyBagStruct();
			}
		}
	}

	// Check linked state property bags consistency
	for (const FCompactStateTreeState& State : States)
	{
		if (State.Type == EStateTreeStateType::Linked && State.LinkedState.IsValid())
		{
			const FCompactStateTreeState& LinkedState = States[State.LinkedState.Index];

			if (State.ParameterTemplateIndex.IsValid() == false
				|| LinkedState.ParameterTemplateIndex.IsValid() == false)
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: Data for state '%s' is malformed. Please recompile the StateTree asset."), *GetPathName(), *State.Name.ToString());
				return false;
			}

			// Check that the bag in linked state matches.
			const FCompactStateTreeParameters& Params = DefaultInstanceData.GetMutableStruct(State.ParameterTemplateIndex.Get()).Get<FCompactStateTreeParameters>();
			const FCompactStateTreeParameters& LinkedStateParams = DefaultInstanceData.GetMutableStruct(LinkedState.ParameterTemplateIndex.Get()).Get<FCompactStateTreeParameters>();

			if (LinkedStateParams.Parameters.GetPropertyBagStruct() != Params.Parameters.GetPropertyBagStruct())
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: The parameters on state '%s' does not match the linked state parameters in state '%s'. Please recompile the StateTree asset."), *GetPathName(), *State.Name.ToString(), *LinkedState.Name.ToString());
				return false;
			}
		}
		else if (State.Type == EStateTreeStateType::LinkedAsset && State.LinkedAsset)
		{
			// Check that the bag in linked state matches.
			const FInstancedPropertyBag& TargetTreeParameters = State.LinkedAsset->Parameters;
			const FCompactStateTreeParameters& Params = DefaultInstanceData.GetMutableStruct(State.ParameterTemplateIndex.Get()).Get<FCompactStateTreeParameters>();

			if (TargetTreeParameters.GetPropertyBagStruct() != Params.Parameters.GetPropertyBagStruct())
			{
				UE_LOG(LogStateTree, Error, TEXT("%s: The parameters on state '%s' does not match the linked asset parameters '%s'. Please recompile the StateTree asset."),
					*GetPathName(), *State.Name.ToString(), *State.LinkedAsset->GetPathName());
				return false;
			}
		}
	}

	TMap<FStateTreeDataHandle, FStateTreeDataView> DataViews;
	TMap<FStateTreeIndex16, FStateTreeDataView> BindingBatchDataView;

	// Tree parameters
	DataViews.Add(FStateTreeDataHandle(GlobalParameterDataType), Parameters.GetMutableValue());

	// Setup data views for context data. Since the external data is passed at runtime, we can only provide the type.
	for (const FStateTreeExternalDataDesc& DataDesc : ContextDataDescs)
	{
		DataViews.Add(DataDesc.Handle.DataHandle, FStateTreeDataView(DataDesc.Struct, nullptr));
	}
	
	// Setup data views for state parameters.
	for (FCompactStateTreeState& State : States)
	{
		if (State.ParameterDataHandle.IsValid())
		{
			FCompactStateTreeParameters& Params = DefaultInstanceData.GetMutableStruct(State.ParameterTemplateIndex.Get()).Get<FCompactStateTreeParameters>();
			DataViews.Add(State.ParameterDataHandle, Params.Parameters.GetMutableValue());
			if (State.ParameterBindingsBatch.IsValid())
			{
				BindingBatchDataView.Add(State.ParameterBindingsBatch, Params.Parameters.GetMutableValue());
			}
		}
	}

	// Setup data views for all nodes.
	for (FConstStructView NodeView : Nodes)
	{
		const FStateTreeNodeBase& Node = NodeView.Get<const FStateTreeNodeBase>();

		FStateTreeInstanceData* SourceInstanceData = &DefaultInstanceData;
		if (NodeView.GetScriptStruct()->IsChildOf<FStateTreeConditionBase>()
            || NodeView.GetScriptStruct()->IsChildOf<FStateTreeConsiderationBase>()
            || NodeView.GetScriptStruct()->IsChildOf<FStateTreePropertyFunctionBase>())
		{
			// Conditions, Considerations, and PropertyFunctions are stored in shared instance data.
			SourceInstanceData = &SharedInstanceData;
		}

		FStateTreeDataView NodeDataView = Node.InstanceDataHandle.IsObjectSource()
						? FStateTreeDataView(SourceInstanceData->GetMutableObject(Node.InstanceTemplateIndex.Get()))
						: FStateTreeDataView(SourceInstanceData->GetMutableStruct(Node.InstanceTemplateIndex.Get()));

		DataViews.Add(Node.InstanceDataHandle, NodeDataView);

		if (Node.BindingsBatch.IsValid())
		{
			BindingBatchDataView.Add(Node.BindingsBatch, NodeDataView);
		}
	}
	
	auto GetDataSourceView = [&DataViews](const FStateTreeDataHandle Handle) -> FStateTreeDataView
	{
		if (const FStateTreeDataView* ViewPtr = DataViews.Find(Handle))
		{
			return *ViewPtr;
		}
		return FStateTreeDataView();
	};

	auto GetBindingBatchDataView = [&BindingBatchDataView](const FStateTreeIndex16 Index) -> FStateTreeDataView
	{
		if (const FStateTreeDataView* ViewPtr = BindingBatchDataView.Find(Index))
		{
			return *ViewPtr;
		}
		return FStateTreeDataView();
	};


	for (int32 BatchIndex = 0; BatchIndex < CopyBatches.Num(); ++BatchIndex)
	{
		const FPropertyBindingCopyInfoBatch& Batch = CopyBatches[BatchIndex];

		// Find data view for the binding target.
		FStateTreeDataView TargetView = GetBindingBatchDataView(FStateTreeIndex16(BatchIndex));
		if (!TargetView.IsValid())
		{
			UE_LOG(LogStateTree, Error, TEXT("%hs: '%s' Invalid target struct when trying to bind to '%s'")
				, __FUNCTION__
				, *GetPathName()
				, *Batch.TargetStruct.Get().Name.ToString());
			return false;
		}

		FString ErrorMsg;
		for (int32 Index = Batch.BindingsBegin.Get(); Index != Batch.BindingsEnd.Get(); Index++)
		{
			FStateTreePropertyPathBinding& Binding = PropertyPathBindings[Index];

			const EStateTreeDataSourceType Source = Binding.GetSourceDataHandle().GetSource();
			const bool bIsSourceEvent = Source == EStateTreeDataSourceType::TransitionEvent || Source == EStateTreeDataSourceType::StateEvent;

			if(!bIsSourceEvent)
			{
				FStateTreeDataView SourceView = GetDataSourceView(Binding.GetSourceDataHandle());

				if (!Binding.GetMutableSourcePath().UpdateSegmentsFromValue(SourceView, &ErrorMsg))
				{
					UE_LOG(LogStateTree, Error, TEXT("%hs: '%s' Failed to update source instance structs for property binding '%s'. Reason: %s")
						, __FUNCTION__
						, *GetPathName()
						, *Binding.GetTargetPath().ToString()
						, *ErrorMsg);
					return false;
				}
			}

			if (!Binding.GetMutableTargetPath().UpdateSegmentsFromValue(TargetView, &ErrorMsg))
			{
				UE_LOG(LogStateTree, Error, TEXT("%hs: '%s' Failed to update target instance structs for property binding '%s'. Reason: %s")
					, __FUNCTION__
					, *GetPathName()
					, *Binding.GetTargetPath().ToString()
					, *ErrorMsg);
				return false;
			}
		}
	}

	return true;
}

#if WITH_EDITOR

void FStateTreeMemoryUsage::AddUsage(const FConstStructView View)
{
	if (const UScriptStruct* ScriptStruct = View.GetScriptStruct())
	{
		EstimatedMemoryUsage = Align(EstimatedMemoryUsage, ScriptStruct->GetMinAlignment());
		EstimatedMemoryUsage += ScriptStruct->GetStructureSize();
	}
}

void FStateTreeMemoryUsage::AddUsage(const UObject* Object)
{
	if (Object != nullptr)
	{
		check(Object->GetClass());
		EstimatedMemoryUsage += Object->GetClass()->GetStructureSize();
	}
}

TArray<FStateTreeMemoryUsage> UStateTree::CalculateEstimatedMemoryUsage() const
{
	TArray<FStateTreeMemoryUsage> MemoryUsages;
	TArray<TPair<int32, int32>> StateLinks;

	if (!bIsLinked
		|| States.IsEmpty()
		|| !Nodes.IsValid())
	{
		return MemoryUsages;
	}

	const int32 TreeMemUsageIndex = MemoryUsages.Emplace(TEXT("State Tree Max"));
	const int32 InstanceMemUsageIndex = MemoryUsages.Emplace(TEXT("Instance Overhead"));
	const int32 EvalMemUsageIndex = MemoryUsages.Emplace(TEXT("Evaluators"));
	const int32 GlobalTaskMemUsageIndex = MemoryUsages.Emplace(TEXT("GlobalTask"));
	const int32 SharedMemUsageIndex = MemoryUsages.Emplace(TEXT("Shared Data"));

	auto GetRootStateHandle = [this](const FStateTreeStateHandle InState) -> FStateTreeStateHandle
	{
		FStateTreeStateHandle Result = InState;
		while (Result.IsValid() && States[Result.Index].Parent.IsValid())
		{
			Result = States[Result.Index].Parent;
		}
		return Result;		
	};

	auto GetUsageIndexForState = [&MemoryUsages, this](const FStateTreeStateHandle InStateHandle) -> int32
	{
		check(InStateHandle.IsValid());
		
		const int32 FoundMemUsage = MemoryUsages.IndexOfByPredicate([InStateHandle](const FStateTreeMemoryUsage& MemUsage) { return MemUsage.Handle == InStateHandle; });
		if (FoundMemUsage != INDEX_NONE)
		{
			return FoundMemUsage;
		}

		const FCompactStateTreeState& CompactState = States[InStateHandle.Index];
		
		return MemoryUsages.Emplace(TEXT("State ") + CompactState.Name.ToString(), InStateHandle);
	};

	// Calculate memory usage per state.
	TArray<FStateTreeMemoryUsage> TempStateMemoryUsages;
	TempStateMemoryUsages.SetNum(States.Num());

	for (int32 Index = 0; Index < States.Num(); Index++)
	{
		const FStateTreeStateHandle StateHandle((uint16)Index);
		const FCompactStateTreeState& CompactState = States[Index];
		const FStateTreeStateHandle ParentHandle = GetRootStateHandle(StateHandle);
		const int32 ParentUsageIndex = GetUsageIndexForState(ParentHandle);
		
		FStateTreeMemoryUsage& MemUsage = CompactState.Parent.IsValid() ? TempStateMemoryUsages[Index] : MemoryUsages[GetUsageIndexForState(StateHandle)];
		
		MemUsage.NodeCount += CompactState.TasksNum;

		if (CompactState.Type == EStateTreeStateType::Linked)
		{
			const int32 LinkedUsageIndex = GetUsageIndexForState(CompactState.LinkedState);
			StateLinks.Emplace(ParentUsageIndex, LinkedUsageIndex);
		}
		
		if (CompactState.ParameterTemplateIndex.IsValid())
		{
			MemUsage.NodeCount++;
			MemUsage.AddUsage(DefaultInstanceData.GetStruct(CompactState.ParameterTemplateIndex.Get()));
		}
		
		for (int32 TaskIndex = CompactState.TasksBegin; TaskIndex < (CompactState.TasksBegin + CompactState.TasksNum); TaskIndex++)
		{
			if (const FStateTreeTaskBase* Task = Nodes[TaskIndex].GetPtr<const FStateTreeTaskBase>())
			{
				if (Task->InstanceDataHandle.IsObjectSource())
				{
					MemUsage.NodeCount++;
					MemUsage.AddUsage(DefaultInstanceData.GetObject(Task->InstanceTemplateIndex.Get()));
				}
				else
				{
					MemUsage.NodeCount++;
					MemUsage.AddUsage(DefaultInstanceData.GetStruct(Task->InstanceTemplateIndex.Get()));
				}
			}
		}
	}

	// Combine max child usage to parents. Iterate backwards to update children first.
	for (int32 Index = States.Num() - 1; Index >= 0; Index--)
	{
		const FStateTreeStateHandle StateHandle((uint16)Index);
		const FCompactStateTreeState& CompactState = States[Index];

		FStateTreeMemoryUsage& MemUsage = CompactState.Parent.IsValid() ? TempStateMemoryUsages[Index] : MemoryUsages[GetUsageIndexForState(StateHandle)];

		int32 MaxChildStateMem = 0;
		int32 MaxChildStateNodes = 0;
		
		for (uint16 ChildState = CompactState.ChildrenBegin; ChildState < CompactState.ChildrenEnd; ChildState = States[ChildState].GetNextSibling())
		{
			const FStateTreeMemoryUsage& ChildMemUsage = TempStateMemoryUsages[ChildState];
			if (ChildMemUsage.EstimatedMemoryUsage > MaxChildStateMem)
			{
				MaxChildStateMem = ChildMemUsage.EstimatedMemoryUsage;
				MaxChildStateNodes = ChildMemUsage.NodeCount;
			}
		}

		MemUsage.EstimatedMemoryUsage += MaxChildStateMem;
		MemUsage.NodeCount += MaxChildStateNodes;
	}

	// Accumulate linked states.
	for (int32 Index = StateLinks.Num() - 1; Index >= 0; Index--)
	{
		FStateTreeMemoryUsage& ParentUsage = MemoryUsages[StateLinks[Index].Get<0>()];
		const FStateTreeMemoryUsage& LinkedUsage = MemoryUsages[StateLinks[Index].Get<1>()];
		const int32 LinkedTotalUsage = LinkedUsage.EstimatedMemoryUsage + LinkedUsage.EstimatedChildMemoryUsage;
		if (LinkedTotalUsage > ParentUsage.EstimatedChildMemoryUsage)
		{
			ParentUsage.EstimatedChildMemoryUsage = LinkedTotalUsage;
			ParentUsage.ChildNodeCount = LinkedUsage.NodeCount + LinkedUsage.ChildNodeCount;
		}
	}

	// Evaluators
	FStateTreeMemoryUsage& EvalMemUsage = MemoryUsages[EvalMemUsageIndex];
	for (int32 EvalIndex = EvaluatorsBegin; EvalIndex < (EvaluatorsBegin + EvaluatorsNum); EvalIndex++)
	{
		const FStateTreeEvaluatorBase& Eval = Nodes[EvalIndex].Get<const FStateTreeEvaluatorBase>();
		if (Eval.InstanceDataHandle.IsObjectSource())
		{
			EvalMemUsage.AddUsage(DefaultInstanceData.GetObject(Eval.InstanceTemplateIndex.Get()));
		}
		else
		{
			EvalMemUsage.AddUsage(DefaultInstanceData.GetStruct(Eval.InstanceTemplateIndex.Get()));
		}
		EvalMemUsage.NodeCount++;
	}

	// Global Tasks
	FStateTreeMemoryUsage& GlobalTaskMemUsage = MemoryUsages[GlobalTaskMemUsageIndex];
	for (int32 TaskIndex = GlobalTasksBegin; TaskIndex < (GlobalTasksBegin + GlobalTasksNum); TaskIndex++)
	{
		const FStateTreeTaskBase& Task = Nodes[TaskIndex].Get<const FStateTreeTaskBase>();
		if (Task.InstanceDataHandle.IsObjectSource())
		{
			GlobalTaskMemUsage.AddUsage(DefaultInstanceData.GetObject(Task.InstanceTemplateIndex.Get()));
		}
		else
		{
			GlobalTaskMemUsage.AddUsage(DefaultInstanceData.GetStruct(Task.InstanceTemplateIndex.Get()));
		}
		GlobalTaskMemUsage.NodeCount++;
	}

	// Estimate highest combined usage.
	FStateTreeMemoryUsage& TreeMemUsage = MemoryUsages[TreeMemUsageIndex];

	// Exec state
	TreeMemUsage.AddUsage(DefaultInstanceData.GetStruct(0));
	TreeMemUsage.NodeCount++;

	TreeMemUsage.EstimatedMemoryUsage += EvalMemUsage.EstimatedMemoryUsage;
	TreeMemUsage.NodeCount += EvalMemUsage.NodeCount;

	TreeMemUsage.EstimatedMemoryUsage += GlobalTaskMemUsage.EstimatedMemoryUsage;
	TreeMemUsage.NodeCount += GlobalTaskMemUsage.NodeCount;

	FStateTreeMemoryUsage& InstanceMemUsage = MemoryUsages[InstanceMemUsageIndex];
	// FStateTreeInstanceData overhead.
	InstanceMemUsage.EstimatedMemoryUsage += sizeof(FStateTreeInstanceData);
	// FInstancedStructContainer overhead.
	InstanceMemUsage.EstimatedMemoryUsage += TreeMemUsage.NodeCount * FInstancedStructContainer::OverheadPerItem;

	TreeMemUsage.EstimatedMemoryUsage += InstanceMemUsage.EstimatedMemoryUsage;
	
	int32 MaxSubtreeUsage = 0;
	int32 MaxSubtreeNodeCount = 0;
	
	for (const FStateTreeMemoryUsage& MemUsage : MemoryUsages)
	{
		if (MemUsage.Handle.IsValid())
		{
			const int32 TotalUsage = MemUsage.EstimatedMemoryUsage + MemUsage.EstimatedChildMemoryUsage;
			if (TotalUsage > MaxSubtreeUsage)
			{
				MaxSubtreeUsage = TotalUsage;
				MaxSubtreeNodeCount = MemUsage.NodeCount + MemUsage.ChildNodeCount;
			}
		}
	}

	TreeMemUsage.EstimatedMemoryUsage += MaxSubtreeUsage;
	TreeMemUsage.NodeCount += MaxSubtreeNodeCount;

	FStateTreeMemoryUsage& SharedMemUsage = MemoryUsages[SharedMemUsageIndex];
	SharedMemUsage.NodeCount = SharedInstanceData.Num();
	SharedMemUsage.EstimatedMemoryUsage = SharedInstanceData.GetEstimatedMemoryUsage();

	return MemoryUsages;
}

void UStateTree::OnPreBeginPIE(const bool bIsSimulating)
{
	CompileIfChanged();
}

void UStateTree::CompileIfChanged()
{
	if (UE::StateTree::Delegates::OnRequestCompile.IsBound() && UE::StateTree::Delegates::OnRequestEditorHash.IsBound())
	{
		const uint32 CurrentHash = UE::StateTree::Delegates::OnRequestEditorHash.Execute(*this);
		if (LastCompiledEditorDataHash != CurrentHash)
		{
			UE_LOG(LogStateTree, Log, TEXT("%s: Editor data has changed. Recompiling state tree."), *GetPathName());
			UE::StateTree::Delegates::OnRequestCompile.Execute(*this);
		}
	}
	else
	{
		ResetCompiled();
		UE_LOG(LogStateTree, Warning, TEXT("%s: could not compile. Please resave the StateTree asset."), *GetPathName());
	}
}

void UStateTree::Compile()
{
	if (UE::StateTree::Delegates::OnRequestCompile.IsBound())
	{
		UE_LOG(LogStateTree, Log, TEXT("%s: Editor data has changed. Recompiling state tree."), *GetPathName());
		UE::StateTree::Delegates::OnRequestCompile.Execute(*this);
	}
	else
	{
		ResetCompiled();
		UE_LOG(LogStateTree, Warning, TEXT("%s: could not compile. Please resave the StateTree asset."), *GetPathName());
	}
}
#endif //WITH_EDITOR

#if WITH_EDITOR || WITH_STATETREE_DEBUG
FString UStateTree::DebugInternalLayoutAsString() const
{
	FStringBuilderBase DebugString;
	DebugString << TEXT("StateTree (asset: '");
	GetFullName(DebugString);
	DebugString << TEXT("')\n");

	auto PrintObjectNameSafe = [&DebugString](int32 Index, const UObject* Obj)
		{
			DebugString << TEXT("  (");
			DebugString << Index;
			DebugString << TEXT(")");
			if (Obj)
			{
				DebugString << Obj->GetFName();
			}
			else
			{
				DebugString << TEXT("null");
			}
			DebugString << TEXT('\n');
		};
	auto PrintViewNameSafe = [&DebugString](int32 Index, const FConstStructView& View)
		{
			DebugString << TEXT("  (");
			DebugString << Index;
			DebugString << TEXT(")");
			if (View.IsValid())
			{
				DebugString << View.GetScriptStruct()->GetFName();
			}
			else
			{
				DebugString << TEXT("null");
			}
			DebugString << TEXT('\n');
		};

	// Tree items (e.g. tasks, evaluators, conditions)
	DebugString.Appendf(TEXT("\nNodes(%d)\n"), Nodes.Num());
	for (int32 Index = 0; Index < Nodes.Num(); Index++)
	{
		const FConstStructView Node = Nodes[Index];
		PrintViewNameSafe(Index, Node);
	}

	// Instance InstanceData data (e.g. tasks)
	DebugString.Appendf(TEXT("\nInstance Data(%d)\n"), DefaultInstanceData.Num());
	for (int32 Index = 0; Index < DefaultInstanceData.Num(); Index++)
	{
		if (DefaultInstanceData.IsObject(Index))
		{
			const UObject* Data = DefaultInstanceData.GetObject(Index);
			PrintObjectNameSafe(Index, Data);
		}
		else
		{
			const FConstStructView Data = DefaultInstanceData.GetStruct(Index);
			PrintViewNameSafe(Index, Data);
		}
	}

	// External data (e.g. fragments, subsystems)
	DebugString.Appendf(TEXT("\nExternal Data(%d)\n"), ExternalDataDescs.Num());
	if (ExternalDataDescs.Num())
	{
		DebugString.Appendf(TEXT("  [ (Idx) | %-40s | %-8s | %15s ]\n"), TEXT("Name"), TEXT("Optional"), TEXT("Handle"));
		for (int32 DataDescIndex = 0; DataDescIndex < ExternalDataDescs.Num(); ++DataDescIndex)
		{
			const FStateTreeExternalDataDesc& Desc = ExternalDataDescs[DataDescIndex];
			DebugString.Appendf(TEXT("  | (%3d) | %-40s | %8s | %15s |\n"),
				DataDescIndex,
				Desc.Struct ? *Desc.Struct->GetName() : TEXT("null"),
				*UEnum::GetDisplayValueAsText(Desc.Requirement).ToString(),
				*Desc.Handle.DataHandle.Describe());
		}
	}

	// Bindings
#if WITH_PROPERTYBINDINGUTILS_DEBUG
	DebugString << PropertyBindings.DebugAsString();
#endif

	// States
	DebugString.Appendf(TEXT("\nStates(%d)\n"), States.Num());
	if (States.Num())
	{
		DebugString.Appendf(TEXT("  [ (Idx) | %-30s | %15s | %5s [%3s:%-3s[ | Begin Idx : %4s %4s %4s %4s | Num : %4s %4s %4s %4s ]\n"),
			TEXT("Name"), TEXT("Parent"), TEXT("Child"), TEXT("Beg"), TEXT("End"),
			TEXT("Cond"), TEXT("Tr"), TEXT("Tsk"), TEXT("Uti"), TEXT("Cond"), TEXT("Tr"), TEXT("Tsk"), TEXT("Uti"));
		for (int32 StateIndex = 0; StateIndex < States.Num(); ++StateIndex)
		{
			const FCompactStateTreeState& State = States[StateIndex];
			DebugString.Appendf(TEXT("  | (%3d) | %-30s | %15s | %5s [%3d:%-3d[ | %9s   %4d %4d %4d %4d | %3s   %4d %4d %4d %4d |\n"),
				StateIndex,
				*State.Name.ToString(),
				*State.Parent.Describe(),
				TEXT(" "), State.ChildrenBegin, State.ChildrenEnd,
				TEXT(" "), State.EnterConditionsBegin, State.TransitionsBegin, State.TasksBegin, State.UtilityConsiderationsBegin,
				TEXT(" "), State.EnterConditionsNum, State.TransitionsNum, State.TasksNum, State.UtilityConsiderationsNum);
		}
	}

	// Transitions
	DebugString.Appendf(TEXT("\nTransitions(%d)\n"), Transitions.Num());
	if (Transitions.Num())
	{
		DebugString.Appendf(TEXT("  [ (Idx) | %-3s | %15s | %-20s | %-40s | %-40s | %-8s ]\n")
			, TEXT("Idx"), TEXT("State"), TEXT("Transition Trigger"), TEXT("Transition Event Tag"), TEXT("Transition Event Payload"), TEXT("Cond:Num"));
		for (int32 TransitionIndex = 0; TransitionIndex < Transitions.Num(); ++TransitionIndex)
		{
			const FCompactStateTransition& Transition = Transitions[TransitionIndex];
			DebugString.Appendf(TEXT("  | (%3d) | %3d | %15s | %-20s | %-40s | %-40s | %4d:%3d |\n"),
				TransitionIndex,
				Transition.ConditionsBegin,
				*Transition.State.Describe(),
				*UEnum::GetDisplayValueAsText(Transition.Trigger).ToString(),
				*Transition.RequiredEvent.Tag.ToString(),
				Transition.RequiredEvent.PayloadStruct ? *Transition.RequiredEvent.PayloadStruct->GetName() : TEXT("None"),
				Transition.ConditionsBegin,
				Transition.ConditionsNum);
		}
	}

	// Evaluators
	DebugString.Appendf(TEXT("\nEvaluators(%d)\n"), EvaluatorsNum);
	if (EvaluatorsNum)
	{
		DebugString.Appendf(TEXT("  [ (Idx) | %-30s | %8s | %10s ]\n"),
			TEXT("Name"), TEXT("Bindings"), TEXT("Struct Idx"));
		for (int32 EvalIndex = EvaluatorsBegin; EvalIndex < (EvaluatorsBegin + EvaluatorsNum); EvalIndex++)
		{
			const FStateTreeEvaluatorBase& Eval = Nodes[EvalIndex].Get<const FStateTreeEvaluatorBase>();
			DebugString.Appendf(TEXT("  | (%3d) | %-30s | %8d | %10s |\n"),
				EvalIndex,
				*Eval.Name.ToString(),
				Eval.BindingsBatch.Get(),
				*Eval.InstanceDataHandle.Describe());
		}
	}

	// Tasks
	DebugString.Appendf(TEXT("\nTasks\n  [ (Idx) | %-30s | %-30s | %8s | %10s ]\n"),
		TEXT("State"), TEXT("Name"), TEXT("Bindings"), TEXT("Struct Idx"));
	for (const FCompactStateTreeState& State : States)
	{
		if (State.TasksNum)
		{
			for (int32 TaskIndex = State.TasksBegin; TaskIndex < (State.TasksBegin + State.TasksNum); TaskIndex++)
			{
				const FStateTreeTaskBase& Task = Nodes[TaskIndex].Get<const FStateTreeTaskBase>();
				DebugString.Appendf(TEXT("  | (%3d) | %-30s | %-30s | %8d | %10s |\n"),
					TaskIndex,
					*State.Name.ToString(),
					*Task.Name.ToString(),
					Task.BindingsBatch.Get(),
					*Task.InstanceDataHandle.Describe());
			}
		}
	}
	for (int32 TaskIndex = GlobalTasksBegin; TaskIndex < (GlobalTasksBegin + GlobalTasksNum); TaskIndex++)
	{
		const FStateTreeTaskBase& Task = Nodes[TaskIndex].Get<const FStateTreeTaskBase>();
		DebugString.Appendf(TEXT("  | (%3d) | %-30s | %-30s | %8d | %10s |\n"),
			TaskIndex,
			TEXT("Global"),
			*Task.Name.ToString(),
			Task.BindingsBatch.Get(),
			*Task.InstanceDataHandle.Describe());
	}

	// Conditions
	DebugString.Appendf(TEXT("\nConditions\n  [ (Idx) | %-30s | %8s | %12s | %10s ]\n"),
		TEXT("Name"), TEXT("Operand"), TEXT("Evaluation"), TEXT("Struct Idx"));
	{
		for (int32 Index = 0; Index < Nodes.Num(); ++Index)
		{
			if(const FStateTreeConditionBase* Cond = Nodes[Index].GetPtr<const FStateTreeConditionBase>())
			{
				DebugString.Appendf(TEXT("  | (%3d) | %-30s | %8s | %12s | %10s |\n"),
					Index,
					*Cond->Name.ToString(),
					*UEnum::GetDisplayValueAsText(Cond->Operand).ToString(),
					*UEnum::GetDisplayValueAsText(Cond->EvaluationMode).ToString(),
					*Cond->InstanceDataHandle.Describe());
			}
		}
	}

	return DebugString.ToString();
}
#endif // WITH_EDITOR || WITH_STATETREE_DEBUG

