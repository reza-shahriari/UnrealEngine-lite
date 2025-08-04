// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosModularVehicle/ModularVehicleBaseComponent.h"
#include "ChaosModularVehicle/ChaosSimModuleManager.h"
#include "ChaosModularVehicle/VehicleSimBaseComponent.h"
#include "ChaosModularVehicle/ModularVehicleDefaultAsyncInput.h"
#include "ChaosModularVehicle/ModularVehicleAnimationInstance.h"
#include "ChaosModularVehicle/InputProducer.h"
#include "Engine/Engine.h"
#include "Engine/Canvas.h"
#include "Engine/OverlapResult.h"
#include "PhysicsEngine/PhysicsSettings.h"
#include "PhysicsEngine/ClusterUnionComponent.h"
#include "PhysicsEngine/PhysicsObjectExternalInterface.h"
#include "PhysicsEngine/BodySetup.h"
#include "Net/Core/PushModel/PushModel.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Physics/Experimental/PhysScene_Chaos.h"
#include "SimModule/SimModuleTree.h"
#include "PhysicsProxy/SingleParticlePhysicsProxy.h"
#include "PhysicsProxy/ClusterUnionPhysicsProxy.h"
#include "GeometryCollection/GeometryCollectionComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"

#include "ChaosModularVehicle/ChaosSimModuleManagerAsyncCallback.h"

DEFINE_LOG_CATEGORY(LogModularBase);


bool bModularVehicle_SuspensionConstraint_Enabled = true;
FAutoConsoleVariableRef CVarModularVehicleSuspensionConstraintEnabled(TEXT("p.ModularVehicle.SuspensionConstraint.Enabled"), bModularVehicle_SuspensionConstraint_Enabled, TEXT("Enable/Disable suspension constraint falling back to simple forces when constraint disabled (requires restart)."));

bool bModularVehicle_DumpModuleTreeStructure_Enabled = false;
FAutoConsoleVariableRef CVarModularVehicleDumpModuleTreeStructureEnabled(TEXT("p.ModularVehicle.DumpModuleTreeStructure.Enabled"), bModularVehicle_DumpModuleTreeStructure_Enabled, TEXT("Enable/Disable logging of module tree structure every time there is a change."));

UModularVehicleBaseComponent::UModularVehicleBaseComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = false;
	bRequiresControllerForInputs = true;

	LinearDamping = 0.01f;
	AngularDamping = 0.0f;

	bAutoAddComponentsFromWorld = false;
	AutoAddOverlappingBoxSize = FVector(500, 500, 500);
	ClusteringCount = 0;
	DelayClusteringCount = 0;

	SetIsReplicatedByDefault(true);
	bUsingNetworkPhysicsPrediction = Chaos::FPhysicsSolverBase::IsNetworkPhysicsPredictionEnabled();

	SuspensionTraceCollisionResponses = FCollisionResponseContainer::GetDefaultResponseContainer();
	SuspensionTraceCollisionResponses.Vehicle = ECR_Ignore;
	SuspensionCollisionChannel = ECollisionChannel::ECC_WorldDynamic;
	bSuspensionTraceComplex = true;
	bKeepVehicleAwake = true;

	CurrentGear = 0;
	EngineRPM = 0.0f;
	EngineTorque = 0.0f;

	if (bUsingNetworkPhysicsPrediction)
	{
		static const FName NetworkPhysicsComponentName(TEXT("PC_NetworkPhysicsComponent"));

		NetworkPhysicsComponent = CreateDefaultSubobject<UNetworkPhysicsComponent, UNetworkPhysicsComponent>(NetworkPhysicsComponentName);
		NetworkPhysicsComponent->SetNetAddressable();
		NetworkPhysicsComponent->SetIsReplicated(true);
	}

	bIsLocallyControlled = false;

	InputProducerClass = UVehicleDefaultInputProducer::StaticClass();
}

UModularVehicleBaseComponent::~UModularVehicleBaseComponent()
{
}

APlayerController* UModularVehicleBaseComponent::GetPlayerController() const
{
	if (AController* Controller = GetController())
	{
		return Cast<APlayerController>(Controller);
	}

	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (AController* Controller = Pawn->GetController())
		{
			return Cast<APlayerController>(Controller);
		}

		if (APlayerController* PC = Cast<APlayerController>(Pawn->GetOwner()))
		{
			return PC;
		}

	}
	return nullptr;
}


bool UModularVehicleBaseComponent::IsLocallyControlled() const
{
	if (bIsLocallyControlled)
	{
		return true;
	}

	if (APlayerController* PlayerController = GetPlayerController())
	{
		return PlayerController->IsLocalController();
	}
	return false;
}

void UModularVehicleBaseComponent::ProduceInput(int32 PhysicsStep, int32 NumSteps)
{
	if (InputProducer)
	{
		InputProducer->ProduceInput(PhysicsStep, NumSteps, InputNameMap, InputsContainer);
	}
}

//void UModularVehicleBaseComponent::GenerateInputModifiers(const TArray<FModuleInputSetup>& CombinedInputConfiguration)
//{
//	for (const FModuleInputSetup& InputSetup : CombinedInputConfiguration)
//	{
//		if (InputSetup.InputModifierClass != nullptr)
//		{
//			UDefaultModularVehicleInputModifier* NewPtr = NewObject<UDefaultModularVehicleInputModifier>(this, InputSetup.InputModifierClass);
//			InputModifiers.Add(NewPtr);
//		}
//		else
//		{
//			InputModifiers.Add(nullptr);
//		}
//	}
//}

//void UModularVehicleBaseComponent::ApplyInputModifiers(float DeltaTime, const FModuleInputContainer& RawValue)
//{
//	check(InputModifiers.Num() == InputsContainer.GetNumInputs());
//	for (int I = 0; I < InputsContainer.GetNumInputs(); I++)
//	{
//		if (InputModifiers[I] != nullptr)
//		{
//			InputsContainer.SetValueAtIndex(I, InputModifiers[I]->InterpInputValue(DeltaTime, InputsContainer.GetValueAtIndex(I), RawValue.GetValueAtIndex(I)));
//		}
//		else
//		{
//			InputsContainer.SetValueAtIndex(I, RawValue.GetValueAtIndex(I));
//		}
//	}
//}


void UModularVehicleBaseComponent::OnCreatePhysicsState()
{
	Super::OnCreatePhysicsState();

	if (ClusterUnionComponent)
	{
		// piggyback on the Add/Remove component events from the cluster union to add/remove simulation modules
		ClusterUnionComponent->OnComponentAddedNativeEvent.AddUObject(this, &UModularVehicleBaseComponent::AddComponentToSimulation);
		ClusterUnionComponent->OnComponentRemovedNativeEvent.AddUObject(this, &UModularVehicleBaseComponent::RemoveComponentFromSimulation);

		// #TODO: this doesn't appear to be working
		if (bKeepVehicleAwake && ClusterUnionComponent->GetPhysicsProxy())
		{
			if (auto* Particle = ClusterUnionComponent->GetPhysicsProxy()->GetParticle_External())
			{
				Particle->SetSleepType(Chaos::ESleepType::NeverSleep);
			}
		}
	}

	// convert the game thread side UVehicleSimBaseComponent(s) to physics thread simulation ISimulationModuleBase(s)
	CreateVehicleSim();

	if (bUsingNetworkPhysicsPrediction)
	{
		if (NetworkPhysicsComponent)
		{
			// register interface to handle network prediction callbacks
			// #Note: in our case we don't yet know what the replication data will be since the modules are built after this point at runtime
			FScopedModuleInputInitializer SetSetup(InputConfig);
			NetworkPhysicsComponent->CreateDataHistory<FPhysicsModularVehicleTraits>(this);

			if (bIsLocallyControlled)
			{
				NetworkPhysicsComponent->SetIsRelayingLocalInputs(bIsLocallyControlled);
			}

		}
	}

	NextConstructionIndex = 0;

	ActorsToIgnore.Add(GetOwner()); // vehicle ignore self in scene query

}

