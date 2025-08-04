// Copyright Epic Games, Inc. All Rights Reserved.

#include "ReplicationSystemServerClientTestFixture.h"
#include "HAL/IConsoleManager.h"
#include "Iris/Core/IrisLog.h"
#include "Iris/Metrics/NetMetrics.h"
#include "Iris/ReplicationSystem/ReplicationRecord.h"
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#include "Iris/ReplicationSystem/ReplicationSystemInternal.h"
#include "Iris/Serialization/NetBitStreamReader.h"
#include "Iris/Serialization/NetBitStreamWriter.h"
#include "Logging/LogScopedVerbosityOverride.h"
#include "Misc/ScopeExit.h"
#include "Net/Core/NetToken/NetToken.h"

namespace UE::Net::Private
{

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySingleObject)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0,0);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server handle now also exists on client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObject->NetRefHandle));

	// Destroy the spawned object on server
	Server->DestroyObject(ServerObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that object now is destroyed on client as well
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObject->NetRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDroppedDestroyed)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0,0);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server handle now also exists on client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObject->NetRefHandle));

	// Destroy the spawned object on server
	Server->DestroyObject(ServerObject);

	// Send and drop packet
	Server->UpdateAndSend({Client}, DoNotDeliverPacket);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that object now is destroyed on client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObject->NetRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDestroyWhilePendingCreate)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0,0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;

	// Send and drop packet
	Server->UpdateAndSend({Client}, DoNotDeliverPacket);

	// Destroy
	Server->DestroyObject(ServerObject);

	// Verify that the object does not exist on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));

	// Send and drop packet
	Server->UpdateAndSend({Client}, DoNotDeliverPacket);

	// Send and deliver packet
	Server->UpdateAndSend({Client}, DeliverPacket);

	// Verify the object still doesn't exist on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDestroyWhileWaitingOnCreate)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0,0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;

	// Write packet with create
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Destroy while we are waiting for confirmation
	Server->DestroyObject(ServerObject);

	// Write packet with destroy
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Drop packet with create
	Server->DeliverTo(Client, DoNotDeliverPacket);

	// Verify that the object does not exists on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));

	// Deliver packet with destroy
	Server->DeliverTo(Client, DeliverPacket);

	// Verify the object still doesn't exist on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDestroyWithDataInFlight)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	Server->GetReplicationSystem()->SetStaticPriority(ServerObjectRefHandle, 1.f);

	// Send packet with create
	Server->UpdateAndSend({Client});

	// Verify that the object exists on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));

	// Modify some data to mark object dirty
	ServerObject->IntA = 13;

	// Write a packet with updated data
	Server->NetUpdate();
	UE_NET_ASSERT_TRUE(Server->SendTo(Client));
	Server->PostSendUpdate();

	// Destroy while we are waiting for ack on update
	Server->DestroyObject(ServerObject);

	// Write packet with destroy
	Server->NetUpdate();
	UE_NET_ASSERT_TRUE(Server->SendTo(Client));
	Server->PostSendUpdate();

	// Drop and report packet with update as lost
	Server->DeliverTo(Client, DoNotDeliverPacket);

	// Verify that the object still exists on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	
	// Deliver packet with destroy
	Server->DeliverTo(Client, DeliverPacket);

	// Verify that the object is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySubObject)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0,0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn second object on server as a subobject
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server objects now also exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that only the subobject is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroyMultipleSubObjects)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn subobject on server
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server objects now also exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that the subobject is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));

	// Spawn second object on server as a subobject
	ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that second subobject replicated properly to server
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned object on server
	Server->DestroyObject(ServerSubObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that the second subobjects object is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySubObjectAndDestroyOwner)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn subobject on server
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server handles now also exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// Destroy owner after spawned subobject on server
	Server->DestroyObject(ServerObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that the subobject is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));

	// Verify that the root object is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySubObjectAndDestroyOwnerWithDataInFlight)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn subobject on server
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server handles now also exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// Send and drop packet
	Server->UpdateAndSend({Client}, DoNotDeliverPacket);

	// Destroy owner after we spawned subobject on server
	Server->DestroyObject(ServerObject);

	// Send and deliver packet
	Server->UpdateAndSend({Client}, DeliverPacket);

	// Verify that the subobject is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySubObjectWithLostData)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn subobject on server
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that created server objects exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// Send and drop packet
	Server->UpdateAndSend({Client}, DoNotDeliverPacket);

	// Send and deliver packet
	Server->UpdateAndSend({Client}, DeliverPacket);

	// Verify that subobject is destroyed on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, ReplicateAndDestroySubObjectPendingCreateConfirmation)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;
	// Spawn subobject on server
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	uint32 NumUnAcknowledgedPackets = 0;
	// Write a packet
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Destroy the spawned subobject on server
	Server->DestroyObject(ServerSubObject);

	// We have no data to send but we want to tick ReplicationSystem to capture state change
	Server->NetUpdate();
	UE_NET_ASSERT_FALSE(Server->SendTo(Client));
	Server->PostSendUpdate();

	// Drop creation packet
	Server->DeliverTo(Client, DoNotDeliverPacket);

	// As the second update did not send any data we do not have anything to deliver
	//Server->DeliverTo(Client, true);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that the subobject does not exist on the client
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));

	// The root object should exist on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
}

// In this test we're going to create a subobject after the root has been created on the client. Then create it with a bit of latency and destroy root prior to subobject being created on the client.
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, LateCreatedSubObjectIsDestroyedWithRoot)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(UTestReplicatedIrisObject::FComponents{});
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));

	// Spawn subobject
	UReplicatedTestObject* ServerSubObject = Server->CreateSubObject(ServerObjectRefHandle, UTestReplicatedIrisObject::FComponents{});
	const FNetRefHandle ServerSubObjectRefHandle = ServerSubObject->NetRefHandle;

	// Put subobject in WaitOnCreateConfirmation state
	Server->NetUpdate();
	Server->SendUpdate(Client->ConnectionIdOnServer);
	Server->PostSendUpdate();

	// Destroy the root object
	Server->DestroyObject(ServerObject);

	// Write a packet
	Server->NetUpdate();
	Server->SendUpdate(Client->ConnectionIdOnServer);
	Server->PostSendUpdate();

	// Deliver first packet
	Server->DeliverTo(Client, DeliverPacket);

	// Verify the root and subobject are created/still created on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Deliver second packet
	Server->DeliverTo(Client, DeliverPacket);

	// Verify the root and subobject are created/still created on the client
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerSubObjectRefHandle));

	// Update and send
	Server->UpdateAndSend({Client});

	// Verify the root and subobject are fully destroyed
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerSubObjectRefHandle));
}

