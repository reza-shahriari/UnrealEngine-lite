// Copyright Epic Games, Inc. All Rights Reserved.

#include "Iris/ReplicationSystem/ReplicationReader.h"
#include "Iris/IrisConfigInternal.h"
#include "Iris/Core/IrisLog.h"
#include "Iris/Core/IrisProfiler.h"
#include "Iris/Core/IrisDebugging.h"
#include "Net/Core/Trace/NetTrace.h"
#include "Net/Core/Trace/NetDebugName.h"
#include "Iris/DataStream/DataStream.h"
#include "Iris/ReplicationSystem/DeltaCompression/DeltaCompressionBaselineManager.h"
#include "Iris/ReplicationSystem/NetRefHandleManager.h"
#include "Iris/Serialization/NetReferenceCollector.h"
#include "Iris/ReplicationSystem/ReplicationOperations.h"
#include "Iris/ReplicationSystem/ReplicationOperationsInternal.h"
#include "Iris/ReplicationSystem/DequantizeAndApplyHelper.h"
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#include "Iris/ReplicationSystem/ReplicationSystemInternal.h"
#include "Iris/ReplicationSystem/NetBlob/NetObjectBlobHandler.h"
#include "Iris/ReplicationSystem/NetBlob/PartialNetObjectAttachmentHandler.h"
#include "Iris/Serialization/InternalNetSerializationContext.h"
#include "Iris/Serialization/NetBitStreamReader.h"
#include "Iris/Serialization/NetBitStreamUtil.h"
#include "Iris/Serialization/NetSerializer.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Class.h"
#include "Algo/RemoveIf.h"
#include "Misc/ScopeExit.h"
#include "Misc/CoreMisc.h"
#include "ProfilingDebugging/CsvProfiler.h"

#if UE_NET_ENABLE_REPLICATIONREADER_LOG
#	define UE_LOG_REPLICATIONREADER(Format, ...)  UE_LOG(LogIris, Log, Format, ##__VA_ARGS__)
#	define UE_LOG_REPLICATIONREADER_CONN(Format, ...)  UE_LOG(LogIris, Log, TEXT("Conn: %u ") Format, Parameters.ConnectionId, ##__VA_ARGS__)
#else
#	define UE_LOG_REPLICATIONREADER(...)
#	define UE_LOG_REPLICATIONREADER_CONN(Format, ...)
#endif

#define UE_LOG_REPLICATIONREADER_CONN_WARNING(Format, ...) UE_LOG(LogIris, Warning, TEXT("Conn: %u ") Format, Parameters.ConnectionId, ##__VA_ARGS__)

CSV_DECLARE_CATEGORY_EXTERN(IrisClient);

namespace UE::Net::Private
{

static bool bUseResolvingHandleCache = true;
static FAutoConsoleVariableRef CVarUseResolvingHandleCache(
	TEXT("net.Iris.UseResolvingHandleCache"),
	bUseResolvingHandleCache,
	TEXT("Enable the use of a hot and cold cache when resolving unresolved caches to reduce the time spent resolving references."));

static int32 HotResolvingLifetimeMS = 1000;
static FAutoConsoleVariableRef CVarHotResolvingLifetimeMS(
	TEXT("net.Iris.HotResolvingLifetimeMS"),
	HotResolvingLifetimeMS,
	TEXT("An unresolved reference is considered hot if it was created within this many milliseconds, and cold otherwise."));

static int32 ColdResolvingRetryTimeMS = 200;
static FAutoConsoleVariableRef CVarColdResolvingRetryTimeMS(
	TEXT("net.Iris.ColdResolvingRetryTimeMS"),
	ColdResolvingRetryTimeMS,
	TEXT("Resolve unresolved cold references after this many milliseconds."));

// When true, the function UpdateObjectReferenceTracking_Fast() is used and UpdateObjectReferenceTracking()
// otherwise. Once UpdateObjectReferenceTracking_Fast() has been tested sufficiently it will become the only
// version of this function and the cvar removed.
static bool bUseOptObjectRefTracking = false;
static FAutoConsoleVariableRef CVarUseOptObjectRefTracking(
	TEXT("net.Iris.UseOptObjectRefTracking"),
	bUseOptObjectRefTracking,
	TEXT("Use a more optimized version of FReplicationReader::UpdateObjectReferenceTracking()."));

static bool bExecuteReliableRPCsBeforeApplyState = true;
static FAutoConsoleVariableRef CVarExecuteReliableRPCsBeforeApplyState(
		TEXT("net.Iris.ExecuteReliableRPCsBeforeApplyState"),
		bExecuteReliableRPCsBeforeApplyState,
		TEXT("If true and Iris runs in backwards compatibility mode then reliable RPCs will be executed before we apply state data on the target object unless we first need to spawn the object."));

static bool bDeferEndReplication = true;
static FAutoConsoleVariableRef CVarDeferEndReplication(
	TEXT("net.Iris.DeferEndReplication"),
	bDeferEndReplication,
	TEXT("if true calls to EndReplication will be defered until after we have applied statedata. Default is true."));

static bool bImmediateDispatchEndReplicationForSubObjects = true;
static FAutoConsoleVariableRef CVarImmediateDispatchEndReplicationForSubObjects(
	TEXT("net.Iris.ImmediateDispatchEndReplicationForSubObjects"),
	bImmediateDispatchEndReplicationForSubObjects,
	TEXT("if true replicated calls to EndReplication for SubObjects will dispatched immediately. Default is true."));

static bool bDispatchUnresolvedPreviouslyReceivedChanges = false;
static FAutoConsoleVariableRef CvarDispatchUnresolvedPreviouslyReceivedChanges(
	TEXT("net.Iris.DispatchUnresolvedPreviouslyReceivedChanges"),
	bDispatchUnresolvedPreviouslyReceivedChanges,
	TEXT("Whether to include previously received changes with unresolved object references to data received this frame when applying state data. This can call rep notify functions to be called despite being unchanged. Default is false."));

static bool bRemapDynamicObjects = true;
static FAutoConsoleVariableRef CvarRemapDynamicObjects(
		TEXT("net.Iris.RemapDynamicObjects"),
		bRemapDynamicObjects,
		TEXT("Allow remapping of dynamic objects on the receiving end. This allows properties previously pointing to a particular object to be updated if the object is re-created. Default is true."));

static bool bResolvedObjectsDispatchDebugging = false;
static FAutoConsoleVariableRef CvarResolvedObjectsDispatchDebugging(
	TEXT("net.Iris.ResolvedObjectsDispatchDebugging"),
	bResolvedObjectsDispatchDebugging,
	TEXT("Debug logging of resolved object state dispatching. Default is false."));

static int32 QueuedBatchTimeoutWarningInterval = 30;
static FAutoConsoleVariableRef CVarQueuedBatchTimeoutWarningInterval(
	TEXT("net.QueuedBatchTimeoutWarningInterval"),
	QueuedBatchTimeoutWarningInterval,
	TEXT("Number of ticks between checks for queuedbatches timeout."));

static float QueuedBatchTimeoutSeconds = 30.f;
static FAutoConsoleVariableRef CVarQueuedBatchTimeoutSeconds(
	TEXT("net.QueuedBatchTimeoutSeconds"),
	QueuedBatchTimeoutSeconds,
	TEXT("Time in seconds to wait for queued batches to flush before logging a warning."));

static int32 MaxMustBeMappedHandleArray = 16;
static FAutoConsoleVariableRef CVarMaxMustBeMappedHandleArray(
	TEXT("net.Iris.MaxMustBeMappedHandleArray"),
	MaxMustBeMappedHandleArray,
	TEXT("Maximum number of unresolved references that will be included with the QueuedBatch timeout error message."));

static bool bGracefullyHandleReachingEndOfBitstream = true;
static FAutoConsoleVariableRef CvarGracefullyHandleReachingEndOfBitstream(
		TEXT("net.Iris.ReplicationReader.GracefullyHandleReachingEndOfBitstream"),
		bGracefullyHandleReachingEndOfBitstream,
		TEXT("Gracefully handle reaching end of the ReplicationReader bitstream prior to reading as many object batches as expected. Default is true."));

static const FName NetError_FailedToFindAttachmentQueue("Failed to find attachment queue");

//------------------------------------------------------------------------

class FResolveAndCollectUnresolvedAndResolvedReferenceCollector
{
public:
	typedef FNetReferenceCollector::FReferenceInfoArray FReferenceInfoArray;

	void CollectReferences(FObjectReferenceCache& ObjectReferenceCache,
		const FNetObjectResolveContext& ResolveContext,
		bool bInIncludeInitState, 
		const FNetBitArrayView* ChangeMask,
		uint8* RESTRICT InternalBuffer, const FReplicationProtocol* Protocol)
	{
		bIncludeInitState = bInIncludeInitState;

		// Setup context
		FNetSerializationContext Context;
		Context.SetChangeMask(ChangeMask);
		Context.SetIsInitState(bInIncludeInitState);

		FNetReferenceCollector Collector;
		FReplicationProtocolOperationsInternal::CollectReferences(Context, Collector, InternalBuffer, Protocol);

		// Iterate over result and process results
		for (const FNetReferenceCollector::FReferenceInfo& Info : MakeArrayView(Collector.GetCollectedReferences()))
		{
			if (!ObjectReferenceCache.ResolveObjectReference(Info.Reference, ResolveContext))
			{
				UnresolvedReferenceInfos.Add(Info);
			}
			else
			{
				ResolvedReferenceInfos.Add(Info);
			}		
		}		
	}

	void Reset()
	{ 
		UnresolvedReferenceInfos.Reset();
		ResolvedReferenceInfos.Reset();
	}

	const FReferenceInfoArray& GetResolvedReferences() const { return ResolvedReferenceInfos; }
	const FReferenceInfoArray& GetUnresolvedReferences() const { return UnresolvedReferenceInfos; }
	bool IsInitStateIncluded() const { return bIncludeInitState; }

private:
	FReferenceInfoArray UnresolvedReferenceInfos;
	FReferenceInfoArray ResolvedReferenceInfos;
	bool bIncludeInitState = false;
};

//------------------------------------------------------------------------

struct FReplicationReader::FDispatchObjectInfo
{
	FInternalNetRefIndex InternalIndex = FNetRefHandleManager::InvalidInternalIndex;
	FChangeMaskStorageOrPointer ChangeMaskOrPointer;
	uint32 bIsInitialState : 1 = false;
	uint32 bHasState : 1 = false;
	uint32 bHasAttachments : 1 = false;
	uint32 bDestroy : 1 = false;
	uint32 bTearOff : 1 = false;
	uint32 bDeferredEndReplication : 1 = false;
	uint32 bReplicatedDestroyForSubObject : 1 = false;
	uint32 bProcessEndReplicationForHierarchy : 1 = false;
	uint32 bShouldCallSubObjectCreatedFromReplication : 1 = false;
	uint32 bDynamicObjectCreated : 1 = false;
};

//------------------------------------------------------------------------

// Helper class to deal with management of ObjectsToDispatch allocations from our temporary allocator
class FReplicationReader::FObjectsToDispatchArray
{
public:

	FObjectsToDispatchArray(uint32 InitialCapacity, FMemStackBase& Allocator)
	: ObjectsToDispatchCount(0U)
	, Capacity(InitialCapacity + ObjectsToDispatchSlackCount)
	{			
		ObjectsToDispatch = new (Allocator) FDispatchObjectInfo[Capacity];
	}

	void Grow(uint32 Count, FMemStackBase& Allocator)
	{
		if (Capacity < (ObjectsToDispatchCount + Count))
		{
			Capacity = ObjectsToDispatchCount + Count + ObjectsToDispatchSlackCount;
			FDispatchObjectInfo* NewObjectsToDispatch = new (Allocator) FDispatchObjectInfo[Capacity];

			// If we already had allocated data we need to copy the old elements
			if (ObjectsToDispatchCount)
			{
				FPlatformMemory::Memcpy(NewObjectsToDispatch, ObjectsToDispatch, ObjectsToDispatchCount*sizeof(FDispatchObjectInfo));
			}
			ObjectsToDispatch = NewObjectsToDispatch;
		}		
	}

	FDispatchObjectInfo& AddPendingDispatchObjectInfo(FMemStackBase& Allocator)
	{
		Grow(1, Allocator);
		ObjectsToDispatch[ObjectsToDispatchCount] = FDispatchObjectInfo();

		return ObjectsToDispatch[ObjectsToDispatchCount];
	}

	void CommitPendingDispatchObjectInfo()
	{
		checkSlow(ObjectsToDispatchCount < Capacity);
		++ObjectsToDispatchCount;
	}

	uint32 Num() const { return ObjectsToDispatchCount; }
	TArrayView<FDispatchObjectInfo> GetObjectsToDispatch() { return MakeArrayView(ObjectsToDispatch, ObjectsToDispatchCount); }

private:

	FDispatchObjectInfo* ObjectsToDispatch;
	uint32 ObjectsToDispatchCount;
	uint32 Capacity;		
};

//------------------------------------------------------------------------

FReplicationReader::FReplicatedObjectInfo::FReplicatedObjectInfo()
: InternalIndex(FNetRefHandleManager::InvalidInternalIndex)
, Value(0U)
{
	FMemory::Memzero(StoredBaselines);
	LastStoredBaselineIndex = FDeltaCompressionBaselineManager::InvalidBaselineIndex;
	PrevStoredBaselineIndex = FDeltaCompressionBaselineManager::InvalidBaselineIndex;
}

bool FReplicationReader::FReplicatedObjectInfo::RemoveUnresolvedHandleCount(FNetRefHandle RefHandle)
{
	int16* HandleCount = UnresolvedHandleCount.Find(RefHandle);
	if (ensureMsgf(HandleCount != nullptr, TEXT("Unresolved handle counter could not be found for %s"), ToCStr(RefHandle.ToString())))
	{
		int16& HandleCountRef = (*HandleCount);

		ensure(HandleCountRef > 0);

		HandleCountRef--;

		if (HandleCountRef <= 0)
		{
			UnresolvedHandleCount.Remove(RefHandle);
			return true;
		}
	}

	return false;
}

bool FReplicationReader::FReplicatedObjectInfo::RemoveResolvedDynamicHandleCount(FNetRefHandle RefHandle)
{
	int16* HandleCount = ResolvedDynamicHandleCount.Find(RefHandle);
	if (ensureMsgf(HandleCount != nullptr, TEXT("Resolved dynamic handle counter could not be found for% s"), ToCStr(RefHandle.ToString())))
	{
		int16& HandleCountRef = (*HandleCount);

		ensure(HandleCountRef > 0);

		HandleCountRef--;

		if (HandleCountRef <= 0)
		{
			ResolvedDynamicHandleCount.Remove(RefHandle);
			return true;
		}
	}

	return false;
}

FReplicationReader::FReplicationReader()
: TempLinearAllocator()
, TempChangeMaskAllocator(&TempLinearAllocator)
, ReplicationSystemInternal(nullptr)
, NetRefHandleManager(nullptr)
, StateStorage(nullptr)
, ObjectsToDispatchArray(nullptr)
, NetBlobHandlerManager(nullptr)
, NetObjectBlobType(InvalidNetBlobType)
, DelayAttachmentsWithUnresolvedReferences(IConsoleManager::Get().FindConsoleVariable(TEXT("net.DelayUnmappedRPCs"), false /* bTrackFrequentCalls */))
{
}

FReplicationReader::~FReplicationReader()
{
	checkf(ReplicatedObjects.IsEmpty(), TEXT("Possible leak detected in FReplicationReader. Nothing should be registered after Deinit()"));
	checkf(PendingBatches.PendingBatches.IsEmpty(), TEXT("Possible leak detected in FReplicationReader. Nothing should be registered after Deinit()"));
}

void FReplicationReader::Init(const FReplicationParameters& InParameters)
{
	// Store copy of parameters
	Parameters = InParameters;

	// Cache internal systems
	ReplicationSystemInternal = Parameters.ReplicationSystem->GetReplicationSystemInternal();
	NetRefHandleManager = &ReplicationSystemInternal->GetNetRefHandleManager();
	StateStorage = &ReplicationSystemInternal->GetReplicationStateStorage();
	NetBlobHandlerManager = &ReplicationSystemInternal->GetNetBlobHandlerManager();
	ObjectReferenceCache = &ReplicationSystemInternal->GetObjectReferenceCache();
	ReplicationBridge = Parameters.ReplicationSystem->GetReplicationBridge();

	// Init resolve context
	ResolveContext.ConnectionId = InParameters.ConnectionId;
	ResolveContext.RemoteNetTokenStoreState = Parameters.ReplicationSystem->GetNetTokenStore()->GetRemoteNetTokenStoreState(InParameters.ConnectionId);

	// Find out if there's a PartialNetObjectAttachmentHandler so we can re-assemble split blobs
	if (const UPartialNetObjectAttachmentHandler* Handler = ReplicationSystemInternal->GetNetBlobManager().GetPartialNetObjectAttachmentHandler())
	{
		FNetObjectAttachmentsReaderInitParams InitParams;
		InitParams.PartialNetObjectAttachmentHandler = Handler;
		Attachments.Init(InitParams);
	}

	if (const UNetBlobHandler* Handler = ReplicationSystemInternal->GetNetBlobManager().GetNetObjectBlobHandler())
	{
		NetObjectBlobType = Handler->GetNetBlobType();
	}

	// reserve index 0
	StartReplication(ObjectIndexForOOBAttachment);
}

void FReplicationReader::Deinit()
{
	for (FPendingBatchData& PendingBatchData : PendingBatches.PendingBatches)
	{
		UE_LOG(LogIris, Warning, TEXT("FReplicationReader::Deinit NetHandle %s has %d unprocessed data batches"), *PendingBatchData.Handle.ToString(), PendingBatchData.QueuedDataChunks.Num());

		// Make sure to release all references that we are holding on to
		if (ObjectReferenceCache)
		{
			for (const FNetRefHandle& RefHandle : PendingBatchData.ResolvedReferences)
			{
				ObjectReferenceCache->RemoveTrackedQueuedBatchObjectReference(RefHandle);
			}
		}		
	}
	PendingBatches.PendingBatches.Empty();

	// Cleanup any allocation stored in the per object info
	for (auto& ObjectIt : ReplicatedObjects)
	{
		CleanupObjectData(ObjectIt.Value);
	}
	ReplicatedObjects.Empty();
}

// Read incomplete handle
FNetRefHandle FReplicationReader::ReadNetRefHandleId(FNetSerializationContext& Context, FNetBitStreamReader& Reader) const
{
	UE_NET_TRACE_NAMED_OBJECT_SCOPE(ReferenceScope, FNetRefHandle::GetInvalid(), *Context.GetBitStreamReader(), Context.GetTraceCollector(), ENetTraceVerbosity::Verbose);

	const uint64 NetId = ReadPackedUint64(&Reader);
	FNetRefHandle RefHandle = FNetRefHandleManager::MakeNetRefHandleFromId(NetId);

	UE_NET_TRACE_SET_SCOPE_OBJECTID(ReferenceScope, RefHandle);

	if (RefHandle.GetId() != NetId)
	{
		Context.SetError(GNetError_InvalidNetHandle);
		return FNetRefHandle::GetInvalid();
	}

	return RefHandle;
}
	
void FReplicationReader::ReadStreamDebugFeatures(FNetSerializationContext& Context)
{
	FNetBitStreamReader* Reader = Context.GetBitStreamReader();
	StreamDebugFeatures = ReadReplicationDataStreamDebugFeatures(Reader);

#if !UE_NET_REPLICATIONDATASTREAM_DEBUG
	// If datastream debug features isn't enabled in this build the debug functions won't do any useful work and will cause bitstream errors.
	if (!ensure(StreamDebugFeatures == EReplicationDataStreamDebugFeatures::None))
	{
		UE_LOG(LogIris, Error, TEXT("StreamDebugFeatures enabled on sending side despite receiving side not being able to use them."));
		Context.SetError(GNetError_InvalidValue);
		return;
	}
#endif
}

uint32 FReplicationReader::ReadObjectsPendingDestroy(FNetSerializationContext& Context)
{
	return ReadRootObjectsPendingDestroy(Context);
}

uint32 FReplicationReader::ReadObjectsAndSubObjectsPendingDestroy(FNetSerializationContext& Context)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	UE_NET_TRACE_SCOPE(ObjectsAndSubObjectsPendingDestroy, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

	// Read how many destroyed objects we have
	constexpr uint32 DestroyObjectBitCount = 16U;
	constexpr uint32 MaxDestroyObjectCount = (1U << DestroyObjectBitCount) - 1U;
	const uint32 ObjectsToRead = Reader.ReadBits(DestroyObjectBitCount);
	
	if (Context.HasErrorOrOverflow())
	{
		return 0;
	}

	const bool bHasPendingBatches = PendingBatches.GetHasPendingBatches();
	
	for (uint32 It = 0; It < ObjectsToRead; ++It)
	{
		UE_NET_TRACE_NAMED_OBJECT_SCOPE(DestroyedObjectScope, FNetRefHandle::GetInvalid(), Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

		FNetRefHandle IncompleteHandle = ReadNetRefHandleId(Context, Reader);
		FNetRefHandle SubObjectRootOrHandle = IncompleteHandle;
		if (Reader.ReadBool())
		{
			SubObjectRootOrHandle = ReadNetRefHandleId(Context, Reader);
		}
		const bool bShouldDestroyInstance = Reader.ReadBool();
		if (Context.HasErrorOrOverflow())
		{
			break;
		}

		if (FPendingBatchData* PendingBatchData = bHasPendingBatches ? PendingBatches.Find(SubObjectRootOrHandle) : nullptr)
		{
			// We expect end replication requests for subobjects too so we should only handle one object at a time.
			constexpr bool bShouldProcessHierarchy = false;
			EnqueueEndReplication(PendingBatchData, bShouldDestroyInstance, bShouldProcessHierarchy, IncompleteHandle);
			continue;
		}

		{
			// Resolve handle and destroy using bridge
			const uint32 InternalIndex = NetRefHandleManager->GetInternalIndex(IncompleteHandle);
			if (InternalIndex != FNetRefHandleManager::InvalidInternalIndex)
			{
				UE_NET_TRACE_SET_SCOPE_OBJECTID(DestroyedObjectScope, IncompleteHandle);

				// Defer EndReplication until after applying state data
				if (bDeferEndReplication)
				{
					FDispatchObjectInfo& Info = ObjectsToDispatchArray->AddPendingDispatchObjectInfo(TempLinearAllocator);

					Info.bDestroy = bShouldDestroyInstance;
					Info.bTearOff = false;
					Info.bDeferredEndReplication = true;
					Info.InternalIndex = InternalIndex;
					Info.bIsInitialState = 0U;
					Info.bHasState = 0U;
					Info.bHasAttachments = 0U;
					Info.bShouldCallSubObjectCreatedFromReplication = 0U;

					// Mark for dispatch
					ObjectsToDispatchArray->CommitPendingDispatchObjectInfo();
				}
				else
				{
					EndReplication(InternalIndex, false, bShouldDestroyInstance);
				}
			}
			else
			{
				// If we did not find the object or associated bridge, the packet that would have created the object may have been lost.
				UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::Read Tried to destroy object %s. This can occur if the server sends destroy for an object that has not yet been confirmed as created."), *IncompleteHandle.ToString());
			}
		}
	}

	return ObjectsToRead;
}

uint32 FReplicationReader::ReadRootObjectsPendingDestroy(FNetSerializationContext& Context)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	UE_NET_TRACE_SCOPE(RootObjectsPendingDestroy, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

	// Read how many destroyed objects we have
	constexpr uint32 DestroyObjectBitCount = 16U;
	constexpr uint32 MaxDestroyObjectCount = (1U << DestroyObjectBitCount) - 1U;
	const uint32 ObjectsToRead = Reader.ReadBits(DestroyObjectBitCount);
	
	if (Context.HasErrorOrOverflow())
	{
		return 0;
	}

	const bool bHasPendingBatches = PendingBatches.GetHasPendingBatches();
	
	for (uint32 It = 0; It < ObjectsToRead; ++It)
	{
		UE_NET_TRACE_NAMED_OBJECT_SCOPE(DestroyedObjectScope, FNetRefHandle::GetInvalid(), Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

		FNetRefHandle IncompleteHandle = ReadNetRefHandleId(Context, Reader);
		const bool bShouldDestroyInstance = Reader.ReadBool();
		if (Context.HasErrorOrOverflow())
		{
			break;
		}

		if (FPendingBatchData* PendingBatchData = bHasPendingBatches ? PendingBatches.Find(IncompleteHandle) : nullptr)
		{
			constexpr bool bShouldProcessHierarchy = true;
			EnqueueEndReplication(PendingBatchData, bShouldDestroyInstance, bShouldProcessHierarchy, IncompleteHandle);
			continue;
		}

		{
			// Resolve handle and destroy using bridge
			const FInternalNetRefIndex InternalIndex = NetRefHandleManager->GetInternalIndex(IncompleteHandle);
			if (InternalIndex != FNetRefHandleManager::InvalidInternalIndex)
			{
				UE_NET_TRACE_SET_SCOPE_OBJECTID(DestroyedObjectScope, IncompleteHandle);

				// Defer EndReplication until after applying state data
				if (bDeferEndReplication)
				{
					FDispatchObjectInfo& Info = ObjectsToDispatchArray->AddPendingDispatchObjectInfo(TempLinearAllocator);

					Info.bDestroy = bShouldDestroyInstance;
					Info.bTearOff = false;
					Info.bDeferredEndReplication = true;
					Info.bProcessEndReplicationForHierarchy = true;
					Info.InternalIndex = InternalIndex;
					Info.bIsInitialState = 0U;
					Info.bHasState = 0U;
					Info.bHasAttachments = 0U;
					Info.bShouldCallSubObjectCreatedFromReplication = 0U;

					// Mark for dispatch
					ObjectsToDispatchArray->CommitPendingDispatchObjectInfo();
				}
				else
				{
					constexpr bool bTearOff = false;
					EndReplication(InternalIndex, bTearOff, bShouldDestroyInstance);
					for (FInternalNetRefIndex SubObjectIndex : NetRefHandleManager->GetSubObjects(InternalIndex))
					{
						// End replication for all subobjects and destroy dynamic ones
						const FNetRefHandle SubObjectRefHandle = NetRefHandleManager->GetNetRefHandleFromInternalIndex(SubObjectIndex);
						const bool bShouldDestroySubObject = SubObjectRefHandle.IsDynamic();
						EndReplication(SubObjectIndex, bTearOff, bShouldDestroySubObject);
					}
				}
			}
			else
			{
				// If we did not find the object or associated bridge, the packet that would have created the object may have been lost.
				UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::Read Tried to destroy object %s. This can occur if the server sends destroy for an object that has not yet been confirmed as created."), *IncompleteHandle.ToString());
			}
		}
	}

	return ObjectsToRead;
}

FReplicationReader::FReplicatedObjectInfo& FReplicationReader::StartReplication(uint32 InternalIndex)
{
	check(!ReplicatedObjects.Contains(InternalIndex));

	// Create ReadObjectInfo
	FReplicatedObjectInfo& ObjectInfo = ReplicatedObjects.Emplace(InternalIndex);
	ObjectInfo = FReplicatedObjectInfo();
	ObjectInfo.InternalIndex = InternalIndex;

	// Allocate changemask (if needed)
	if (InternalIndex != 0U)
	{		
		const FNetRefHandleManager::FReplicatedObjectData& Data = NetRefHandleManager->GetReplicatedObjectDataNoCheck(InternalIndex);
		ObjectInfo.ChangeMaskBitCount = Data.Protocol->ChangeMaskBitCount;

		// Alloc and init changemask
		FNetBitArrayView ChangeMask = FChangeMaskStorageOrPointer::AllocAndInitBitArray(ObjectInfo.UnresolvedChangeMaskOrPointer, ObjectInfo.ChangeMaskBitCount, PersistentChangeMaskAllocator);
	}

	return ObjectInfo;
}

FReplicationReader::FReplicatedObjectInfo* FReplicationReader::GetReplicatedObjectInfo(uint32 InternalIndex)
{
	return ReplicatedObjects.Find(InternalIndex);
}

const FReplicationReader::FReplicatedObjectInfo* FReplicationReader::GetReplicatedObjectInfo(uint32 InternalIndex) const
{
	return ReplicatedObjects.Find(InternalIndex);
}

void FReplicationReader::CleanupObjectData(FReplicatedObjectInfo& ObjectInfo)
{
	// Remove from pending resolve
	if (ObjectInfo.InternalIndex != 0U)
	{
		FChangeMaskStorageOrPointer::Free(ObjectInfo.UnresolvedChangeMaskOrPointer, ObjectInfo.ChangeMaskBitCount, PersistentChangeMaskAllocator);
	}

	// Release stored baselines
	if (ObjectInfo.LastStoredBaselineIndex != FDeltaCompressionBaselineManager::InvalidBaselineIndex)
	{
		StateStorage->FreeBaseline(ObjectInfo.InternalIndex, ObjectInfo.StoredBaselines[ObjectInfo.LastStoredBaselineIndex]);
	}
	if (ObjectInfo.PrevStoredBaselineIndex != FDeltaCompressionBaselineManager::InvalidBaselineIndex)
	{
		StateStorage->FreeBaseline(ObjectInfo.InternalIndex, ObjectInfo.StoredBaselines[ObjectInfo.PrevStoredBaselineIndex]);
	}
}

void FReplicationReader::EndReplication(uint32 InternalIndex, bool bTearOff, bool bDestroyInstance)
{
	if (!ensure(InternalIndex != ObjectIndexForOOBAttachment))
	{
		return;
	}
	if (FReplicatedObjectInfo* ObjectInfo = ReplicatedObjects.Find(InternalIndex))
	{
		const FNetRefHandleManager::FReplicatedObjectData& Data = NetRefHandleManager->GetReplicatedObjectDataNoCheck(InternalIndex);

		CleanupReferenceTracking(ObjectInfo);
		Attachments.DropAllAttachments(ENetObjectAttachmentType::Normal, InternalIndex);

		EReplicationBridgeDestroyInstanceReason DestroyReason = EReplicationBridgeDestroyInstanceReason::DoNotDestroy;
		if (bTearOff)
		{
			DestroyReason = EReplicationBridgeDestroyInstanceReason::TearOff;
		}
		else if (bDestroyInstance)
		{
			DestroyReason = EReplicationBridgeDestroyInstanceReason::Destroy;
		}
		EReplicationBridgeDestroyInstanceFlags DestroyFlags = (Data.bAllowDestroyInstanceFromRemote ? EReplicationBridgeDestroyInstanceFlags::AllowDestroyInstanceFromRemote : EReplicationBridgeDestroyInstanceFlags::None);
		ReplicationBridge->DestroyNetObjectFromRemote(Data.RefHandle, DestroyReason, DestroyFlags);

		CleanupObjectData(*ObjectInfo);

		ReplicatedObjects.Remove(InternalIndex);
	}
}

void FReplicationReader::DeserializeObjectStateDelta(FNetSerializationContext& Context, uint32 InternalIndex, FDispatchObjectInfo& Info, FReplicatedObjectInfo& ObjectInfo, const FNetRefHandleManager::FReplicatedObjectData& ObjectData, uint32& OutNewBaselineIndex)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	uint32 BaselineIndex = FDeltaCompressionBaselineManager::InvalidBaselineIndex;
	BaselineIndex = Reader.ReadBits(FDeltaCompressionBaselineManager::BaselineIndexBitCount);
	if (BaselineIndex != FDeltaCompressionBaselineManager::InvalidBaselineIndex)
	{ 
		const bool bIsNewBaseline = Reader.ReadBool();

		if (Reader.IsOverflown())
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::DeserializeObjectStateDelta Bitstream corrupted."));
			return;
		}

		if (bIsNewBaseline)
		{
			OutNewBaselineIndex = (BaselineIndex + 1) % FDeltaCompressionBaselineManager::MaxBaselineCount;
		}

		// If we are compressing against the LastStoredBaselineIndex we can release older baselines to reduce memory overhead
		if (!bIsNewBaseline && BaselineIndex == ObjectInfo.LastStoredBaselineIndex && ObjectInfo.PrevStoredBaselineIndex != FDeltaCompressionBaselineManager::InvalidBaselineIndex)
		{
			StateStorage->FreeBaseline(InternalIndex, ObjectInfo.StoredBaselines[ObjectInfo.PrevStoredBaselineIndex]);
			ObjectInfo.StoredBaselines[ObjectInfo.PrevStoredBaselineIndex] = nullptr;
			ObjectInfo.PrevStoredBaselineIndex = FDeltaCompressionBaselineManager::InvalidBaselineIndex;
		}

		check(ObjectInfo.StoredBaselines[BaselineIndex]);

		UE_NET_TRACE_SCOPE(DeltaCompressed, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
		FReplicationProtocolOperations::DeserializeWithMaskDelta(Context, Info.ChangeMaskOrPointer.GetPointer(ObjectInfo.ChangeMaskBitCount), ObjectData.ReceiveStateBuffer, ObjectInfo.StoredBaselines[BaselineIndex], ObjectData.Protocol);
	}
	else
	{
		const uint32 NewBaselineIndex = Reader.ReadBits(FDeltaCompressionBaselineManager::BaselineIndexBitCount);
		if (Reader.IsOverflown())
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::DeserializeObjectStateDelta Bitstream corrupted."));
			return;
		}
		OutNewBaselineIndex = NewBaselineIndex;
		FReplicationProtocolOperations::DeserializeWithMask(Context, Info.ChangeMaskOrPointer.GetPointer(ObjectInfo.ChangeMaskBitCount), ObjectData.ReceiveStateBuffer, ObjectData.Protocol);
	}
}

FPendingBatchData* FReplicationReader::UpdateUnresolvedMustBeMappedReferences(FNetRefHandle InHandle, TArray<FNetRefHandle>& MustBeMappedReferences)
{
	FPendingBatchData* PendingBatch = PendingBatches.Find(InHandle);
	// If we already have a pending batch we append any new must be mapped references to it.
	if (PendingBatch)
	{
		for (const FNetRefHandle Ref : PendingBatch->PendingMustBeMappedReferences)
		{
			MustBeMappedReferences.AddUnique(Ref);
		}
	}

	// Resolve
	TArray<FNetRefHandle, TInlineAllocator<4>> Unresolved;
	TArray<TPair<FNetRefHandle, UObject*>, TInlineAllocator<4>> QueuedObjectsToTrack;

	Unresolved.Reserve(MustBeMappedReferences.Num());
	QueuedObjectsToTrack.SetNum(MustBeMappedReferences.Num());
	
	for (FNetRefHandle Handle : MustBeMappedReferences)
	{
		UObject* ResolvedObject = nullptr;
		// TODO: Report broken status in same call to avoid map lookup
		ENetObjectReferenceResolveResult ResolveResult = ObjectReferenceCache->ResolveObjectReference(FObjectReferenceCache::MakeNetObjectReference(Handle), ResolveContext, ResolvedObject);
		if (EnumHasAnyFlags(ResolveResult, ENetObjectReferenceResolveResult::HasUnresolvedMustBeMappedReferences) && !ObjectReferenceCache->IsNetRefHandleBroken(Handle, true))
		{
			Unresolved.Add(Handle);
		}
		else if (ResolveResult == ENetObjectReferenceResolveResult::None)
		{
			QueuedObjectsToTrack.Emplace(Handle, ResolvedObject);
		}
	}

	if (Unresolved.Num())
	{
		FPendingBatchData* Batch = PendingBatch;
		// We must create a new batch
		if (!Batch)
		{
			Batch = &PendingBatches.PendingBatches.AddDefaulted_GetRef();
			Batch->Handle = InHandle;
			Batch->PendingBatchStartCycles = FPlatformTime::Cycles64();
		}

		// Update
		Batch->PendingMustBeMappedReferences = MoveTemp(Unresolved);
	
		// If we resolved more references, add them to tracking list
		for (TPair<FNetRefHandle, UObject*>& NetRefHandleObjectPair : QueuedObjectsToTrack)
		{
			if (!Batch->ResolvedReferences.Contains(NetRefHandleObjectPair.Key))
			{
				Batch->ResolvedReferences.Add(NetRefHandleObjectPair.Key);
				ObjectReferenceCache->AddTrackedQueuedBatchObjectReference(NetRefHandleObjectPair.Key, NetRefHandleObjectPair.Value);
			}
		}

		return Batch;
	}
	else if (PendingBatch)
	{
		PendingBatch->PendingMustBeMappedReferences.Reset();

		// If we resolved more references, add them to tracking list
		for (TPair<FNetRefHandle, UObject*>& NetRefHandleObjectPair : QueuedObjectsToTrack)
		{
			if (!PendingBatch->ResolvedReferences.Contains(NetRefHandleObjectPair.Key))
			{
				PendingBatch->ResolvedReferences.Add(NetRefHandleObjectPair.Key);
				ObjectReferenceCache->AddTrackedQueuedBatchObjectReference(NetRefHandleObjectPair.Key, NetRefHandleObjectPair.Value);
			}
		}
		
		return PendingBatch;
	}
	
	return nullptr;
}

uint32 FReplicationReader::ReadObjectsInBatchWithoutSizes(FNetSerializationContext& Context, FNetRefHandle IncompleteHandle, bool bHasBatchOwnerData, uint32 BatchEndBitPosition)
{
	uint32 ReadObjectCount = 0;
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	// If the batch owner had state, we read it now
	if (bHasBatchOwnerData)
	{
		ReadObjectInBatch(Context, IncompleteHandle, false);
		if (Context.HasErrorOrOverflow())
		{
			return 0U;
		}
		++ReadObjectCount;
	}

	if (!ensure(Reader.GetPosBits() <= BatchEndBitPosition))
	{
		Context.SetError(GNetError_BitStreamOverflow);
		return 0U;
	}

	// ReadSubObjects 
	while (Reader.GetPosBits() < BatchEndBitPosition)
	{
		ReadObjectInBatch(Context, IncompleteHandle, true);
		if (Context.HasErrorOrOverflow())
		{
			return 0U;
		}
		++ReadObjectCount;
	}

	return ReadObjectCount;
}

uint32 FReplicationReader::ReadObjectsInBatchWithSizes(FNetSerializationContext& Context, FNetRefHandle IncompleteHandle, const bool bHasBatchOwnerData, uint32 BatchEndBitPosition)
{
#if !UE_NET_REPLICATIONDATASTREAM_DEBUG
	return 0;
#else
	const uint32 BatchSize = BatchEndBitPosition - Context.GetBitStreamReader()->GetPosBits();
	FNetBitStreamReader BatchReader = Context.GetBitStreamReader()->CreateSubstream(BatchSize);

	FNetSerializationContext ObjectContext;
	FNetBitStreamReader ObjectReader;

	bool bIsSubObject = !bHasBatchOwnerData;
	uint32 ReadObjectCount = 0;
	while (BatchReader.GetBitsLeft() > 0U)
	{
		{
			UE_NET_TRACE_SCOPE(BatchSize, BatchReader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
			const uint32 ObjectSize = BatchReader.ReadBits(Parameters.NumBitsUsedForHugeObjectBatchSize);
			ObjectReader = BatchReader.CreateSubstream(ObjectSize);

			// If we were unable to create a substream of the correct size we have bitstream corruption.
			if (ObjectReader.GetBitsLeft() != ObjectSize)
			{
				BatchReader.DiscardSubstream(ObjectReader);
				Context.GetBitStreamReader()->DiscardSubstream(BatchReader);
				Context.SetError(GNetError_BitStreamError);
				break;
			}
		}

		ObjectContext = Context.MakeSubContext(&ObjectReader);
		ReadObjectInBatch(ObjectContext, IncompleteHandle, bIsSubObject);

		// Check for error
		if (ObjectContext.HasErrorOrOverflow())
		{
			BatchReader.DiscardSubstream(ObjectReader);
			Context.GetBitStreamReader()->DiscardSubstream(BatchReader);
			Context.SetError(ObjectContext.GetError(), ObjectReader.IsOverflown());
			break;
		}

		// Check for underflow
		if (ObjectReader.GetBitsLeft() != 0U)
		{
			BatchReader.DiscardSubstream(ObjectReader);
			Context.GetBitStreamReader()->DiscardSubstream(BatchReader);
			Context.SetError(GNetError_BitStreamError);
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObjectsInBatchWithSizes Bitstream underflow after reading replicated object with Handle: %s. %s"), ToCStr(ObjectContext.GetErrorHandleContext().ToString()), ToCStr(ObjectContext.PrintReadJournal()));
			break;
		}

		BatchReader.CommitSubstream(ObjectReader);

		++ReadObjectCount;
		// Only the first batch can be the subobject owner. The rest will be subobjects.
		bIsSubObject = true;
	}

	if (Context.HasErrorOrOverflow())
	{
		return 0U;
	}
	else
	{
		Context.GetBitStreamReader()->CommitSubstream(BatchReader);
		return ReadObjectCount;
	}
#endif
}

uint32 FReplicationReader::ReadObjectsInBatch(FNetSerializationContext& Context, FNetRefHandle IncompleteHandle, bool bHasBatchOwnerData, uint32 BatchEndBitPosition)
{
#if UE_NET_REPLICATIONDATASTREAM_DEBUG
	if (EnumHasAnyFlags(StreamDebugFeatures, EReplicationDataStreamDebugFeatures::BatchSizePerObject))
	{
		return ReadObjectsInBatchWithSizes(Context, IncompleteHandle, bHasBatchOwnerData, BatchEndBitPosition);
	}
	else
#endif
	{
		return ReadObjectsInBatchWithoutSizes(Context, IncompleteHandle, bHasBatchOwnerData, BatchEndBitPosition);
	}
}

uint32 FReplicationReader::ReadObjectBatch(FNetSerializationContext& Context, uint32 ReadObjectFlags)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	UE_NET_TRACE_SCOPE(Batch, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

	UE_ADD_READ_JOURNAL_ENTRY(Context, TEXT("ReadObjectBatch"));

	// Special handling for destruction infos
	if (const bool bIsDestructionInfo = Reader.ReadBool())
	{
		FReplicationBridgeSerializationContext BridgeContext(Context, Parameters.ConnectionId, true);

		// For destruction infos we inline the exports
		FForceInlineExportScope ForceInlineExportScope(Context.GetInternalContext());
		ReplicationBridge->ReadAndExecuteDestructionInfoFromRemote(BridgeContext);

		ReadSentinel(Context, TEXT("DestructionInfo"));

		if (Context.HasErrorOrOverflow())
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read destruction info. \n%s"), *Context.PrintReadJournal());
			Context.SetError(GNetError_BitStreamError);
			return 0U;
		}

		return 1U;
	}
	
	if (!ReadSentinel(Context, TEXT("Object")))
	{
		UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read object sentinel. \n%s"), *Context.PrintReadJournal());
		Context.SetError(GNetError_BitStreamError);
		return 0U;
	}

	uint32 ObjectsReadInBatch = 0U;
	
	// A batch starts with (RefHandleId | BatchSize | bHasBatchObjectData | bHasExports)
	// If the batch has exports we must to seek to the end of the batch to read and process exports before reading/processing batch data
	const FNetRefHandle IncompleteHandle = ReadNetRefHandleId(Context, Reader);

	uint32 BatchSize = 0U;
	// Read Batch size
	{
		const uint32 NumBitsUsedForBatchSize = (ReadObjectFlags & EReadObjectFlag::ReadObjectFlag_IsReadingHugeObjectBatch) == 0U ? Parameters.NumBitsUsedForBatchSize : Parameters.NumBitsUsedForHugeObjectBatchSize;

		UE_NET_TRACE_SCOPE(BatchSize, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
		BatchSize = Reader.ReadBits(NumBitsUsedForBatchSize);
	}

	// Store the current handle if we encounter errors
	Context.SetErrorHandleContext(IncompleteHandle);

	if (Context.HasErrorOrOverflow() || BatchSize > Reader.GetBitsLeft())
	{
		Context.SetError(GNetError_InvalidValue);
		return 0U;
	}

	// This either marks the end of the data associated with this batch or the offset in the stream where exports are stored.
	const uint32 BatchEndOrStartOfExportsPos = Reader.GetPosBits() + BatchSize;

	// Do we have state data or attachments for the owner of the batch?
	const bool bHasBatchOwnerData = Reader.ReadBool();

	// Do we have exports or not?
	const bool bHasExports = Reader.ReadBool();
	
	uint32 ReadObjectCount = 0U;

	// First we need to read exports, they are stored at the end of the batch
	uint32 BatchEndPos = BatchEndOrStartOfExportsPos;

	TempMustBeMappedReferences.Reset();
	if (bHasExports)
	{
		const uint32 ReturnPos = Reader.GetPosBits();

		// Seek to the export section
		Reader.Seek(BatchEndPos);

		// Read exports and any must be mapped references
		ObjectReferenceCache->ReadExports(Context, &TempMustBeMappedReferences);
		if (Context.HasErrorOrOverflow())
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read exports for handle: %s.\n%s"), *IncompleteHandle.ToString(), *Context.PrintReadJournal());
			return 0U;
		}

		// Update BatchEndPos if we successfully read exports
		BatchEndPos = Reader.GetPosBits();

		// Seek back to state data
		Reader.Seek(ReturnPos);
	}

	// Skip over broken objects
	const bool bIsBroken = BrokenObjects.FindByPredicate([IncompleteHandle](const FNetRefHandle& Entry) { return Entry.GetId() == IncompleteHandle.GetId(); } ) != nullptr;
	if (bIsBroken)
	{
		UE_NET_TRACE_OBJECT_SCOPE(IncompleteHandle, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
		UE_NET_TRACE_SCOPE(SkippedData, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

		Reader.Seek(BatchEndPos);
		return 0U;
	}

	FPendingBatchData* PendingBatch = PendingBatches.Find(IncompleteHandle);

	// This object has pending must be mapped references that must be resolved before we can process the data.
	FPendingBatchData* PendingBatchData = ObjectReferenceCache->ShouldAsyncLoad() ? UpdateUnresolvedMustBeMappedReferences(IncompleteHandle, TempMustBeMappedReferences) : nullptr;
	if (PendingBatchData)
	{
		UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::ReadObjectBatch Handle %s will be defered as it has unresolved must be mapped references"), *IncompleteHandle.ToString());

		UE_NET_TRACE_OBJECT_SCOPE(IncompleteHandle, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
		UE_NET_TRACE_SCOPE(QueuedBatch, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

		// Enqueue BatchData
		FQueuedDataChunk DataChunk;
		
		const uint32 NumDataBits = BatchEndOrStartOfExportsPos - Reader.GetPosBits();
		const uint32 NumDataWords = (NumDataBits + 31U) / 32U;

		DataChunk.NumBits = NumDataBits;
		DataChunk.StorageOffset = PendingBatchData->DataChunkStorage.Num();
		DataChunk.bHasBatchOwnerData = bHasBatchOwnerData;
		DataChunk.bIsEndReplicationChunk = false;
		DataChunk.StreamDebugFeatures = StreamDebugFeatures;

		// Make sure we have space
		PendingBatchData->DataChunkStorage.AddUninitialized(NumDataWords);
	
		// Store batch data
		Reader.ReadBitStream(PendingBatchData->DataChunkStorage.GetData() + DataChunk.StorageOffset, DataChunk.NumBits);

		if (Context.HasErrorOrOverflow())
		{
			// Log error, this is something we cannot recover from.
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read object batch data chunk for handle:%s \n%s"), ToCStr(IncompleteHandle.ToString()), *Context.PrintReadJournal());

			return 0U;
		}

		PendingBatchData->QueuedDataChunks.Add(MoveTemp(DataChunk));
	}
	else
	{
		ReadObjectCount = ReadObjectsInBatch(Context, IncompleteHandle, bHasBatchOwnerData, BatchEndOrStartOfExportsPos);

		if (Context.HasErrorOrOverflow())
		{
			if (Context.GetError() == GNetError_BrokenNetHandle)
			{
				ReplicationBridge->SendErrorWithNetRefHandle(UE::Net::ENetRefHandleError::ReplicationDisabled, IncompleteHandle, Parameters.ConnectionId);
 
				// Log error and try to recover, if get more incoming data for an object in the broken state we will skip it.
				UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read object batch handle: %s skipping batch data"), ToCStr(IncompleteHandle.ToString()));
					
				BrokenObjects.AddUnique(IncompleteHandle);

				Context.ResetErrorContext();

				UE_NET_TRACE_OBJECT_SCOPE(IncompleteHandle, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);
				UE_NET_TRACE_SCOPE(SkippedData, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

				// Skip this batch
				Reader.Seek(BatchEndPos);
			}
			
			return 0U;
		}
	}

	if (!ensure(Reader.GetPosBits() == BatchEndOrStartOfExportsPos))
	{
		UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObjectsInBatch Did not read the expected number of bits when reading batch: %s. \n%s"), *IncompleteHandle.ToString(), *Context.PrintReadJournal());
		Context.SetError(GNetError_BitStreamOverflow, true);
		return 0U;
	}

	// Skip to the end as we already have read any exports
	Reader.Seek(BatchEndPos);

	return ReadObjectCount;
}

void FReplicationReader::ReadObjectInBatch(FNetSerializationContext& Context, FNetRefHandle BatchHandle, bool bIsSubObject)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	const FNetRefHandle IncompleteHandle = !bIsSubObject ? BatchHandle : ReadNetRefHandleId(Context, Reader);
	
	// Read replicated destroy header if necessary. We don't know the internal index yet so can't do the more appropriate check IsObjectIndexForOOBAttachment.
	const bool bReadReplicatedDestroyHeader = IncompleteHandle.IsValid();
	const uint32 ReplicatedDestroyHeaderFlags = bReadReplicatedDestroyHeader ? Reader.ReadBits(ReplicatedDestroyHeaderFlags_BitCount) : ReplicatedDestroyHeaderFlags_None;

	const bool bHasState = Reader.ReadBool();
	if (bHasState)
	{
		if (!ReadSentinel(Context, TEXT("HasState")))
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read replicated object with Handle: %s. Error '%s'. %s"), ToCStr(IncompleteHandle.ToString()), (Context.HasError() ? ToCStr(Context.GetError().ToString()) : TEXT("BitStream Overflow")), ToCStr(Context.PrintReadJournal()));
			return;
		}
	}

	uint32 NewBaselineIndex = FDeltaCompressionBaselineManager::InvalidBaselineIndex;

	const bool bIsInitialState = bHasState && Reader.ReadBool();
	bool bShouldCallSubObjectCreatedFromReplication = false;
	uint32 InternalIndex = ObjectIndexForOOBAttachment;

	UE_NET_TRACE_OBJECT_SCOPE(IncompleteHandle, Reader, Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

	// Store the current handle in case we encounter errors
	if (IncompleteHandle != BatchHandle)
	{
		Context.SetErrorHandleContext(IncompleteHandle);
	}

	//UE_LOG_REPLICATIONREADER(TEXT("FReplicationReader::Read Object with %s InitialState: %u"), *IncompleteHandle.ToString(), bIsInitialState ? 1u : 0u);

	bool bHasErrors = false;
	bool bIsReplicatedDestroyForInvalidObject = false;

	// Read creation data
	Context.SetIsInitState(bIsInitialState);
	if (bIsInitialState)
	{
		UE_NET_TRACE_SCOPE(CreationInfo, *Context.GetBitStreamReader(), Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

		UE_ADD_READ_JOURNAL_ENTRY(Context, TEXT("ReadCreationInfo"));

		// SubObject data for initial state
		FNetRefHandle RootObjectOfSubObject;
		if (bIsSubObject)
		{
			// The owner is the same as the Batch owner
			const FNetRefHandle IncompleteOwnerHandle = BatchHandle;
				
			FInternalNetRefIndex RootObjectInternalIndex = NetRefHandleManager->GetInternalIndex(IncompleteOwnerHandle);
			if (Reader.IsOverflown() || RootObjectInternalIndex == FNetRefHandleManager::InvalidInternalIndex)
			{
				UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Invalid subobjectowner handle. %s"), ToCStr(IncompleteOwnerHandle.ToString()));
				const FName& NetError = (Reader.IsOverflown() ? GNetError_BitStreamOverflow : GNetError_InvalidNetHandle);
				Context.SetError(NetError);

				bHasErrors = true;
				goto ErrorHandling;
			}

			RootObjectOfSubObject = NetRefHandleManager->GetReplicatedObjectDataNoCheck(RootObjectInternalIndex).RefHandle;
		}

		const bool bIsDeltaCompressed = Reader.ReadBool();
		if (bIsDeltaCompressed)
		{
			UE_LOG_REPLICATIONREADER(TEXT("DeltaCompression is enabled for Handle %s"), *IncompleteHandle.ToString());
			NewBaselineIndex = Reader.ReadBits(FDeltaCompressionBaselineManager::BaselineIndexBitCount);
		}
		
		// We got a read error
		if (Reader.IsOverflown() || !IncompleteHandle.IsValid())
		{
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Bitstream corrupted."));
			const FName& NetError = (Reader.IsOverflown() ? GNetError_BitStreamOverflow : GNetError_BitStreamError);
			Context.SetError(NetError);

			bHasErrors = true;
			goto ErrorHandling;
		}
	
		// Get Bridge
		FReplicationBridgeSerializationContext BridgeContext(Context, Parameters.ConnectionId);
		const FReplicationBridgeCreateNetRefHandleResult CreateResult = ReplicationBridge->CallCreateNetRefHandleFromRemote(RootObjectOfSubObject, IncompleteHandle, BridgeContext);
		FNetRefHandle NetRefHandle = CreateResult.NetRefHandle;
		if (!NetRefHandle.IsValid())
		{	
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Unable to create handle for %s."), *IncompleteHandle.ToString());

			// Mark error, but do not mark the bitstream as overflown as we want to handle this error.
			Context.SetError(GNetError_BrokenNetHandle, false);

			bHasErrors = true;
			goto ErrorHandling;
		}

		// If this handle is considered unresolved, add it to the hot cache to force a resolve.
		RemoveFromUnresolvedCache(NetRefHandle);

		InternalIndex = NetRefHandleManager->GetInternalIndex(NetRefHandle);
		FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(InternalIndex);
		ObjectData.bAllowDestroyInstanceFromRemote = EnumHasAnyFlags(CreateResult.Flags, EReplicationBridgeCreateNetRefHandleResultFlags::AllowDestroyInstanceFromRemote);
		bShouldCallSubObjectCreatedFromReplication = EnumHasAnyFlags(CreateResult.Flags, EReplicationBridgeCreateNetRefHandleResultFlags::ShouldCallSubObjectCreatedFromReplication);
		
		FReplicatedObjectInfo& ObjectInfo = StartReplication(InternalIndex);

		ObjectInfo.bIsDeltaCompressionEnabled = bIsDeltaCompressed;

#if IRIS_CLIENT_PROFILER_ENABLE
		if (UE::Net::FClientProfiler::IsCapturing())
		{
			UObject* Object = NetRefHandleManager->GetReplicatedObjectInstance(InternalIndex);
			if (Object)
			{
				UE::Net::FClientProfiler::RecordObjectCreate(Object->GetClass()->GetFName(), bIsSubObject);
			}
		}
#endif
	}
	else
	{
		bHasErrors = Context.HasErrorOrOverflow();
		UE_CLOG(bHasErrors, LogIris, Error, TEXT("FReplicationReader::ReadObject ErrorOrOverFlow after reading object header. Overflow: %c Error: %s Bit stream bits left: %u position: %u %s"), TEXT("YN")[Context.HasError()], ToCStr(Context.GetError().ToString()), Reader.GetBitsLeft(), Reader.GetPosBits(), *Context.PrintReadJournal())

		if (bHasErrors || !IncompleteHandle.IsValid())
		{
			InternalIndex = ObjectIndexForOOBAttachment;
		}
		else
		{
			// If we get back an invalid internal index then either the object has been deleted or there's bitstream corruption.
			InternalIndex = NetRefHandleManager->GetInternalIndex(IncompleteHandle);

			if (InternalIndex == FNetRefHandleManager::InvalidInternalIndex)
			{
				if ((ReplicatedDestroyHeaderFlags & ReplicatedDestroyHeaderFlags_EndReplication) == 0U)
				{
					bHasErrors = true;
					UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Handle %s not bound to any InternalIndex"), *IncompleteHandle.ToString());
				}
				else
				{
					// If this is a subobject that is being destroyed this was no error as we send destroy info for unconfirmed object
					bIsReplicatedDestroyForInvalidObject = true;
				}
			}
		}
	}

	if (bHasErrors)
	{
		Context.SetError(GNetError_InvalidNetHandle);
		goto ErrorHandling;
	}

	// Read state data and attachments
	{
		const FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(InternalIndex);

		UE_ADD_READ_JOURNAL_ENTRY(Context, ObjectData.Protocol ? ObjectData.Protocol->DebugName->Name : TEXT("OOB"));

		// Add entry in our received data as we postpone state application until we have received all data in order to be able to properly resolve references
		FDispatchObjectInfo& Info = ObjectsToDispatchArray->AddPendingDispatchObjectInfo(TempLinearAllocator);

		// Update info based on ReplicatedDestroyHeader
		Info.bDestroy = !!(ReplicatedDestroyHeaderFlags & (ReplicatedDestroyHeaderFlags_TearOff | ReplicatedDestroyHeaderFlags_DestroyInstance));
		Info.bTearOff = !!(ReplicatedDestroyHeaderFlags & ReplicatedDestroyHeaderFlags_TearOff);
		// We set one of these flags
		if (bImmediateDispatchEndReplicationForSubObjects && Info.bDestroy && !Info.bTearOff)
		{
			Info.bReplicatedDestroyForSubObject = 1U;
			Info.bDeferredEndReplication = 0U;
		}
		else
		{
			Info.bReplicatedDestroyForSubObject = 0U;
			Info.bDeferredEndReplication = !!(ReplicatedDestroyHeaderFlags & (ReplicatedDestroyHeaderFlags_TearOff | ReplicatedDestroyHeaderFlags_EndReplication));
		}
		Info.bShouldCallSubObjectCreatedFromReplication = bShouldCallSubObjectCreatedFromReplication;

		if (bHasState)
		{
			UE_ADD_READ_JOURNAL_ENTRY(Context, TEXT("State"));

			if (IsObjectIndexForOOBAttachment(InternalIndex) || bIsReplicatedDestroyForInvalidObject)
			{
				bHasErrors = true;
				UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Bitstream corrupted. Getting state when not expecting state data."));
				Context.SetError(GNetError_BitStreamError);
				goto ErrorHandling;
			}

			FReplicatedObjectInfo* ObjectInfo = GetReplicatedObjectInfo(InternalIndex);
			checkSlow(ObjectInfo);

			const uint32 ChangeMaskBitCount = ObjectData.Protocol->ChangeMaskBitCount;

			// Temporary changemask
			FChangeMaskStorageOrPointer::Alloc(Info.ChangeMaskOrPointer, ChangeMaskBitCount, TempChangeMaskAllocator);

			if (bIsInitialState)
			{
				FReplicationProtocolOperations::DeserializeInitialStateWithMask(Context, Info.ChangeMaskOrPointer.GetPointer(ChangeMaskBitCount), ObjectData.ReceiveStateBuffer, ObjectData.Protocol);
			}
			else
			{
				if (ObjectInfo->bIsDeltaCompressionEnabled)
				{
					DeserializeObjectStateDelta(Context, InternalIndex, Info, *ObjectInfo, ObjectData, NewBaselineIndex);
				}
				else
				{
					FReplicationProtocolOperations::DeserializeWithMask(Context, Info.ChangeMaskOrPointer.GetPointer(ChangeMaskBitCount), ObjectData.ReceiveStateBuffer, ObjectData.Protocol);
				}
			}

			if (!ReadSentinel(Context, TEXT("HasStateEnd")))
			{
				bHasErrors = true;
				goto ErrorHandling;
			}

			// Should we store a new baseline?
			if (NewBaselineIndex != FDeltaCompressionBaselineManager::InvalidBaselineIndex)
			{
				// This object uses delta compression, store the last received state as a baseline with the specified index
				UE_LOG_REPLICATIONREADER(TEXT("Storing new baselineindex: %u for (:%u) Handle %s"), NewBaselineIndex, InternalIndex, *ObjectData.RefHandle.ToString());

				check(NewBaselineIndex < FDeltaCompressionBaselineManager::MaxBaselineCount);
				if (ObjectInfo->StoredBaselines[NewBaselineIndex])
				{
					// Clone into already allocated state, unfortunately we have to free dynamic state
					FReplicationProtocolOperations::FreeDynamicState(Context, ObjectInfo->StoredBaselines[NewBaselineIndex], ObjectData.Protocol);
					FReplicationProtocolOperationsInternal::CloneQuantizedState(Context, ObjectInfo->StoredBaselines[NewBaselineIndex], ObjectData.ReceiveStateBuffer, ObjectData.Protocol);
				}
				else
				{
					// Allocate new baseline and initialize from current RecvState
					ObjectInfo->StoredBaselines[NewBaselineIndex] = StateStorage->AllocBaseline(InternalIndex, EReplicationStateType::CurrentRecvState);
				}
	
				// Make sure that PrevStoredBaselineIndex is not set to the same as the NewBaselineIndex
				const uint32 OldPrevStoredBaselineIndex = ObjectInfo->PrevStoredBaselineIndex;
				const uint32 NewPrevStoredBaselineIndex = NewBaselineIndex != ObjectInfo->LastStoredBaselineIndex ? ObjectInfo->LastStoredBaselineIndex : FDeltaCompressionBaselineManager::InvalidBaselineIndex;
				ObjectInfo->PrevStoredBaselineIndex = NewPrevStoredBaselineIndex;
				if (NewPrevStoredBaselineIndex == FDeltaCompressionBaselineManager::InvalidBaselineIndex && NewPrevStoredBaselineIndex != OldPrevStoredBaselineIndex)
				{
					uint8* PrevBaseline = ObjectInfo->StoredBaselines[OldPrevStoredBaselineIndex];
					ObjectInfo->StoredBaselines[OldPrevStoredBaselineIndex] = nullptr;
					StateStorage->FreeBaseline(InternalIndex, PrevBaseline);
				}
				ObjectInfo->LastStoredBaselineIndex = NewBaselineIndex;
			}
		}

		const bool bHasAttachments = Reader.ReadBool();
		ENetObjectAttachmentType AttachmentType = ENetObjectAttachmentType::Normal;
		if (bHasAttachments)
		{
			UE_ADD_READ_JOURNAL_ENTRY(Context, TEXT("Attachments"));

			if (IsObjectIndexForOOBAttachment(InternalIndex))
			{
				bHasErrors = bIsReplicatedDestroyForInvalidObject;
				UE_CLOG(bHasErrors, LogIris, Error, TEXT("FReplicationReader::ReadObject Bitstream corrupted. Reading attachments when this was a destroy info message."));

				if (!bHasErrors)
				{
					const bool bIsHugeObject = Reader.ReadBool();
					AttachmentType = (bIsHugeObject ? ENetObjectAttachmentType::HugeObject : ENetObjectAttachmentType::OutOfBand);
					bHasErrors = (!Parameters.bAllowReceivingAttachmentsFromRemoteObjectsNotInScope && AttachmentType == ENetObjectAttachmentType::OutOfBand);
					UE_CLOG(bHasErrors, LogIris, Error, TEXT("FReplicationReader::ReadObject Bitstream corrupted. Reading OutOfBand attachment for object not in scope."));
				}

				if (bHasErrors)
				{
					Context.SetError(GNetError_InvalidNetHandle);
					goto ErrorHandling;
				}
			}

			Attachments.Deserialize(Context, AttachmentType, InternalIndex, ObjectData.RefHandle);
		}

		if (Context.HasErrorOrOverflow())
		{
			bHasErrors = true;
			UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject ErrorOrOverflow after reading bitstream."));
			goto ErrorHandling;
		}

		// Fill in ReadObjectInfo, we must skip objects that has not been created and HugeObjects as they are not added to the dispatch list until they are fully assembled
		const bool bShouldCommitPendingDispatchObjectInfo = (AttachmentType != ENetObjectAttachmentType::HugeObject) && !bIsReplicatedDestroyForInvalidObject;
		if (bShouldCommitPendingDispatchObjectInfo)
		{
			Info.InternalIndex = InternalIndex;
			Info.bIsInitialState = bIsInitialState ? 1U : 0U;
			Info.bHasState = bHasState ? 1U : 0U;
			Info.bHasAttachments = bHasAttachments ? 1U : 0U;

			ObjectsToDispatchArray->CommitPendingDispatchObjectInfo();
		}
	}
	
ErrorHandling:
	if (bHasErrors)
	{
		Context.SetErrorHandleContext(IncompleteHandle);
		UE_LOG(LogIris, Error, TEXT("FReplicationReader::ReadObject Failed to read replicated object with Handle: %s. Error '%s'. %s"), *IncompleteHandle.ToString(), (Context.HasError() ? ToCStr(Context.GetError().ToString()) : TEXT("BitStream Overflow")), *Context.PrintReadJournal());
	}
}

// Update reference tracking maps for the current object. It is assumed the ObjectReferenceTracker do no include duplicates for a given key.
void FReplicationReader::UpdateObjectReferenceTracking(FReplicatedObjectInfo* ReplicationInfo, FNetBitArrayView ChangeMask, bool bIncludeInitState, FResolvedNetRefHandlesArray& OutNewResolvedRefHandles, const FObjectReferenceTracker& NewUnresolvedReferences, const FObjectReferenceTracker& NewMappedDynamicReferences)
{
	IRIS_PROFILER_SCOPE(FReplicationReader_UpdateObjectReferenceTracking)

	/*
	 * As we store references per changemask we need to construct a set of all unresolved references and
	 * compare with the new set of unresolved references. The new set is found by first updating the 
	 * references that were found in the changemask.
	 */
	{
		// Try to avoid dynamic allocations during the update of the UnresolvedObjectReferences.
		ReplicationInfo->UnresolvedObjectReferences.Reserve(ReplicationInfo->UnresolvedObjectReferences.Num() + NewUnresolvedReferences.Num());

		TSet<FNetRefHandle> OldUnresolvedSet;
		OldUnresolvedSet.Reserve(ReplicationInfo->UnresolvedObjectReferences.Num());
		for (const FObjectReferenceTracker::ElementType& Element : ReplicationInfo->UnresolvedObjectReferences)
		{
			OldUnresolvedSet.Add(Element.Value);
		}

		// Replace each entry in UnresolvedObjectReferences for the given changemask
		auto UpdateUnresolvedReferencesForChange = [ReplicationInfo, &NewUnresolvedReferences](uint32 ChangeBit)
		{
			FObjectReferenceTracker& UnresolvedObjectReferences = ReplicationInfo->UnresolvedObjectReferences;

			// Use the Hash variants of the map methods to provide a tiny speedup.
			const uint32 KeyHash = ::GetTypeHash(ChangeBit);
			UnresolvedObjectReferences.RemoveByHash(KeyHash, ChangeBit);

			for (auto It = NewUnresolvedReferences.CreateConstKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();
				UnresolvedObjectReferences.AddByHash(KeyHash, ChangeBit, RefHandle);
			}
		};

		ChangeMask.ForAllSetBits(UpdateUnresolvedReferencesForChange);
		if (bIncludeInitState)
		{
			UpdateUnresolvedReferencesForChange(FakeInitChangeMaskOffset);
		}

		// The unresolved set is now updated with the current status of unresolved references.
		TSet<FNetRefHandle> NewUnresolvedSet;
		NewUnresolvedSet.Reserve(NewUnresolvedReferences.Num());
		for (const FObjectReferenceTracker::ElementType& Element : ReplicationInfo->UnresolvedObjectReferences)
		{
			NewUnresolvedSet.Add(Element.Value);
		}

		// Update ReplicationInfo with the status of unresolved references
		ReplicationInfo->bHasUnresolvedReferences = NewUnresolvedSet.Num() > 0;
		ReplicationInfo->bHasUnresolvedInitialReferences = ReplicationInfo->UnresolvedObjectReferences.Find(FakeInitChangeMaskOffset) != nullptr;

		// Remove resolved or no longer existing references
		const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;
		for (FNetRefHandle Handle : OldUnresolvedSet)
		{
			if (!NewUnresolvedSet.Contains(Handle))
			{
				// Store new resolved handles so we can update partially resolved references properly
				OutNewResolvedRefHandles.Add(Handle);

				// Remove from tracking
				UnresolvedHandleToDependents.RemoveSingle(Handle, OwnerInternalIndex);
				RemoveFromUnresolvedCache(Handle);
				UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Removing unresolved reference %s for %s"), ToCStr(Handle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
			}
		}

		// Add new unresolved references
		for (FNetRefHandle Handle : NewUnresolvedSet)
		{
			if (!OldUnresolvedSet.Contains(Handle))
			{
				// Add to tracking
				UnresolvedHandleToDependents.Add(Handle, OwnerInternalIndex);
				UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Adding unresolved reference %s for %s"), ToCStr(Handle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
			}
		}
	}

	// Update tracking for resolved dynamic references
	if (bRemapDynamicObjects)
	{
		// Try to avoid dynamic allocations during the update of the ResolvedDynamicObjectReferences.
		ReplicationInfo->ResolvedDynamicObjectReferences.Reserve(ReplicationInfo->ResolvedDynamicObjectReferences.Num() + NewMappedDynamicReferences.Num());

		TSet<FNetRefHandle> OldResolvedSet;
		OldResolvedSet.Reserve(ReplicationInfo->ResolvedDynamicObjectReferences.Num());
		for (const FObjectReferenceTracker::ElementType& Element : ReplicationInfo->ResolvedDynamicObjectReferences)
		{
			OldResolvedSet.Add(Element.Value);
		}

		// Replace each entry in ResolvedDynamicObjectReferences for the given changemask
		auto UpdateResolvedReferencesForChange = [ReplicationInfo, &NewMappedDynamicReferences](uint32 ChangeBit)
		{
			FObjectReferenceTracker& ResolvedDynamicObjectReferences = ReplicationInfo->ResolvedDynamicObjectReferences;

			const uint32 KeyHash = ::GetTypeHash(ChangeBit);
			ResolvedDynamicObjectReferences.RemoveByHash(KeyHash, ChangeBit);

			for (auto It = NewMappedDynamicReferences.CreateConstKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();
				ResolvedDynamicObjectReferences.AddByHash(KeyHash, ChangeBit, RefHandle);
			}
		};

		ChangeMask.ForAllSetBits(UpdateResolvedReferencesForChange);
		// Intentionally leaving out init state. It seems weird to call rep notifies and update init only properties after
		// the initial state has already been applied.

		// The resolved set is now updated with the current status of resolved references.
		TSet<FNetRefHandle> NewResolvedSet;
		NewResolvedSet.Reserve(ReplicationInfo->ResolvedDynamicObjectReferences.Num());
		for (const FObjectReferenceTracker::ElementType& Element : ReplicationInfo->ResolvedDynamicObjectReferences)
		{
			NewResolvedSet.Add(Element.Value);
		}

		// Remove now unresolved or no longer existing references
		const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;
		for (FNetRefHandle Handle : OldResolvedSet)
		{
			if (!NewResolvedSet.Contains(Handle))
			{
				// Remove from tracking
				ResolvedDynamicHandleToDependents.RemoveSingle(Handle, OwnerInternalIndex);
				//UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Removing resolved dynamic reference %s for %s"), ToCStr(Handle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
			}
		}

		// Add new resolved dynamic references
		for (FNetRefHandle Handle : NewResolvedSet)
		{
			if (Handle.IsDynamic() && !OldResolvedSet.Contains(Handle))
			{
				// Add to tracking
				ResolvedDynamicHandleToDependents.Add(Handle, OwnerInternalIndex);
				//UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Adding resolved dynamic reference %s for %s"), ToCStr(Handle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
			}
		}
	}
}

void FReplicationReader::UpdateObjectReferenceTracking_Fast(FReplicatedObjectInfo* ReplicationInfo, FNetBitArrayView ChangeMask, bool bIncludeInitState, FResolvedNetRefHandlesArray& OutNewResolvedRefHandles, const FObjectReferenceTracker& NewUnresolvedReferences, const FObjectReferenceTracker& NewMappedDynamicReferences)
{
	IRIS_PROFILER_SCOPE(FReplicationReader_UpdateObjectReferenceTracking);

	/*
	 * As we store references per changemask we need to construct a set of all unresolved references and
	 * compare with the new set of unresolved references. The new set is found by first updating the 
	 * references that were found in the changemask.
	 */
	{
		// Try to avoid dynamic allocations during the update of the UnresolvedObjectReferences.
		ReplicationInfo->UnresolvedObjectReferences.Reserve(ReplicationInfo->UnresolvedObjectReferences.Num() + NewUnresolvedReferences.Num());
		ReplicationInfo->UnresolvedHandleCount.Reserve(ReplicationInfo->UnresolvedObjectReferences.Num() + NewUnresolvedReferences.Num());

		// Replace each entry in UnresolvedObjectReferences for the given changemask
		auto UpdateUnresolvedReferencesForChange = [ReplicationInfo, &NewUnresolvedReferences, &OutNewResolvedRefHandles, this](uint32 ChangeBit)
		{
			FObjectReferenceTracker& UnresolvedObjectReferences = ReplicationInfo->UnresolvedObjectReferences;

			bool bUnresolvedHandleCountShrink = false;

			for (FObjectReferenceTracker::TKeyIterator It = UnresolvedObjectReferences.CreateKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();

				const bool bInNewUnresolved = (NewUnresolvedReferences.FindPair(ChangeBit, RefHandle) != nullptr);
				if (!bInNewUnresolved)
				{
					It.RemoveCurrent();

					if (ReplicationInfo->RemoveUnresolvedHandleCount(RefHandle))
					{
						bUnresolvedHandleCountShrink = true;
							
						// Store new resolved handles so we can update partially resolved references properly
						OutNewResolvedRefHandles.Add(RefHandle);

						// Remove from tracking
						const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;
						UnresolvedHandleToDependents.Remove(RefHandle, OwnerInternalIndex);
						RemoveFromUnresolvedCache(RefHandle);
						UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Removing unresolved reference %s for %s (OwnerInternalIndex=%d)"), ToCStr(RefHandle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()), OwnerInternalIndex);
					}
				}
			}

			for (FObjectReferenceTracker::TConstKeyIterator It = NewUnresolvedReferences.CreateConstKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();
				const bool bInCurrUnresolved = (UnresolvedObjectReferences.FindPair(ChangeBit, RefHandle) != nullptr);

				if (!bInCurrUnresolved)
				{
					UnresolvedObjectReferences.Add(ChangeBit, RefHandle);

					int16& HandleCount = ReplicationInfo->UnresolvedHandleCount.FindOrAdd(RefHandle, 0);

					HandleCount++;

					// Add to tracking
					const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;

					// Calling TMultiMap::Add() as this is O(1) performance as oppossed to TMultiMap::AddUnique() which is O(n). This does 
					// mean that UnresolvedHandleToDependents can contain duplicate (RefHandle, OwnerInternalIndex) pairs but is handled
					// gracefully by the rest of the code. It does mean that TMultiMap::Remove() must be called instead of TMultiMap::RemoveSingle()
					// to remove all pairs.
					UnresolvedHandleToDependents.Add(RefHandle, OwnerInternalIndex);
					UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Adding unresolved reference %s for %s (OwnerInternalIndex=%d)"), ToCStr(RefHandle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()), OwnerInternalIndex);
				}
			}

			if (bUnresolvedHandleCountShrink)
			{
				ReplicationInfo->UnresolvedHandleCount.Shrink();
			}
		};

		ChangeMask.ForAllSetBits(UpdateUnresolvedReferencesForChange);
		if (bIncludeInitState)
		{
			UpdateUnresolvedReferencesForChange(FakeInitChangeMaskOffset);
		}

		// Update ReplicationInfo with the status of unresolved references
		ReplicationInfo->bHasUnresolvedReferences = ReplicationInfo->UnresolvedHandleCount.Num() > 0;
		ReplicationInfo->bHasUnresolvedInitialReferences = ReplicationInfo->UnresolvedObjectReferences.Find(FakeInitChangeMaskOffset) != nullptr;
	}

	// Update tracking for resolved dynamic references
	if (bRemapDynamicObjects)
	{
		// Try to avoid dynamic allocations during the update of the ResolvedDynamicObjectReferences.
		ReplicationInfo->ResolvedDynamicObjectReferences.Reserve(ReplicationInfo->ResolvedDynamicObjectReferences.Num() + NewMappedDynamicReferences.Num());
		ReplicationInfo->ResolvedDynamicHandleCount.Reserve(ReplicationInfo->ResolvedDynamicObjectReferences.Num() + NewMappedDynamicReferences.Num());

		// Replace each entry in ResolvedDynamicObjectReferences for the given changemask
		auto UpdateResolvedReferencesForChange = [ReplicationInfo, &NewMappedDynamicReferences, this](uint32 ChangeBit)
		{
			FObjectReferenceTracker& ResolvedDynamicObjectReferences = ReplicationInfo->ResolvedDynamicObjectReferences;

			bool bResolvedDynamicObjectReferenceShrink = false;

			for (FObjectReferenceTracker::TKeyIterator It = ResolvedDynamicObjectReferences.CreateKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();

				const bool bInNewResolved = (NewMappedDynamicReferences.FindPair(ChangeBit, RefHandle) != nullptr);
				if (!bInNewResolved)
				{
					It.RemoveCurrent();

					if (ReplicationInfo->RemoveResolvedDynamicHandleCount(RefHandle))
					{
						bResolvedDynamicObjectReferenceShrink = true;

						// Remove from tracking
						const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;
						ResolvedDynamicHandleToDependents.Remove(RefHandle, OwnerInternalIndex);
						UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Removing resolved dynamic reference %s for %s"), ToCStr(RefHandle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
					}
				}
			}

			for (FObjectReferenceTracker::TConstKeyIterator It = NewMappedDynamicReferences.CreateConstKeyIterator(ChangeBit); It; ++It)
			{
				const FNetRefHandle RefHandle = It.Value();
				const bool bInCurrResolved = (ResolvedDynamicObjectReferences.FindPair(ChangeBit, RefHandle) != nullptr);

				if (!bInCurrResolved)
				{
					ResolvedDynamicObjectReferences.Add(ChangeBit, RefHandle);

					int16& HandleCount = ReplicationInfo->ResolvedDynamicHandleCount.FindOrAdd(RefHandle, 0);

					HandleCount++;

					// Add to tracking
					const uint32 OwnerInternalIndex = ReplicationInfo->InternalIndex;

					// Calling TMultiMap::Add() as this is O(1) performance as oppossed to TMultiMap::AddUnique() which is O(n). This does 
					// mean that UnresolvedHandleToDependents can contain duplicate (RefHandle, OwnerInternalIndex) pairs but is handled
					// gracefully by the rest of the code. It does mean that TMultiMap::Remove() must be called instead of TMultiMap::RemoveSingle()
					// to remove all pairs.
					ResolvedDynamicHandleToDependents.Add(RefHandle, OwnerInternalIndex);
					UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::UpdateObjectReferenceTracking Adding resolved dynamic reference %s for %s"), ToCStr(RefHandle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(OwnerInternalIndex).ToString()));
				}
			}

			if (bResolvedDynamicObjectReferenceShrink)
			{
				ReplicationInfo->ResolvedDynamicHandleCount.Shrink();
			}
		};

		ChangeMask.ForAllSetBits(UpdateResolvedReferencesForChange);
		// Intentionally leaving out init state. It seems weird to call rep notifies and update init only properties after
		// the initial state has already been applied.
	}
}

void FReplicationReader::RemoveUnresolvedObjectReferenceInReplicationInfo(FReplicatedObjectInfo* ReplicationInfo, FNetRefHandle Handle)
{
	for (FObjectReferenceTracker::TIterator It = ReplicationInfo->UnresolvedObjectReferences.CreateIterator(); It; ++It)
	{
		const FNetRefHandle RefHandle = It->Value;
		if (RefHandle == Handle)
		{
			It.RemoveCurrent();

			if (bUseOptObjectRefTracking)
			{
				ReplicationInfo->RemoveUnresolvedHandleCount(RefHandle);
			}
		}
	}
}

void FReplicationReader::RemoveResolvedObjectReferenceInReplicationInfo(FReplicatedObjectInfo* ReplicationInfo, FNetRefHandle Handle)
{
	for (FObjectReferenceTracker::TIterator It = ReplicationInfo->ResolvedDynamicObjectReferences.CreateIterator(); It; ++It)
	{
		const FNetRefHandle RefHandle = It->Value;
		if (RefHandle == Handle)
		{
			It.RemoveCurrent();

			if (bUseOptObjectRefTracking)
			{
				ReplicationInfo->RemoveResolvedDynamicHandleCount(RefHandle);
			}
		}
	}
}

bool FReplicationReader::MoveResolvedObjectReferenceToUnresolvedInReplicationInfo(FReplicatedObjectInfo* ReplicationInfo, FNetRefHandle UnresolvableHandle)
{
	bool bFoundHandle = false;
	bool bHasUnresolvedReferences = ReplicationInfo->bHasUnresolvedReferences;
	bool bHasUnresolvedInitialReferences = ReplicationInfo->bHasUnresolvedInitialReferences;
	FNetBitArrayView UnresolvedChangeMask = FChangeMaskUtil::MakeChangeMask(ReplicationInfo->UnresolvedChangeMaskOrPointer, ReplicationInfo->ChangeMaskBitCount);
	FObjectReferenceTracker& UnresolvedObjectReferences = ReplicationInfo->UnresolvedObjectReferences;
	TMap<FNetRefHandle, int16>& UnresolvedHandleCount = ReplicationInfo->UnresolvedHandleCount;
	TMap<FNetRefHandle, int16>& ResolvedDynamicHandleCount = ReplicationInfo->ResolvedDynamicHandleCount;
	for (FObjectReferenceTracker::TIterator It = ReplicationInfo->ResolvedDynamicObjectReferences.CreateIterator(); It; ++It)
	{
		const FNetRefHandle RefHandle = It->Value;
		if (RefHandle == UnresolvableHandle)
		{
			bFoundHandle = true;

			const uint32 ChangemaskOffset = It->Key;
			if (ChangemaskOffset == FakeInitChangeMaskOffset)
			{
				bHasUnresolvedInitialReferences = true;
			}
			else
			{
				bHasUnresolvedReferences = true;
				UnresolvedChangeMask.SetBit(ChangemaskOffset);
			}

			// At this point we'd like to skip iteration to the next key as a handle can only be found once per changemask.
			It.RemoveCurrent();

			if (bUseOptObjectRefTracking)
			{
				ReplicationInfo->RemoveResolvedDynamicHandleCount(UnresolvableHandle);
			}

			// This handle should only have existed once in the ResolvedDynamicObjectReferences map and should not be able to
			// already exist in the UnresolvedObjectReferences map, so no need to call AddUnique.
			UnresolvedObjectReferences.Add(ChangemaskOffset, UnresolvableHandle);
			
			if (bUseOptObjectRefTracking)
			{
				int16& UnresolvableHandleCount = UnresolvedHandleCount.FindOrAdd(UnresolvableHandle, 0);
				UnresolvableHandleCount++;
			}

			UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::MoveResolvedObjectReferenceToUnresolvedInReplicationInfo Moving from resolved to unresolved reference %s for %s"), ToCStr(UnresolvableHandle.ToString()), ToCStr(NetRefHandleManager->GetNetRefHandleFromInternalIndex(ReplicationInfo->InternalIndex).ToString()));
		}
	}

	ReplicationInfo->bHasUnresolvedInitialReferences = bHasUnresolvedInitialReferences;
	ReplicationInfo->bHasUnresolvedReferences = bHasUnresolvedReferences;

	return bFoundHandle;
}

// Remove all references for object
void FReplicationReader::CleanupReferenceTracking(FReplicatedObjectInfo* ObjectInfo)
{
	const uint32 ObjectIndex = ObjectInfo->InternalIndex;

	// Remove from unresolved references
	for (FObjectReferenceTracker::ElementType Element : ObjectInfo->UnresolvedObjectReferences)
	{
		// Remove from tracking
		FNetRefHandle Handle = Element.Value;
		UnresolvedHandleToDependents.Remove(Handle, ObjectIndex);
		RemoveFromUnresolvedCache(Handle);
		UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::CleanupReferenceTracking Removing unresolved reference %s for %s"), *Handle.ToString(), *(NetRefHandleManager->GetNetRefHandleFromInternalIndex(ObjectIndex).ToString()));
	}
	ObjectInfo->UnresolvedObjectReferences.Reset();
	ObjectInfo->UnresolvedHandleCount.Reset();

	// Remove from resolved dynamic references
	for (FObjectReferenceTracker::ElementType Element : ObjectInfo->ResolvedDynamicObjectReferences)
	{
		// Remove from tracking
		const FNetRefHandle Handle = Element.Value;
		ResolvedDynamicHandleToDependents.Remove(Handle, ObjectIndex);
		UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::CleanupReferenceTracking Removing resolved dynamic reference %s for %s"), *Handle.ToString(), *(NetRefHandleManager->GetNetRefHandleFromInternalIndex(ObjectIndex).ToString()));
	}
	ObjectInfo->ResolvedDynamicObjectReferences.Reset();
	ObjectInfo->ResolvedDynamicHandleCount.Reset();

	// Remove from attachment resolve
	ObjectsWithAttachmentPendingResolve.Remove(ObjectIndex);
}

void FReplicationReader::BuildUnresolvedChangeMaskAndUpdateObjectReferenceTracking(const FResolveAndCollectUnresolvedAndResolvedReferenceCollector& Collector, FNetBitArrayView CollectorChangeMask, FReplicatedObjectInfo* ReplicationInfo, FNetBitArrayView& OutUnresolvedChangeMask, FResolvedNetRefHandlesArray& OutNewResolvedRefHandles)
{
	OutUnresolvedChangeMask.ClearAllBits();
	bool bHasUnresolvedInitReferences = false;

	UnresolvedReferencesCache.Reset();
	MappedDynamicReferencesCache.Reset();
	
	for (const auto& RefInfo : Collector.GetUnresolvedReferences())
	{
		const FNetSerializerChangeMaskParam& ChangeMaskInfo = RefInfo.ChangeMaskInfo;
		if (ChangeMaskInfo.BitCount)
		{
			OutUnresolvedChangeMask.SetBit(ChangeMaskInfo.BitOffset);
		}
		else
		{
			bHasUnresolvedInitReferences = true;
		}

		const uint32 BitOffset = (ChangeMaskInfo.BitCount > 0U ? ChangeMaskInfo.BitOffset : FakeInitChangeMaskOffset);
		UnresolvedReferencesCache.AddUnique(BitOffset, RefInfo.Reference.GetRefHandle());
	}

	for (const auto& RefInfo : Collector.GetResolvedReferences())
	{
		if (RefInfo.Reference.GetRefHandle().IsDynamic())
		{
			const uint32 BitOffset = (RefInfo.ChangeMaskInfo.BitCount > 0U ? RefInfo.ChangeMaskInfo.BitOffset : FakeInitChangeMaskOffset);
			MappedDynamicReferencesCache.AddUnique(BitOffset, RefInfo.Reference.GetRefHandle());
		}
	}

	// Update object specific
	if (bUseOptObjectRefTracking)
	{
		UpdateObjectReferenceTracking_Fast(ReplicationInfo, CollectorChangeMask, Collector.IsInitStateIncluded(), OutNewResolvedRefHandles, UnresolvedReferencesCache, MappedDynamicReferencesCache);
	}
	else
	{
		UpdateObjectReferenceTracking(ReplicationInfo, CollectorChangeMask, Collector.IsInitStateIncluded(), OutNewResolvedRefHandles, UnresolvedReferencesCache, MappedDynamicReferencesCache);
	}
}

void FReplicationReader::ResolveAndDispatchUnresolvedReferencesForObject(FNetSerializationContext& Context, uint32 InternalIndex)
{
	IRIS_PROFILER_SCOPE(FReplicationReader_ResolveAndDispatchUnresolvedReferencesForObject);

	FReplicatedObjectInfo* ReplicationInfo = GetReplicatedObjectInfo(InternalIndex);
	// Unexpected. Get more info.
	if (!ReplicationInfo)
	{
		static bool bHasLogged = false;
		UE_CLOG(!bHasLogged, LogIris, Error, TEXT("Trying to resolve references for non-existing object ( InternalIndex: %u )"), InternalIndex);
		bHasLogged = true;
		ensure(false);
		return;
	}

	const bool bObjectHasAttachments = ReplicationInfo->bHasAttachments;
	const bool bObjectHasReferences = ReplicationInfo->bHasUnresolvedInitialReferences | ReplicationInfo->bHasUnresolvedReferences;

	ENetObjectAttachmentDispatchFlags AttachmentDispatchedFlags = ENetObjectAttachmentDispatchFlags::None;
	if (bObjectHasReferences)
	{
		const FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(ReplicationInfo->InternalIndex);
		UE_LOG(LogIris, VeryVerbose, TEXT("ResolveAndDispatchUnresolvedReferencesForObject %s RefHandle %s"), ObjectData.Protocol->DebugName->Name, ToCStr(ObjectData.RefHandle.ToString()));
		const uint32 ChangeMaskBitCount = ReplicationInfo->ChangeMaskBitCount;
		
		// Try to resolve references and collect unresolved references
		const bool bOldHasUnresolvedInitReferences = ReplicationInfo->bHasUnresolvedInitialReferences;

		FNetBitArrayView UnresolvedChangeMask = FChangeMaskUtil::MakeChangeMask(ReplicationInfo->UnresolvedChangeMaskOrPointer, ChangeMaskBitCount);
	
		// Need a temporary changemask for the unresolved changes due to UnresolvedChangeMask being written to by BuildUnresolvedChangeMaskAndUpdateObjectReferenceTracking
		FChangeMaskStorageOrPointer TempChangeMaskOrPointer;
		FNetBitArrayView TempChangeMask = FChangeMaskStorageOrPointer::AllocAndInitBitArray(TempChangeMaskOrPointer, ChangeMaskBitCount, TempChangeMaskAllocator);
		FNetBitArrayView TempUnresolvedChangeMask = TempChangeMask;
		TempUnresolvedChangeMask.Copy(UnresolvedChangeMask);

		// Try to resolve references and collect resolved and references still pending resolve
		FResolveAndCollectUnresolvedAndResolvedReferenceCollector Collector;
		Collector.CollectReferences(*ObjectReferenceCache, ResolveContext, ReplicationInfo->bHasUnresolvedInitialReferences, &UnresolvedChangeMask, ObjectData.ReceiveStateBuffer, ObjectData.Protocol);

		// We need to track previously unresolved NetRefHandles that now are resolvable
		FResolvedNetRefHandlesArray NewResolvedRefHandles;

		// Build UnresolvedChangeMask from collected data and update replication info
		BuildUnresolvedChangeMaskAndUpdateObjectReferenceTracking(Collector, TempUnresolvedChangeMask, ReplicationInfo, UnresolvedChangeMask, NewResolvedRefHandles);

		// Re-purpose temp changemask for members that has resolved references.
		TempChangeMask.ClearAllBits();
		FNetBitArrayView ResolvedChangeMask = TempChangeMask;

		// Merge in partially resolved changes
		bool bHasResolvedInitReferences = false;
		if (NewResolvedRefHandles.Num())
		{
			for (const FNetReferenceCollector::FReferenceInfo& ReferenceInfo : Collector.GetResolvedReferences())
			{
				const FNetRefHandle& MatchRefHandle = ReferenceInfo.Reference.GetRefHandle();
				if (NewResolvedRefHandles.ContainsByPredicate([&MatchRefHandle](const FNetRefHandle& RefHandle) { return RefHandle == MatchRefHandle;} ))
				{
					const FNetSerializerChangeMaskParam& ChangeMaskInfo = ReferenceInfo.ChangeMaskInfo;
					if (ChangeMaskInfo.BitCount)
					{
						ResolvedChangeMask.SetBit(ChangeMaskInfo.BitOffset);
					}
					else
					{
						// If we had old unresolved init dependencies we need to include the init state when we update references
						bHasResolvedInitReferences = bOldHasUnresolvedInitReferences;
					}
				}
			}
		}

		if (ObjectData.InstanceProtocol)
		{
			// Apply resolved references, this is a blunt tool as we currently push out full dirty properties rather than only the resolved references
			if (ResolvedChangeMask.IsAnyBitSet() || bHasResolvedInitReferences)
			{
				if (bObjectHasAttachments && bExecuteReliableRPCsBeforeApplyState && !bHasResolvedInitReferences)
				{
					AttachmentDispatchedFlags = ENetObjectAttachmentDispatchFlags::Reliable;
					ResolveAndDispatchAttachments(Context, ReplicationInfo, ENetObjectAttachmentDispatchFlags::Reliable);
				}

				Context.SetIsInitState(bHasResolvedInitReferences);

				FDequantizeAndApplyParameters Params;
				Params.Allocator = &TempLinearAllocator;
				Params.ChangeMaskData = ResolvedChangeMask.GetData();
				Params.UnresolvedReferencesChangeMaskData = ReplicationInfo->bHasUnresolvedReferences ? ReplicationInfo->UnresolvedChangeMaskOrPointer.GetPointer(ChangeMaskBitCount) : nullptr;
				Params.InstanceProtocol = ObjectData.InstanceProtocol;
				Params.Protocol = ObjectData.Protocol;
				Params.SrcObjectStateBuffer = ObjectData.ReceiveStateBuffer;
				Params.bHasUnresolvedInitReferences = ReplicationInfo->bHasUnresolvedInitialReferences;

				if (bResolvedObjectsDispatchDebugging && UE_LOG_ACTIVE(LogIris, VeryVerbose))
				{
					uint32 CurrentChangeMaskBitOffset = 0;
					for (const FReplicationStateDescriptor* StateDescriptor : MakeArrayView(ObjectData.Protocol->ReplicationStateDescriptors, ObjectData.Protocol->ReplicationStateCount))
					{
						if (ResolvedChangeMask.IsAnyBitSet(CurrentChangeMaskBitOffset, StateDescriptor->ChangeMaskBitCount))
						{
							for (uint32 MemberIt = 0, MemberEndIt = StateDescriptor->MemberCount; MemberIt != MemberEndIt; ++MemberIt)
							{
								const FReplicationStateMemberChangeMaskDescriptor* MemberChangeMaskDesc = StateDescriptor->MemberChangeMaskDescriptors + MemberIt;
								if (ResolvedChangeMask.IsAnyBitSet(CurrentChangeMaskBitOffset + MemberChangeMaskDesc->BitOffset, MemberChangeMaskDesc->BitCount))
								{
									if (const FProperty* MemberProperty = StateDescriptor->MemberProperties[MemberIt])
									{
										UE_LOG(LogIris, VeryVerbose, TEXT("ResolvedChangeMask State %s Property %s"), ToCStr(StateDescriptor->DebugName->Name), ToCStr(MemberProperty->GetName()));
									}
								}
							}
						}

						CurrentChangeMaskBitOffset += StateDescriptor->ChangeMaskBitCount;
					}
				}

				FReplicationInstanceOperations::DequantizeAndApply(Context, Params);
			}
		}
		else
		{
			// $IRIS: $TODO: Figure out how to handle this, currently we do not crash but we probably want to
			// handle this properly by accumulating changemask for later instantiation
			UE_LOG(LogIris, Verbose, TEXT("Cannot dispatch state data for not instantiated %s"), *ObjectData.RefHandle.ToString());
		}
	}

	// Dispatch attachment and enqueue for later resolving
	if (bObjectHasAttachments)
	{
		// If we haven't dispatched reliable attachments for this object then do so now in addition to unreliable attachments.
		const ENetObjectAttachmentDispatchFlags AttachmentDispatchFlags = ENetObjectAttachmentDispatchFlags::Unreliable | (AttachmentDispatchedFlags ^ ENetObjectAttachmentDispatchFlags::Reliable);
		ResolveAndDispatchAttachments(Context, ReplicationInfo, AttachmentDispatchFlags);
	}
}

// Dispatch all data received for the frame, this includes trying to resolve object references
void FReplicationReader::DispatchStateData(FNetSerializationContext& Context)
{
	IRIS_PROFILER_SCOPE(FReplicationReader::DispatchStateData);

	// In order to execute PostNetRecv/PostRepNotifies after we have applied the actual state 
	// we need to cache some information during dispatch and execute the logic in multiple passes
	// Note: Currently all objects received in the packet are treated a a single batch
	struct FPostDispatchObjectInfo
	{
		FReplicatedObjectInfo* ReplicationInfo;
		FDispatchObjectInfo* Info;
		FDequantizeAndApplyHelper::FContext* DequantizeAndApplyContext;
		ENetObjectAttachmentDispatchFlags AttachmentDispatchedFlags;		
	};

	// Allocate temporary space for post dispatch
	FPostDispatchObjectInfo* PostDispatchObjectInfos = new (TempLinearAllocator) FPostDispatchObjectInfo[ObjectsToDispatchArray->Num()];
	uint32 NumObjectsPendingPostDistpatch = 0U;

	// Function to flush all objects pending post dispatch
	auto FlushPostDispatchForBatch = [PostDispatchObjectInfos, &NumObjectsPendingPostDistpatch, this, &Context]()
	{
		// Dispatch replicated subobject destroy here to behave as close as possible to subobject replication in ActorChannel.
		// Added to solve issues with assumptions made by blueprint logic when replacing a component with another of the same type
		if (bImmediateDispatchEndReplicationForSubObjects)
		{
			for (FPostDispatchObjectInfo& PostDispatchObjectInfo : MakeArrayView(PostDispatchObjectInfos, NumObjectsPendingPostDistpatch))
			{
				FDispatchObjectInfo& Info = *PostDispatchObjectInfo.Info;
				if (Info.bReplicatedDestroyForSubObject)
				{
					EndReplication(Info.InternalIndex, Info.bTearOff, Info.bDestroy);
				}
			}
		}

		// When all received states have been applied we invoke PostReplicate and RepNotifies
		for (FPostDispatchObjectInfo& PostDispatchObjectInfo : MakeArrayView(PostDispatchObjectInfos, NumObjectsPendingPostDistpatch))
		{
			FDispatchObjectInfo& Info = *PostDispatchObjectInfo.Info;

			// Execute legacy post replicate functions
			if (Info.bHasState && PostDispatchObjectInfo.DequantizeAndApplyContext)
			{
				Context.SetIsInitState(Info.bIsInitialState);
				FDequantizeAndApplyHelper::CallLegacyPostApplyFunctions(PostDispatchObjectInfo.DequantizeAndApplyContext, Context);
			}
		}

		// In the last pass, RPC's and cleanup cached data
		for (FPostDispatchObjectInfo& PostDispatchObjectInfo : MakeArrayView(PostDispatchObjectInfos, NumObjectsPendingPostDistpatch))
		{
			FDispatchObjectInfo& Info = *PostDispatchObjectInfo.Info;

			// If the object was created this frame it's initial state is now applied
			if (Info.bHasState && Info.bIsInitialState)
			{
				ReplicationBridge->CallPostApplyInitialState(Info.InternalIndex);
			}

			// Dispatch attachment and enqueue for later resolving
			if (Info.bHasAttachments)
			{
				// If we haven't dispatched reliable attachments for this object then do so now in addition to unreliable attachments.
				const ENetObjectAttachmentDispatchFlags AttachmentDispatchFlags = ENetObjectAttachmentDispatchFlags::Unreliable | (PostDispatchObjectInfo.AttachmentDispatchedFlags ^ ENetObjectAttachmentDispatchFlags::Reliable);
				ResolveAndDispatchAttachments(Context, PostDispatchObjectInfo.ReplicationInfo, AttachmentDispatchFlags);
			}

			// Cleanup temporary state data
			if (PostDispatchObjectInfo.DequantizeAndApplyContext)
			{
				FDequantizeAndApplyHelper::Deinitialize(PostDispatchObjectInfo.DequantizeAndApplyContext);
			}
		}

		NumObjectsPendingPostDistpatch = 0U;
	};

	// In order to properly execute legacy callbacks we need to batch apply state data for owner/subobjects
	FInternalNetRefIndex LastDispatchedRootInternalIndex = 0U;
	
	// Dispatch and apply received state data
	for (FDispatchObjectInfo& Info : ObjectsToDispatchArray->GetObjectsToDispatch())
	{
		FReplicatedObjectInfo* ReplicationInfo = GetReplicatedObjectInfo(Info.InternalIndex);

		const FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(Info.InternalIndex);

		// Before starting a potentially new batch we want to flush rpc:s and legacy callbacks belonging to the previous batch
		const FInternalNetRefIndex RootInternalIndex = ObjectData.SubObjectRootIndex == FNetRefHandleManager::InvalidInternalIndex ? Info.InternalIndex : ObjectData.SubObjectRootIndex;
		if (RootInternalIndex != LastDispatchedRootInternalIndex && NumObjectsPendingPostDistpatch)
		{
			FlushPostDispatchForBatch();
		}
		LastDispatchedRootInternalIndex = RootInternalIndex;

		FPostDispatchObjectInfo PostDispatchObjectInfo;
		PostDispatchObjectInfo.ReplicationInfo = ReplicationInfo;
		PostDispatchObjectInfo.Info = &Info;
		PostDispatchObjectInfo.DequantizeAndApplyContext = nullptr;
		PostDispatchObjectInfo.AttachmentDispatchedFlags = ENetObjectAttachmentDispatchFlags::None;

		// For SubObjects we call this method after applying state data for the owner, in order to remain backwards compatible.
		if (Info.bShouldCallSubObjectCreatedFromReplication)
		{
			if (ObjectData.SubObjectRootIndex != FNetRefHandleManager::InvalidInternalIndex)
			{
				ReplicationBridge->CallSubObjectCreatedFromReplication(ObjectData.SubObjectRootIndex, ObjectData.RefHandle);
			}
		}

		// If we are running in backwards compatibility mode, execute Reliable RPC`s before applying state data unless object is already created.
		if (Info.bHasAttachments && bExecuteReliableRPCsBeforeApplyState && !Info.bIsInitialState)
		{
			PostDispatchObjectInfo.AttachmentDispatchedFlags |= ENetObjectAttachmentDispatchFlags::Reliable;
			ResolveAndDispatchAttachments(Context, PostDispatchObjectInfo.ReplicationInfo, ENetObjectAttachmentDispatchFlags::Reliable);
			// Update if we have attachments or not since we might have processed all of them in the first pass.
			Info.bHasAttachments = PostDispatchObjectInfo.ReplicationInfo->bHasAttachments;
		}

		// If we have any object references we want to update any unresolved ones, including previously unresolved references
		if (Info.bHasState)
		{

			const uint32 ChangeMaskBitCount = ReplicationInfo->ChangeMaskBitCount;

			FNetBitArrayView ChangeMask = FChangeMaskUtil::MakeChangeMask(Info.ChangeMaskOrPointer, ChangeMaskBitCount);
			// If we have pending unresolved changes we include them as well 
			FNetBitArrayView UnresolvedChangeMask = FChangeMaskUtil::MakeChangeMask(ReplicationInfo->UnresolvedChangeMaskOrPointer, ChangeMaskBitCount);

			FChangeMaskStorageOrPointer ChangeMaskForResolveAllocation;
			FNetBitArrayView ChangeMaskForResolve;
			const bool bHadUnresolvedReferences = ReplicationInfo->bHasUnresolvedReferences;
			if (bHadUnresolvedReferences)
			{
				if (bDispatchUnresolvedPreviouslyReceivedChanges)
				{
					// Combine the changemask with the unresolved changemask so that result is used for the apply operation as well.
					ChangeMask.Combine(UnresolvedChangeMask, FNetBitArrayView::OrOp);
					ChangeMaskForResolve = ChangeMask;
				}
				else
				{
					// Memory for the changemask allocation will be freed when the TempLinearAllocator is reset via FMemMark scope. TempChangeMaskAllocator uses TempLinearAllocator.
					ChangeMaskForResolveAllocation.Alloc(ChangeMaskForResolveAllocation, ChangeMaskBitCount, TempChangeMaskAllocator);
					ChangeMaskForResolve = MakeNetBitArrayView(ChangeMaskForResolveAllocation.GetPointer(ChangeMaskBitCount), ChangeMaskBitCount, FNetBitArrayView::NoResetNoValidate);
					ChangeMaskForResolve.Set(ChangeMask, FNetBitArrayView::OrOp, UnresolvedChangeMask);
				}
			}
			else
			{
				ChangeMaskForResolve = ChangeMask;
			}

			// Collect all unresolvable references, including old pending references
			FResolveAndCollectUnresolvedAndResolvedReferenceCollector Collector;
			Collector.CollectReferences(*ObjectReferenceCache, ResolveContext, Info.bIsInitialState | ReplicationInfo->bHasUnresolvedInitialReferences, &ChangeMaskForResolve, ObjectData.ReceiveStateBuffer, ObjectData.Protocol);

			// If we have or had any object references we need to track them and update the unresolved mask
			if (bHadUnresolvedReferences || Collector.GetUnresolvedReferences().Num() > 0 || Collector.GetResolvedReferences().Num() > 0)
			{
				FChangeMaskStorageOrPointer ChangeMaskForPrevUnresolvedAllocation;
				FNetBitArrayView PrevUnresolvedChangeMask;

				// If we're avoiding dispatching state we didn't receive and we didn't resolve anything for we need to figure out what got resolves and combine that with the received changemask.
				const bool bMergeResolvedReferencesWithChangeMask = bHadUnresolvedReferences && !bDispatchUnresolvedPreviouslyReceivedChanges;
				if (bMergeResolvedReferencesWithChangeMask)
				{
					ChangeMaskForPrevUnresolvedAllocation.Alloc(ChangeMaskForPrevUnresolvedAllocation, ChangeMaskBitCount, TempChangeMaskAllocator);
					PrevUnresolvedChangeMask = MakeNetBitArrayView(ChangeMaskForPrevUnresolvedAllocation.GetPointer(ChangeMaskBitCount), ChangeMaskBitCount, FNetBitArrayView::NoResetNoValidate);
					PrevUnresolvedChangeMask.Copy(UnresolvedChangeMask);
				}

				// We need to track previously unresolved NetRefHandles that now are resolvable
				FResolvedNetRefHandlesArray NewResolvedRefHandles;

				BuildUnresolvedChangeMaskAndUpdateObjectReferenceTracking(Collector, ChangeMaskForResolve, ReplicationInfo, UnresolvedChangeMask, NewResolvedRefHandles);
			
				// Allow resolved changes to be part of the state to be applied.
				if (bMergeResolvedReferencesWithChangeMask)
				{
					// Merge in no longer unresolved changes
					ChangeMask.CombineMultiple(FNetBitArrayView::OrOp, PrevUnresolvedChangeMask, FNetBitArrayView::AndNotOp, UnresolvedChangeMask);

					// Merge in partially resolved changes
					if (NewResolvedRefHandles.Num())
					{
						for (const FNetReferenceCollector::FReferenceInfo& ReferenceInfo : Collector.GetResolvedReferences())
						{
							const FNetRefHandle& MatchRefHandle = ReferenceInfo.Reference.GetRefHandle();
							if (NewResolvedRefHandles.ContainsByPredicate([&MatchRefHandle](const FNetRefHandle& RefHandle) { return RefHandle == MatchRefHandle;} ))
							{
								ChangeMask.SetBit(ReferenceInfo.ChangeMaskInfo.BitOffset);
							}
						}
					}
				}
			}

			// Apply state data
			if (ObjectData.InstanceProtocol)
			{
				Context.SetIsInitState(Info.bIsInitialState);

				FDequantizeAndApplyParameters Params;
				Params.Allocator = &TempLinearAllocator;
				Params.ChangeMaskData = Info.ChangeMaskOrPointer.GetPointer(ChangeMaskBitCount);
				Params.UnresolvedReferencesChangeMaskData = ReplicationInfo->bHasUnresolvedReferences ? ReplicationInfo->UnresolvedChangeMaskOrPointer.GetPointer(ChangeMaskBitCount) : nullptr;
				Params.InstanceProtocol = ObjectData.InstanceProtocol;
				Params.Protocol = ObjectData.Protocol;
				Params.SrcObjectStateBuffer = ObjectData.ReceiveStateBuffer;
				Params.bHasUnresolvedInitReferences = ReplicationInfo->bHasUnresolvedInitialReferences;

				// Dequantize state data, call PreReplicate and apply received state
				PostDispatchObjectInfo.DequantizeAndApplyContext = FDequantizeAndApplyHelper::Initialize(Context, Params);
				FDequantizeAndApplyHelper::ApplyAndCallLegacyPreApplyFunction(PostDispatchObjectInfo.DequantizeAndApplyContext, Context);
			}
			else
			{
				// $IRIS: $TODO: Figure out how to handle this, currently we do not crash but we probably want to
				// handle this properly by accumulating changemask for later instantiation
				UE_LOG(LogIris, Verbose, TEXT("Cannot dispatch state data for not instantiated %s"), *(ObjectData.RefHandle.ToString()));
			}
		}

		// Add to post dispatch
		PostDispatchObjectInfos[NumObjectsPendingPostDistpatch++] = PostDispatchObjectInfo;
	}

	FlushPostDispatchForBatch();
}

void FReplicationReader::ResolveAndDispatchUnresolvedReferences()
{
	IRIS_PROFILER_SCOPE(FReplicationReader_ResolveAndDispatchUnresolvedReferences);
	CSV_SCOPED_TIMING_STAT(IrisClient, ResolveAndDispatchUnresolvedReferences);

	// Setup context for dispatch
	FInternalNetSerializationContext InternalContext;
	FInternalNetSerializationContext::FInitParameters InternalContextInitParams;
	InternalContextInitParams.ReplicationSystem = Parameters.ReplicationSystem;
	InternalContextInitParams.ObjectResolveContext = ResolveContext;
	InternalContextInitParams.PackageMap = ReplicationSystemInternal->GetIrisObjectReferencePackageMap();
	InternalContext.Init(InternalContextInitParams);

	FNetSerializationContext Context;
	Context.SetLocalConnectionId(ResolveContext.ConnectionId);
	Context.SetInternalContext(&InternalContext);

	// Currently we brute force this by iterating over all handles pending resolve and update all objects pending resolve
	VisitedUnresolvedHandles.Reset();
	InternalObjectsToResolve.Reset();

	const uint32 CurrTimeMS = static_cast<uint32>(FPlatformTime::Seconds() * 1000.0f);
	const uint32 HotLifetimeMS = HotResolvingLifetimeMS > 0 ? static_cast<uint32>(HotResolvingLifetimeMS) : 0;
	const uint32 ColdRetryTimeMS = ColdResolvingRetryTimeMS > 0 ? static_cast<uint32>(ColdResolvingRetryTimeMS) : 0;

	for (const TPair<FNetRefHandle, uint32>& It : UnresolvedHandleToDependents)
	{
		const FNetRefHandle& Handle = It.Key;

		if (!VisitedUnresolvedHandles.Contains(Handle))
		{
			// Determine if the handle should be resolved.
			if (bUseResolvingHandleCache)
			{
				// If the handle is in the hot cache it should be resolved every time ResolveAndDispatchUnresolvedReferences() is called
				// and will be moved to the cold cache after a fixed period of time.
				if (const uint32* LifetimeMS = HotUnresolvedHandleCache.Find(Handle))
				{
					if ((CurrTimeMS - *LifetimeMS) > HotLifetimeMS)
					{
						HotUnresolvedHandleCache.Remove(Handle);
						ColdUnresolvedHandleCache.Add(Handle);
					}
				}
				// If the handle is in the cold cache it will only be resolved at a fixed interval and will remain in this cache indefinitely.
				else if (uint32* LastResolvedMS = ColdUnresolvedHandleCache.Find(Handle))
				{
					if ((CurrTimeMS - *LastResolvedMS) < ColdRetryTimeMS)
					{
						continue;
					}

					*LastResolvedMS = CurrTimeMS;
				}
				// If the handle is in neither the hot or cold cache, put it in the hot cache.
				else
				{
					HotUnresolvedHandleCache.Add(Handle, CurrTimeMS);
				}
			}

			// Only check this handle once per call.
			VisitedUnresolvedHandles.Add(Handle);
		}
	}

	for (FNetRefHandle Handle : VisitedUnresolvedHandles)
	{
		// Only make sense to update dependant objects if handle is resolvable
		if (ObjectReferenceCache->ResolveObjectReferenceHandle(Handle, ResolveContext) != nullptr)
		{
			for (auto It = UnresolvedHandleToDependents.CreateConstKeyIterator(Handle); It; ++It)
			{
				InternalObjectsToResolve.Add(It.Value());
			}
		}
	}

	// Add in any handles with pending attachments to resolve
	InternalObjectsToResolve.Append(ObjectsWithAttachmentPendingResolve);

	// Try to resolve objects with updated references
	for (uint32 InternalIndex : InternalObjectsToResolve)
	{
		ResolveAndDispatchUnresolvedReferencesForObject(Context, InternalIndex);
	}

#if IRIS_CLIENT_PROFILER_ENABLE
	CSV_CUSTOM_STAT(IrisClient, HotUnresolvedHandleCache, HotUnresolvedHandleCache.Num(), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(IrisClient, ColdUnresolvedHandleCache, ColdUnresolvedHandleCache.Num(), ECsvCustomStatOp::Set);

	CSV_CUSTOM_STAT(IrisClient, UnresolvedHandlesToResolve, VisitedUnresolvedHandles.Num(), ECsvCustomStatOp::Accumulate);
	CSV_CUSTOM_STAT(IrisClient, UnresolvedObjectsToResolve, InternalObjectsToResolve.Num(), ECsvCustomStatOp::Accumulate);

	const int32 TotalCacheSize = static_cast<int32>(
		HotUnresolvedHandleCache.GetAllocatedSize() +
		ColdUnresolvedHandleCache.GetAllocatedSize() +
		VisitedUnresolvedHandles.GetAllocatedSize() +
		InternalObjectsToResolve.GetAllocatedSize());
	CSV_CUSTOM_STAT(IrisClient, UnresolvedHandleBufferSizes, TotalCacheSize, ECsvCustomStatOp::Set);
#endif

	if (NumHandlesPendingResolveLastUpdate != VisitedUnresolvedHandles.Num() || ObjectsWithAttachmentPendingResolve.Num() > 0)
	{
		UE_LOG_REPLICATIONREADER(TEXT("FReplicationReader::ResolveAndDispatchUnresolvedReferences NetHandles pending: %u Attachments pending: %u)"), VisitedUnresolvedHandles.Num(), ObjectsWithAttachmentPendingResolve.Num());
		NumHandlesPendingResolveLastUpdate = VisitedUnresolvedHandles.Num();
	}
}


void FReplicationReader::UpdateUnresolvableReferenceTracking()
{
	constexpr uint32 AssumedMaxDependentCount = 256;
	TArray<uint32, TInlineAllocator<AssumedMaxDependentCount>> Dependents;

	// Naively go through every object pending destroy, see if it's dynamic and update dependent's unresolved tracking
	TArrayView<const FInternalNetRefIndex> ObjectsPendingDestroy = NetRefHandleManager->GetObjectsPendingDestroy();
	for (const FInternalNetRefIndex InternalIndex : ObjectsPendingDestroy)
	{
		const FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(InternalIndex);
		const FNetRefHandle DestroyedHandle = ObjectData.RefHandle;
		if (!DestroyedHandle.IsDynamic())
		{
			continue;
		}

		// For torn off objects we want to remove both resolved and unresolved references to it as it will never be replicated again.
		if (ObjectData.bTearOff)
		{
			constexpr bool bMaintainOrder = false;
			Dependents.Reset();
			UnresolvedHandleToDependents.MultiFind(DestroyedHandle, Dependents, bMaintainOrder);
			UnresolvedHandleToDependents.Remove(DestroyedHandle);
			RemoveFromUnresolvedCache(DestroyedHandle);
			for (const uint32 DependentObjectIndex : Dependents)
			{
				FReplicatedObjectInfo* ReplicationInfo = GetReplicatedObjectInfo(DependentObjectIndex);
				if (ensureMsgf(ReplicationInfo != nullptr, TEXT("Unable to find torn off unresolved replicated object info for %s"), *NetRefHandleManager->PrintObjectFromIndex(InternalIndex)))
				{
					RemoveUnresolvedObjectReferenceInReplicationInfo(ReplicationInfo, DestroyedHandle);
				}
			}
		}

		// For any previously resolved handles make sure to move them to unresolved status.
		constexpr bool bMaintainOrder = false;
		Dependents.Reset();
		ResolvedDynamicHandleToDependents.MultiFind(DestroyedHandle, Dependents, bMaintainOrder);
		if (Dependents.Num() > 0)
		{
			ResolvedDynamicHandleToDependents.Remove(DestroyedHandle);
			// Torn off objects will get new handles if replicated again so they can never be remapped.
			if (ObjectData.bTearOff)
			{
				for (const uint32 DependentObjectIndex : Dependents)
				{
					FReplicatedObjectInfo* ReplicationInfo = GetReplicatedObjectInfo(DependentObjectIndex);
					if (ensureMsgf(ReplicationInfo != nullptr, TEXT("Unable to find torn off resolved replicated object info for %s"), *NetRefHandleManager->PrintObjectFromIndex(InternalIndex)))
					{
						RemoveResolvedObjectReferenceInReplicationInfo(ReplicationInfo, DestroyedHandle);
					}
				}
			}
			else
			{
				for (const uint32 DependentObjectIndex : Dependents)
				{
					FReplicatedObjectInfo* ReplicationInfo = GetReplicatedObjectInfo(DependentObjectIndex);
					if (ensureMsgf(ReplicationInfo != nullptr, TEXT("Unable to find resolved replicated object info for %s"), *NetRefHandleManager->PrintObjectFromIndex(InternalIndex)))
					{
						if (MoveResolvedObjectReferenceToUnresolvedInReplicationInfo(ReplicationInfo, DestroyedHandle))
						{
							UnresolvedHandleToDependents.Add(DestroyedHandle, DependentObjectIndex);
						}
					}
				}
			}
		}
	}
}

void FReplicationReader::DispatchEndReplication(FNetSerializationContext& Context)
{
	for (FDispatchObjectInfo& Info : ObjectsToDispatchArray->GetObjectsToDispatch())
	{
		if (Info.bDeferredEndReplication)
		{
			// Detach and destroy object
			EndReplication(Info.InternalIndex, Info.bTearOff, Info.bDestroy);
			if (Info.bProcessEndReplicationForHierarchy)
			{
				for (FInternalNetRefIndex SubObjectIndex : NetRefHandleManager->GetSubObjects(Info.InternalIndex))
				{
					// End replication for all subobjects and destroy dynamic ones
					const FNetRefHandle SubObjectRefHandle = NetRefHandleManager->GetNetRefHandleFromInternalIndex(SubObjectIndex);
					const bool bShouldDestroySubObject = SubObjectRefHandle.IsDynamic();
					EndReplication(SubObjectIndex, Info.bTearOff, bShouldDestroySubObject);
				}
			}
		}
	}
}

void FReplicationReader::ReadObjects(FNetSerializationContext& Context, uint32 ObjectBatchCountToRead, uint32 ReadObjectFlags)
{
	IRIS_PROFILER_SCOPE(ReplicationReader_ReadObjects);

	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();
	
	while (ObjectBatchCountToRead && !Context.HasErrorOrOverflow() && (!bGracefullyHandleReachingEndOfBitstream || (Reader.GetBitsLeft() > 0)))
	{
		ReadObjectBatch(Context, ReadObjectFlags);
		--ObjectBatchCountToRead;
	}

	UE_CLOG(Context.HasErrorOrOverflow(), LogIris, Error, TEXT("Overflow: %c Error: %s Bit stream bits left: %u position: %u %s"), TEXT("YN")[Context.HasError()], ToCStr(Context.GetError().ToString()), Reader.GetBitsLeft(), Reader.GetPosBits(), *Context.PrintReadJournal());

	if (ensure(!Context.HasErrorOrOverflow()))
	{
		if (bGracefullyHandleReachingEndOfBitstream)
		{
			// ObjectBatchCountToRead should be zero at this point otherwise there's a problem on the writing side.
			UE_CLOG(ObjectBatchCountToRead > 0, LogIris, Error, TEXT("Reached end of bitstream prior to reading all expected objects. %u left to read"), ObjectBatchCountToRead);
			ensure(ObjectBatchCountToRead == 0);
		}
	}
}

void FReplicationReader::ProcessHugeObjectAttachment(FNetSerializationContext& Context, const TRefCountPtr<FNetBlob>& Attachment)
{
	if (Attachment->GetCreationInfo().Type != NetObjectBlobType)
	{
		Context.SetError(GNetError_UnsupportedNetBlob);
		return;
	}

	IRIS_PROFILER_SCOPE(FReplicationReader_ProcessHugeObjectAttachment)

	FNetTraceCollector* HugeObjectTraceCollector = nullptr;
#if UE_NET_TRACE_ENABLED
	FNetTraceCollector HugeObjectTraceCollectorOnStack;
	HugeObjectTraceCollector = &HugeObjectTraceCollectorOnStack;
#endif

	const FNetObjectBlob& NetObjectBlob = *static_cast<FNetObjectBlob*>(Attachment.GetReference());

	FNetBitStreamReader HugeObjectReader;
	HugeObjectReader.InitBits(NetObjectBlob.GetRawData().GetData(), NetObjectBlob.GetRawDataBitCount());
	FNetSerializationContext HugeObjectSerializationContext = Context.MakeSubContext(&HugeObjectReader);
	HugeObjectSerializationContext.SetTraceCollector(HugeObjectTraceCollector);

	UE_NET_TRACE_NAMED_SCOPE(HugeObjectTraceScope, HugeObjectState, HugeObjectReader, HugeObjectTraceCollector, ENetTraceVerbosity::Trace);

	ON_SCOPE_EXIT
	{
#if UE_NET_TRACE_ENABLED
		UE_NET_TRACE_EXIT_NAMED_SCOPE(HugeObjectTraceScope);

		// Append huge object state at end of stream.
		if (FNetTraceCollector* TraceCollector = Context.GetTraceCollector())
		{
			FNetBitStreamReader& Reader = *Context.GetBitStreamReader();
			// Inject after all other trace events
			FNetTrace::FoldTraceCollector(TraceCollector, HugeObjectTraceCollector, GetBitStreamPositionForNetTrace(Reader));
		}
#endif
	};

	// Find out how many objects to read so we can reserve object dispatch infos.
	FNetObjectBlob::FHeader HugeObjectHeader = {};
	FNetObjectBlob::DeserializeHeader(HugeObjectSerializationContext, HugeObjectHeader);
	ReadStreamDebugFeatures(HugeObjectSerializationContext);
	if (HugeObjectSerializationContext.HasErrorOrOverflow() || HugeObjectHeader.ObjectCount < 1U)
	{
		if (!Context.HasError())
		{
			Context.SetError(GNetError_BitStreamError);
			return;
		}
	}

	// Reserve space for more dispatch infos as needed, we allocate some extra to account for subobjects etc
	ObjectsToDispatchArray->Grow(HugeObjectHeader.ObjectCount + ObjectsToDispatchSlackCount, TempLinearAllocator);

	const uint32 ReadObjectFlags = EReadObjectFlag::ReadObjectFlag_IsReadingHugeObjectBatch;
	ReadObjects(HugeObjectSerializationContext, HugeObjectHeader.ObjectCount, ReadObjectFlags);
	if (HugeObjectSerializationContext.HasErrorOrOverflow())
	{
		Context.SetError(GNetError_BitStreamError);
		return;
	}
}

bool FReplicationReader::EnqueueEndReplication(FPendingBatchData* PendingBatchData, bool bShouldDestroyInstance, bool bShouldProcessHierarchy, FNetRefHandle NetRefHandleToEndReplication)
{
	UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::EnqueueEndReplication for %s since %s has queued batches"), *NetRefHandleToEndReplication.ToString(), *PendingBatchData->Handle.ToString());
		
	const uint32 MaxNumDataBits = 66U;
	const uint32 NumDataWords = (MaxNumDataBits + 31U) / 32U;

	// Enqueue BatchData
	FQueuedDataChunk DataChunk;

	DataChunk.NumBits = MaxNumDataBits;
	DataChunk.StorageOffset = PendingBatchData->DataChunkStorage.Num();
	DataChunk.bHasBatchOwnerData = false;
	DataChunk.bIsEndReplicationChunk = true;

	// Make sure we have space
	PendingBatchData->DataChunkStorage.AddUninitialized(NumDataWords);

	FNetBitStreamWriter Writer;
	Writer.InitBytes(PendingBatchData->DataChunkStorage.GetData() + DataChunk.StorageOffset, NumDataWords*sizeof(uint32));

	// Write data to be parsed by ProcessQueuedBatches
	WriteUint64(&Writer, NetRefHandleToEndReplication.GetId());
	Writer.WriteBool(bShouldDestroyInstance);
	Writer.WriteBool(bShouldProcessHierarchy);
	Writer.CommitWrites();
	ensure(Writer.GetPosBits() <= MaxNumDataBits);
	
	// Set chunk's actual bit count
	DataChunk.NumBits = Writer.GetPosBits();

	if (Writer.IsOverflown())
	{
		UE_LOG(LogIris, Error, TEXT("Failed to EnqueueEndReplication for %s, Should never occur unless size of NetRefHandle has been increased."), *NetRefHandleToEndReplication.ToString());
		ensure(false);

		return false;
	}

	PendingBatchData->QueuedDataChunks.Add(MoveTemp(DataChunk));

	return true;
}

void FReplicationReader::RemoveFromUnresolvedCache(const FNetRefHandle Handle)
{
	if (bUseResolvingHandleCache && !UnresolvedHandleToDependents.Contains(Handle))
	{
		HotUnresolvedHandleCache.Remove(Handle);
		ColdUnresolvedHandleCache.Remove(Handle);
	}
}

void FReplicationReader::ProcessQueuedBatches()
{
	UE_NET_TRACE_FRAME_STATSCOUNTER(Parameters.ReplicationSystem->GetId(), ReplicationReader.PendingQueuedBatches, PendingBatches.PendingBatches.Num(), ENetTraceVerbosity::Trace);

	if (!PendingBatches.GetHasPendingBatches())
	{
		//Nothing to do.
		return;
	}

	// Setup context for dispatch
	FInternalNetSerializationContext InternalContext;
	FInternalNetSerializationContext::FInitParameters InternalContextInitParams;
	InternalContextInitParams.ReplicationSystem = Parameters.ReplicationSystem;
	InternalContextInitParams.ObjectResolveContext = ResolveContext;
	InternalContextInitParams.PackageMap = ReplicationSystemInternal->GetIrisObjectReferencePackageMap();
	InternalContext.Init(InternalContextInitParams);

	FNetBitStreamReader Reader;
	FNetSerializationContext Context(&Reader);
	Context.SetLocalConnectionId(ResolveContext.ConnectionId);
	Context.SetInternalContext(&InternalContext);
	Context.SetNetBlobReceiver(&ReplicationSystemInternal->GetNetBlobHandlerManager());

	for (int BatchIt = 0; BatchIt < PendingBatches.PendingBatches.Num(); )
	{
		FPendingBatchData& PendingBatchData = PendingBatches.PendingBatches[BatchIt];

		// Try to resolve remaining must be mapped references
		TempMustBeMappedReferences.Reset();
		UpdateUnresolvedMustBeMappedReferences(PendingBatchData.Handle, TempMustBeMappedReferences);

		// If we have no more pending must be references we can apply the received state
		if (PendingBatchData.PendingMustBeMappedReferences.IsEmpty())		
		{
			UE_LOG(LogIris, Verbose, TEXT("ProcessQueuedBatches processing %d queued batches for Handle %s "), PendingBatchData.QueuedDataChunks.Num(), *PendingBatchData.Handle.ToString());

			// Reset for batch
			Context.ResetErrorContext();
			Context.SetErrorHandleContext(PendingBatchData.Handle);
			UE_RESET_READ_JOURNAL(Context);
			UE_ADD_READ_JOURNAL_ENTRY(Context, TEXT("ProcessQueuedBatches"));

			// Process batched data & dispatch data
			for (const FQueuedDataChunk& CurrentChunk : PendingBatchData.QueuedDataChunks)
			{
				Reader.InitBits(PendingBatchData.DataChunkStorage.GetData() + CurrentChunk.StorageOffset, CurrentChunk.NumBits);

				// Chunks marked as bIsEndReplicationChunk are dispatched immediately as we do not know if the next chunk tries to re-create the instance
				if (CurrentChunk.bIsEndReplicationChunk)
				{
					// Read data stored for objects ending replication, 
					// this can be the batch root or a subobject owned by the batched root.
					const uint64 NetRefHandleIdToEndReplication = ReadUint64(&Reader);
					const bool bShouldDestroyInstance = Reader.ReadBool();
					const bool bShouldProcessHierarchy = Reader.ReadBool();
					
					const FNetRefHandle NetRefHandleToEndReplication = FNetRefHandleManager::MakeNetRefHandleFromId(NetRefHandleIdToEndReplication);
					
					// End replication for object
					const uint32 InternalIndex = NetRefHandleManager->GetInternalIndex(NetRefHandleToEndReplication);
					if (InternalIndex != FNetRefHandleManager::InvalidInternalIndex)
					{
						constexpr bool bTearOff = false;
						if (bShouldProcessHierarchy)
						{
							EndReplication(InternalIndex, bTearOff, bShouldDestroyInstance);
							for (const FInternalNetRefIndex SubObjectIndex : NetRefHandleManager->GetSubObjects(InternalIndex))
							{
								// End replication for all subobjects and destroy dynamic ones
								const FNetRefHandle SubObjectRefHandle = NetRefHandleManager->GetNetRefHandleFromInternalIndex(SubObjectIndex);
								const bool bShouldDestroySubObject = SubObjectRefHandle.IsDynamic();
								EndReplication(SubObjectIndex, bTearOff, bShouldDestroySubObject);
							}
						}
						else
						{
							EndReplication(InternalIndex, bTearOff, bShouldDestroyInstance);
						}
					}

					// Remove from broken list
					BrokenObjects.SetNum(Algo::RemoveIf(BrokenObjects, [&NetRefHandleToEndReplication](const FNetRefHandle& Handle)
					{
						return Handle.GetId() == NetRefHandleToEndReplication.GetId();
					}));

					UE_LOG(LogIris, Verbose, TEXT("FReplicationReader::ProcessQueuedBatches EndReplication for %s while processing queued batches for %s"), *NetRefHandleToEndReplication.ToString(), *PendingBatchData.Handle.ToString());
					continue;
				}

				// Skip over broken objects, we still process remaining chunk if the object has been destroyed.
				const bool bIsBroken = (BrokenObjects.FindByPredicate([&PendingBatchData](const FNetRefHandle& Entry) { return Entry.GetId() == PendingBatchData.Handle.GetId(); }) != nullptr);
				if (bIsBroken)
				{
					continue;
				}

				// Read and process chunk as it was a received packet for now at least
				FMemMark TempAllocatorScope(TempLinearAllocator);

				// We need to set this up to store temporary dispatch data, the array will grow if needed
				FObjectsToDispatchArray TempObjectsToDispatchArray(ObjectsToDispatchSlackCount, TempLinearAllocator);

				// Need to set this pointer as we are dealing with temporary linear allocations
				ObjectsToDispatchArray = &TempObjectsToDispatchArray;

				// Use whatver StreamDebugFeatures were set when receiving the chunk
				StreamDebugFeatures = CurrentChunk.StreamDebugFeatures;

				// $IRIS: $TODO: Implement special dispatch to defer RepNotifies if we are processing multiple batches for the same object.
				ReadObjectsInBatch(Context, PendingBatchData.Handle, CurrentChunk.bHasBatchOwnerData, CurrentChunk.NumBits);

				if (Context.HasErrorOrOverflow())
				{
					BrokenObjects.AddUnique(PendingBatchData.Handle);

					// Log error  if get more incoming data for an object in the broken state we will skip it.
					UE_LOG(LogIris, Error, TEXT("FReplicationReader::ProcessQueuedBatches Failed to process object batch handle: %s skipping batch data. bIsEndReplication: %d"), ToCStr(PendingBatchData.Handle.ToString()), int(CurrentChunk.bIsEndReplicationChunk));

					if (Context.GetError() == GNetError_BrokenNetHandle)
					{
						ReplicationBridge->SendErrorWithNetRefHandle(UE::Net::ENetRefHandleError::ReplicationDisabled, PendingBatchData.Handle, Parameters.ConnectionId);

						Context.ResetErrorContext();
					}
				}
			
				// Apply received data and resolve dependencies
				DispatchStateData(Context);

				// Resolve
				ResolveAndDispatchUnresolvedReferences();

				// EndReplication for all objects in the batch that should no longer replicate
				DispatchEndReplication(Context);

				// Drop temporary data
				ObjectsToDispatchArray = nullptr;
			}
	
			// Make sure to release all references that we hold on to
			for (const FNetRefHandle& RefHandle : PendingBatchData.ResolvedReferences)
			{
				ObjectReferenceCache->RemoveTrackedQueuedBatchObjectReference(RefHandle);
			}

			// Not optimal, but we want to preserve the order if we can as there might be batches waiting for the same reference
			PendingBatches.PendingBatches.RemoveAt(BatchIt);
		}
		else
		{
			// Warn if we have been waiting for too long
			if ((QueuedBatchTimeoutWarningInterval > 0) && (PendingBatchData.PendingBatchTryProcessCount > QueuedBatchTimeoutWarningInterval) && FPlatformProperties::RequiresCookedData())
			{
				const double QueuedBatchPendingTime = FPlatformTime::ToSeconds64(FPlatformTime::Cycles64() - PendingBatchData.PendingBatchStartCycles);
				if (QueuedBatchPendingTime > QueuedBatchTimeoutSeconds)
				{
					UE_LOG(LogIris, Warning, TEXT("FReplicationWriter::ProcessQueuedBatches: Queued batches for longer than normal. %s, Queued: %i, PendingNetRefHandleResolves: %i, BatchPendingTime: %f")
						, *NetRefHandleManager->PrintObjectFromNetRefHandle(PendingBatchData.Handle)
						, PendingBatchData.QueuedDataChunks.Num()
						, PendingBatchData.PendingMustBeMappedReferences.Num()
						, QueuedBatchPendingTime);

					TArray<FNetRefHandle> PendingMustBeMapped;
					PendingMustBeMapped.Reserve(MaxMustBeMappedHandleArray);

					for (const FNetRefHandle& NetRefHandle : PendingBatchData.PendingMustBeMappedReferences)
					{
						UE_LOG(LogIris, Log, TEXT("  NetRefHandle Pending resolve [%s]"), *ObjectReferenceCache->DescribeObjectReference(FObjectReferenceCache::MakeNetObjectReference(NetRefHandle), ResolveContext));
						if (PendingMustBeMapped.Num() < MaxMustBeMappedHandleArray)
						{
							PendingMustBeMapped.Emplace(NetRefHandle);
						}
					}

					ReplicationBridge->SendErrorWithNetRefHandle(UE::Net::ENetRefHandleError::BlockedByMustBeMapped, PendingBatchData.Handle, Parameters.ConnectionId, PendingMustBeMapped);

					// Reset counter
					PendingBatchData.PendingBatchTryProcessCount = 0;
				}
			}
			else
			{
				// Increment counter
				++PendingBatchData.PendingBatchTryProcessCount;
			}

			++BatchIt;
		}
	}
}

void FReplicationReader::ProcessHugeObject(FNetSerializationContext& Context)
{
	if (!Attachments.HasUnprocessedAttachments(ENetObjectAttachmentType::HugeObject, ObjectIndexForOOBAttachment))
	{
		return;
	}

	FNetObjectAttachmentReceiveQueue* AttachmentQueue = Attachments.GetQueue(ENetObjectAttachmentType::HugeObject, ObjectIndexForOOBAttachment);
	while (const TRefCountPtr<FNetBlob>* Attachment = AttachmentQueue->PeekReliable())
	{
		ProcessHugeObjectAttachment(Context, *Attachment);
		AttachmentQueue->PopReliable();
		if (Context.HasError())
		{
			return;
		}
	}
	while (const TRefCountPtr<FNetBlob>* Attachment = AttachmentQueue->PeekUnreliable())
	{
		ProcessHugeObjectAttachment(Context, *Attachment);
		AttachmentQueue->PopUnreliable();
		if (Context.HasError())
		{
			return;
		}
	}
}

void FReplicationReader::Read(FNetSerializationContext& Context)
{
	FNetBitStreamReader& Reader = *Context.GetBitStreamReader();

	// Setup internal context
	FInternalNetSerializationContext InternalContext;
	FInternalNetSerializationContext::FInitParameters InternalContextInitParams;
	InternalContextInitParams.ReplicationSystem = Parameters.ReplicationSystem;
	InternalContextInitParams.PackageMap = ReplicationSystemInternal->GetIrisObjectReferencePackageMap();
	InternalContextInitParams.ObjectResolveContext = ResolveContext;
	InternalContext.Init(InternalContextInitParams);
	
	Context.SetLocalConnectionId(Parameters.ConnectionId);
	Context.SetInternalContext(&InternalContext);
	Context.SetNetBlobReceiver(&ReplicationSystemInternal->GetNetBlobHandlerManager());

	UE_NET_TRACE_SCOPE(ReplicationData, *Context.GetBitStreamReader(), Context.GetTraceCollector(), ENetTraceVerbosity::Trace);

	ReadStreamDebugFeatures(Context);
	if (Context.HasErrorOrOverflow())
	{
		return;
	}

	FMemMark TempAllocatorScope(TempLinearAllocator);

	// Sanity check received object count
	const uint32 MaxObjectBatchCountToRead = 8192U;
	const uint32 ReceivedObjectBatchCountToRead = Reader.ReadBits(16);
	uint32 ObjectBatchCountToRead = ReceivedObjectBatchCountToRead;

	if (Reader.IsOverflown() || ObjectBatchCountToRead >= MaxObjectBatchCountToRead)
	{
		const FName& NetError = (Reader.IsOverflown() ? GNetError_BitStreamOverflow : GNetError_BitStreamError);
		Context.SetError(NetError);

		return;
	}

	if (ObjectBatchCountToRead == 0)
	{
		return;
	}

	// Allocate tracking info for objects we receive this packet from temporary allocator
	// We need to set this up to store temporary dispatch data, the array will grow if needed
	FObjectsToDispatchArray TempObjectsToDispatchArray(ObjectBatchCountToRead + ObjectsToDispatchSlackCount, TempLinearAllocator);

	// Need to set this pointer as we are dealing with temporary linear allocations
	ObjectsToDispatchArray = &TempObjectsToDispatchArray;

	uint32 DestroyedObjectCount = ReadObjectsPendingDestroy(Context);

	ObjectBatchCountToRead -= DestroyedObjectCount;

	// Nothing more to do or we failed and should disconnect
	if (Context.HasErrorOrOverflow() || (ObjectBatchCountToRead == 0 && ObjectsToDispatchArray->Num() == 0))
	{
		return;
	}

	const uint32 ReadObjectFlags = 0U;
	ReadObjects(Context, ObjectBatchCountToRead, ReadObjectFlags);
	if (Context.HasErrorOrOverflow())
	{
		return;
	}

	// Assemble and deserialize huge object if present
	ProcessHugeObject(Context);
	if (Context.HasErrorOrOverflow())
	{
		return;
	}

	// Stats
	UE_NET_TRACE_PACKET_STATSCOUNTER(Parameters.ReplicationSystem->GetId(), Parameters.ConnectionId, ReplicationReader.ReadObjectBatchCount, ReceivedObjectBatchCountToRead, ENetTraceVerbosity::Trace);
	UE_NET_TRACE_PACKET_STATSCOUNTER(Parameters.ReplicationSystem->GetId(), Parameters.ConnectionId, ReplicationReader.ReadObjectsAndSubObjectsToDispatchCount, ObjectsToDispatchArray->Num(), ENetTraceVerbosity::Trace);

	// Apply received data and resolve dependencies
	DispatchStateData(Context);
	if (Context.HasErrorOrOverflow())
	{
		return;
	}

	// Resolve
	ResolveAndDispatchUnresolvedReferences();

	// EndReplication for all objects that should no longer replicate
	DispatchEndReplication(Context);

	// Drop temporary dispatch data
	ObjectsToDispatchArray = nullptr;
}

void FReplicationReader::ResolveAndDispatchAttachments(FNetSerializationContext& Context, FReplicatedObjectInfo* ReplicationInfo, ENetObjectAttachmentDispatchFlags DispatchFlags)
{
	if (DispatchFlags == ENetObjectAttachmentDispatchFlags::None)
	{
		return;
	}

	// Cache configurables before processing attachments
	const bool bDispatchReliableAttachments = EnumHasAnyFlags(DispatchFlags, ENetObjectAttachmentDispatchFlags::Reliable);
	const bool bDispatchUnreliableAttachments = EnumHasAnyFlags(DispatchFlags, ENetObjectAttachmentDispatchFlags::Unreliable);
	const bool bCanDelayAttachments = Parameters.bAllowDelayingAttachmentsWithUnresolvedReferences && (DelayAttachmentsWithUnresolvedReferences != nullptr && DelayAttachmentsWithUnresolvedReferences->GetInt() > 0);
	const uint32 InternalIndex = ReplicationInfo->InternalIndex;

	/**
	 * This code path handles all cases where the initial state has already been applied. An object can have multiple entries in ObjectsPendingResolve.
	 * Reliable attachments will be dispatched if they can be resolved or if CVarDelayUnmappedRPCs is <= 0. Unreliable but ordered attachments will always be dispatched.
	 */
	bool bHasUnresolvedReferences = false;
	const ENetObjectAttachmentType AttachmentType = (IsObjectIndexForOOBAttachment(InternalIndex) ? ENetObjectAttachmentType::OutOfBand : ENetObjectAttachmentType::Normal);
	if (FNetObjectAttachmentReceiveQueue* AttachmentQueue = Attachments.GetQueue(AttachmentType, InternalIndex))
	{
		if (bDispatchReliableAttachments)
		{
			while (const TRefCountPtr<FNetBlob>* Attachment = AttachmentQueue->PeekReliable())
			{
				// Delay reliable attachments with unresolved pending references
				const bool bIsReliable = EnumHasAnyFlags(Attachment->GetReference()->GetCreationInfo().Flags, ENetBlobFlags::Reliable);
				if (bIsReliable && bCanDelayAttachments)
				{
					bool bDelayRpc = false;

					FNetReferenceCollector Collector;
					Attachment->GetReference()->CollectObjectReferences(Context, Collector);

					// Check status of references, as we already should have queued up any unmapped references at the batch level, it should be enough to only check if we have any unresolved references pending async load.
					// NOTE: Behavior is slightly different between Iris and old replication system due to the fact that Iris processes incoming packet data prior to dispatching received stats and RPC:s, that means that 
					// we expect to be able to resolve all dynamic references contained in the same data packet and does not delay the RPC until the next tick to solve that as the old system does. 
					// The difference is that the old system might be able to resolve incoming dynamic references from later packets processed for the same tick, but as this is far from guaranteed we currently do not try to mimic this.
					for (const FNetReferenceCollector::FReferenceInfo& Info : MakeArrayView(Collector.GetCollectedReferences()))
					{
						if (ObjectReferenceCache->IsNetRefHandlePending(Info.Reference.GetRefHandle(), PendingBatches))
						{
							bDelayRpc = true;
							break;
						}
					}

					if (bDelayRpc)
					{
						const FReplicationStateDescriptor* Descriptor = Attachment->GetReference()->GetReplicationStateDescriptor();

						UE_LOG(LogIris, Verbose, TEXT("Delaying Attachment - %s for InternalIndex %u." ), (Descriptor != nullptr ? ToCStr(Descriptor->DebugName) : TEXT("N/A")), InternalIndex);
						break;
					}
				}

				NetBlobHandlerManager->OnNetBlobReceived(Context, *reinterpret_cast<const TRefCountPtr<FNetBlob>*>(Attachment));
				AttachmentQueue->PopReliable();

				if (Context.HasError())
				{
					return;
				}
			}
		}

		if (bDispatchUnreliableAttachments)
		{
			while (const TRefCountPtr<FNetBlob>* Attachment = AttachmentQueue->PeekUnreliable())
			{
				NetBlobHandlerManager->OnNetBlobReceived(Context, *Attachment);
				AttachmentQueue->PopUnreliable();

				if (Context.HasError())
				{
					return;
				}
			}
		}
		
		if (AttachmentQueue->IsSafeToDestroy())
		{
			// N.B. AttachmentQueue is no longer valid after this call
			Attachments.DropAllAttachments(AttachmentType, InternalIndex);
		}
		else
		{
			bHasUnresolvedReferences = AttachmentQueue->HasUnprocessed();
		}
	}
	else
	{
		// Should not get here, if we do something is out of sync and we should disconnect
		Context.SetError(NetError_FailedToFindAttachmentQueue);
		ensure(AttachmentType == ENetObjectAttachmentType::OutOfBand);
	}

	// Update tracking of objects with attachments pending resolve
	if (bHasUnresolvedReferences && !ReplicationInfo->bHasAttachments)
	{
		ObjectsWithAttachmentPendingResolve.Add(InternalIndex);
	}
	else if (!bHasUnresolvedReferences && ReplicationInfo->bHasAttachments)
	{
		ObjectsWithAttachmentPendingResolve.RemoveSwap(InternalIndex);
	}
	ReplicationInfo->bHasAttachments = bHasUnresolvedReferences;
}

bool FReplicationReader::ReadSentinel(FNetSerializationContext& Context, const TCHAR* DebugName)
{
	bool bSuccess = true;

#if UE_NET_REPLICATIONDATASTREAM_DEBUG
	if (EnumHasAnyFlags(StreamDebugFeatures, EReplicationDataStreamDebugFeatures::Sentinels))
	{
		bSuccess = ReadAndVerifySentinelBits(Context.GetBitStreamReader(), DebugName, 8U);
		if (!bSuccess)
		{
			Context.SetError(GNetError_BitStreamError);
		}
	}
#endif

	return bSuccess;
}

FString FReplicationReader::PrintObjectInfo(FInternalNetRefIndex ObjectIndex, FNetRefHandle NetRefHandle) const
{
	const FReplicatedObjectInfo* ObjectInfo = GetReplicatedObjectInfo(ObjectIndex);
	if (!ObjectInfo)
	{
		return FString::Printf(TEXT("No object info for (InternalIndex: %u)"), ObjectIndex);
	}

	TStringBuilder<512> InfoBuilder;

	const FNetRefHandleManager::FReplicatedObjectData& ObjectData = NetRefHandleManager->GetReplicatedObjectDataNoCheck(ObjectIndex);
	const bool bIsSubObject = ObjectData.SubObjectRootIndex != FNetRefHandleManager::InvalidInternalIndex;
	InfoBuilder.Appendf(TEXT("Status info: 0x%llx (%s)"), ObjectInfo->Value, bIsSubObject?TEXT("SubObject"):TEXT("RootObject"));

	const bool bHasPendingBatches = PendingBatches.GetHasPendingBatches();
	const FInternalNetRefIndex RootInternalIndex = bIsSubObject ? ObjectData.SubObjectRootIndex : ObjectIndex;
	if (const FPendingBatchData* PendingBatchData = bHasPendingBatches ? PendingBatches.Find(NetRefHandleManager->GetNetRefHandleFromInternalIndex(RootInternalIndex)) : nullptr)
	{
		InfoBuilder.Appendf(TEXT("| PendingBatches: QueuedChunks: %d | PendingReferences(%d): "), PendingBatchData->QueuedDataChunks.Num(), PendingBatchData->PendingMustBeMappedReferences.Num());
		for (const FNetRefHandle& PendingHandle : PendingBatchData->PendingMustBeMappedReferences)
		{
			InfoBuilder << PendingHandle.ToString() << ", ";
		}
	}
	else
	{
		InfoBuilder << TEXT("| NoPendingBatches");
	}

	return InfoBuilder.ToString();
}

} // end namespace UE::Net::Private