void UModularVehicleBaseComponent::OnDestroyPhysicsState()
{
	Super::OnDestroyPhysicsState();

	if (ClusterUnionComponent)
	{
		ClusterUnionComponent->OnComponentAddedNativeEvent.RemoveAll(this);
		ClusterUnionComponent->OnComponentRemovedNativeEvent.RemoveAll(this);
	}

	DestroyVehicleSim();

	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
	{
		NetworkPhysicsComponent->RemoveDataHistory();
	}

}

int UModularVehicleBaseComponent::GenerateNewGuid()
{
	static int Val = 0;
	return Val++;
}

void UModularVehicleBaseComponent::CacheRootPhysicsObject(IPhysicsProxyBase* Proxy)
{
	Chaos::EnsureIsInGameThreadContext();
	using namespace Chaos;
	RootPhysicsObject = nullptr;
	
	if (Proxy == nullptr)
	{
		return;
	}

	switch (Proxy->GetType())
	{
		case EPhysicsProxyType::ClusterUnionProxy:
		{
			if (FClusterUnionPhysicsProxy* CUProxy = static_cast<FClusterUnionPhysicsProxy*>(Proxy))
			{
				RootPhysicsObject = CUProxy->GetPhysicsObjectHandle();
			}
		}
		break;

		case EPhysicsProxyType::SingleParticleProxy:
		{
			if (FSingleParticlePhysicsProxy* ParticleProxy = static_cast<FSingleParticlePhysicsProxy*>(Proxy))
			{
				RootPhysicsObject = ParticleProxy->GetPhysicsObject();
			}
		}
		break;

		default:
		{
			UE_LOG(LogModularBase, Error, TEXT("Unsupported Particle type"));
		}
		break;
	}
}

void UModularVehicleBaseComponent::CreateAssociatedSimComponents(USceneComponent* ParentComponent, USceneComponent* AttachedComponent, int ParentIndex, int InTransformIndex, Chaos::FSimTreeUpdates& TreeUpdatesOut)
{
	using namespace Chaos;
	if (AttachedComponent == nullptr || ClusterUnionComponent == nullptr)
	{
		return;
	}

	UE_LOG(LogModularBase, Log, TEXT("CreateAssociatedSimComponents Attaching %s, TransformIndex %d"), *AttachedComponent->GetName(), InTransformIndex);

	TArray<Chaos::FClusterUnionChildData> ChildParticles = ClusterUnionComponent->GetPhysicsProxy()->GetSyncedData_External().ChildParticles;

	if (InTransformIndex >= ChildParticles.Num())
	{
		// #TODO: look into the underlying cause of this indexing issue, possibly not an easy fix
		ensureMsgf(false, TEXT("Unexpected state - likely due to adding and removing components on same frame leading to an array indexing issue") );
		return;
	}

	int TransformIndex = InTransformIndex;
	if (RootPhysicsObject == nullptr)
	{
		CacheRootPhysicsObject(GetPhysicsProxy());
	}


	if (IVehicleSimBaseComponentInterface* ComponentInterface = Cast<IVehicleSimBaseComponentInterface>(AttachedComponent))
	{

		// if we find the parent is a skeletal mesh then the indices are ...
		if (ParentComponent)
		{
			if (USkeletalMeshComponent* SKMComponent = Cast<USkeletalMeshComponent>(ParentComponent))
			{
				const FName BoneName = ComponentInterface->GetBoneName();

				int32 BodyBoneIndex = INDEX_NONE;

				for (int32 ArrayIndex = 0; ArrayIndex < SKMComponent->Bodies.Num(); ArrayIndex++)
				{
					// Get body index of bone
					if (SKMComponent->Bodies[ArrayIndex] && SKMComponent->Bodies[ArrayIndex]->IsValidBodyInstance())
					{
						const FName BodyName = SKMComponent->Bodies[ArrayIndex]->GetBodySetup()->BoneName;

						if (BodyName == BoneName)
						{
							BodyBoneIndex = ArrayIndex;
							break;
						}

					}
				}

				if (BodyBoneIndex != INDEX_NONE)
				{
					TransformIndex = InTransformIndex + BodyBoneIndex;
				}

			}
			else
			{
				TransformIndex = InTransformIndex;
			}

		}

		ensure(TransformIndex < ChildParticles.Num());


		FTransform ClusterUnionComponentTransform = ClusterUnionComponent->GetComponentTransform();
		FTransform ComponentTransform = AttachedComponent->GetComponentTransform().GetRelativeTransform(ClusterUnionComponentTransform);

		int TreeIndex = INDEX_NONE;

		Chaos::ISimulationModuleBase* NewModule = ComponentInterface->CreateNewCoreModule();

		TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		FString DebugString;
		NewModule->GetDebugString(DebugString);
		UE_LOG(LogModularBase, Log, TEXT("CreateAssociatedSimComponents Found Sim Component %s (%s)"), *NewModule->GetDebugName(), *DebugString);
#endif

		FVector LocalOffset(0, 0, 0);
		FVector LocalOffsetCOM(0, 0, 0);
		FTransform PhysicsTransform = FTransform::Identity;

		FTransform InitialTransform = ChildParticles[TransformIndex].ChildToParent;
		InitialTransform.SetLocation(InitialTransform.GetLocation());
		NewModule->SetSimModuleTree(SimModuleTree.Get());
		TreeIndex = TreeUpdatesOut.AddNodeBelow(ParentIndex, NewModule);
		NewModule->SetTransformIndex(TransformIndex);
		NewModule->SetParticleIndex(ChildParticles[TransformIndex].ParticleIdx);

		NewModule->SetIntactTransform(FTransform::Identity);
		NewModule->SetClusteredTransform(InitialTransform);
		NewModule->SetClustered(true);

		FVehicleComponentData& ComponentData = ComponentToPhysicsObjects.FindOrAdd(AttachedComponent);
		ComponentData.Guid = GenerateNewGuid();
		NewModule->SetGuid(ComponentData.Guid);

		NewModule->SetInitialParticleTransform(InitialTransform);
		NewModule->SetComponentTransform(ComponentTransform);

		FTransform ClusterredTransform(FQuat::Identity, InitialTransform.GetLocation());
		NewModule->SetClusteredTransform(ClusterredTransform);

		const bool bIsAnimationEnabled = ComponentInterface->GetAnimationEnabled();
		const FName ComponentBoneName = ComponentInterface->GetBoneName();
		// Now always using ModuleAnimationSetups even when there is no bone specified. ModuleAnimationSetups can be used to drive
		// transforms in components, not just skeletal mesh bones.
		if (bIsAnimationEnabled)
		{
			// if bone already exists then use that (seperate wheel and suspension modules can share same bone)

			int FoundIndex = INDEX_NONE;
			if (ComponentBoneName != NAME_None)
			{
				for (int I = 0; I < ModuleAnimationSetups.Num(); I++)
				{
					if (ModuleAnimationSetups[I].BoneName == ComponentBoneName)
					{
						FoundIndex = I;
						break;
					}
				}
			}

			const FVector& ComponentAnimationOffset = ComponentInterface->GetAnimationOffset();
			if (FoundIndex != INDEX_NONE)
			{
				NewModule->SetAnimationData(ComponentBoneName, ComponentAnimationOffset, FoundIndex);
			}
			else
			{
				NewModule->SetAnimationData(ComponentBoneName, ComponentAnimationOffset, ModuleAnimationSetups.Num());
				FModuleAnimationSetup AnimSetup(NewModule->GetBoneName(), TransformIndex);
				ModuleAnimationSetups.Add(AnimSetup);
			}
		}

		// store the tree index in the original sim component
		ComponentInterface->SetTreeIndex(TreeIndex);
		ParentIndex = TreeIndex;

		if (GetPhysicsProxy())
		{
			NewModule->OnConstruction_External(RootPhysicsObject);
		}
	}

	// get this components children, not all children
	TArray<USceneComponent*> Children;
	AttachedComponent->GetChildrenComponents(false, Children);

	// recurse down tree, converting all SimComponents to proper simulation modules
	for (USceneComponent* Child : Children)
	{
		if (IVehicleSimBaseComponentInterface* ChildSimComponent = Cast<IVehicleSimBaseComponentInterface>(Child))
		{
			CreateAssociatedSimComponents(AttachedComponent, Child, ParentIndex, TransformIndex, TreeUpdatesOut);
		}
	}

	UpdatePhysicalProperties();
}