// In this test we're going to try destroying an object with thousands of subobjects atomically.
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, CanDestroyObjectHierarchyAtomically)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(UTestReplicatedIrisObject::FComponents{});
	const FNetRefHandle ServerObjectRefHandle = ServerObject->NetRefHandle;

	constexpr unsigned SubObjectCount = 2001;
	TArray<FNetRefHandle> ServerSubObjectRefHandles;
	ServerSubObjectRefHandles.Reserve(SubObjectCount);

	// Spawn thousands of subobjects over several frames to avoid huge object path.
	for (;ServerSubObjectRefHandles.Num() < SubObjectCount;)
	{
		constexpr uint32 MaxSubObjectCreationCountPerFrame = 15;
		for (unsigned It = 0; It < MaxSubObjectCreationCountPerFrame && ServerSubObjectRefHandles.Num() < SubObjectCount; ++It)
		{
			UReplicatedTestObject* ServerSubObject = Server->CreateSubObject<UReplicatedTestObject>(ServerObjectRefHandle);
			ServerSubObjectRefHandles.Add(ServerSubObject->NetRefHandle);
		}

		Server->UpdateAndSend({Client});
	}

	// We expect to be done creating the object hierarchy by now. Make sure of it.
	UE_NET_ASSERT_FALSE(Server->UpdateAndSend({ Client }));
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(ServerObjectRefHandle));
	for (const FNetRefHandle SubObjectRefHandle : ServerSubObjectRefHandles)
	{
		UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(SubObjectRefHandle));
	}

	// Destroy the root object
	Server->DestroyObject(ServerObject);

	for (unsigned It = 0, MaxTryCount = SubObjectCount/50; It < MaxTryCount; ++It)
	{
		const bool bDidSendSomething = Server->UpdateAndSend({ Client });

		// Verify the object hierarchy is destroyed as a whole or not at all. Once we've stopped sending data we should have destroyed the object as a whole on the client.
		const bool bRootIsResolvable = Client->IsResolvableNetRefHandle(ServerObjectRefHandle);
		if (bRootIsResolvable && bDidSendSomething)
		{
			for (const FNetRefHandle SubObjectRefHandle : ServerSubObjectRefHandles)
			{
				UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(SubObjectRefHandle));
			}
		}
		else
		{
			UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(ServerObjectRefHandle));
			for (const FNetRefHandle SubObjectRefHandle : ReverseIterate(ServerSubObjectRefHandles))
			{
				UE_NET_ASSERT_FALSE(Client->IsResolvableNetRefHandle(SubObjectRefHandle));
			}
		}

		if (!bDidSendSomething)
		{
			break;
		}
	}
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestSubObjectDefaultReplicationOrder)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);

	// Spawn some subobjects
	UReplicatedSubObjectOrderObject* ServerSubObject0 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectOrderObject* ServerSubObject1 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectOrderObject* ServerSubObject2 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle);

	UReplicatedSubObjectOrderObject::RepOrderCounter = 0U;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that objects have replicated
	UReplicatedSubObjectOrderObject* ClientSubObject0 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject0->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject1 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject1->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject2 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject2->NetRefHandle));

	UE_NET_ASSERT_NE(ClientSubObject0, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject1, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject2, nullptr);

	// Verify that they have replicated in expected order
	UE_NET_ASSERT_EQ(ClientSubObject0->LastRepOrderCounter, 1U);
	UE_NET_ASSERT_GT(ClientSubObject1->LastRepOrderCounter, ClientSubObject0->LastRepOrderCounter);
	UE_NET_ASSERT_GT(ClientSubObject2->LastRepOrderCounter, ClientSubObject1->LastRepOrderCounter);
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestSubObjectSpecifiedReplicationOrder)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);

	// Spawn some subobjects
	UReplicatedSubObjectOrderObject* ServerSubObject0 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle);
	// Specify Subobject1 to replicate with SubObject0, which means that it will replicate before Subobjet0 is replicated
	UReplicatedSubObjectOrderObject* ServerSubObject1 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle, ServerSubObject0->NetRefHandle, UReplicationBridge::ESubObjectInsertionOrder::ReplicateWith);
	// Specify SubObect 2 to replicate with no specific order (it will be added to the owner and thus replicate last)
	UReplicatedSubObjectOrderObject* ServerSubObject2 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle, ServerSubObject1->NetRefHandle, UReplicationBridge::ESubObjectInsertionOrder::None);

	UReplicatedSubObjectOrderObject::RepOrderCounter = 0U;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that objects have replicated
	UReplicatedSubObjectOrderObject* ClientSubObject0 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject0->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject1 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject1->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject2 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject2->NetRefHandle));

	UE_NET_ASSERT_NE(ClientSubObject0, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject1, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject2, nullptr);

	// Verify that they have replicated in expected order setup earlier
	UE_NET_ASSERT_EQ(ClientSubObject1->LastRepOrderCounter, 1U);
	UE_NET_ASSERT_EQ(ClientSubObject0->LastRepOrderCounter, 2U);
	UE_NET_ASSERT_EQ(ClientSubObject2->LastRepOrderCounter, 3U);
}


UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestSubObjectInsertAtStart)
{
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedObjectTestSubObjectCreationOrder* ServerObject = Server->CreateObject<UReplicatedObjectTestSubObjectCreationOrder>();

	// Spawn a subobject
	UReplicatedSubObjectOrderObject* ServerSubObject0 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle);
	
	// Spawn a subobject and make it replicate before SubObject0
	UReplicatedSubObjectOrderObject* ServerSubObject1 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle, ServerSubObject0->NetRefHandle, UReplicationBridge::ESubObjectInsertionOrder::ReplicateWith);
	
	// Spawn a subobject and make it replicate first
	UReplicatedSubObjectOrderObject* ServerSubObject2 = Server->CreateSubObject<UReplicatedSubObjectOrderObject>(ServerObject->NetRefHandle, FNetRefHandle::GetInvalid(), UReplicationBridge::ESubObjectInsertionOrder::InsertAtStart);

	UReplicatedSubObjectOrderObject::RepOrderCounter = 0U;

	// Send and deliver packet
	Server->UpdateAndSend({ Client });

	// Verify that objects have replicated
	UReplicatedSubObjectOrderObject* ClientSubObject0 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject0->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject1 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject1->NetRefHandle));
	UReplicatedSubObjectOrderObject* ClientSubObject2 = Cast<UReplicatedSubObjectOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject2->NetRefHandle));

	UE_NET_ASSERT_NE(ClientSubObject0, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject1, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject2, nullptr);

	// Verify that they were created in the expected order
	UE_NET_ASSERT_EQ(ClientSubObject2->CreationOrder, 1);
	UE_NET_ASSERT_EQ(ClientSubObject1->CreationOrder, 2);
	UE_NET_ASSERT_EQ(ClientSubObject0->CreationOrder, 3);

	// Verify that they have replicated in the expected order
	UE_NET_ASSERT_EQ(ClientSubObject2->LastRepOrderCounter, 1U);
	UE_NET_ASSERT_EQ(ClientSubObject1->LastRepOrderCounter, 2U);
	UE_NET_ASSERT_EQ(ClientSubObject0->LastRepOrderCounter, 3U);
}

class FTestNetTokensFixture : public FReplicationSystemServerClientTestFixture
{
public:
	FStringTokenStore* ServerStringTokenStore = nullptr;
	FStringTokenStore* ClientStringTokenStore = nullptr;
	FReplicationSystemTestClient* Client = nullptr;
	const FNetTokenStoreState* ClientRemoteNetTokenState;
	const FNetTokenStoreState* ServerRemoteNetTokenState;

	FNetToken CreateAndExportNetToken(const FString& TokenString)
	{
		FNetToken Token = ServerStringTokenStore->GetOrCreateToken(TokenString);
		UNetTokenDataStream* NetTokenDataStream = Cast<UNetTokenDataStream>(Server->GetReplicationSystem()->GetDataStream(Client->ConnectionIdOnServer, FName("NetToken")));
		if (NetTokenDataStream)
		{
			NetTokenDataStream->AddNetTokenForExplicitExport(Token);
		}

		return Token;
	}

	FNetToken CreateAndExportNetTokenOnClient(const FString& TokenString)
	{
		FNetToken Token = ClientStringTokenStore->GetOrCreateToken(TokenString);
		UNetTokenDataStream* NetTokenDataStream = Cast<UNetTokenDataStream>(Client->GetReplicationSystem()->GetDataStream(Client->LocalConnectionId, FName("NetToken")));
		if (NetTokenDataStream)
		{
			NetTokenDataStream->AddNetTokenForExplicitExport(Token);
		}

		return Token;
	}

