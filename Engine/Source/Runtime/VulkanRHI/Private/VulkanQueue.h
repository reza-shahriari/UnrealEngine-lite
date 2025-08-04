// Copyright Epic Games, Inc. All Rights Reserved..

/*=============================================================================
	VulkanQueue.h: Private Vulkan RHI definitions.
=============================================================================*/

#pragma once

#include "VulkanConfiguration.h"
#include "VulkanBarriers.h"
#include "VulkanCommandBuffer.h"

class FVulkanDynamicRHI;
class FVulkanDevice;
class FVulkanPayload;
class FVulkanTiming;

namespace VulkanRHI
{
	class FFence;
	class FSemaphore;
}

enum class EVulkanQueueType : uint8
{
	Graphics = 0,
	AsyncCompute,
	Transfer,

	Count,
};

struct FBinarySemaphoreSignalInfo
{
	uint64 TimelineValue;
	EVulkanQueueType QueueType;
};

class FVulkanQueue
{
public:
	FVulkanQueue(FVulkanDevice& InDevice, uint32 InFamilyIndex, EVulkanQueueType InQueueType);
	~FVulkanQueue();

	EVulkanQueueType GetQueueType()
	{
		return QueueType;
	}

	uint32 GetFamilyIndex() const
	{
		return FamilyIndex;
	}

	uint32 GetQueueIndex() const
	{
		return QueueIndex;
	}

	VkQueue GetHandle() const
	{
		return Queue;
	}

	FVulkanCommandBufferPool* AcquireCommandBufferPool(EVulkanCommandBufferType CommandBufferType);
	void ReleaseCommandBufferPool(FVulkanCommandBufferPool* CommandBufferPool);

	VkPipelineStageFlags GetSupportedStageBits() const
	{
		return SupportedStages;
	}

	VkAccessFlags GetSupportedAccessFlags() const
	{
		return SupportedAccess;
	}

	const VulkanRHI::FSemaphore* GetTimelineSemaphore() const
	{
		return TimelineSempahore;
	}

	uint64 GetLastSubmittedTimelineSemaphoreValue() const
	{
		return NextTimelineSemaphoreValue - 1;
	}

	uint64 GetCompletedTimelineSemaphoreValue() const
	{
		return CompletedTimelineSemaphoreValue;
	}

	void EnqueuePayload(FVulkanPayload* InPayload)
	{
		PendingSubmission.Enqueue(InPayload);
	}

	int32 SubmitQueuedPayloads(TMap<VkSemaphore, FBinarySemaphoreSignalInfo>& SignaledSemas);
	int32 ProcessInterruptQueue(uint64 Timeout);

#if RHI_NEW_GPU_PROFILER
	UE::RHI::GPUProfiler::FQueue GetProfilerQueue() const;
#endif
	
private:
	void SubmitPayloads(TArrayView<FVulkanPayload*> Payloads, TMap<VkSemaphore, FBinarySemaphoreSignalInfo>& InOutSignaledSemas);
	void Submit(TArrayView<VkSubmitInfo> InSubmitInfos, VulkanRHI::FFence* Fence);

	// Used by submission pipe which holds the proper locks to access this queue
	FVulkanPayload* GetNextInterruptPayload()
	{
		FVulkanPayload* Payload = nullptr;
		PendingInterrupt.Peek(Payload);
		return Payload;
	}
	friend FVulkanDynamicRHI;

	VkQueue Queue;
	uint32 FamilyIndex;
	uint32 QueueIndex;
	EVulkanQueueType QueueType;
	FVulkanDevice& Device;

	FCriticalSection CommandBufferPoolCS;
	TStaticArray<TArray<FVulkanCommandBufferPool*>, (int32)EVulkanCommandBufferType::Count> CommandBufferPools;

	const bool bUseTimelineSemaphores;
	VulkanRHI::FSemaphore* TimelineSempahore = nullptr;
	uint64 NextTimelineSemaphoreValue = 1;
	uint64 CompletedTimelineSemaphoreValue = 0;

	uint64 SubmitCounter;
	VkPipelineStageFlags SupportedStages = VK_PIPELINE_STAGE_NONE;
	VkAccessFlags SupportedAccess = VK_ACCESS_NONE;

	TQueue<FVulkanPayload*, EQueueMode::Mpsc> PendingSubmission;
	TQueue<FVulkanPayload*, EQueueMode::Spsc> PendingInterrupt;

#if RHI_NEW_GPU_PROFILER
	// The active timing struct on this queue. Updated / accessed by the interrupt thread.
	FVulkanTiming* Timing = nullptr;
#endif

	void FillSupportedStageBits();
};