void UModularVehicleBaseComponent::CreateIndependentSimComponents(USceneComponent* RootComponent, USceneComponent* AttachedComponent, int ParentIndex, int TransformIndex, Chaos::FSimTreeUpdates& TreeUpdatesOut)
{
	using namespace Chaos;
	if (AttachedComponent == nullptr || RootComponent == nullptr)
	{
		return;
	}

	if (RootPhysicsObject == nullptr)
	{
		CacheRootPhysicsObject(GetPhysicsProxy()); //#TODO where to put / refresh?
	}

	UE_LOG(LogModularBase, Log, TEXT("CreateAssociatedSimComponents Attaching %s, TransformIndex %d"), *AttachedComponent->GetName(), TransformIndex);

	if (IVehicleSimBaseComponentInterface* ComponentInterface = Cast<IVehicleSimBaseComponentInterface>(AttachedComponent))
	{
		FTransform RootComponentTransform = RootComponent->GetComponentTransform();
		FTransform ComponentTransform = AttachedComponent->GetComponentTransform().GetRelativeTransform(RootComponentTransform);

		int TreeIndex = INDEX_NONE;

		Chaos::ISimulationModuleBase* NewModule = ComponentInterface->CreateNewCoreModule();

		TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		FString DebugString;
		NewModule->GetDebugString(DebugString);
		UE_LOG(LogModularBase, Log, TEXT("CreateAssociatedSimComponents Found Sim Component %s (%s)"), *NewModule->GetDebugName(), *DebugString);
#endif

		FVector LocalOffset(0, 0, 0);
		FVector LocalOffsetCOM(0, 0, 0);
		FTransform PhysicsTransform = FTransform::Identity;

		FTransform InitialTransform = ComponentTransform;
		InitialTransform.SetLocation(InitialTransform.GetLocation());
		NewModule->SetSimModuleTree(SimModuleTree.Get());
		TreeIndex = TreeUpdatesOut.AddNodeBelow(ParentIndex, NewModule);
		NewModule->SetTransformIndex(TransformIndex);
		NewModule->SetParticleIndex(Chaos::FUniqueIdx());

		NewModule->SetIntactTransform(FTransform::Identity);
		NewModule->SetClusteredTransform(InitialTransform);
		NewModule->SetClustered(true);

		FVehicleComponentData& ComponentData = ComponentToPhysicsObjects.FindOrAdd(AttachedComponent);
		ComponentData.Guid = GenerateNewGuid();
		NewModule->SetGuid(ComponentData.Guid);

		NewModule->SetInitialParticleTransform(InitialTransform);
		NewModule->SetComponentTransform(ComponentTransform);

		FTransform ClusterredTransform(FQuat::Identity, InitialTransform.GetLocation());
		NewModule->SetClusteredTransform(ClusterredTransform);

		const bool bIsAnimationEnabled = ComponentInterface->GetAnimationEnabled();
		const FName ComponentBoneName = ComponentInterface->GetBoneName();
		// Changed behaviour - now always using ModuleAnimationSetups even when there is no bone setup. ModuleAnimationSetups is used to drive
		// transforms in components, not just skeletal mesh bones.
		if (bIsAnimationEnabled)
		{
			// if bone already exists then use that (seperate wheel and suspension modules can share same bone)

			int FoundIndex = INDEX_NONE;
			for (int I = 0; I < ModuleAnimationSetups.Num(); I++)
			{
				if (ModuleAnimationSetups[I].BoneName == ComponentBoneName)
				{
					FoundIndex = I;
					break;
				}
			}

			const FVector& ComponentAnimationOffset = ComponentInterface->GetAnimationOffset();
			if (FoundIndex != INDEX_NONE)
			{
				NewModule->SetAnimationData(ComponentBoneName, ComponentAnimationOffset, FoundIndex);
			}
			else
			{
				NewModule->SetAnimationData(ComponentBoneName, ComponentAnimationOffset, ModuleAnimationSetups.Num());
				FModuleAnimationSetup AnimSetup(NewModule->GetBoneName(), TransformIndex);
				ModuleAnimationSetups.Add(AnimSetup);
			}
		}

		// store the tree index in the original sim component
		ComponentInterface->SetTreeIndex(TreeIndex);
		ParentIndex = TreeIndex;

		if (GetPhysicsProxy())
		{
			NewModule->OnConstruction_External(RootPhysicsObject);
		}
	}

	// get this components children, not all children
	TArray<USceneComponent*> Children;
	AttachedComponent->GetChildrenComponents(false, Children);

	// recurse down tree, converting all SimComponents to proper simulation modules
	for (USceneComponent* Child : Children)
	{
		if (IVehicleSimBaseComponentInterface* ChildSimComponent = Cast<IVehicleSimBaseComponentInterface>(Child))
		{
			CreateIndependentSimComponents(RootComponent, Child, ParentIndex, TransformIndex, TreeUpdatesOut);
		}
	}

	UpdatePhysicalProperties();
}


int UModularVehicleBaseComponent::AddSimModule(Chaos::ISimulationModuleBase* NewModule, const FTransform& LocalTransform, int ParentIndex, int TransformIndex)
{
	if (VehicleSimulationPT == nullptr)
	{
		ensureMsgf(false, TEXT("VehicleSimulationPT ptr is null, This is unexpected behaviour."));
		return INDEX_NONE;
	}

	TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();

	FVector LocalOffset(0, 0, 0);
	FVector LocalOffsetCOM(0, 0, 0);
	FTransform PhysicsTransform = FTransform::Identity;

	FTransform InitialTransform = LocalTransform;
	InitialTransform.SetLocation(InitialTransform.GetLocation());
	NewModule->SetSimModuleTree(SimModuleTree.Get());
	int TreeIndex = StoredTreeUpdates.AddNodeBelow(ParentIndex, NewModule);

	NewModule->SetGuid(GenerateNewGuid());

	NewModule->SetTransformIndex(TransformIndex);
	NewModule->SetParticleIndex(Chaos::FUniqueIdx());

	NewModule->SetIntactTransform(FTransform::Identity);
	NewModule->SetClusteredTransform(InitialTransform);
	NewModule->SetClustered(true);

	NewModule->SetInitialParticleTransform(InitialTransform);
	NewModule->SetComponentTransform(LocalTransform);

	FTransform ClusterredTransform(FQuat::Identity, InitialTransform.GetLocation());
	NewModule->SetClusteredTransform(ClusterredTransform);

	NewModule->SetAnimationData(NewModule->GetBoneName(), NewModule->GetAnimationOffset(), ModuleAnimationSetups.Num());
	FModuleAnimationSetup AnimSetup(NewModule->GetBoneName(), TransformIndex, NewModule->GetGuid());
	AnimSetup.InitialRotOffset = InitialTransform.GetRotation();
	AnimSetup.InitialLocOffset = InitialTransform.GetTranslation();
	ModuleAnimationSetups.Add(AnimSetup);

	return TreeIndex;
}

void UModularVehicleBaseComponent::RemoveSimModule(int ModuleGuid)
{
	StoredTreeUpdates.RemoveNode(ModuleGuid);
}