	virtual void SetUp() override
	{
		FReplicationSystemServerClientTestFixture::SetUp();

		Client = CreateClient();
		{
			FNetTokenStore* ServerTokenStore = Server->GetReplicationSystem()->GetNetTokenStore();
			ServerStringTokenStore = ServerTokenStore->GetDataStore<FStringTokenStore>();		
			ServerRemoteNetTokenState = ServerTokenStore->GetRemoteNetTokenStoreState(Client->ConnectionIdOnServer);
		}
		{
			FNetTokenStore* ClientTokenStore = Client->GetReplicationSystem()->GetNetTokenStore();
			ClientStringTokenStore = ClientTokenStore->GetDataStore<FStringTokenStore>();
			ClientRemoteNetTokenState = ClientTokenStore->GetRemoteNetTokenStoreState(Client->LocalConnectionId);
		}
	}
};

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetToken)
{
	// Create token
	FString TokenStringA(TEXT("MyStringToken"));
	FNetToken StringTokenA = CreateAndExportNetToken(TokenStringA);

	// Send and drop packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	{
		LOG_SCOPE_VERBOSITY_OVERRIDE(LogNetToken, ELogVerbosity::Fatal);

		// Verify that we cannot resolve the token on the client
		UE_NET_ASSERT_NE(TokenStringA, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
	}

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that we can resolve the token on the client
	UE_NET_ASSERT_EQ(TokenStringA, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenResendWithFullPacket)
{
	// Create token
	FString TokenStringA(TEXT("MyStringToken"));
	FNetToken StringTokenA = CreateAndExportNetToken(TokenStringA);

	// Send and drop packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Limit packet size
	Server->SetMaxSendPacketSize(128U);

	// Create a new token that will not fit in the packet and only fit the resend data
	FString TokenStringB(TEXT("MyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongString"));
	FNetToken StringTokenB = CreateAndExportNetToken(TokenStringB);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that we can resolve the token first token on the client even though second one should not fit
	UE_NET_ASSERT_EQ(TokenStringA, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
	{
		LOG_SCOPE_VERBOSITY_OVERRIDE(LogNetToken, ELogVerbosity::Fatal);
		UE_NET_ASSERT_NE(TokenStringB, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
	}

	// Restore packet size and make sure that we get the second token through
	Server->SetMaxSendPacketSize(1024U);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(TokenStringB, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenResendWithFullPacketAfterFirstResend)
{
	// Create token
	FString TestStringA(TEXT("MyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongString"));
	FNetToken StringTokenA = CreateAndExportNetToken(TestStringA);	

	// Send and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Create a new token that will not fit in the packet and only fit the resend data
	FString TestStringB(TEXT("MyOtherLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongStringMyLongString"));
	FNetToken StringTokenB = CreateAndExportNetToken(TestStringB);

	// Send and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	Server->DeliverTo(Client, false);
	Server->DeliverTo(Client, false);

	// Verify that tokens has not been received
	{
		LOG_SCOPE_VERBOSITY_OVERRIDE(LogNetToken, ELogVerbosity::Fatal);

		UE_NET_ASSERT_NE(TestStringA, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
		UE_NET_ASSERT_NE(TestStringB, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
	}

	// Send and deliver packet which now should contain two entries in the resend queue
	Server->SetMaxSendPacketSize(1024);

	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that we can resolve the token
	UE_NET_ASSERT_EQ(TestStringA, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
	UE_NET_ASSERT_EQ(TestStringB, FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenSequenceTest)
{
	const FString TestStrings[] = {
		FString(TEXT("TokenA")),
		FString(TEXT("TokenB")),
		FString(TEXT("TokenC")),
		FString(TEXT("TokenD")),
		FString(TEXT("TokenE")),
		FString(TEXT("TokenF")),
	};

	const uint32 TokenCount = UE_ARRAY_COUNT(TestStrings);

	// Create token
	FNetToken StringTokenA = CreateAndExportNetToken(TestStrings[0]);
	FNetToken StringTokenB = CreateAndExportNetToken(TestStrings[1]);

	// Send packet
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Create token
	FNetToken StringTokenC = CreateAndExportNetToken(TestStrings[2]);

	// Create token
	FNetToken StringTokenD = CreateAndExportNetToken(TestStrings[3]);

	// Send packet
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Drop packet 
	Server->DeliverTo(Client, false);

	// Deliver packet 
	Server->DeliverTo(Client, true);

	// Create local tokens
	ClientStringTokenStore->GetOrCreateToken(TEXT("LocalTokenA"));
	ClientStringTokenStore->GetOrCreateToken(TEXT("LocalTokenB"));

	// Send packet with resend data
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(TestStrings[0], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
	UE_NET_ASSERT_EQ(TestStrings[1], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
	UE_NET_ASSERT_EQ(TestStrings[2], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenC, *ClientRemoteNetTokenState)));
	UE_NET_ASSERT_EQ(TestStrings[3], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenD, *ClientRemoteNetTokenState)));
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenResendAndDataInSamePacketTest)
{
	const FString TestStrings[] = {
		FString(TEXT("TokenA")),
		FString(TEXT("TokenB")),
	};

	const uint32 TokenCount = UE_ARRAY_COUNT(TestStrings);


	// Create token
	FNetToken StringTokenA = CreateAndExportNetToken(TestStrings[0]);

	// Send packet
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// drop data
	Server->DeliverTo(Client, false);

	// Create token
	FNetToken StringTokenB = CreateAndExportNetToken(TestStrings[1]);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(TestStrings[0], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenA, *ClientRemoteNetTokenState)));
	UE_NET_ASSERT_EQ(TestStrings[1], FString(ClientStringTokenStore->ResolveRemoteToken(StringTokenB, *ClientRemoteNetTokenState)));
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenAuthority)
{
	// Create token
	FString TokenStringA(TEXT("MyStringToken"));
	FNetToken NonAuthToken = CreateAndExportNetTokenOnClient(TokenStringA);

	UE_NET_ASSERT_EQ(NonAuthToken.IsAssignedByAuthority(), false);

	// Send from server
	Server->UpdateAndSend({Client});

	// Send from client
	Client->UpdateAndSend(Server);

	// We should be able to resolve the token on the server using remote
	UE_NET_ASSERT_EQ(TokenStringA, FString(ServerStringTokenStore->ResolveToken(NonAuthToken, ServerRemoteNetTokenState)));

	// Find server token.
	FNetToken AuthToken = CreateAndExportNetToken(TokenStringA);

	// It should be a different token as the server is authoriative
	UE_NET_ASSERT_FALSE(AuthToken == NonAuthToken);

	// Send from server
	Server->UpdateAndSend({Client});

	// Client should be able to resolve ServerToken
	UE_NET_ASSERT_EQ(TokenStringA, FString(ClientStringTokenStore->ResolveToken(AuthToken, ClientRemoteNetTokenState)));

	// If we now try to create a token for the string also received from the authority we expect it to give us the server token and allow us to use that instead of the local exported token.
	FNetToken NewClientToken = ClientStringTokenStore->GetOrCreateToken(TokenStringA);

	// We expect the tokens to be identical.
	UE_NET_ASSERT_TRUE(AuthToken == NewClientToken);
}

UE_NET_TEST_FIXTURE(FTestNetTokensFixture, NetTokenAuthTokenIsNotExportedFromClient)
{
	// Create token
	FString TokenStringA(TEXT("MyStringToken"));
	FNetToken AuthToken = CreateAndExportNetToken(TokenStringA);

	UE_NET_ASSERT_EQ(AuthToken.IsAssignedByAuthority(), true);

	// Send from server
	Server->UpdateAndSend({Client});

	// Expect to get auth token
	FNetToken ClientExpectedAuthToken = CreateAndExportNetTokenOnClient(TokenStringA);
	UE_NET_ASSERT_EQ(ClientExpectedAuthToken.IsAssignedByAuthority(), true);

	// Send from client
	Client->UpdateAndSend(Server);

	// $TODO: Expose some stats that we can query for exports.
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, AddRemoveFromConnectionScopeTest)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Add to group
	FNetObjectGroupHandle Group = ReplicationSystem->CreateGroup(NAME_None);
	ReplicationSystem->AddToGroup(Group, ServerObject->NetRefHandle);

	ReplicationSystem->AddExclusionFilterGroup(Group);
	ReplicationSystem->SetGroupFilterStatus(Group, ENetFilterStatus::Allow);

	// Start replicating object
	
	// Send packet
	// Expected state to be WaitOnCreateConfirmation
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Make sure we have data in flight
	++ServerObject->IntA;

	// Disallow group to trigger state change from PendingCreateConfirmation->PendingDestroy
	ReplicationSystem->SetGroupFilterStatus(Group, ENetFilterStatus::Disallow);	

	// Expect client to create object
	Server->DeliverTo(Client, true);
	UE_NET_ASSERT_NE(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle), nullptr);

	// Send packet
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Allow group to trigger state to ensure that we restart replication
	ReplicationSystem->SetGroupFilterStatus(Group, ENetFilterStatus::Allow);

	// Expect client to destroy object
	Server->DeliverTo(Client, true);
	UE_NET_ASSERT_EQ(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle), nullptr);

	// Trigger replication
	++ServerObject->IntA;

	// Send packet
	// WaitOnDestroyConfirmation -> WaitOnCreateConfirmation
	Server->UpdateAndSend({ Client });

	// Verify that the object got created again
	UE_NET_ASSERT_NE(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle), nullptr);
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestNetTemporary)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);
	UTestReplicatedIrisObject* ServerObject1 = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;
	ServerObject1->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that client has received the data
	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObject->IntA);
	}

	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject1->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_EQ(ServerObject1->IntA, ClientObject->IntA);

	}

	// Mark the object as a net temporary
	ReplicationSystem->SetIsNetTemporary(ServerObject->NetRefHandle);

	// Modify the value
	ServerObject->IntA = 2;
	ServerObject1->IntA = 2;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that client has not received the data for changed temporary
	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_NE(ServerObject->IntA, ClientObject->IntA);
	}

	// Verify that client has received the data for normal object
	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject1->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_EQ(ServerObject1->IntA, ClientObject->IntA);
	}

	// Test Late join
	// Add a client
	FReplicationSystemTestClient* Client2 = CreateClient();

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client2, true);
	Server->PostSendUpdate();

	// We should now have the latest state for both objects
	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client2->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObject->IntA);
	}

	// Verify that client has received the data for normal object
	{
		UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client2->GetReplicationBridge()->GetReplicatedObject(ServerObject1->NetRefHandle));

		UE_NET_ASSERT_TRUE(ClientObject != nullptr);
		UE_NET_ASSERT_EQ(ServerObject1->IntA, ClientObject->IntA);
	}
}

