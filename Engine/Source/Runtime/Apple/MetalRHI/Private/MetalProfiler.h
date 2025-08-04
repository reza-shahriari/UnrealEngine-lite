// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MetalRHIPrivate.h"
#include "MetalCounterSampler.h"
#include "GPUProfiler.h"
#include "RHIBreadcrumbs.h"

DECLARE_DELEGATE_OneParam(FMetalCommandBufferCompletionHandler, MTL::CommandBuffer*);

// Stats
DECLARE_CYCLE_STAT_EXTERN(TEXT("MakeDrawable time"),STAT_MetalMakeDrawableTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Draw call time"),STAT_MetalDrawCallTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("PrepareDraw time"),STAT_MetalPrepareDrawTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SwitchToNone time"),STAT_MetalSwitchToNoneTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SwitchToRender time"),STAT_MetalSwitchToRenderTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SwitchToCompute time"),STAT_MetalSwitchToComputeTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SwitchToBlit time"),STAT_MetalSwitchToBlitTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("PrepareToRender time"),STAT_MetalPrepareToRenderTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("PrepareToDispatch time"),STAT_MetalPrepareToDispatchTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("CommitRenderResourceTables time"),STAT_MetalCommitRenderResourceTablesTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SetRenderState time"),STAT_MetalSetRenderStateTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("SetRenderPipelineState time"),STAT_MetalSetRenderPipelineStateTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("PipelineState time"),STAT_MetalPipelineStateTime,STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Buffer Page-Off time"), STAT_MetalBufferPageOffTime, STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Texture Page-Off time"), STAT_MetalTexturePageOffTime, STATGROUP_MetalRHI, );

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Uniform Memory Allocated Per-Frame"), STAT_MetalUniformMemAlloc, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Uniform Memory Freed Per-Frame"), STAT_MetalUniformMemFreed, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Vertex Memory Allocated Per-Frame"), STAT_MetalVertexMemAlloc, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Vertex Memory Freed Per-Frame"), STAT_MetalVertexMemFreed, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Index Memory Allocated Per-Frame"), STAT_MetalIndexMemAlloc, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Index Memory Freed Per-Frame"), STAT_MetalIndexMemFreed, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Texture Memory Updated Per-Frame"), STAT_MetalTextureMemUpdate, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Buffer Memory"), STAT_MetalBufferMemory, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Texture Memory"), STAT_MetalTextureMemory, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Heap Memory"), STAT_MetalHeapMemory, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Unused Buffer Memory"), STAT_MetalBufferUnusedMemory, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Unused Texture Memory"), STAT_MetalTextureUnusedMemory, STATGROUP_MetalRHI, );

DECLARE_MEMORY_STAT_EXTERN(TEXT("Uniform Memory In Flight"), STAT_MetalUniformMemoryInFlight, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Allocated Uniform Pool Memory"), STAT_MetalUniformAllocatedMemory, STATGROUP_MetalRHI, );
DECLARE_MEMORY_STAT_EXTERN(TEXT("Uniform Memory Per Frame"), STAT_MetalUniformBytesPerFrame, STATGROUP_MetalRHI, );

DECLARE_MEMORY_STAT_EXTERN(TEXT("Allocated Frame Temp Memory"), STAT_MetalTempAllocatorAllocatedMemory, STATGROUP_MetalRHI, );

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Buffer Count"), STAT_MetalBufferCount, STATGROUP_MetalRHI, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Texture Count"), STAT_MetalTextureCount, STATGROUP_MetalRHI, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Heap Count"), STAT_MetalHeapCount, STATGROUP_MetalRHI, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Fence Count"), STAT_MetalFenceCount, STATGROUP_MetalRHI, );