void UModularVehicleBaseComponent::FinalizeModuleUpdates()
{
	ActionTreeUpdates(&StoredTreeUpdates);
	StoredTreeUpdates.ClearUpdates();

	if (RootPhysicsObject == nullptr)
	{
		ensure(GetPhysicsProxy() != nullptr); // should never happen
		CacheRootPhysicsObject(GetPhysicsProxy()); //#TODO where to put / refresh?
	}

	UpdatePhysicalProperties();
}


void UModularVehicleBaseComponent::UpdatePhysicalProperties()
{
	if (ClusterUnionComponent)
	{
		if (Chaos::FClusterUnionPhysicsProxy* Proxy = ClusterUnionComponent->GetPhysicsProxy())
		{
			Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>();
			Solver->EnqueueCommandImmediate([Proxy, this]() mutable
				{
					Proxy->GetParticle_Internal()->SetLinearEtherDrag(LinearDamping);
					Proxy->GetParticle_Internal()->SetAngularEtherDrag(AngularDamping);
				});
		}
	}

	if (RootPhysicsObject)
	{
		TArray<Chaos::FPhysicsObjectHandle> PhysicsObjects{ RootPhysicsObject };
		FLockedWritePhysicsObjectExternalInterface Interface = FPhysicsObjectExternalInterface::LockWrite(PhysicsObjects);
		Interface->SetLinearEtherDrag(PhysicsObjects, LinearDamping);
		Interface->SetAngularEtherDrag(PhysicsObjects, AngularDamping);
	}
}

void UModularVehicleBaseComponent::BeginPlay()
{
	Super::BeginPlay();

	const bool bHasAuthority = GetOwner()->HasAuthority();
	if (bHasAuthority)
	{
		if (bAutoAddComponentsFromWorld && (DelayClusteringCount == 0))
		{
			AddOverlappingComponentsToCluster();
		}
		else
		{
			AddComponentsFromOwnedActor();
		}
	}

	//AddSimulationComponentsFromOwnedActor();
	
	// control input setup - unfortunately can't do this in OnCreatePhysics since RootComponent->GetChildrenComponents will not work 
	// at that time and AssimilateComponentInputs will not find any controls in the component hierarchy
	TArray<FModuleInputSetup> CombinedInputConfiguration;
	AssimilateComponentInputs(CombinedInputConfiguration);

	if (!InputProducer && InputProducerClass)
	{
		InputProducer = NewObject<UVehicleInputProducerBase>(this, InputProducerClass);
	}

	if (InputProducer)
	{
		InputProducer->InitializeContainer(CombinedInputConfiguration, InputNameMap);
		if (InputProducer->GetTestInputBuffer())
		{
			// deliberate deep copy rather than copy pointer
			VehicleSimulationPT->SetTestInputBuffer(*InputProducer->GetTestInputBuffer(), InputProducer->IsLoopingTestInputBuffer());
		}
	}

	InputsContainer.Initialize(CombinedInputConfiguration, InputNameMap);
	StateInputContainer.Initialize(StateInputConfiguration, StateNameMap);

	if (!bUsingNetworkPhysicsPrediction)
	{
		ReplicatedState.Container = InputsContainer;
	}
	// #TODO reinstate ? GenerateInputModifiers(CombinedInputConfiguration);

	if (VehicleSimulationPT)
	{
		VehicleSimulationPT->SetInputMappings(InputNameMap);
		VehicleSimulationPT->SetStateMappings(StateNameMap);
	}

}

void UModularVehicleBaseComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const bool bHasAuthority = GetOwner()->HasAuthority();
	if (bAutoAddComponentsFromWorld && bHasAuthority && (++ClusteringCount == DelayClusteringCount))
	{
		AddOverlappingComponentsToCluster();
	}

}


int32 UModularVehicleBaseComponent::FindComponentAddOrder(USceneComponent* InComponent)
{
	int32 ComponentAddOrder = INDEX_NONE;
	for (const FConstructionData& Data : ConstructionDatas)
	{
		if (Data.Component == InComponent)
		{
			ComponentAddOrder = Data.ConstructionIndex;
			break;
		}
	}
	return ComponentAddOrder;
}

bool UModularVehicleBaseComponent::FindAndRemoveNextPendingUpdate(int32 NextIndex, Chaos::FSimTreeUpdates* OutData)
{
	bool bFound = false;
	for (const TPair<TObjectKey<USceneComponent>, Chaos::FSimTreeUpdates>& Kvp : PendingTreeUpdates)
	{
		if (FindComponentAddOrder(Kvp.Key.ResolveObjectPtr()) == NextIndex)
		{
			*OutData = PendingTreeUpdates.FindAndRemoveChecked(Kvp.Key.ResolveObjectPtr());
			bFound = true;
			break;
		}
	}

	return bFound;
}

void UModularVehicleBaseComponent::AddActorsToIgnore(TArray<AActor*>& ActorsIn)
{
	for (AActor* Actor : ActorsIn)
	{
		ActorsToIgnore.AddUnique(Actor);
	}
}

void UModularVehicleBaseComponent::RemoveActorsToIgnore(TArray<AActor*>& ActorsIn)
{
	for (AActor* Actor : ActorsIn)
	{
		ActorsToIgnore.Remove(Actor);
	}
}

void UModularVehicleBaseComponent::PreTickGT(float DeltaTime)
{
	if (PendingTreeUpdates.Num() > 0)
	{
		Chaos::FSimTreeUpdates Updates;
		while (FindAndRemoveNextPendingUpdate(LastComponentAddIndex + 1, &Updates))
		{
			ActionTreeUpdates(&Updates);
			LastComponentAddIndex++;
		}
	}

	// process control inputs and other data
	UpdateState(DeltaTime);

}

void UModularVehicleBaseComponent::UpdateState(float DeltaTime)
{
	// update input values
	bool bProcessLocally = IsLocallyControlled();

	// IsLocallyControlled will fail if the owner is unpossessed (i.e. Controller == nullptr);
	// Should we remove input instead of relying on replicated state in that case?
	if (bProcessLocally && PVehicleOutput)
	{
		//ApplyInputModifiers(DeltaTime, RawInputsContainer); #TODO: If we put this back where does it go

		if (!bUsingNetworkPhysicsPrediction)
		{
			// and send to server - (ServerUpdateState_Implementation below)
			ServerUpdateState(InputsContainer, bKeepVehicleAwake);
		}

		if (PawnOwner && PawnOwner->IsNetMode(NM_Client))
		{
			MarkForClientCameraUpdate();
		}
	}
	else if (!bUsingNetworkPhysicsPrediction)
	{
		// use replicated values for remote pawns
		InputsContainer = ReplicatedState.Container;
		bKeepVehicleAwake = ReplicatedState.KeepAwake;
	}
}

bool UModularVehicleBaseComponent::ServerUpdateState_Validate(const FModuleInputContainer& InputsIn, bool KeepAwake)
{
	return true;
}

void UModularVehicleBaseComponent::ServerUpdateState_Implementation(const FModuleInputContainer& InputsIn, bool KeepAwake)
{
	// update state of inputs
	ReplicatedState.KeepAwake = KeepAwake;
	ReplicatedState.Container = InputsIn;
}


TUniquePtr<FModularVehicleAsyncInput> UModularVehicleBaseComponent::SetCurrentAsyncData(int32 InputIdx, FChaosSimModuleManagerAsyncOutput* CurOutput, FChaosSimModuleManagerAsyncOutput* NextOutput, float Alpha, int32 VehicleManagerTimestamp)
{
	TUniquePtr<FModularVehicleAsyncInput> CurInput = MakeUnique<FModularVehicleAsyncInput>();
	SetCurrentAsyncDataInternal(CurInput.Get(), InputIdx, CurOutput, NextOutput, Alpha, VehicleManagerTimestamp);
	return CurInput;
}