// Tests for TearOff

// Test TearOff for existing confirmed object
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffExistingObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to object 
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// Modify the value
	ServerObject->IntA = 2;

	// TearOff the object
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off and that the final state was applied
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}

// Test TearOff for new object
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffOnNewlyCreatedObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// We should not have any created objects
	const int32 NumObjectsCreatedOnClientBeforeReplication = Client->CreatedObjects.Num();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Set state
	ServerObject->IntA = 1;

	// TearOff the object before first replication
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Client should have created a object
	UE_NET_ASSERT_EQ(NumObjectsCreatedOnClientBeforeReplication + 1, Client->CreatedObjects.Num());

	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);

	// We should be able to get the object from the created objects array to validate the state
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->CreatedObjects[NumObjectsCreatedOnClientBeforeReplication].Get());

	// Verify that we replicated the expected state
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
}

// Test TearOff resend for existing confirmed object with no state changes
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffResendForExistingObjectWithoutDirtyState)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, DeliverPacket);
	Server->PostSendUpdate();

	// Store Pointer to object 
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_NE(ClientObjectThatWillBeTornOff, nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// TearOff the object
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and do not deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, DoNotDeliverPacket);
	Server->PostSendUpdate();

	// The ClientObject should still be found using the NetRefHandle
	UE_NET_ASSERT_NE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)), nullptr);

	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, DeliverPacket);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off
	UE_NET_ASSERT_EQ(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)), nullptr);
}

// Test TearOff for new object and resend, this requires creation info to be cached.
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffImmediateOnNewlyCreatedObjectResend)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// We should not have any created objects
	const int32 NumObjectsCreatedOnClientBeforeReplication = Client->CreatedObjects.Num();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Set state
	ServerObject->IntA = 1;

	// TearOff the object before first replication
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and deliver packet
	Server->UpdateAndSend({Client}, false);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Client should have created a object
	UE_NET_ASSERT_EQ(NumObjectsCreatedOnClientBeforeReplication + 1, Client->CreatedObjects.Num());

	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);

	// We should be able to get the object from the created objects array to validate the state
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->CreatedObjects[NumObjectsCreatedOnClientBeforeReplication].Get());

	// Verify that we replicated the expected state
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
}

// Test TearOff for new subobject and resend, this requires creation info to be cached.
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffImmediateOnNewlyCreatedSubObjectResend)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// We should not have any created objects
	const int32 NumObjectsCreatedOnClientBeforeReplication = Client->CreatedObjects.Num();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Set state
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Set state
	ServerSubObject->IntA = 1;

	// TearOff the subobject before first replication
	Server->ReplicationBridge->EndReplication(ServerSubObject, EEndReplicationFlags::TearOff);

	// Send and drop
	Server->UpdateAndSend({Client}, false);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Client should have created a object + subobject
	UE_NET_ASSERT_EQ(NumObjectsCreatedOnClientBeforeReplication + 2, Client->CreatedObjects.Num());

	// But as we have torn off the subobject it should no longer be a replicated object
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) != nullptr);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);

	// We should be able to get the object from the created objects array to validate the state
	UTestReplicatedIrisObject* ClientSubObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->CreatedObjects[NumObjectsCreatedOnClientBeforeReplication + 1].Get());

	// Verify that we replicated the expected state
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDefferedTearOffOnNewlyCreatedObjectResend)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// We should not have any created objects
	const int32 NumObjectsCreatedOnClientBeforeReplication = Client->CreatedObjects.Num();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Set state
	ServerObject->IntA = 1;

	// TearOff the object before first replication
	Server->ReplicationSystem->TearOffNextUpdate(ServerObject->NetRefHandle);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();
	
	// End replication and destroy object
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::Destroy);

	// Client should have created a object
	UE_NET_ASSERT_EQ(NumObjectsCreatedOnClientBeforeReplication + 1, Client->CreatedObjects.Num());

	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);

	// We should be able to get the object from the created objects array to validate the state
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->CreatedObjects[NumObjectsCreatedOnClientBeforeReplication].Get());

	// Verify that we replicated the expected state
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
}

// Test TearOff for existing not yet confirmed object
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffObjectPendingCreateConfirmation)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send packet to get put the object in flight
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// TearOff the object
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Deliver Object (should now be created)
	Server->DeliverTo(Client, true);

	// Store Pointer to object and verify initial state
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is TornOFf and that the final state was applied
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}

