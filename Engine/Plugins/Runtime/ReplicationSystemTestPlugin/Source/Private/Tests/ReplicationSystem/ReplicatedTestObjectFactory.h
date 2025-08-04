// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Iris/ReplicationSystem/NetObjectFactory.h"

#include "ReplicatedTestObjectFactory.generated.h"

namespace UE::Net
{

class FReplicationTestCreationHeader : public FNetObjectCreationHeader
{
public:
	FString ArchetypeName;
	uint32 NumComponentsToSpawn = 0;
	uint32 NumIrisComponentsToSpawn = 0;
	uint32 NumDynamicComponentsToSpawn = 0;
	uint32 NumConnectionFilteredComponentsToSpawn = 0;
	uint32 NumObjectReferenceComponentsToSpawn = 0;
	bool bForceFailCreateRemoteInstance = false;

	virtual FString ToString() const override;

	bool Serialize(const FCreationHeaderContext& Context) const;
	bool Deserialize(const FCreationHeaderContext& Context);
};

} // end namespace UE::Net

UCLASS()
class UReplicatedTestObjectFactory : public UNetObjectFactory
{
	GENERATED_BODY()

public:

	static FName GetFactoryName();

	virtual FInstantiateResult InstantiateReplicatedObjectFromHeader(const FInstantiateContext& Context, const UE::Net::FNetObjectCreationHeader* InHeader) override;

	// UNetObjectFactory interface
	virtual void PostInit(const FPostInitContext& Context) override;
	virtual void DestroyReplicatedObject(const FDestroyedContext& Context) override;
	virtual void GetWorldInfo(const FWorldInfoContext& Context, FWorldInfoData& OutData) override;
	virtual void SubObjectCreatedFromReplication(UE::Net::FNetRefHandle RootObject, UE::Net::FNetRefHandle SubObjectCreated) override;
	virtual void SubObjectDestroyedFromReplication(const FDestroyedContext& Context) override;

	//------------------------------------------------------------------------

	void SetCreatedObjectsOnNode(TArray<TStrongObjectPtr<UObject>>* InCreatedObjectsOnNode)
	{
		CreatedObjectsOnNode = InCreatedObjectsOnNode;
	}

	//------------------------------------------------------------------------

	void SetWorldUpdateFunctor(TFunction<void(UE::Net::FNetRefHandle, const UObject*, FVector&, float&)> LocUpdateFunctor)
	{
		GetInstanceWorldObjectInfoFunction = LocUpdateFunctor;
	}

public:

	TArray<TStrongObjectPtr<UObject>>* CreatedObjectsOnNode = nullptr;

protected:

	virtual TUniquePtr<UE::Net::FNetObjectCreationHeader> CreateAndFillHeader(UE::Net::FNetRefHandle Handle) override;
	
	virtual bool SerializeHeader(const UE::Net::FCreationHeaderContext& Context, const UE::Net::FNetObjectCreationHeader* Header) override;

	virtual TUniquePtr<UE::Net::FNetObjectCreationHeader> CreateAndDeserializeHeader(const UE::Net::FCreationHeaderContext& Context) override;

private:

	TFunction<void(UE::Net::FNetRefHandle, const UObject*, FVector&, float&)> GetInstanceWorldObjectInfoFunction;
};

	