/************************************************************************/
/* Setup the current async I/O data                                     */
/************************************************************************/
void UModularVehicleBaseComponent::SetCurrentAsyncDataInternal(FModularVehicleAsyncInput* CurInput, int32 InputIdx, FChaosSimModuleManagerAsyncOutput* CurOutput, FChaosSimModuleManagerAsyncOutput* NextOutput, float Alpha, int32 VehicleManagerTimestamp)
{
	ensure(CurAsyncInput == nullptr);	//should be reset after it was filled
	ensure(CurAsyncOutput == nullptr);	//should get reset after update is done

	CurAsyncInput = CurInput;
	CurAsyncInput->SetVehicle(this);
	NextAsyncOutput = nullptr;
	OutputInterpAlpha = 0.f;

	// We need to find our vehicle in the output given
	if (CurOutput)
	{
		for (int32 PendingOutputIdx = 0; PendingOutputIdx < OutputsWaitingOn.Num(); ++PendingOutputIdx)
		{
			// Found the correct pending output, use index to get the vehicle.
			if (OutputsWaitingOn[PendingOutputIdx].Timestamp == CurOutput->Timestamp)
			{
				const int32 VehicleIdx = OutputsWaitingOn[PendingOutputIdx].Idx;
				FModularVehicleAsyncOutput* VehicleOutput = CurOutput->VehicleOutputs[VehicleIdx].Get();
				if (VehicleOutput && VehicleOutput->bValid && VehicleOutput->Type == CurAsyncType)
				{
					CurAsyncOutput = VehicleOutput;

					if (NextOutput && NextOutput->Timestamp == CurOutput->Timestamp)
					{
						// This can occur when sub-stepping - in this case, VehicleOutputs will be in the same order in NextOutput and CurOutput.
						FModularVehicleAsyncOutput* VehicleNextOutput = NextOutput->VehicleOutputs[VehicleIdx].Get();
						if (VehicleNextOutput && VehicleNextOutput->bValid && VehicleNextOutput->Type == CurAsyncType)
						{
							NextAsyncOutput = VehicleNextOutput;
							OutputInterpAlpha = Alpha;
						}
					}
				}

				// these are sorted by timestamp, we are using latest, so remove entries that came before it.
				TArray<FAsyncOutputWrapper> NewOutputsWaitingOn;
				for (int32 CopyIndex = PendingOutputIdx; CopyIndex < OutputsWaitingOn.Num(); ++CopyIndex)
				{
					NewOutputsWaitingOn.Add(OutputsWaitingOn[CopyIndex]);
				}

				OutputsWaitingOn = MoveTemp(NewOutputsWaitingOn);
				break;
			}
		}

	}

	if (NextOutput && CurOutput)
	{
		if (NextOutput->Timestamp != CurOutput->Timestamp)
		{
			// NextOutput and CurOutput occurred in different steps, so we need to search for our specific vehicle.
			for (int32 PendingOutputIdx = 0; PendingOutputIdx < OutputsWaitingOn.Num(); ++PendingOutputIdx)
			{
				// Found the correct pending output, use index to get the vehicle.
				if (OutputsWaitingOn[PendingOutputIdx].Timestamp == NextOutput->Timestamp)
				{
					FModularVehicleAsyncOutput* VehicleOutput = NextOutput->VehicleOutputs[OutputsWaitingOn[PendingOutputIdx].Idx].Get();
					if (VehicleOutput && VehicleOutput->bValid && VehicleOutput->Type == CurAsyncType)
					{
						NextAsyncOutput = VehicleOutput;
						OutputInterpAlpha = Alpha;
					}
					break;
				}
			}
		}
	}

	FAsyncOutputWrapper& NewOutput = OutputsWaitingOn.AddDefaulted_GetRef();
	NewOutput.Timestamp = VehicleManagerTimestamp;
	NewOutput.Idx = InputIdx;
}

IPhysicsProxyBase* UModularVehicleBaseComponent::GetPhysicsProxy() const 
{ 
	if (CachedPhysicsProxy)
	{
		return CachedPhysicsProxy;
	}

	// ClusterUnionPhysicsProxy case
	if (ClusterUnionComponent)
	{
		return CachedPhysicsProxy = ClusterUnionComponent->GetPhysicsProxy();
	}
	
	// SingleParticlePhysicsProxy case
	if (const FBodyInstance* BodyInstance = UpdatedPrimitive ? UpdatedPrimitive->GetBodyInstance() : nullptr)
	{
		return CachedPhysicsProxy = BodyInstance->GetPhysicsActor();
	}

	return nullptr;
}


void UModularVehicleBaseComponent::SetClusterComponent(UClusterUnionComponent* InPhysicalComponent)
{
	ClusterUnionComponent = InPhysicalComponent;
}

/***************************************************************************/
/* READ OUTPUT DATA - Access the async output data from the Physics Thread */
/***************************************************************************/
void UModularVehicleBaseComponent::ParallelUpdate(float DeltaTime)
{
	if (FModularVehicleAsyncOutput* CurrentOutput = static_cast<FModularVehicleAsyncOutput*>(CurAsyncOutput))
	{
		if (CurrentOutput->bValid && PVehicleOutput)
		{
			PVehicleOutput->Clean();
			int NumItems = CurrentOutput->VehicleSimOutput.SimTreeOutputData.Num();
			PVehicleOutput->SimTreeOutputData.Reserve(NumItems);

			if (const FModularVehicleAsyncOutput* NextOutput = static_cast<FModularVehicleAsyncOutput*>(NextAsyncOutput))
			{
				for (int I = 0; I < NumItems; I++)
				{
					if (I < NextOutput->VehicleSimOutput.SimTreeOutputData.Num())
					{
						Chaos::FSimOutputData* CurrentSimData = CurrentOutput->VehicleSimOutput.SimTreeOutputData[I];
						Chaos::FSimOutputData* NextSimData = NextOutput->VehicleSimOutput.SimTreeOutputData[I];
						PVehicleOutput->SimTreeOutputData.EmplaceAt(I, CurrentSimData->MakeNewData());
						PVehicleOutput->SimTreeOutputData[I]->Lerp(*CurrentSimData, *NextSimData, OutputInterpAlpha);
					}
					else
					{
						Chaos::FSimOutputData* CurrentSimData = CurrentOutput->VehicleSimOutput.SimTreeOutputData[I];
						PVehicleOutput->SimTreeOutputData.EmplaceAt(I, CurrentSimData->MakeNewData());
						*PVehicleOutput->SimTreeOutputData[I] = *CurrentSimData;
					}
				}
			}
			else
			{
				for (int I = 0; I < NumItems; I++)
				{
					Chaos::FSimOutputData* CurrentSimData = CurrentOutput->VehicleSimOutput.SimTreeOutputData[I];
					PVehicleOutput->SimTreeOutputData.EmplaceAt(I, CurrentSimData->MakeNewData());
					*PVehicleOutput->SimTreeOutputData[I] = *CurrentSimData;
				}
			}


			for (int I = 0; I < NumItems; I++)
			{
				// extract/cache some generally useful values as we go as trying to locate this data later requires a search
				if (PVehicleOutput->SimTreeOutputData[I]->IsSimType<Chaos::FTransmissionSimModule>())
				{
					// if there is more than one transmission then the last one will inform us of the current gear
					CurrentGear = static_cast<Chaos::FTransmissionOutputData*>(PVehicleOutput->SimTreeOutputData[I])->CurrentGear;
				}
				else if (PVehicleOutput->SimTreeOutputData[I]->IsSimType<Chaos::FEngineSimModule>())
				{
					// if there is more than one engine then the last one will inform us of the engine RPM
					Chaos::FEngineOutputData* Engine = static_cast<Chaos::FEngineOutputData*>(CurrentOutput->VehicleSimOutput.SimTreeOutputData[I]);

					EngineRPM = Engine->RPM;
					EngineTorque = Engine->Torque;
				}


				if (Chaos::FSimOutputData* ModuleOutput = PVehicleOutput->SimTreeOutputData[I])
				{
					const Chaos::FSimModuleAnimationData& AnimData = ModuleOutput->AnimationData;
					int Index = AnimData.AnimationSetupIndex;
					if ((Index >= 0) && (Index < ModuleAnimationSetups.Num()))
					{
						ModuleAnimationSetups[Index].AnimFlags |= AnimData.AnimFlags;
						ModuleAnimationSetups[Index].CombinedRotation = AnimData.CombinedRotation;

						if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimateRotation)
						{
							ModuleAnimationSetups[Index].RotOffset = AnimData.AnimationRotOffset;
						}

						if (AnimData.AnimFlags & Chaos::EAnimationFlags::AnimatePosition)
						{
							ModuleAnimationSetups[Index].LocOffset = AnimData.AnimationLocOffset;
						}
					}
				}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
				if (PVehicleOutput && !PVehicleOutput->SimTreeOutputData.IsEmpty() && PVehicleOutput->SimTreeOutputData[I])
				{
					PVehicleOutput->SimTreeOutputData[I]->DebugString = CurrentOutput->VehicleSimOutput.SimTreeOutputData[I]->DebugString;
				}
#endif

			}


		}
	}
}