// Test TearOff for existing object pending destroy (should do nothing)
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffExistingObjectPendingDestroy)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to object 
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// Modify the value
	ServerObject->IntA = 2;

	// Mark the object for destroy
	Server->ReplicationBridge->EndReplication(ServerObject);

	// TearOff the object
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is not tornOff and that the final state was not applied as we issued tearoff after ending replication
	UE_NET_ASSERT_NE(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}

// Test TearOff resend 
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffResend)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to object 
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// Modify the value
	ServerObject->IntA = 2;

	// TearOff the object
	Server->ReplicationSystem->TearOffNextUpdate(ServerObject->NetRefHandle);

	// Send and deliver packet, in this case the packet containing 2 was lost, but, we did not know that when we 
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Object should now be torn-off, so it should not copy the latest state
	ServerObject->IntA = 3;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off and that the expected final state was applied
	UE_NET_ASSERT_EQ(2, ClientObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}

// Test TearOff does not pickup statechanges after tear off
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTornedOffObjectDoesNotCopyStateChanges)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Trigger replication
	ServerObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to object 
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));

	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	// Modify the value
	ServerObject->IntA = 2;

	// TearOff the object
	Server->ReplicationSystem->TearOffNextUpdate(ServerObject->NetRefHandle);

	// Send and drop packet containing the value 2
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Object should now be torn-off, so it should not copy the latest state but instead resend the last copied state (2) along with the tear-off
	ServerObject->IntA = 3;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is TornOFf and that the expected final state was applied
	UE_NET_ASSERT_EQ(2, ClientObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}

// Test TearOff and SubObjects, SubObjects must apply state?
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestImmediateTearOffExistingObjectWithSubObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Trigger replication
	ServerObject->IntA = 1;
	ServerSubObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to objects
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	UTestReplicatedIrisObject* ClientSubObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientSubObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);

	// Modify the value of subobject only
	ServerSubObject->IntA = 2;

	// TearOff the object using immediate tear-off
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off and that the final state was applied to subObject 
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}

// Test TearOff and SubObjects, SubObjects must apply state?
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestImmediateTearOffExistingObjectWithSubObjectDroppedData)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Trigger replication
	ServerObject->IntA = 1;
	ServerSubObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to objects
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	UTestReplicatedIrisObject* ClientSubObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientSubObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);

	// Modify the value of subobject only
	ServerSubObject->IntA = 2;

	// TearOff the object using immediate tear-off
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Send and do not deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off and that the final state was applied to subObject 
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}

// Test dropped creation of subobject dirties owner
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestDroppedCreationForSubobjectDirtiesOwner)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to objects
	UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientObject != nullptr);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Send and do not deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject now is created as expected
	UTestReplicatedIrisObject* ClientSubObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientSubObject != nullptr);
}

// Test replicated destroy for not created object
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicatedDestroyForNotCreatedObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Update and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Destroy object
	Server->ReplicationBridge->EndReplication(ServerObject);

	// Update and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Drop first packet containing creation info for object
	Server->DeliverTo(Client, false);

	// Deliver second packet that should contain destroy
	Server->DeliverTo(Client, true);

	// Verify that the object does not exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) == nullptr);
}


// Test replicated SubObjectDestroy for not created subobject
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicatedSubObjectDestroyForNotCreatedObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Replicate object
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerSubObject = Server->CreateSubObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>(ServerObject->NetRefHandle);

	// Update and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Destroy subobject
	Server->ReplicationBridge->EndReplication(ServerSubObject);

	// Update and delay delivery
	Server->NetUpdate();
	Server->SendTo(Client);
	Server->PostSendUpdate();

	// Drop first packet containing creation info for subobject
	Server->DeliverTo(Client, false);

	// Deliver second packet that should contain replicated subobject destroy
	Server->DeliverTo(Client, true);

	// Verify that the object still exists on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) != nullptr);

	// Verify that the subobject does not exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}

// Test replicated SubObjectDestroy for filtered out subobject
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicatedSubObjectDestroyForFilteredOutSubObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerSubObject = Server->CreateSubObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>(ServerObject->NetRefHandle);

	// Replicate object
	Server->UpdateAndSend({Client});

	// Verify that the subobject does  exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) != nullptr);

	// Set condition
	Server->ReplicationBridge->SetSubObjectNetCondition(ServerSubObject->NetRefHandle, ELifetimeCondition::COND_Never);

	// Destroy subobject
	Server->ReplicationBridge->EndReplication(ServerSubObject);

	// Replicate object
	Server->UpdateAndSend({Client});

	// Verify that the object still exists on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) != nullptr);

	// Verify that the subobject does not exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}

// Test replicated SubObjectDestroy for filtered out subobject
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicatedSubObjectDestroyForFilteredOutSubObjectBeforeSend)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerSubObject = Server->CreateSubObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>(ServerObject->NetRefHandle);

	// Set condition
	Server->ReplicationBridge->SetSubObjectNetCondition(ServerSubObject->NetRefHandle, ELifetimeCondition::COND_Never);

	// Replicate object
	Server->UpdateAndSend({Client});

	// Verify that the subobject does not exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);

	// Destroy subobject
	Server->ReplicationBridge->EndReplication(ServerSubObject);

	// Replicate object
	Server->UpdateAndSend({Client});

	// Verify that the object still exists on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle)) != nullptr);

	// Verify that the subobject does not exist on client
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}


// Test tear-off object in PendingCreate state to ensure that tear-off logic works as expected
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffObjectWithNoFragmentsDoesNotTriggerCheckIfPendingCreateWhenDestroyed)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerObject = Server->CreateObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>();

	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Tear-off using immediate tear-off
	Server->ReplicationBridge->EndReplication(ServerObject, EEndReplicationFlags::TearOff);

	// Trigger the next update but avoid sending any data so that we keep the object in the PendingCreation state while we flush the Handles PendingTearOff Array which occurs in PostSendUpdate
	Server->NetUpdate();
	Server->PostSendUpdate();
}

// Test tear-off subobject in PendingCreate state to ensure that tear-off logic works as expected
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffSubObjectWithNoFragmentsDoesNotTriggerCheckIfPendingCreateWhenDestroyed)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerSubObject = Server->CreateSubObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>(ServerObject->NetRefHandle);

	// Update and drop
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, false);
	Server->PostSendUpdate();

	// Tear-off using immediate tear-off
	Server->ReplicationBridge->EndReplication(ServerSubObject, EEndReplicationFlags::TearOff);

	// Trigger the next update but avoid sending any data so that we keep the sub-object in the PendingCreation state while we flush the Handles PendingTearOff Array which occurs in PostSendUpdate
	Server->NetUpdate();
	Server->PostSendUpdate();
}

// Test TearOff and SubObjects, SubObjects must apply state?
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffNextUpdateExistingObjectWithSubObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Trigger replication
	ServerObject->IntA = 1;
	ServerSubObject->IntA = 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to objects
	UTestReplicatedIrisObject* ClientObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerObject->IntA, ClientObjectThatWillBeTornOff->IntA);

	UTestReplicatedIrisObject* ClientSubObjectThatWillBeTornOff = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle));
	UE_NET_ASSERT_TRUE(ClientSubObjectThatWillBeTornOff != nullptr);
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);

	// Modify the value of subobject only
	ServerSubObject->IntA = 2;

	// TearOff the object using immediate tear-off
	Server->ReplicationSystem->TearOffNextUpdate(ServerObject->NetRefHandle);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that ClientObject is torn-off and that the final state was applied to subObject 
	UE_NET_ASSERT_EQ(ServerSubObject->IntA, ClientSubObjectThatWillBeTornOff->IntA);
	UE_NET_ASSERT_TRUE(Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject->NetRefHandle)) == nullptr);
}

