// Copyright Epic Games, Inc. All Rights Reserved.


#include "ServerInstancedActorsSpawnerSubsystem.h"
#include "InstancedActorsComponent.h"
#include "InstancedActorsData.h"
#include "Engine/World.h"
#include "MassEntitySubsystem.h"
#include "InstancedActorsSettings.h"

//-----------------------------------------------------------------------------
// UServerInstancedActorsSpawnerSubsystem
//-----------------------------------------------------------------------------
bool UServerInstancedActorsSpawnerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	// @todo Add support for non-replay NM_Standalone where we should use UServerInstancedActorsSpawnerSubsystem for 
	// authoritative actor spawning.
	UWorld* World = Cast<UWorld>(Outer);
	return (World != nullptr && World->GetNetMode() != NM_Client);
}

bool UServerInstancedActorsSpawnerSubsystem::ReleaseActorToPool(AActor* Actor)
{
	return Super::ReleaseActorToPool(Actor);
}

void UServerInstancedActorsSpawnerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UMassEntitySubsystem* EntitySubsystem = Collection.InitializeDependency<UMassEntitySubsystem>();
	check(EntitySubsystem);
	EntityManager = EntitySubsystem->GetMutableEntityManager().AsShared();
}

void UServerInstancedActorsSpawnerSubsystem::Deinitialize()
{
	EntityManager.Reset();

	Super::Deinitialize();
}

ESpawnRequestStatus UServerInstancedActorsSpawnerSubsystem::SpawnActor(FConstStructView SpawnRequestView, TObjectPtr<AActor>& OutSpawnedActor, FActorSpawnParameters& InOutSpawnParameters) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UServerInstancedActorsSpawnerSubsystem::SpawnActor);

	UWorld* World = GetWorld();
	check(World);
	check(World->GetNetMode() != NM_Client);

	const FMassActorSpawnRequest& SpawnRequest = SpawnRequestView.Get<const FMassActorSpawnRequest>();
	UInstancedActorsData* InstanceData = UInstancedActorsData::GetInstanceDataForEntity(*EntityManager, SpawnRequest.MassAgent);
	check(InstanceData);
	const FInstancedActorsInstanceIndex InstanceIndex = InstanceData->GetInstanceIndexForEntity(SpawnRequest.MassAgent);
	const FInstancedActorsInstanceHandle InstanceHandle(*InstanceData, InstanceIndex);

	// Record currently spawning IA instance for OnInstancedActorComponentInitialize to check
	TransientActorSpawningInstance = InstanceHandle;
	ON_SCOPE_EXIT 
	{
		TransientActorBeingSpawned = nullptr;
		TransientActorSpawningInstance.Reset();
	};

	InOutSpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	// we’re going to call FinishSpawning only if the input parameters don’t indicate that the caller wants to handle it themselves.
	const bool bCallFinishSpawning = (InOutSpawnParameters.bDeferConstruction == false);
	// we always defer construction to have a chance to configure the UInstancedActorsComponent instances
	// before their InitializeComponent gets called. From the callers point of view nothing changes.
	InOutSpawnParameters.bDeferConstruction = true;

	OutSpawnedActor = World->SpawnActor<AActor>(SpawnRequest.Template, SpawnRequest.Transform, InOutSpawnParameters);
	if (ensureMsgf(OutSpawnedActor, TEXT("Failed to spawn actor of class %s"), *GetNameSafe(SpawnRequest.Template.Get())))
	{
		// @todo this is a temporary solution, the whole idea is yucky and needs to be reimplemented.
		// Before this addition TransientActorBeingSpawned was only being set in Juno's custom 
		// InOutSpawnParameters.CustomPreSpawnInitalization delegate
		TransientActorBeingSpawned = OutSpawnedActor;

		// Add an UInstancedActorsComponent if one isn't present and ensure replication is enabled to replicate the InstanceHandle 
		// to clients for Mass entity matchup in UInstancedActorsComponent::OnRep_InstanceHandle
		UInstancedActorsComponent* InstancedActorComponent = OutSpawnedActor->GetComponentByClass<UInstancedActorsComponent>();
		if (InstancedActorComponent)
		{
			// If the component is set to replicate by default, we assume AddComponentTypesAllowListedForReplication has 
			// already been performed.
			if (!InstancedActorComponent->GetIsReplicated())
			{
				InstancedActorComponent->SetIsReplicated(true);
			}
		}
		else
		{
			// No existing UInstancedActorsComponent class or subclass, add a new UInstancedActorsComponent
			InstancedActorComponent = NewObject<UInstancedActorsComponent>(OutSpawnedActor);
			if (OutSpawnedActor->GetIsReplicated() == false)
			{
				OutSpawnedActor->SetReplicates(true);
			}
			InstancedActorComponent->SetIsReplicated(true);
			InstancedActorComponent->RegisterComponent();
		}

		if (bCallFinishSpawning)
		{
			OutSpawnedActor->FinishSpawning(SpawnRequest.Transform);
		}
	}
	
	return IsValid(OutSpawnedActor) ? ESpawnRequestStatus::Succeeded : ESpawnRequestStatus::Failed;
}

void UServerInstancedActorsSpawnerSubsystem::OnInstancedActorComponentInitialize(UInstancedActorsComponent& InstancedActorComponent) const
{
	// Does this component belong to an actor we're in the middle of spawning in UServerInstancedActorsSpawnerSubsystem::SpawnActor?
	//
	// Note: This may not always be the case, as OnInstancedActorComponentInitialize is called by UInstancedActorsComponent::InitializeComponent 
	// regardless of whether the actor was spawned by Instanced Actors or not, as we can't yet know if it was (this callback is in place to
	// *attempt* to find that out). Actors using UInstancedActorComponents aren't 'required' to be spawned with Instanced Actors, rather: the
	// components are expected to provide functionality without Mass and simply provide *additional* ability to continue their functionality once
	// the actor is 'dehydrated' into the lower LOD representation in Mass.
	if (InstancedActorComponent.GetOwner() == TransientActorBeingSpawned)
	{
		// Pass the IA instance responsible for spawning this actor. Importantly the UInstancedActorsComponent will now have a link
		// to Mass before / by the time it receives BeginPlay.
		check(TransientActorSpawningInstance.IsValid());
		InstancedActorComponent.InitializeComponentForInstance(TransientActorSpawningInstance);
	}
}