/************************************************************************/
/* PASS ANY INPUTS TO THE PHYSICS THREAD SIMULATION IN HERE             */
/************************************************************************/
void UModularVehicleBaseComponent::Update(float DeltaTime)
{
	if (CurAsyncInput && GetPhysicsProxy())
	{

		CurAsyncInput->Proxy = GetPhysicsProxy();

		FModularVehicleAsyncInput* AsyncInput = static_cast<FModularVehicleAsyncInput*>(CurAsyncInput);

		AsyncInput->PhysicsInputs.NetworkInputs.VehicleInputs.KeepAwake = bKeepVehicleAwake;

		// All control inputs
		AsyncInput->PhysicsInputs.NetworkInputs.VehicleInputs.Container = InputsContainer;

		FCollisionQueryParams TraceParams(NAME_None, FCollisionQueryParams::GetUnknownStatId(), false, nullptr);
		TraceParams.bReturnPhysicalMaterial = true;	// we need this to get the surface friction coefficient
		TraceParams.AddIgnoredActors(ActorsToIgnore);
		TraceParams.bTraceComplex = bSuspensionTraceComplex;
		AsyncInput->PhysicsInputs.CollisionChannel = SuspensionCollisionChannel;
		AsyncInput->PhysicsInputs.TraceParams = TraceParams;
		AsyncInput->PhysicsInputs.TraceCollisionResponse = SuspensionTraceCollisionResponses;
		AsyncInput->PhysicsInputs.TraceType = TraceType;

		// game state container
		AsyncInput->PhysicsInputs.StateInputs.StateInputContainer = StateInputContainer;
	}


}

void UModularVehicleBaseComponent::FinalizeSimCallbackData(FChaosSimModuleManagerAsyncInput& Input)
{
	CurAsyncInput = nullptr;
	CurAsyncOutput = nullptr;
}

const FTransform& UModularVehicleBaseComponent::GetComponentTransform() const
{
	return MyComponent->GetComponentTransform();
}


void UModularVehicleBaseComponent::ActionTreeUpdates(Chaos::FSimTreeUpdates* NextTreeUpdates)
{
	if (IPhysicsProxyBase* Proxy = GetPhysicsProxy())
	{
		if (Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>())
		{
			Solver->EnqueueCommandImmediate([Proxy, this, NextTreeUpdates = *NextTreeUpdates]() mutable
			{
				if (IsValidChecked(this) && bPhysicsStateCreated && VehicleSimulationPT)
				{
					TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();
					if(SimModuleTree.IsValid())
					{
						SimModuleTree->AppendTreeUpdates(NextTreeUpdates);
						FModularVehicleBuilder::FixupTreeLinks(SimModuleTree);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
						if (bModularVehicle_DumpModuleTreeStructure_Enabled)
						{
							UE_LOG(LogModularBase, Warning, TEXT("SimTreeModules:") );
							for (int I = 0; I < SimModuleTree->GetNumNodes(); I++)
							{
								if (Chaos::ISimulationModuleBase* Module = SimModuleTree->GetNode(I).SimModule)
								{
									FString String;
									Module->GetDebugString(String);
									UE_LOG(LogModularBase, Warning, TEXT("..%s"), *String);
								}
							}
						}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

						// Network replication data needs to be updated, this is currently studily slow
						if (NetworkPhysicsComponent)
						{
							TSharedPtr<Chaos::FBaseRewindHistory>& History = NetworkPhysicsComponent->GetStateHistory_Internal();
							Chaos::TDataRewindHistory<FNetworkModularVehicleStates>* StateHistory = static_cast<Chaos::TDataRewindHistory<FNetworkModularVehicleStates>*>(History.Get());
							if (StateHistory)
							{
								// #TODO: we are rebuilding from scratch every time there is a single change, there must be a better way!
								// not sure of it is safe to update the data at this time?
								for (int I = 0; I < StateHistory->GetDataHistory().Num(); I++)
								{
									FNetworkModularVehicleStates& State = StateHistory->GetDataHistory()[I];
									State.ModuleData.Empty();
									TUniquePtr<Chaos::FSimModuleTree>& InnerSimModuleTree = VehicleSimulationPT->AccessSimComponentTree();
									if(InnerSimModuleTree.IsValid())
									{
										InnerSimModuleTree->GenerateReplicationStructure(State.ModuleData);
									}
								}
							}
						}
					}
				}
			});
		}
	}
}

int32 UModularVehicleBaseComponent::FindParentsLastSimComponent(const USceneComponent* AttachedComponent)
{
	if (USceneComponent* AttachParent = AttachedComponent->GetAttachParent())
	{
		TArray<USceneComponent*> Children;
		AttachParent->GetChildrenComponents(false, Children);

		for (int32 ChildIndex = Children.Num() - 1; ChildIndex >= 0; --ChildIndex)
		{
			if (IVehicleSimBaseComponentInterface* ChildSimComponent = Cast<IVehicleSimBaseComponentInterface>(Children[ChildIndex]))
			{
				return ChildSimComponent->GetTreeIndex();
			}
		}
	}

	return INDEX_NONE;
}

void UModularVehicleBaseComponent::AddComponentToSimulation(UPrimitiveComponent* InComponent, const TArray<FClusterUnionBoneData>& BonesData, const TArray<FClusterUnionBoneData>& RemovedBoneIDs, bool bIsNew)
{
	AddComponentToSimulationImpl(InComponent, BonesData, RemovedBoneIDs, bIsNew);
}