// Test TearOff and destroy of SubObjects that are still pending create/tearoff
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestTearOffNextUpdateExistingObjectWithSubObjectPendingCreation)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;
	FNetRefHandleManager* NetRefHandleManager = &ReplicationSystem->GetReplicationSystemInternal()->GetNetRefHandleManager();

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0,0);

	// Spawn second object on server as a subobject
	UTestReplicatedIrisObject* ServerSubObject = Server->CreateSubObject(ServerObject->NetRefHandle, 0, 0);

	// Trigger replication
	ServerObject->IntA = 1;
	ServerSubObject->IntA = 1;

	const FInternalNetRefIndex ServerObjectInternalIndex = NetRefHandleManager->GetInternalIndex(ServerObject->NetRefHandle);
	const FInternalNetRefIndex SubObjectObjectInternalIndex = NetRefHandleManager->GetInternalIndex(ServerSubObject->NetRefHandle);

	// Trigger presend without send to add the objects to scope
	Server->NetUpdate();
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(uint16(1), NetRefHandleManager->GetNetObjectRefCount(ServerObjectInternalIndex));
	UE_NET_ASSERT_EQ(uint16(1), NetRefHandleManager->GetNetObjectRefCount(SubObjectObjectInternalIndex));

	// TearOff the object this will also tear-off subobject
	Server->ReplicationSystem->TearOffNextUpdate(ServerObject->NetRefHandle);

	// Update logic, object should be removed from scope but still exist as pending create in
	Server->NetUpdate();
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(uint16(1), NetRefHandleManager->GetNetObjectRefCount(ServerObjectInternalIndex));
	UE_NET_ASSERT_EQ(uint16(1), NetRefHandleManager->GetNetObjectRefCount(SubObjectObjectInternalIndex));

	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Destroy the object
	Server->DestroyObject(ServerObject);

	// Verify that we no longer have any references to the object
	UE_NET_ASSERT_EQ(uint16(0), NetRefHandleManager->GetNetObjectRefCount(ServerObjectInternalIndex));
	UE_NET_ASSERT_EQ(uint16(0), NetRefHandleManager->GetNetObjectRefCount(SubObjectObjectInternalIndex));
}

// Test that we can replicate an object with no replicated properties
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicatedObjectWithNoReplicatedProperties)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObjectWithNoReplicatedMembers* ServerObject = Server->CreateObject<UTestReplicatedIrisObjectWithNoReplicatedMembers>();
	const FNetRefHandle ServerHandle = ServerObject->NetRefHandle;

	UE_NET_ASSERT_TRUE(ServerHandle.IsValid());

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	UTestReplicatedIrisObjectWithNoReplicatedMembers* ClientObject = Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerHandle));
	UE_NET_ASSERT_TRUE(ClientObject != nullptr);

	// Destroy object
	Server->DestroyObject(ServerObject);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	ClientObject = Cast<UTestReplicatedIrisObjectWithNoReplicatedMembers>(Client->GetReplicationBridge()->GetReplicatedObject(ServerHandle));
	UE_NET_ASSERT_TRUE(ClientObject == nullptr);
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestObjectPollFramePeriod)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UTestReplicatedIrisObject* ServerObject = Server->CreateObject(0, 0);

	// Spawn second object on server that later will be added as a dependent object
	UObjectReplicationBridge::FRootObjectReplicationParams Params;
	Params.PollFrequency = Server->ConvertPollPeriodIntoFrequency(1U);
	UTestReplicatedIrisObject* ServerObjectPolledEveryOtherFrame = Server->CreateObject(Params);

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Store Pointer to objects and verify state after initial replication
	UTestReplicatedIrisObject* ClientObject = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObject->NetRefHandle));
	UTestReplicatedIrisObject* ClientObjectPolledEveryOtherFrame = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectPolledEveryOtherFrame->NetRefHandle));

	UE_NET_ASSERT_NE(ClientObjectPolledEveryOtherFrame, nullptr);
	UE_NET_ASSERT_NE(ClientObject, nullptr);
	UE_NET_ASSERT_EQ(ClientObject->IntA, ServerObject->IntA);
	UE_NET_ASSERT_EQ(ClientObjectPolledEveryOtherFrame->IntA, ServerObjectPolledEveryOtherFrame->IntA);

	// After two value updates it's expected that the polling occurs exactly one time for the object with poll frame period 1 (meaning every other frame).
	bool SlowPollObjectHasBeenEqual = false;
	bool SlowPollObjectHasBeenInequal = false;

	// Update values
	ServerObject->IntA += 1;
	ServerObjectPolledEveryOtherFrame->IntA += 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	UE_NET_ASSERT_EQ(ClientObject->IntA, ServerObject->IntA);
	SlowPollObjectHasBeenEqual |= (ClientObjectPolledEveryOtherFrame->IntA == ServerObjectPolledEveryOtherFrame->IntA);
	SlowPollObjectHasBeenInequal |= (ClientObjectPolledEveryOtherFrame->IntA != ServerObjectPolledEveryOtherFrame->IntA);

	// Update values
	ServerObject->IntA += 1;
	ServerObjectPolledEveryOtherFrame->IntA += 1;

	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify that both objects now are in sync
	UE_NET_ASSERT_EQ(ClientObject->IntA, ServerObject->IntA);
	SlowPollObjectHasBeenEqual |= (ClientObjectPolledEveryOtherFrame->IntA == ServerObjectPolledEveryOtherFrame->IntA);
	SlowPollObjectHasBeenInequal |= (ClientObjectPolledEveryOtherFrame->IntA != ServerObjectPolledEveryOtherFrame->IntA);

	UE_NET_ASSERT_TRUE(SlowPollObjectHasBeenEqual);
	UE_NET_ASSERT_TRUE(SlowPollObjectHasBeenInequal);
}

// Test that broken objects can be skipped by client
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestClientCanSkipBrokenObject)
{
	UReplicationSystem* ReplicationSystem = Server->ReplicationSystem;

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn objects on server
	UTestReplicatedIrisObject* ServerObjectA = Server->CreateObject(0,0);
	UTestReplicatedIrisObject* ServerObjectB = Server->CreateObject(0,0);

	{
		// Setup client to fail to create next remote object
		ServerObjectA->bForceFailToInstantiateOnRemote = true;

		// Suppress ensure that will occur due to failing to instantiate the object
		UReplicatedTestObjectBridge::FSupressCreateInstanceFailedEnsureScope SuppressEnsureScope(*Client->GetReplicationBridge());

		// Disable error logging as we know we will fail.
		auto IrisLogVerbosity = UE_GET_LOG_VERBOSITY(LogIris);
		LogIris.SetVerbosity(ELogVerbosity::NoLogging);

		// Send and deliver packet
		Server->NetUpdate();
		Server->SendAndDeliverTo(Client, true);
		Server->PostSendUpdate();

		// Restore LogVerbosity
		LogIris.SetVerbosity(IrisLogVerbosity);
	}

	// We expect replication of ObjectA to have failed
	{
		UTestReplicatedIrisObject* ClientObjectA = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectA->NetRefHandle));
		UE_NET_ASSERT_TRUE(ClientObjectA == nullptr);
	}

	// ObjectB should have been replicated ok
	{
		UTestReplicatedIrisObject* ClientObjectB = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectB->NetRefHandle));
		UE_NET_ASSERT_TRUE(ClientObjectB != nullptr);
	}

	// Modify both objects to make them replicate again
	++ServerObjectA->IntA;
	++ServerObjectB->IntA;

	// Send and deliver packet to verify that client ignores the broken object
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// We expect replication of ObjectA to have failed
	{
		UTestReplicatedIrisObject* ClientObjectA = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectA->NetRefHandle));
		UE_NET_ASSERT_TRUE(ClientObjectA == nullptr);
	}

	// Filter out ObjectA to tell the client that the object has gone out of scope
	ReplicationSystem->AddToGroup(ReplicationSystem->GetNotReplicatedNetObjectGroup(), ServerObjectA->NetRefHandle);

	// Send and deliver packet, the client should now remove the broken object from the list of broken objects
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Enable replication of ObjectA again to try to replicate it to server now that it should succeed
	ReplicationSystem->RemoveFromGroup(ReplicationSystem->GetNotReplicatedNetObjectGroup(), ServerObjectA->NetRefHandle);

	// Set ObjectA to be able instantiate on client again
	ServerObjectA->bForceFailToInstantiateOnRemote = false;

	// Client should now be able to instantiate the object
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// We expect replication of ObjectA to have succeeded this time
	{
		UTestReplicatedIrisObject* ClientObjectA = Cast<UTestReplicatedIrisObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectA->NetRefHandle));
		UE_NET_ASSERT_TRUE(ClientObjectA == nullptr);
	}
}