DECLARE_CYCLE_STAT_EXTERN(TEXT("Texture Page-On time"), STAT_MetalTexturePageOnTime, STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("GPU Work time"), STAT_MetalGPUWorkTime, STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("GPU Idle time"), STAT_MetalGPUIdleTime, STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Present time"), STAT_MetalPresentTime, STATGROUP_MetalRHI, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("CustomPresent time"), STAT_MetalCustomPresentTime, STATGROUP_MetalRHI, );
#if STATS
extern int64 volatile GMetalTexturePageOnTime;
extern int64 volatile GMetalGPUWorkTime;
extern int64 volatile GMetalGPUIdleTime;
extern int64 volatile GMetalPresentTime;
#endif

DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Number Command Buffers Created Per-Frame"), STAT_MetalCommandBufferCreatedPerFrame, STATGROUP_MetalRHI, );
DECLARE_DWORD_COUNTER_STAT_EXTERN(TEXT("Number Command Buffers Committed Per-Frame"), STAT_MetalCommandBufferCommittedPerFrame, STATGROUP_MetalRHI, );

class FMetalRHICommandContext;
class FMetalSyncPoint;

/**
 * Simple struct to hold sortable command buffer start and end timestamps.
 */
struct FMetalCommandBufferTiming
{
	CFTimeInterval StartTime;
	CFTimeInterval EndTime;

	bool operator<(const FMetalCommandBufferTiming& RHS) const
	{
		// Sort by start time and then by length if the commandbuffer started at the same time
		if (this->StartTime < RHS.StartTime)
		{
			return true;
		}
		else if ((this->StartTime == RHS.StartTime) && (this->EndTime > RHS.EndTime))
		{
			return true;
		}
		return false;
	}
};

#if RHI_NEW_GPU_PROFILER == 0

class FMetalCommandBufferTimer
{
public:
	void Submit()
	{
		FScopeLock Lock(&Mutex);
		Counter++;
	}
	
	void AddTiming(FMetalCommandBufferTiming Timing);
	void FrameEnd();
	void RecordFrame();
	
	const TArray<FMetalCommandBufferTiming>& GetTimings()
	{
		return Timings;
	}
	
	static void ResetFrameBufferTimings();
	static FMetalCommandBufferTimer& GetFrameBufferTimer();
	
	static void RecordPresent(MTL::CommandBuffer* CommandBuffer);
	// END WARNING
	
private:
	bool bFrameEnded;
	int32_t Counter = 0;
	TArray<FMetalCommandBufferTiming> Timings;
	FCriticalSection Mutex;
	
	static FMetalCommandBufferTimer* Timer;
};

/** A single perf event node, which tracks information about a appBeginDrawEvent/appEndDrawEvent range. */
class FMetalEventNode : public FGPUProfilerEventNode
{
public:
	
	FMetalEventNode(FMetalRHICommandContext& InContext, const TCHAR* InName, FGPUProfilerEventNode* InParent, bool bIsRoot, bool bInFullProfiling)
	: FGPUProfilerEventNode(InName, InParent)
	, StartTime(0)
	, EndTime(0)
	, Context(InContext)
	, bRoot(bIsRoot)
    , bFullProfiling(bInFullProfiling)
	{
	}
	
	virtual ~FMetalEventNode();
	
	/**
	 * Returns the time in ms that the GPU spent in this draw event.
	 * This blocks the CPU if necessary, so can cause hitching.
	 */
	virtual float GetTiming() override;
	virtual void StartTiming() override;
	virtual void StopTiming() override;
	
	FMetalSyncPoint* SyncPoint = nullptr;

	bool IsRoot() const { return bRoot; }	
	uint64 GetCycles() { return EndTime - StartTime; }
	
	uint64 StartTime;
	uint64 EndTime;
private:
	FMetalRHICommandContext& Context;
	bool bRoot;
    bool bFullProfiling;
};

/** An entire frame of perf event nodes, including ancillary timers. */
class FMetalEventNodeFrame : public FGPUProfilerEventNodeFrame
{
public:
	FMetalEventNodeFrame(FMetalRHICommandContext& InContext, bool bInFullProfiling)
	: RootNode(new FMetalEventNode(InContext, TEXT("Frame"), nullptr, true, bInFullProfiling))
    , bFullProfiling(bInFullProfiling)
	{
	}
	
	virtual ~FMetalEventNodeFrame()
	{
        delete RootNode;
		RootNode = nullptr;
	}
	
	/** Start this frame of per tracking */
	virtual void StartFrame() override;
	
	/** End this frame of per tracking, but do not block yet */
	virtual void EndFrame() override;
	
	/** Calculates root timing base frequency (if needed by this RHI) */
	virtual float GetRootTimingResults() override;
	
	virtual void LogDisjointQuery() override;
	
	FMetalEventNode* RootNode;
    bool bFullProfiling;
};

class FMetalContext;

// This class has multiple inheritance but really FGPUTiming is a static class
class FMetalGPUTiming : public FGPUTiming
{
public:
	
	/**
	 * Constructor.
	 */
	FMetalGPUTiming(FMetalRHICommandContext& Context)
	{
		StaticInitialize((void*)&Context, PlatformStaticInitialize);
	}
	
	void SetCalibrationTimestamp(uint64 GPU, uint64 CPU)
	{
		FGPUTiming::SetCalibrationTimestamp({ GPU, CPU });
	}
	
private:
	
	/**
	 * Initializes the static variables, if necessary.
	 */
	static void PlatformStaticInitialize(void* UserData);
};

struct IMetalStatsScope
{
	FString Name;
	FString Parent;
	TArray<IMetalStatsScope*> Children;
	
	uint64 CPUStartTime;
	uint64 CPUEndTime;
	
	uint64 GPUStartTime;
	uint64 GPUEndTime;
	
	uint64 CPUThreadIndex;
	uint64 GPUThreadIndex;
	
	virtual ~IMetalStatsScope();
	
	virtual void Start(MTL::CommandBuffer*& CommandBuffer) = 0;
	virtual void End(MTL::CommandBuffer*& CommandBuffer) = 0;
	
	FString GetJSONRepresentation(uint32 PID);
};

struct FMetalCPUStats : public IMetalStatsScope
{
	FMetalCPUStats(FString const& Name);
	virtual ~FMetalCPUStats();
	
	void Start(void);
	void End(void);
	
	virtual void Start(MTL::CommandBuffer*& CommandBuffer) final override;
	virtual void End(MTL::CommandBuffer*& CommandBuffer) final override;
};

struct FMetalDisplayStats : public IMetalStatsScope
{
	FMetalDisplayStats(uint32 DisplayID, double OutputSeconds, double Duration);
	virtual ~FMetalDisplayStats();
	
	virtual void Start(MTL::CommandBuffer*& CommandBuffer) final override;
	virtual void End(MTL::CommandBuffer*& CommandBuffer) final override;
};

enum EMTLFenceType
{
	EMTLFenceTypeWait,
	EMTLFenceTypeUpdate,
};

struct FMetalCommandBufferStats : public IMetalStatsScope
{
	FMetalCommandBufferStats(MTL::CommandBuffer* CommandBuffer, uint64 GPUThreadIndex);
	virtual ~FMetalCommandBufferStats();
	
	virtual void Start(MTL::CommandBuffer*& CommandBuffer) final override;
	virtual void End(MTL::CommandBuffer*& CommandBuffer) final override;

	MTL::CommandBuffer* CmdBuffer;
};

class FMetalRHICommandContext;
/**
 * Encapsulates GPU profiling logic and data.
 * There's only one global instance of this struct so it should only contain global data, nothing specific to a frame.
 */
struct FMetalGPUProfiler : public FGPUProfiler
{
	/** GPU hitch profile histories */
	TIndirectArray<FMetalEventNodeFrame> GPUHitchEventNodeFrames;
	
	FMetalGPUProfiler(FMetalRHICommandContext& InContext)
	:	FGPUProfiler()
	,	TimingSupport(InContext)
	,	Context(InContext)
	{}
	
	virtual ~FMetalGPUProfiler() {}
	
	virtual FGPUProfilerEventNode* CreateEventNode(const TCHAR* InName, FGPUProfilerEventNode* InParent) override;
	
	void Cleanup();
	
	virtual void PushEvent(const TCHAR* Name, FColor Color) override;
	virtual void PopEvent() override;
	
	void BeginFrame();
	void EndFrame();
	
	FMetalGPUTiming TimingSupport;
	FMetalRHICommandContext& Context;
};

class FMetalProfiler : public FMetalGPUProfiler
{
	static FMetalProfiler* Self;
public:
	FMetalProfiler(FMetalRHICommandContext& InContext);
	~FMetalProfiler();
	
	static FMetalProfiler* CreateProfiler(FMetalRHICommandContext& InContext);
	static FMetalProfiler* GetProfiler();
	static void DestroyProfiler();
	
	void BeginCapture(int InNumFramesToCapture = -1);
	void EndCapture();
	bool TracingEnabled() const;
	
	void BeginFrame();
	void EndFrame();
	
	void AddDisplayVBlank(uint32 DisplayID, double OutputSeconds, double OutputDuration);
	
	void EncodeDraw(FMetalCommandBufferStats* CmdBufStats, char const* DrawCall, uint32 RHIPrimitives, uint32 RHIVertices, uint32 RHIInstances);
	void EncodeBlit(FMetalCommandBufferStats* CmdBufStats, char const* DrawCall);
	void EncodeBlit(FMetalCommandBufferStats* CmdBufStats, FString DrawCall);
	void EncodeDispatch(FMetalCommandBufferStats* CmdBufStats, char const* DrawCall);
	
	FMetalCPUStats* AddCPUStat(FString const& Name);
	FMetalCommandBufferStats* AllocateCommandBuffer(MTL::CommandBuffer* CommandBuffer, uint64 GPUThreadIndex);
	void AddCommandBuffer(FMetalCommandBufferStats* CommandBuffer);
	virtual void PushEvent(const TCHAR* Name, FColor Color) final override;
	virtual void PopEvent() final override;
	
	void SaveTrace();
	
private:
	FCriticalSection Mutex;
	
	TArray<FMetalCommandBufferStats*> TracedBuffers;
	TArray<FMetalDisplayStats*> DisplayStats;
	TArray<FMetalCPUStats*> CPUStats;
	
	int32 NumFramesToCapture;
	int32 CaptureFrameNumber;
	
	bool bRequestStartCapture;
	bool bRequestStopCapture;
	bool bEnabled;
};

struct FScopedMetalCPUStats
{
	FScopedMetalCPUStats(FString const& Name)
	: Stats(nullptr)
	{
		FMetalProfiler* Profiler = FMetalProfiler::GetProfiler();
		if (Profiler)
		{
			Stats = Profiler->AddCPUStat(Name);
			if (Stats)
			{
				Stats->Start();
			}
		}
	}
	
	~FScopedMetalCPUStats()
	{
		if (Stats)
		{
			Stats->End();
		}
	}
	
	FMetalCPUStats* Stats;
};

#endif

#if RHI_NEW_GPU_PROFILER && WITH_RHI_BREADCRUMBS

enum class EMetalBreadcrumbTrackerType : uint8
{
	Begin,
	End,
	Encode,
};

class FMetalCommandBuffer;
struct FMetalBreadcrumbTrackerObject
{
	EMetalBreadcrumbTrackerType Type;
	FMetalCommandBuffer* CmdBuffer;
	FMetalCounterSamplePtr CounterSample;
	FRHIBreadcrumbNode* Node;
};

// Represents the data required for FRHIBreadcrumbNode
// Samples can be collected across multiple counter samples and this allows us to merge the results
struct FMetalBreadcrumbEvent
{
	FMetalBreadcrumbEvent(bool bRenderPass) : bWithinRenderPass(bRenderPass)
	{}
	
	bool bWithinRenderPass;
	uint64_t* TimestampTOP = nullptr;
	uint64_t* TimestampBOP = nullptr;
	TArray<FMetalCounterSamplePtr> Samples;
};

// Tracks the FRHIBreadcrumbNode's across encoders
class FMetalBreadcrumbProfiler
{
public:
	inline static FMetalBreadcrumbProfiler* GetInstance()
	{
		if(!Instance)
		{
			Instance = new FMetalBreadcrumbProfiler; 
		}
		return Instance;
	}
	
	~FMetalBreadcrumbProfiler()
	{
	}

	inline FMetalBreadcrumbEvent& GetBreadcrumbEvent(FRHIBreadcrumbNode* Breadcrumb, bool bWithinRenderPass)
	{
		FScopeLock Lock(&Mutex);
		FMetalBreadcrumbEvent& Event = CreatedBreadcrumbs.FindOrAdd(Breadcrumb, FMetalBreadcrumbEvent(bWithinRenderPass));
		Event.bWithinRenderPass |= bWithinRenderPass;
		
		return Event;
	}
	
	inline void AddSample(FMetalCounterSamplePtr Sample)
	{
		FScopeLock Lock(&Mutex);
		
		uint64_t StartTime, EndTime;
		Sample->ResolveStageCounters(StartTime, EndTime);
		
		for(FRHIBreadcrumbNode* ActiveBreadcrumb : ActiveBreadcrumbs)
		{
			CreatedBreadcrumbs[ActiveBreadcrumb].Samples.Add(Sample);
		}
	}
	
	inline void OnBreadcrumbBegin(FRHIBreadcrumbNode* Node)
	{
		FScopeLock Lock(&Mutex);
		ActiveBreadcrumbs.Add(Node);
	}
	
	inline void OnBreadcrumbEnd(FRHIBreadcrumbNode* Node)
	{
		FScopeLock Lock(&Mutex);
		
		FMetalBreadcrumbEvent& Event = CreatedBreadcrumbs[Node];
	
		uint64_t* Start = Event.TimestampTOP;
		uint64_t* End = Event.TimestampBOP;
		
		// Ignore samples whe
		if(Event.bWithinRenderPass || Event.Samples.Num() == 0)
		{
			*Start = 0; 
			*End = 0;
		}
		else
		{
			for(FMetalCounterSamplePtr Sample : Event.Samples)
			{
				uint64_t StartTime, EndTime;
				Sample->ResolveStageCounters(StartTime, EndTime);
				
				*Start = *Start > 0 ? FMath::Min(StartTime, *Start) : StartTime;
				*End = *End > 0 ? FMath::Max(EndTime, *End) : EndTime;
			}
		}
		
		ActiveBreadcrumbs.Remove(Node);
		CreatedBreadcrumbs.Remove(Node);
	}
	
	// Resolves all the breakcrumbs added to the command buffer
	void ResolveBreadcrumbTrackerStream(TArray<FMetalBreadcrumbTrackerObject>& BreadcrumbTrackerStream);
	
private:
	TMap<FRHIBreadcrumbNode*, FMetalBreadcrumbEvent> CreatedBreadcrumbs;
	TArray<FRHIBreadcrumbNode*> ActiveBreadcrumbs; 
	
	static FMetalBreadcrumbProfiler* Instance;
	FCriticalSection Mutex;
};

#endif
