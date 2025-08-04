// Copyright Epic Games, Inc. All Rights Reserved.

#include "MutableStreamRequest.h"

#include "MuCO/CustomizableObjectPrivate.h"
#include "MuCO/CustomizableObjectSystemPrivate.h"
#include "HAL/PlatformFileManager.h"

#if WITH_EDITOR
#include "DerivedDataCache.h"
#include "DerivedDataCacheInterface.h"
#include "DerivedDataCacheKey.h"
#include "DerivedDataRequestOwner.h"
#endif

FMutableStreamRequest::FMutableStreamRequest(const TSharedPtr<FModelStreamableBulkData>& InModelStreamableBulkData) :
	ModelStreamableBulkData(InModelStreamableBulkData)
{
}


const TSharedPtr<FModelStreamableBulkData>& FMutableStreamRequest::GetModelStreamableBulkData() const
{
	return ModelStreamableBulkData;
}


void FMutableStreamRequest::AddBlock(const FMutableStreamableBlock& Block, MutablePrivate::EStreamableDataType DataType, uint16 ResourceType, TArrayView<uint8> AllocatedMemoryView)
{
	if (bIsStreaming)
	{
		check(false);
		return;
	}

	IAsyncReadFileHandle* FileHandle = nullptr;

#if WITH_EDITOR
	if (!ModelStreamableBulkData->bIsStoredInDDC)
	{
		int32 FileHandleIndex = OpenFilesIds.Find(Block.FileId);
		if (FileHandleIndex == INDEX_NONE)
		{
			const FString& FullFileName = ModelStreamableBulkData->FullFilePath + GetDataTypeExtension(DataType);
			IAsyncReadFileHandle* ReadFileHandle = FPlatformFileManager::Get().GetPlatformFile().OpenAsyncRead(*FullFileName);
			OpenFileHandles.Emplace(ReadFileHandle);

			FileHandleIndex = OpenFilesIds.Add(Block.FileId);
			check(OpenFileHandles.Num() == OpenFilesIds.Num());
		}

		if (OpenFileHandles.IsValidIndex(FileHandleIndex))
		{
			FileHandle = OpenFileHandles[FileHandleIndex].Get();
		}
	}
#endif
	
	BlockReadInfos.Emplace(Block.Offset, FileHandle, AllocatedMemoryView, Block.FileId, DataType, ResourceType, Block.Flags);
}