// Test that PropertyReplication properly handles partial states during Apply
UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestPartialDequantize)
{
	// Enable cvars to exercise path that store previous state for OnReps to make sure we exercise path that accumulate dirty changes so that we have a complete state.
	IConsoleVariable* CVarUsePrevReceivedStateForOnReps = IConsoleManager::Get().FindConsoleVariable(TEXT("net.Iris.UsePrevReceivedStateForOnReps"));
	check(CVarUsePrevReceivedStateForOnReps != nullptr && CVarUsePrevReceivedStateForOnReps->IsVariableBool());
	const bool bUsePrevReceivedStateForOnReps = CVarUsePrevReceivedStateForOnReps->GetBool();
	CVarUsePrevReceivedStateForOnReps->Set(true, ECVF_SetByCode);

	// Make sure we allow partial dequantize
	IConsoleVariable* CVarForceFullDequantizeAndApply = IConsoleManager::Get().FindConsoleVariable(TEXT("net.iris.ForceFullDequantizeAndApply"));
	check(CVarForceFullDequantizeAndApply != nullptr && CVarForceFullDequantizeAndApply->IsVariableBool());
	const bool bForceFullDequantizeAndApply = CVarForceFullDequantizeAndApply->GetBool();
	CVarForceFullDequantizeAndApply->Set(false, ECVF_SetByCode);

	ON_SCOPE_EXIT
	{
		// Restore cvars
		CVarUsePrevReceivedStateForOnReps->Set(bUsePrevReceivedStateForOnReps, ECVF_SetByCode);
		CVarForceFullDequantizeAndApply->Set(bForceFullDequantizeAndApply, ECVF_SetByCode);
	};

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn objects on server
	UTestReplicatedObjectWithRepNotifies* ServerObjectA = Server->CreateObject<UTestReplicatedObjectWithRepNotifies>();
	
	// Send and deliver packet
	Server->NetUpdate();
	Server->SendAndDeliverTo(Client, true);
	Server->PostSendUpdate();

	// Verify assumptions
	// Object should exist on client and have default state
	UTestReplicatedObjectWithRepNotifies* ClientObjectA = Cast<UTestReplicatedObjectWithRepNotifies>(Client->GetReplicationBridge()->GetReplicatedObject(ServerObjectA->NetRefHandle));
	UE_NET_ASSERT_NE(ClientObjectA, nullptr);

	UE_NET_ASSERT_EQ(ServerObjectA->IntA, ClientObjectA->IntA);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntAStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntB, ClientObjectA->IntB);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntBStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntC, ClientObjectA->IntC);

	// Modify only IntA
	ServerObjectA->IntA = 1;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions
	// Only IntA should have been modified
	UE_NET_ASSERT_EQ(ServerObjectA->IntA, ClientObjectA->IntA);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntAStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntB, ClientObjectA->IntB);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntBStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntC, ClientObjectA->IntC);

	// Modify only IntB
	ServerObjectA->IntB = 1;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions
	// Only IntA should have been modified
	UE_NET_ASSERT_EQ(ServerObjectA->IntA, ClientObjectA->IntA);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntAStoredInOnRep, -1);

	UE_NET_ASSERT_EQ(ServerObjectA->IntB, ClientObjectA->IntB);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntBStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntC, ClientObjectA->IntC);

	// Modify only IntA
	ServerObjectA->IntA = 2;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions
	// IntA should have been modified, and if everything works correctly PrevIntAStoredInOnRep should be 1
	UE_NET_ASSERT_EQ(ServerObjectA->IntA, ClientObjectA->IntA);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntAStoredInOnRep, 1);

	UE_NET_ASSERT_EQ(ServerObjectA->IntB, ClientObjectA->IntB);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntBStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntC, ClientObjectA->IntC);

	// Verify that we do not apply repnotifies if we do not receive data from server by modifying values on the client and verifying that they do not get overwritten
	ServerObjectA->IntB = 2;
	ClientObjectA->IntA = -1;
	ClientObjectA->PrevIntAStoredInOnRep = -1;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions, since we messed with IntA and PrevIntAStoredInOnRep locally they have the value we set but IntB should be updated according to replicated state
	UE_NET_ASSERT_NE(ServerObjectA->IntA, ClientObjectA->IntA);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntAStoredInOnRep, -1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntB, ClientObjectA->IntB);
	UE_NET_ASSERT_EQ(ClientObjectA->PrevIntBStoredInOnRep, 1);
	UE_NET_ASSERT_EQ(ServerObjectA->IntC, ClientObjectA->IntC);

}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestNetMetric)
{
	{
		FNetMetric Metric(50.0);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Double);
	}

	{
		FNetMetric Metric(50.f);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Double);
	}

	{
		FNetMetric Metric;
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::None);
		float Value = 100.f;
		Metric.Set(Value);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Double);
	}

	{
		FNetMetric Metric(5U);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Unsigned);
	}

	{
		FNetMetric Metric;
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::None);
		uint32 Value = 100U;
		Metric.Set(Value);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Unsigned);
	}

	{
		FNetMetric Metric(-5);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Signed);
	}

	{
		FNetMetric Metric;
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::None);
		Metric.Set(5);
		UE_NET_ASSERT_TRUE(Metric.GetDataType() == FNetMetric::EDataType::Signed);
	}
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestReplicationRecordStarvation)
{
	IConsoleVariable* CVarReplicationRecordStarvationThreshold = IConsoleManager::Get().FindConsoleVariable(TEXT("net.Iris.ReplicationWriterReplicationRecordStarvationThreshold"));
	UE_NET_ASSERT_NE(CVarReplicationRecordStarvationThreshold, nullptr);
	UE_NET_ASSERT_TRUE(CVarReplicationRecordStarvationThreshold->IsVariableInt());
	const int32 PrevReplicationRecordStarvationThreshold = CVarReplicationRecordStarvationThreshold->GetInt();
	ON_SCOPE_EXIT
	{
		CVarReplicationRecordStarvationThreshold->Set(PrevReplicationRecordStarvationThreshold, ECVF_SetByCode);
	};
	
	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Set starvation threshold to highest possible
	CVarReplicationRecordStarvationThreshold->Set(FReplicationRecord::MaxReplicationRecordCount, ECVF_SetByCode);

	// Consume one ReplicationRecord to enter starvation
	UReplicatedTestObject* FirstObject = Server->CreateObject(UTestReplicatedIrisObject::FComponents{});
	const FNetRefHandle FirstObjectRefHandle = FirstObject->NetRefHandle;

	Server->NetUpdate();
	Server->SendUpdate(Client->ConnectionIdOnServer);
	Server->PostSendUpdate();

	// Try creating a second object. This should not succeed but we won't be able to test until we've delivered packets. 
	UReplicatedTestObject* SecondObject = Server->CreateObject(UTestReplicatedIrisObject::FComponents{});
	const FNetRefHandle SecondObjectRefHandle = SecondObject->NetRefHandle;

	Server->NetUpdate();
	Server->SendUpdate(Client->ConnectionIdOnServer);
	Server->PostSendUpdate();

	Server->DeliverTo(Client, DeliverPacket);
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(FirstObjectRefHandle));

	// The second packet, if any, should not allow object replication due to starvation.
	Server->DeliverTo(Client, DeliverPacket);
	UE_NET_ASSERT_FALSE(Client->IsResolvableNetRefHandle(SecondObjectRefHandle));

	// Now we should be able replicate objects again. Retry sending the second object.
	Server->NetUpdate();
	Server->SendUpdate(Client->ConnectionIdOnServer);
	Server->PostSendUpdate();

	// Try destroying the first object. This should not succeed. 
	Server->DestroyObject(FirstObject);

	// Deliver the attempt to create the second object and verify it exists on the client.
	Server->DeliverTo(Client, DeliverPacket);
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(SecondObjectRefHandle));

	// The second packet, if any, should not allow object destruction due to starvation.
	Server->DeliverTo(Client, DeliverPacket);
	UE_NET_ASSERT_TRUE(Client->IsResolvableNetRefHandle(FirstObjectRefHandle));

	// Now we should be able to destroy objects again. Retry destroying the first object
	Server->UpdateAndSend({Client});
	UE_NET_ASSERT_FALSE(Client->IsValidNetRefHandle(FirstObjectRefHandle));
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestSubObjectReplicatedDestroyBeforePostNetReceive)
{
	// Make sure that bOldImmediateDispatchEndReplicationForSubObjects is set to true
	IConsoleVariable* LocalCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.Iris.ImmediateDispatchEndReplicationForSubObjects"));
	check(LocalCVar != nullptr && LocalCVar->IsVariableBool());
	const bool bOldImmediateDispatchEndReplicationForSubObjects = LocalCVar->GetBool();
	LocalCVar->Set(true, ECVF_SetByCode);

	ON_SCOPE_EXIT
	{
		// Restore cvars
		LocalCVar->Set(bOldImmediateDispatchEndReplicationForSubObjects, ECVF_SetByCode);
	};

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);

	// Spawn some subobjects
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject0 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject1 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject2 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);

	UReplicatedSubObjectDestroyOrderObject::RepOrderCounter = 0U;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that objects have replicated
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject0 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject0->NetRefHandle));
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject1 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject1->NetRefHandle));
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject2 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject2->NetRefHandle));

	UE_NET_ASSERT_NE(ClientSubObject0, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject1, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject2, nullptr);

	// Verify that they have replicated in expected order
	UE_NET_ASSERT_EQ(ClientSubObject0->LastRepOrderCounter, 1U);
	UE_NET_ASSERT_GT(ClientSubObject1->LastRepOrderCounter, ClientSubObject0->LastRepOrderCounter);
	UE_NET_ASSERT_GT(ClientSubObject2->LastRepOrderCounter, ClientSubObject1->LastRepOrderCounter);	

	// setup a watch on the client
	ClientSubObject2->SetObjectExpectedToBeDestroyed(ClientSubObject1);

	// Dirty some data on server and destroy SubObject1
	++ServerSubObject2->IntA;
	Server->ReplicationBridge->EndReplication(ServerSubObject1, EEndReplicationFlags::Destroy);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions, replicated subobject destroy should have been issued before ClientSubObjects2 s call to PostNetReceive
	UE_NET_ASSERT_TRUE(ClientSubObject2->bObjectExistedInPreNetReceive);
	UE_NET_ASSERT_FALSE(ClientSubObject2->bObjectExistedInPostNetReceive);
	UE_NET_ASSERT_FALSE(ClientSubObject2->ObjectToWatch->IsValid());
}