void UModularVehicleBaseComponent::AddComponentToSimulationImpl(UPrimitiveComponent* InComponent, const TArray<FClusterUnionBoneData>& BonesData, const TArray<FClusterUnionBoneData>& RemovedBoneIDs, bool bIsNew)
{
	check(ClusterUnionComponent);

	int32 ComponentAddOrder = INDEX_NONE;
	if (InComponent && bIsNew)
	{
		UE_LOG(LogModularBase, Log, TEXT("AddComponentToSimulation %s, NetMode %d"), *InComponent->GetName(), InComponent->GetNetMode());
		
		if (ClusterUnionComponent->IsAuthority())
		{
			// retain the order that the components were constructed
			FConstructionData ConstructionData;
			ConstructionData.Component = InComponent;
			ConstructionData.ConstructionIndex = NextConstructionIndex;
			ConstructionDatas.Add(ConstructionData);
			ComponentAddOrder = NextConstructionIndex++;
		}
		else
		{
			ComponentAddOrder = FindComponentAddOrder(InComponent);
		}

		int ParentID = FindParentsLastSimComponent(InComponent);

		Chaos::FSimTreeUpdates LatestTreeUpdates;
		CreateAssociatedSimComponents(nullptr, InComponent, ParentID, NextTransformIndex, LatestTreeUpdates);

		Chaos::FSimTreeUpdates* NextTreeUpdates = nullptr;
		if (LastComponentAddIndex + 1 == ComponentAddOrder)
		{
			// has the next in line just come in..
			NextTreeUpdates = &LatestTreeUpdates;
			LastComponentAddIndex = ComponentAddOrder;
			ActionTreeUpdates(NextTreeUpdates);
		}
		else
		{
			// add to pending and action later
			PendingTreeUpdates.Add(InComponent, MoveTemp(LatestTreeUpdates));
		}

		int Size = 1; // for UStaticMeshComponent && UGeometryCollectionComponent cases
		if (USkeletalMeshComponent* SKMComponent = Cast<USkeletalMeshComponent>(InComponent))
		{
			Size = SKMComponent->Bodies.Num();
		}

		NextTransformIndex += Size;
	}
}

void UModularVehicleBaseComponent::RemoveComponentFromSimulation(UPrimitiveComponent* InComponent, const TArray<FClusterUnionBoneData>& RemovedBonesData)
{
	RemoveComponentFromSimulationImpl(InComponent, RemovedBonesData);
}

void UModularVehicleBaseComponent::RemoveComponentFromSimulationImpl(UPrimitiveComponent* InComponent, const TArray<FClusterUnionBoneData>& RemovedBonesData)
{
	if (InComponent && VehicleSimulationPT)
	{
		UE_LOG(LogModularBase, Log, TEXT("UModularVehicleBaseComponent::RemoveComponentFromSimulation: %s, NetMode %d"), *InComponent->GetFullName(), InComponent->GetNetMode());

		Chaos::FSimTreeUpdates LatestTreeUpdates;

		TArray<USceneComponent*> Components;
		InComponent->GetChildrenComponents(true, Components);

		for (USceneComponent* ComponentPart : Components)
		{
			if (IVehicleSimBaseComponentInterface* ChangedComponent = Cast<IVehicleSimBaseComponentInterface>(ComponentPart))
			{
				if (FVehicleComponentData* ComponentData = ComponentToPhysicsObjects.Find(ComponentPart))
				{
					LatestTreeUpdates.RemoveNode(ComponentData->Guid);
					ComponentToPhysicsObjects.Remove(ComponentPart);
				}
			}
		}

		TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();
		if(SimModuleTree.IsValid())
		{
			for (const Chaos::FPendingModuleDeletions& TreeUpdate : LatestTreeUpdates.GetDeletedModules())
			{
				for (int Index = 0; Index < SimModuleTree->GetNumNodes(); Index++)
				{
					if (Chaos::ISimulationModuleBase* SimModule = SimModuleTree->GetNode(Index).SimModule)
					{
						if (SimModule->GetGuid() == TreeUpdate.Guid)
						{
							SimModule->SetAnimationEnabled(false);
							SimModule->SetStateFlags(Chaos::eSimModuleState::Disabled);
							SimModule->OnTermination_External();
							break;
						}
					}
				}
			}
		}

		Chaos::FClusterUnionPhysicsProxy* Proxy = ClusterUnionComponent->GetPhysicsProxy();
		Chaos::FPBDRigidsSolver* Solver = Proxy->GetSolver<Chaos::FPBDRigidsSolver>();
		Solver->EnqueueCommandImmediate([Proxy, this, LatestTreeUpdates = LatestTreeUpdates]() mutable
			{
				if (IsValidChecked(this) && bPhysicsStateCreated && VehicleSimulationPT)
				{
					TUniquePtr<Chaos::FSimModuleTree>& SimModuleTree = VehicleSimulationPT->AccessSimComponentTree();
					if(SimModuleTree.IsValid())
					{
						SimModuleTree->AppendTreeUpdates(LatestTreeUpdates);
					}
				}
			});

		NextTransformIndex--;
	}

}

void UModularVehicleBaseComponent::SetLocallyControlled(bool bLocallyControlledIn)
{
	bIsLocallyControlled = false;
	if (UWorld* World = GetWorld())
	{
		// guard against invalid case that can lead to bad networking state
		if (GetOwner() && GetOwner()->GetLocalRole() != ENetRole::ROLE_SimulatedProxy)
		{
			bIsLocallyControlled = bLocallyControlledIn;
		}
	}

	if (bUsingNetworkPhysicsPrediction && NetworkPhysicsComponent)
	{
		NetworkPhysicsComponent->SetIsRelayingLocalInputs(bIsLocallyControlled);
	}

}

void UModularVehicleBaseComponent::AssimilateComponentInputs(TArray<FModuleInputSetup>& OutCombinedInputs)
{
	// copy the input setup from this class
	OutCombinedInputs = InputConfig;

	// append the input setup from all module sim components attached to same actor
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (USceneComponent* RootComponent = Pawn->GetRootComponent())
		{
			TArray<USceneComponent*> ChildComponents;
			RootComponent->GetChildrenComponents(true, ChildComponents);

			for (USceneComponent* Component : ChildComponents)
			{
				if (IVehicleSimBaseComponentInterface* GCComponent = Cast<IVehicleSimBaseComponentInterface>(Component))
				{
					// don't add duplicates, i.e. 4 wheels could be looking for a single steering input
					for (FModuleInputSetup& Config : GCComponent->GetInputConfig())
					{
						if (OutCombinedInputs.Find(Config) == INDEX_NONE)
						{
							OutCombinedInputs.Append(GCComponent->GetInputConfig());
						}
					}
				}
			}
		}
	}
}

void UModularVehicleBaseComponent::CreateVehicleSim()
{
	UWorld* World = GetWorld();

	// register our vehicle with the modular vehicle Sim Manager
	if (World)
	{
		if (World->IsGameWorld())
		{
			// create the simulation class
			VehicleSimulationPT = MakeUnique<FModularVehicleSimulation>(bUsingNetworkPhysicsPrediction, (int8)World->GetNetMode());

			// create physics output container
			PVehicleOutput = MakeUnique<FPhysicsVehicleOutput>();

			// generate the simulation tree with initial components
			FModularVehicleBuilder::GenerateSimTree(this);

			FPhysScene* PhysScene = World->GetPhysicsScene();

			if (FChaosSimModuleManager* SimManager = FChaosSimModuleManager::GetManagerFromScene(PhysScene))
			{
				SimManager->AddVehicle(this);
			}
		}
	}

}

void UModularVehicleBaseComponent::DestroyVehicleSim()
{
	UWorld* World = GetWorld();
	if (World->IsGameWorld())
	{
		FPhysScene* PhysScene = World->GetPhysicsScene();
		check(PhysScene);

		if (Chaos::FPhysicsSolver* LocalSolver = PhysScene->GetSolver())
		{
			if (FChaosSimModuleManager* SimManager = FChaosSimModuleManager::GetManagerFromScene(PhysScene))
			{
				SimManager->RemoveVehicle(this);
			}

			if (PVehicleOutput.IsValid())
			{
				PVehicleOutput.Reset(nullptr);
			}

			if (VehicleSimulationPT)
			{
				VehicleSimulationPT->Terminate();
				VehicleSimulationPT.Reset(nullptr);
			}
		}

	}

}