UE::Tasks::FTask FMutableStreamRequest::Stream()
{
	if (bIsStreaming)
	{
		check(false);
		return UE::Tasks::MakeCompletedTask<void>();
	}

	bIsStreaming = true;

	for (const FBlockReadInfo& Block : BlockReadInfos)
	{
		HeapMemory->CompletionEvents.Emplace(TEXT("AsyncReadDataReadyEvent"));
	}

	UE::Tasks::Launch(TEXT("CustomizableObjectReadRequestTask"),
		[
			ModelStreamableBulkData = ModelStreamableBulkData,
			BlockReadInfos = MoveTemp(BlockReadInfos),
			HeapMemory = this->HeapMemory
		]() mutable
		{
			MUTABLE_CPUPROFILER_SCOPE(CustomizableInstanceLoadBlocksAsyncRead_Request);
			FScopeLock Lock(&HeapMemory->ReadRequestLock);

			// Task cancelled, early out.
			if (HeapMemory->bIsCancelled)
			{
				// Trigger preallocated events to complete GatherStreamingRequestsCompletionTask prerequisites
				for (UE::Tasks::FTaskEvent& CompletionEvent : HeapMemory->CompletionEvents)
				{
					CompletionEvent.Trigger();
				}

				return;
			}

#if WITH_EDITOR
			if (ModelStreamableBulkData->bIsStoredInDDC)
			{
				using namespace UE::DerivedData;

				// Skip loading values by default
				FCacheRecordPolicyBuilder PolicyBuilder(ECachePolicy::Default | ECachePolicy::SkipData);

				TArray<FValueId> ResourceIds;
				ResourceIds.Reserve(BlockReadInfos.Num());

				// Override policy for the resources to load.
				for (const FBlockReadInfo& Block : BlockReadInfos)
				{
					FValueId ResourceId = GetDerivedDataValueIdForResource(Block.DataType, Block.FileId, Block.ResourceType, Block.ResourceFlags);

					if (ResourceIds.Find(ResourceId) == INDEX_NONE)
					{
						// Add value policy once.
						PolicyBuilder.AddValuePolicy(ResourceId, ECachePolicy::Default);
					}

					ResourceIds.Add(ResourceId);
				}

				FCacheGetRequest Request;
				Request.Name = ModelStreamableBulkData->FullFilePath;
				Request.Key = ModelStreamableBulkData->DDCKey;
				Request.Policy = PolicyBuilder.Build();

				TSharedPtr<UE::DerivedData::FRequestOwner>& DDCRequest = HeapMemory->DDCReadRequest.Add_GetRef(MakeShared<FRequestOwner>(EPriority::High));

				GetCache().Get(MakeArrayView(&Request, 1), *DDCRequest,
					[HeapMemory, BlockReadInfos = MoveTemp(BlockReadInfos), CapturedResourceIds = MoveTemp(ResourceIds)](FCacheGetResponse&& Response) mutable
					{
						bool bSuccess = Response.Status == EStatus::Ok;

						if (ensure(bSuccess))
						{
							const int32 NumResources = CapturedResourceIds.Num();
							for (int32 Index = 0; Index < NumResources; ++Index)
							{
								const FCompressedBuffer& CompressedBuffer = Response.Record.GetValue(CapturedResourceIds[Index]).GetData();
								if (ensure(!CompressedBuffer.IsNull()))
								{
									const FBlockReadInfo& Block = BlockReadInfos[Index];
									const uint64 Size = Block.AllocatedMemoryView.Num();
									if (Size < CompressedBuffer.GetRawSize())
									{
										check(CompressedBuffer.GetRawSize() >= Block.Offset + Size);
										FSharedBuffer DecompressedBuffer = CompressedBuffer.Decompress();
										FMemory::Memcpy(Block.AllocatedMemoryView.GetData(), reinterpret_cast<const uint8*>(DecompressedBuffer.GetData()) + Block.Offset, Size);
									}
									else
									{
										check(CompressedBuffer.TryDecompressTo(MakeMemoryView(Block.AllocatedMemoryView.GetData(), Size)));
									}
								}
							}
						}

						for (UE::Tasks::FTaskEvent& CompletionEvent : HeapMemory->CompletionEvents)
						{
							CompletionEvent.Trigger();
						}

					});

				return;
			}
#endif

			const bool bUseFBulkData = !ModelStreamableBulkData->StreamableBulkData.IsEmpty();
			const EAsyncIOPriorityAndFlags Priority = CVarMutableHighPriorityLoading.GetValueOnAnyThread() ? AIOP_High : AIOP_Normal;

			const int32 NumBlocks = BlockReadInfos.Num();
			for (int32 Index = 0; Index < NumBlocks; ++Index)
			{
				const FBlockReadInfo& Block = BlockReadInfos[Index];
				UE::Tasks::FTaskEvent& CompletionEvent = HeapMemory->CompletionEvents[Index];

				if (bUseFBulkData)
				{
					FBulkDataIORequestCallBack IOCallback =
						[CompletionEvent, FileId = Block.FileId](bool bWasCancelled, IBulkDataIORequest*) mutable
						{
							CompletionEvent.Trigger();
						};

					check(ModelStreamableBulkData->StreamableBulkData.IsValidIndex(Block.FileId));
					FByteBulkData& ByteBulkData = ModelStreamableBulkData->StreamableBulkData[Block.FileId];

					HeapMemory->BulkReadRequests.Add(TSharedPtr<IBulkDataIORequest>(ByteBulkData.CreateStreamingRequest(
						BulkDataFileOffset + Block.Offset,
						(int64)Block.AllocatedMemoryView.Num(),
						Priority,
						&IOCallback,
						Block.AllocatedMemoryView.GetData())));
				}
				else if (Block.FileHandle)
				{
					FAsyncFileCallBack ReadRequestCallBack =
						[CompletionEvent, FileId = Block.FileId](bool bWasCancelled, IAsyncReadRequest*) mutable
						{
							CompletionEvent.Trigger();
						};

					HeapMemory->ReadRequests.Add(TSharedPtr<IAsyncReadRequest>(Block.FileHandle->ReadRequest(
						BulkDataFileOffset + Block.Offset,
						(int64)Block.AllocatedMemoryView.Num(),
						Priority,
						&ReadRequestCallBack,
						Block.AllocatedMemoryView.GetData())));
				}
				else
				{
					ensure(false);
					CompletionEvent.Trigger();
				}
			}
		},
		UE::Tasks::ETaskPriority::High);
	
		
	return UE::Tasks::Launch(TEXT("GatherStreamingRequestsCompletionTask"),
        [
            OpenFileHandles = OpenFileHandles,
			HeapMemory = this->HeapMemory
        ]() mutable 
        {
			{
				FScopeLock Lock(&HeapMemory->ReadRequestLock);

				for (TSharedPtr<IAsyncReadRequest>& ReadRequest : HeapMemory->ReadRequests)
				{
					if (ReadRequest)
					{
						ReadRequest->WaitCompletion();
					}
				}

				for (TSharedPtr<IBulkDataIORequest>& BulkReadRequest : HeapMemory->BulkReadRequests)
				{
					if (BulkReadRequest)
					{
						BulkReadRequest->WaitCompletion();
					}
				}

				HeapMemory->BulkReadRequests.Empty();
				HeapMemory->ReadRequests.Empty();
			}
			
			OpenFileHandles.Empty();
        },
		HeapMemory->CompletionEvents,
        UE::Tasks::ETaskPriority::High);
}


void FMutableStreamRequest::Cancel()
{
	FScopeLock Lock(&HeapMemory->ReadRequestLock);

	if (!HeapMemory->bIsCancelled)
	{
		HeapMemory->bIsCancelled = true;

		for (TSharedPtr<IAsyncReadRequest>& ReadRequest : HeapMemory->ReadRequests)
		{
			if (ReadRequest)
			{
				ReadRequest->Cancel();
			}
		}

		for (TSharedPtr<IBulkDataIORequest>& BulkReadRequest : HeapMemory->BulkReadRequests)
		{
			if (BulkReadRequest)
			{
				BulkReadRequest->Cancel();
			}
		}
	}
}


FMutableStreamRequest::FBlockReadInfo::FBlockReadInfo(uint64 InOffset, IAsyncReadFileHandle* InFileHandle, TArrayView<uint8>& InAllocatedMemoryView,
	uint32 InFileId, MutablePrivate::EStreamableDataType InDataType, uint16 InResourceType, uint16 InResourceFlags)
	: Offset(InOffset)
	, FileHandle(InFileHandle)
	, AllocatedMemoryView(InAllocatedMemoryView)
	, FileId(InFileId)
	, ResourceType(InResourceType)
	, ResourceFlags(InResourceFlags)
	, DataType(InDataType)
{
}