UE_NET_TEST_FIXTURE(FReplicationSystemServerClientTestFixture, TestSubObjectReplicatedDestroyAfterPostNetReceive)
{
	// Make sure that bOldImmediateDispatchEndReplicationForSubObjects is set to false
	IConsoleVariable* LocalCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.Iris.ImmediateDispatchEndReplicationForSubObjects"));
	check(LocalCVar != nullptr && LocalCVar->IsVariableBool());
	const bool bOldImmediateDispatchEndReplicationForSubObjects = LocalCVar->GetBool();
	LocalCVar->Set(false, ECVF_SetByCode);

	ON_SCOPE_EXIT
	{
		// Restore cvars
		LocalCVar->Set(bOldImmediateDispatchEndReplicationForSubObjects, ECVF_SetByCode);
	};

	// Add a client
	FReplicationSystemTestClient* Client = CreateClient();

	// Spawn object on server
	UReplicatedTestObject* ServerObject = Server->CreateObject(0, 0);

	// Spawn some subobjects
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject0 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject1 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);
	UReplicatedSubObjectDestroyOrderObject* ServerSubObject2 = Server->CreateSubObject<UReplicatedSubObjectDestroyOrderObject>(ServerObject->NetRefHandle);

	UReplicatedSubObjectDestroyOrderObject::RepOrderCounter = 0U;

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify that objects have replicated
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject0 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject0->NetRefHandle));
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject1 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject1->NetRefHandle));
	UReplicatedSubObjectDestroyOrderObject* ClientSubObject2 = Cast<UReplicatedSubObjectDestroyOrderObject>(Client->GetReplicationBridge()->GetReplicatedObject(ServerSubObject2->NetRefHandle));

	UE_NET_ASSERT_NE(ClientSubObject0, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject1, nullptr);
	UE_NET_ASSERT_NE(ClientSubObject2, nullptr);

	// Verify that they have replicated in expected order
	UE_NET_ASSERT_EQ(ClientSubObject0->LastRepOrderCounter, 1U);
	UE_NET_ASSERT_GT(ClientSubObject1->LastRepOrderCounter, ClientSubObject0->LastRepOrderCounter);
	UE_NET_ASSERT_GT(ClientSubObject2->LastRepOrderCounter, ClientSubObject1->LastRepOrderCounter);	

	// setup a watch on the client
	ClientSubObject2->SetObjectExpectedToBeDestroyed(ClientSubObject1);

	// Dirty some data on server and destroy SubObject1
	++ServerSubObject2->IntA;
	Server->ReplicationBridge->EndReplication(ServerSubObject1, EEndReplicationFlags::Destroy);

	// Send and deliver packet
	Server->UpdateAndSend({Client});

	// Verify assumptions, replicated subobject destroy should have been issued after ClientSubObjects2 s call to PostNetReceive
	UE_NET_ASSERT_TRUE(ClientSubObject2->bObjectExistedInPreNetReceive);
	UE_NET_ASSERT_TRUE(ClientSubObject2->bObjectExistedInPostNetReceive);
	UE_NET_ASSERT_FALSE(ClientSubObject2->ObjectToWatch->IsValid());
}

} // end namespace UE::Net::Private