void UModularVehicleBaseComponent::AddComponentToCluster(USceneComponent* Component)
{
	TArray<int32> BoneIds;

	if (UGeometryCollectionComponent* GCComponent = Cast<UGeometryCollectionComponent>(Component))
	{
		BoneIds.Add(0);

		UE_LOG(LogModularBase, Log, TEXT("AddComponentToCluster: Geometry Collection"));
		ClusterUnionComponent->AddComponentToCluster(GCComponent, BoneIds);
	}

	if (UStaticMeshComponent* SMComponent = Cast<UStaticMeshComponent>(Component))
	{
		if (SMComponent->HasValidPhysicsState())
		{
			BoneIds.Add(0);

			UE_LOG(LogModularBase, Log, TEXT("AddComponentToCluster: Static Mesh"));
			ClusterUnionComponent->AddComponentToCluster(SMComponent, BoneIds);
		}
	}

	if (USkeletalMeshComponent* SKMComponent = Cast<USkeletalMeshComponent>(Component))
	{
		for (int I = 0; I < SKMComponent->Bodies.Num(); I++)
		{
			BoneIds.Add(I);
		}
		if (BoneIds.Num() > 0)
		{
			UE_LOG(LogModularBase, Log, TEXT("AddComponentToCluster: Skeletal Mesh"));
			ClusterUnionComponent->AddComponentToCluster(SKMComponent, BoneIds);
		}
	}
}


void UModularVehicleBaseComponent::AddOverlappingComponentsToCluster()
{
	//if (UWorld* World = GetWorld())
	//{
	//	FVector HalfSize = AutoAddOverlappingBoxSize * 0.5f;
	//	FBox LocalBox = FBox(-HalfSize, HalfSize);
	//	TArray<FOverlapResult> OverlapResults;
	//	FCollisionShape CollisionBox;
	//	CollisionBox.SetBox((FVector3f)LocalBox.GetExtent());

	//	const FCollisionQueryParams QueryParams;
	//	const FCollisionResponseParams ResponseParams;
	//	const ECollisionChannel CollisionChannel = ECollisionChannel::ECC_WorldDynamic;
	//	const bool bOverlapHit = World->OverlapMultiByChannel(OverlapResults, GetActorLocation(), FQuat::Identity, CollisionChannel, CollisionBox, QueryParams, ResponseParams);

	//	TArray<int32> BoneIds;
	//	BoneIds.Add(0);
	//	// Test each overlapped object for a hit result
	//	for (FOverlapResult OverlapResult : OverlapResults)
	//	{
	//		if (UGeometryCollectionComponent* OverlapGCComponent = Cast<UGeometryCollectionComponent>(OverlapResult.Component.Get()))
	//		{
	//			ClusterUnionComponent->AddComponentToCluster(OverlapGCComponent, BoneIds);
	//		}
	//	}
	//}
}

bool UModularVehicleBaseComponent::AddComponentsFromOwnedActor()
{
	bool bSuccess = false;

	TArray<int32> BoneIds;
	BoneIds.Add(0);

	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		if (UClusterUnionComponent* ClusterUnion = Pawn->GetComponentByClass<UClusterUnionComponent>())
		{
			TArray<USceneComponent*> ChildComponents;
			ClusterUnion->GetChildrenComponents(true, ChildComponents);

			for (USceneComponent* Component : ChildComponents)
			{
				AddComponentToCluster(Component);
			}
		}
	}

	return bSuccess;
}

void UModularVehicleBaseComponent::AddSimulationComponentsFromOwnedActor()
{
	// get raw simulation components that are not connected with any component - required for scene graph
	if (APawn* Pawn = Cast<APawn>(GetOwner()))
	{
		USceneComponent* RootComponent = Pawn->GetRootComponent();
		TArray<USceneComponent*> ChildComponents;
		RootComponent->GetChildrenComponents(false, ChildComponents);

		int32 ComponentAddOrder = INDEX_NONE;
		int ParentIndex=-1;
		int TransformIndex=0;
		Chaos::FSimTreeUpdates LatestTreeUpdates;

		for (USceneComponent* Component : ChildComponents)
		{
			if (UVehicleSimBaseComponent* SimulationComponent = Cast<UVehicleSimBaseComponent>(Component))
			{
				CreateIndependentSimComponents(RootComponent, SimulationComponent, ParentIndex, TransformIndex, LatestTreeUpdates);
				NextTransformIndex++;
			}
		}

		Chaos::FSimTreeUpdates* NextTreeUpdates = nullptr;
		NextTreeUpdates = &LatestTreeUpdates;
		LastComponentAddIndex = ComponentAddOrder;
		ActionTreeUpdates(NextTreeUpdates);

	}
}

void UModularVehicleBaseComponent::SetInputProducerClass(TSubclassOf<UVehicleInputProducerBase> InInputProducerClass)
{
	InputProducerClass = InInputProducerClass;
	if (!InputProducer)
	{
		InputProducer = NewObject<UVehicleInputProducerBase>(this, InputProducerClass);
	}
}

void UModularVehicleBaseComponent::SetInputBool(const FName Name, const bool Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInputInteger(const FName Name, const int32 Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}


void UModularVehicleBaseComponent::SetInputAxis1D(const FName Name, const double Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInputAxis2D(const FName Name, const FVector2D Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInputAxis3D(const FName Name, const FVector Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInput(const FName& Name, const bool Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInput(const FName& Name, const double Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInput(const FName& Name, const FVector2D& Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetInput(const FName& Name, const FVector& Value)
{
	if (InputProducer)
	{
		InputProducer->BufferInput(InputNameMap, Name, Value);
	}
}

void UModularVehicleBaseComponent::SetState(const FName& InName, const bool InValue)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer);
	Inputs.SetBool(InName, InValue);
}

void UModularVehicleBaseComponent::SetState(const FName& InName, const int32 InValue)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer);
	Inputs.SetInteger(InName, InValue);
}

void UModularVehicleBaseComponent::SetState(const FName& InName, const double InValue)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer);
	Inputs.SetFloat(InName, InValue);
}

void UModularVehicleBaseComponent::SetState(const FName& InName, const FVector2D& InValue)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer);
	Inputs.SetVector2D(InName, InValue);
}

void UModularVehicleBaseComponent::SetState(const FName& InName, const FVector& InValue)
{
	FInputInterface Inputs(StateNameMap, StateInputContainer);
	Inputs.SetVector(InName, InValue);
}

void UModularVehicleBaseComponent::SetGearInput(int32 Gear)
{
	GearInput = Gear;
}

int32 UModularVehicleBaseComponent::GetCurrentGear()
{
	return CurrentGear;
}

bool UModularVehicleBaseComponent::IsReversing()
{
	return (GetCurrentGear() < 0);
}


//-=====================================================
// Networking Replication

void UModularVehicleBaseComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(UModularVehicleBaseComponent, ReplicatedState);
	DOREPLIFETIME(UModularVehicleBaseComponent, ConstructionDatas);
}

void UModularVehicleBaseComponent::ShowDebugInfo(AHUD* HUD, UCanvas* Canvas, const FDebugDisplayInfo& DisplayInfo, float& YL, float& YPos)
{
	UFont* RenderFont = GEngine->GetMediumFont();

	// draw input values
	Canvas->SetDrawColor(FColor::White);

	for (int I = 0; I < InputsContainer.GetNumInputs(); I++)
	{
		float Interpolated = InputsContainer.GetValueAtIndex(I).GetMagnitude();

		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("%s %3.2f"), *InputConfig[I].Name.ToString(), Interpolated), 4, YPos);
	}

	YPos += 10;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	for (Chaos::FSimOutputData* Data : PVehicleOutput->SimTreeOutputData)
	{
		YPos += Canvas->DrawText(RenderFont, FString::Printf(TEXT("%s"), *Data->ToString()), 4, YPos);
	}
#endif

}

void UModularVehicleBaseComponent::LogInputSetup()
{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	const FModuleInputContainer::FInputNameMap& NameMap = InputNameMap;

	for (auto& NamePair : NameMap)
	{ 
		UE_LOG(LogModularBase, Log, TEXT("Input: %s %d"), *NamePair.Key.ToString(), NamePair.Value);
	}
#endif //!(UE_BUILD_SHIPPING || UE_BUILD_TEST)
}
