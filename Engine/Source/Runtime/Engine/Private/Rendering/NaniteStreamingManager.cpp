// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/NaniteStreamingManager.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "GlobalShader.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "Async/ParallelFor.h"
#include "RenderUtils.h"
#include "Rendering/NaniteResources.h"
#include "ShaderCompilerCore.h"
#include "Stats/StatsTrace.h"
#include "RHIGPUReadback.h"
#include "HAL/PlatformFileManager.h"
#include "ShaderPermutationUtils.h"
#include "ProfilingDebugging/IoStoreTrace.h"

#if WITH_EDITOR
#include "DerivedDataCache.h"
#include "DerivedDataRequestOwner.h"
using namespace UE::DerivedData;
#endif

#define MAX_LEGACY_REQUESTS_PER_UPDATE		32u		// Legacy IO requests are slow and cause lots of bubbles, so we NEED to limit them.

#define MAX_RUNTIME_RESOURCE_VERSIONS_BITS	8								// Just needs to be large enough to cover maximum number of in-flight versions
#define MAX_RUNTIME_RESOURCE_VERSIONS_MASK	((1 << MAX_RUNTIME_RESOURCE_VERSIONS_BITS) - 1)	

#define MAX_RESOURCE_PREFETCH_PAGES			16

#define LRU_INDEX_MASK						0x7FFFFFFFu
#define LRU_FLAG_REFERENCED_THIS_UPDATE		0x80000000u

#define DEBUG_TRANSCODE_PAGES_REPEATEDLY	0
#define DEBUG_ALLOCATION_STRESS_TEST		0

static int32 GNaniteStreamingAsync = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingAsync(
	TEXT("r.Nanite.Streaming.Async"),
	GNaniteStreamingAsync,
	TEXT("Perform most of the Nanite streaming on an asynchronous worker thread instead of the rendering thread."),
	ECVF_RenderThreadSafe
);

static float GNaniteStreamingBandwidthLimit = -1.0f;
static FAutoConsoleVariableRef CVarNaniteStreamingBandwidthLimit(
	TEXT("r.Nanite.Streaming.BandwidthLimit" ),
	GNaniteStreamingBandwidthLimit,
	TEXT("Streaming bandwidth limit in megabytes per second. Negatives values are interpreted as unlimited. "),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingPoolSize = 512;
static FAutoConsoleVariableRef CVarNaniteStreamingPoolSize(
	TEXT("r.Nanite.Streaming.StreamingPoolSize"),
	GNaniteStreamingPoolSize,
	TEXT("Size of streaming pool in MB. Does not include memory used for root pages.")
	TEXT("Be careful with setting this close to the GPU resource size limit (typically 2-4GB) as root pages are allocated from the same physical buffer."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingNumInitialRootPages = 2048;
static FAutoConsoleVariableRef CVarNaniteStreamingNumInitialRootPages(
	TEXT("r.Nanite.Streaming.NumInitialRootPages"),
	GNaniteStreamingNumInitialRootPages,
	TEXT("Number of root pages in initial allocation. Allowed to grow on demand if r.Nanite.Streaming.DynamicallyGrowAllocations is enabled."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingNumInitialImposters = 2048;
static FAutoConsoleVariableRef CVarNaniteStreamingNumInitialImposters(
	TEXT("r.Nanite.Streaming.NumInitialImposters"),
	GNaniteStreamingNumInitialImposters,
	TEXT("Number of imposters in initial allocation. Allowed to grow on demand if r.Nanite.Streaming.DynamicallyGrowAllocations is enabled."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingDynamicallyGrowAllocations = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingDynamicallyGrowAllocations(
	TEXT("r.Nanite.Streaming.DynamicallyGrowAllocations"),
	GNaniteStreamingDynamicallyGrowAllocations,
	TEXT("Determines if root page and imposter allocations are allowed to grow dynamically from initial allocation set by r.Nanite.Streaming.NumInitialRootPages and r.Nanite.Streaming.NumInitialImposters"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingMaxPendingPages = 128;
static FAutoConsoleVariableRef CVarNaniteStreamingMaxPendingPages(
	TEXT("r.Nanite.Streaming.MaxPendingPages"),
	GNaniteStreamingMaxPendingPages,
	TEXT("Maximum number of pages that can be pending for installation."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingImposters = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingImposters(
	TEXT("r.Nanite.Streaming.Imposters"),
	GNaniteStreamingImposters,
	TEXT("Load imposters used for faster rendering of distant objects. Requires additional memory and might not be worthwhile for scenes with HLOD or no distant objects."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingMaxPageInstallsPerFrame = 128;
static FAutoConsoleVariableRef CVarNaniteStreamingMaxPageInstallsPerFrame(
	TEXT("r.Nanite.Streaming.MaxPageInstallsPerFrame"),
	GNaniteStreamingMaxPageInstallsPerFrame,
	TEXT("Maximum number of pages that can be installed per frame. Limiting this can limit the overhead of streaming."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingGPURequestsBufferMinSize = 64 * 1024;
static FAutoConsoleVariableRef CVarNaniteStreamingGPURequestsBufferMinSize(
	TEXT("r.Nanite.Streaming.GPURequestsBufferMinSize"),
	GNaniteStreamingGPURequestsBufferMinSize,
	TEXT("The minimum number of elements in the buffer used for GPU feedback.")
	TEXT("Setting Min=Max disables any dynamic buffer size adjustment."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingGPURequestsBufferMaxSize = 1024 * 1024;
static FAutoConsoleVariableRef CVarNaniteStreamingGPURequestsBufferMaxSize(
	TEXT("r.Nanite.Streaming.GPURequestsBufferMaxSize"),
	GNaniteStreamingGPURequestsBufferMaxSize,
	TEXT("The maximum number of elements in the buffer used for GPU feedback.")
	TEXT("Setting Min=Max disables any dynamic buffer size adjustment."),
	ECVF_RenderThreadSafe
);

// Controls for dynamically adjusting quality (pixels per edge) when the streaming pool is being overcommitted.
// This should be a rare condition in practice, but can happen when rendering scenes with lots of unique geometry at high resolutions.
static float GNaniteStreamingQualityScaleMinPoolPercentage = 70.0f;
static FAutoConsoleVariableRef CVarNaniteStreamingQualityScaleMinPoolPercentage(
	TEXT("r.Nanite.Streaming.QualityScale.MinPoolPercentage"),
	GNaniteStreamingQualityScaleMinPoolPercentage,
	TEXT("Adjust quality up whenever the streaming pool load percentage goes below this threshold."),
	ECVF_RenderThreadSafe
);

static float GNaniteStreamingQualityScaleMaxPoolPercentage = 85.0f;
static FAutoConsoleVariableRef CVarNaniteStreamingQualityScaleMaxPoolPercentage(
	TEXT("r.Nanite.Streaming.QualityScale.MaxPoolPercentage"),
	GNaniteStreamingQualityScaleMaxPoolPercentage,
	TEXT("Adjust quality down whenever the streaming pool load percentage goes above this threshold."),
	ECVF_RenderThreadSafe
);

static float GNaniteStreamingQualityScaleMinQuality = 0.3f;
static FAutoConsoleVariableRef CVarNaniteStreamingQualityScaleMinQuality(
	TEXT("r.Nanite.Streaming.QualityScale.MinQuality"),
	GNaniteStreamingQualityScaleMinQuality,
	TEXT("Quality scaling will never go below this limit. 1.0 disables any scaling."),
	ECVF_RenderThreadSafe
);


static int32 GNaniteStreamingAsyncCompute = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingAsyncCompute(
	TEXT("r.Nanite.Streaming.AsyncCompute"),
	GNaniteStreamingAsyncCompute,
	TEXT("Schedule GPU work in async compute queue."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingExplicitRequests = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingExplicitRequests(
	TEXT("r.Nanite.Streaming.Debug.ExplicitRequests"),
	GNaniteStreamingExplicitRequests,
	TEXT("Process requests coming from explicit calls to RequestNanitePages()."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingGPURequests = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingGPUFeedback(
	TEXT("r.Nanite.Streaming.Debug.GPURequests"),
	GNaniteStreamingGPURequests,
	TEXT("Process requests coming from GPU rendering feedback"),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingPrefetch = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingPrefetch(
	TEXT("r.Nanite.Streaming.Debug.Prefetch"),
	GNaniteStreamingPrefetch,
	TEXT("Process resource prefetch requests from calls to PrefetchResource()."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingPoolResize = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingPoolResize(
	TEXT("r.Nanite.Streaming.Debug.StreamingPoolResize"),
	GNaniteStreamingPoolResize,
	TEXT("Allow streaming pool to be resized at runtime."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingReservedResourceIgnoreInitialRootAllocation = 1;
static FAutoConsoleVariableRef CVarNaniteStreamingReservedResourceIgnoreInitialRootAllocation(
	TEXT("r.Nanite.Streaming.Debug.ReservedResourceIgnoreInitialRootAllocation"),
	GNaniteStreamingReservedResourceIgnoreInitialRootAllocation,
	TEXT("Ignore root page initial allocation size for reserved resources."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static int32 GNaniteStreamingReservedResourceRootPageGrowOnly = 0;
static FAutoConsoleVariableRef CVarNaniteStreamingReservedResourceGrowOnly(
	TEXT("r.Nanite.Streaming.Debug.ReservedResourceRootPageGrowOnly"),
	GNaniteStreamingReservedResourceRootPageGrowOnly,
	TEXT("Root page allocator only grows."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static TAutoConsoleVariable<int32> CVarNaniteStreamingTranscodeWaveSize(
	TEXT("r.Nanite.Streaming.TranscodeWaveSize"), 0,
	TEXT("Overrides the wave size to use for transcoding.\n")
	TEXT(" 0: Automatic (default);\n")
	TEXT(" 4: Wave size 4;\n")
	TEXT(" 8: Wave size 8;\n")
	TEXT(" 16: Wave size 16;\n")
	TEXT(" 32: Wave size 32;\n")
	TEXT(" 64: Wave size 64;\n")
	TEXT(" 128: Wave size 128;\n"),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingDynamicPageUploadBuffer = 0;
static FAutoConsoleVariableRef CVarNaniteStreamingDynamicPageUploadBuffer(
	TEXT("r.Nanite.Streaming.DynamicPageUploadBuffer"),
	GNaniteStreamingDynamicPageUploadBuffer,
	TEXT("Set Dynamic flag on the page upload buffer. This can eliminate a buffer copy on some platforms, but potentially also make the transcode shader slower."),
	ECVF_RenderThreadSafe
);

static int32 GNaniteStreamingReservedResources = 0;
static FAutoConsoleVariableRef CVarNaniteStreamingReservedResources(
	TEXT("r.Nanite.Streaming.ReservedResources"),
	GNaniteStreamingReservedResources,
	TEXT("Allow allocating Nanite GPU resources as reserved resources for better memory utilization and more efficient resizing (EXPERIMENTAL)"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe
);

static bool bPendingPoolReset = false;
static FAutoConsoleCommand CVarYourCommand(
	TEXT("r.Nanite.Streaming.ResetStreamingPool"),
	TEXT("Resets the Nanite streaming pool on next update."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args) {
		bPendingPoolReset = true;
	}),
	ECVF_Default
);

static_assert(NANITE_MAX_GPU_PAGES_BITS + MAX_RUNTIME_RESOURCE_VERSIONS_BITS + NANITE_STREAMING_REQUEST_MAGIC_BITS <= 32,		"Streaming request member RuntimeResourceID_Magic doesn't fit in 32 bits");
static_assert(NANITE_MAX_RESOURCE_PAGES_BITS + NANITE_MAX_GROUP_PARTS_BITS + NANITE_STREAMING_REQUEST_MAGIC_BITS <= 32,			"Streaming request member PageIndex_NumPages_Magic doesn't fit in 32 bits");

DECLARE_STATS_GROUP_SORTBYNAME(	TEXT("NaniteStreaming"),					STATGROUP_NaniteStreaming,								STATCAT_Advanced);

DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("Nanite Resources"),					STAT_NaniteStreaming00_NaniteResources,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("Imposters"),							STAT_NaniteStreaming01_Imposters,						STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("HierarchyNodes"),						STAT_NaniteStreaming02_HierarchyNodes,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("    Peak"),							STAT_NaniteStreaming03_PeakHierarchyNodes,				STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("    Max Depth"),						STAT_NaniteStreaming04_MaxHierarchyLevels,				STATGROUP_NaniteStreaming);

DECLARE_DWORD_ACCUMULATOR_STAT( TEXT("Root Pages"),							STAT_NaniteStreaming06_RootPages,						STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("    Peak"),							STAT_NaniteStreaming07_PeakRootPages,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("    Allocated"),						STAT_NaniteStreaming08_AllocatedRootPages,				STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("    Limit"),							STAT_NaniteStreaming09_RootPageLimit,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("Streaming Pool Pages"),				STAT_NaniteStreaming0A_StreamingPoolPages,				STATGROUP_NaniteStreaming);
DECLARE_DWORD_ACCUMULATOR_STAT(	TEXT("Total Streaming Pages"),				STAT_NaniteStreaming0B_TotalStreamingPages,				STATGROUP_NaniteStreaming);

DECLARE_FLOAT_ACCUMULATOR_STAT( TEXT("Imposter Size (MB)"),					STAT_NaniteStreaming10_ImpostersSizeMB,					STATGROUP_NaniteStreaming);
DECLARE_FLOAT_ACCUMULATOR_STAT(	TEXT("Hierarchy Size (MB)"),				STAT_NaniteStreaming11_HiearchySizeMB,					STATGROUP_NaniteStreaming);

DECLARE_FLOAT_ACCUMULATOR_STAT(	TEXT("Total Pool Size (MB)"),				STAT_NaniteStreaming12_TotalPoolSizeMB,					STATGROUP_NaniteStreaming);
DECLARE_FLOAT_ACCUMULATOR_STAT(	TEXT("    Root Pool Size (MB)"),			STAT_NaniteStreaming13_AllocatedRootPagesSizeMB,		STATGROUP_NaniteStreaming);
DECLARE_FLOAT_ACCUMULATOR_STAT(	TEXT("    Streaming Pool Size (MB)"),		STAT_NaniteStreaming14_StreamingPoolSizeMB,				STATGROUP_NaniteStreaming);
DECLARE_FLOAT_ACCUMULATOR_STAT(	TEXT("Total Pool Size Limit (MB)"),			STAT_NaniteStreaming15_TotalPoolSizeLimitMB,			STATGROUP_NaniteStreaming);

DECLARE_DWORD_COUNTER_STAT(		TEXT("Page Requests"),						STAT_NaniteStreaming20_PageRequests,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    GPU"),							STAT_NaniteStreaming21_PageRequestsGPU,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    Explicit"),						STAT_NaniteStreaming22_PageRequestsExplicit,			STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    Prefetch"),						STAT_NaniteStreaming23_PageRequestsPrefetch,			STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    Parents"),						STAT_NaniteStreaming24_PageRequestsParents,				STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    Unique"),							STAT_NaniteStreaming25_PageRequestsUnique,				STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    Registered"),						STAT_NaniteStreaming26_PageRequestsRegistered,			STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("    New"),							STAT_NaniteStreaming27_PageRequestsNew,					STATGROUP_NaniteStreaming);

DECLARE_FLOAT_COUNTER_STAT(		TEXT("Visible Streaming Data Size (MB)"),	STAT_NaniteStreaming30_VisibleStreamingDataSizeMB,		STATGROUP_NaniteStreaming);
DECLARE_FLOAT_COUNTER_STAT(		TEXT("    Streaming Pool Percentage"),		STAT_NaniteStreaming31_VisibleStreamingPoolPercentage,	STATGROUP_NaniteStreaming);
DECLARE_FLOAT_COUNTER_STAT(		TEXT("    Quality Scale"),					STAT_NaniteStreaming32_VisibleStreamingQualityScale,	STATGROUP_NaniteStreaming);


DECLARE_FLOAT_COUNTER_STAT(		TEXT("IO Request Size (MB)"),				STAT_NaniteStreaming40_IORequestSizeMB,					STATGROUP_NaniteStreaming);

DECLARE_DWORD_COUNTER_STAT(		TEXT("Readback Size"),						STAT_NaniteStreaming41_ReadbackSize,					STATGROUP_NaniteStreaming);
DECLARE_DWORD_COUNTER_STAT(		TEXT("Readback Buffer Size"),				STAT_NaniteStreaming42_ReadbackBufferSize,				STATGROUP_NaniteStreaming);


DECLARE_CYCLE_STAT(				TEXT("BeginAsyncUpdate"),					STAT_NaniteStreaming_BeginAsyncUpdate,					STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("AsyncUpdate"),						STAT_NaniteStreaming_AsyncUpdate,						STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("ProcessRequests"),					STAT_NaniteStreaming_ProcessRequests,					STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("InstallReadyPages"),					STAT_NaniteStreaming_InstallReadyPages,					STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("UploadTask"),							STAT_NaniteStreaming_UploadTask,						STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("ApplyFixup"),							STAT_NaniteStreaming_ApplyFixup,						STATGROUP_NaniteStreaming);

DECLARE_CYCLE_STAT(				TEXT("EndAsyncUpdate"),						STAT_NaniteStreaming_EndAsyncUpdate,					STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentRequests"),					STAT_NaniteStreaming_AddParentRequests,					STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentRegisteredRequests"),		STAT_NaniteStreaming_AddParentRegisteredRequests,		STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("AddParentNewRequests"),				STAT_NaniteStreaming_AddParentNewRequests,				STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("ClearReferencedArray"),				STAT_NaniteStreaming_ClearReferencedArray,				STATGROUP_NaniteStreaming);

DECLARE_CYCLE_STAT(				TEXT("CompactLRU"),							STAT_NaniteStreaming_CompactLRU,						STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("UpdateLRU"),							STAT_NaniteStreaming_UpdateLRU,							STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("ProcessGPURequests"),					STAT_NaniteStreaming_ProcessGPURequests,				STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("SelectHighestPriority"),				STAT_NaniteStreaming_SelectHighestPriority,				STATGROUP_NaniteStreaming);

DECLARE_CYCLE_STAT(				TEXT("Heapify"),							STAT_NaniteStreaming_Heapify,							STATGROUP_NaniteStreaming);
DECLARE_CYCLE_STAT(				TEXT("VerifyLRU"),							STAT_NaniteStreaming_VerifyLRU,							STATGROUP_NaniteStreaming);


DECLARE_LOG_CATEGORY_EXTERN(LogNaniteStreaming, Log, All);
DEFINE_LOG_CATEGORY(LogNaniteStreaming);

CSV_DEFINE_CATEGORY(NaniteStreaming, true);
CSV_DEFINE_CATEGORY(NaniteStreamingDetail, false);

namespace Nanite
{
#if WITH_EDITOR
	const FValueId NaniteValueId = FValueId::FromName("NaniteStreamingData");
#endif

// Round up to smallest value greater than or equal to x of the form k*2^s where k < 2^NumSignificantBits.
// This is the same as RoundUpToPowerOfTwo when NumSignificantBits=1.
// For larger values of NumSignificantBits each po2 bucket is subdivided into 2^(NumSignificantBits-1) linear steps.
// This gives more steps while still maintaining an overall exponential structure and keeps numbers nice and round (in the po2 sense).

// Example:
// Representable values for different values of NumSignificantBits.
// 1: ..., 16, 32, 64, 128, 256, 512, ...
// 2: ..., 16, 24, 32,  48,  64,  96, ...
// 3: ..., 16, 20, 24,  28,  32,  40, ...
static uint32 RoundUpToSignificantBits(uint32 x, uint32 NumSignificantBits)
{
	check(NumSignificantBits <= 32);

	const int32_t Shift = FMath::Max((int32)FMath::CeilLogTwo(x) - (int32)NumSignificantBits, 0);
	const uint32 Mask = (1u << Shift) - 1u;
	return (x + Mask) & ~Mask;
}

static uint32 GetMaxPagePoolSizeInMB()
{
	if (IsRHIDeviceAMD())
	{
		return 4095;
	}
	else
	{
		return 2048;
	}
}

class FTranscodePageToGPU_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTranscodePageToGPU_CS);
	SHADER_USE_PARAMETER_STRUCT(FTranscodePageToGPU_CS, FGlobalShader);

	class FTranscodePassDim : SHADER_PERMUTATION_SPARSE_INT("NANITE_TRANSCODE_PASS", NANITE_TRANSCODE_PASS_INDEPENDENT, NANITE_TRANSCODE_PASS_PARENT_DEPENDENT);
	class FGroupSizeDim : SHADER_PERMUTATION_SPARSE_INT("GROUP_SIZE", 4, 8, 16, 32, 64, 128);
	using FPermutationDomain = TShaderPermutationDomain<FTranscodePassDim, FGroupSizeDim>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32,													StartClusterIndex)
		SHADER_PARAMETER(uint32,													NumClusters)
		SHADER_PARAMETER(uint32,													ZeroUniform)
		SHADER_PARAMETER(FIntVector4,												PageConstants)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedClusterInstallInfo>,ClusterInstallInfoBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>,						PageDependenciesBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(ByteAddressBuffer,							SrcPageBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer,						DstPageBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (!UE::ShaderPermutationUtils::ShouldCompileWithWaveSize(Parameters, PermutationVector.Get<FGroupSizeDim>()))
		{
			return false;
		}
		
		return DoesPlatformSupportNanite(Parameters.Platform);
	}

	static EShaderPermutationPrecacheRequest ShouldPrecachePermutation(const FShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (!UE::ShaderPermutationUtils::ShouldPrecacheWithWaveSize(Parameters, PermutationVector.Get<FGroupSizeDim>()))
		{
			return EShaderPermutationPrecacheRequest::NotUsed;
		}

		return FGlobalShader::ShouldPrecachePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);

		OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		OutEnvironment.CompilerFlags.Add(CFLAG_HLSL2021);
		OutEnvironment.CompilerFlags.Add(CFLAG_WarningsAsErrors);
	}
};
IMPLEMENT_GLOBAL_SHADER(FTranscodePageToGPU_CS, "/Engine/Private/Nanite/NaniteTranscode.usf", "TranscodePageToGPU", SF_Compute);

class FClearStreamingRequestCount_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearStreamingRequestCount_CS);
	SHADER_USE_PARAMETER_STRUCT(FClearStreamingRequestCount_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FStreamingRequest>, OutStreamingRequests)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}
};
IMPLEMENT_GLOBAL_SHADER(FClearStreamingRequestCount_CS, "/Engine/Private/Nanite/NaniteStreaming.usf", "ClearStreamingRequestCount", SF_Compute);

class FUpdateClusterLeafFlags_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FUpdateClusterLeafFlags_CS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateClusterLeafFlags_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, NumClusterUpdates)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, PackedClusterUpdates)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, ClusterPageBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}
};
IMPLEMENT_GLOBAL_SHADER(FUpdateClusterLeafFlags_CS, "/Engine/Private/Nanite/NaniteStreaming.usf", "UpdateClusterLeafFlags", SF_Compute);

class FMemcpy_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMemcpy_CS);
	SHADER_USE_PARAMETER_STRUCT(FMemcpy_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, SrcOffset)
		SHADER_PARAMETER(uint32, DstOffset)
		SHADER_PARAMETER(uint32, NumThreads)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, ClusterPageBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}
};
IMPLEMENT_GLOBAL_SHADER(FMemcpy_CS, "/Engine/Private/Nanite/NaniteStreaming.usf", "Memcpy", SF_Compute);

class FRelocateHierarchy_CS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FRelocateHierarchy_CS);
	SHADER_USE_PARAMETER_STRUCT(FRelocateHierarchy_CS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, OldRootPageStart)
		SHADER_PARAMETER(uint32, NewRootPageStart)
		SHADER_PARAMETER(uint32, NumRelocations)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FUintVector3>, RelocationsBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWByteAddressBuffer, HierarchyBufferUAV)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportNanite(Parameters.Platform);
	}
};
IMPLEMENT_GLOBAL_SHADER(FRelocateHierarchy_CS, "/Engine/Private/Nanite/NaniteStreaming.usf", "RelocateHierarchy", SF_Compute);


static void AddPass_ClearStreamingRequestCount(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef BufferUAVRef)
{
	// Need to always clear streaming requests on all GPUs.  We sometimes write to streaming request buffers on a mix of
	// GPU masks (shadow rendering on all GPUs, other passes on a single GPU), and we need to make sure all are clear
	// when they get used again.
	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

	FClearStreamingRequestCount_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearStreamingRequestCount_CS::FParameters>();
	PassParameters->OutStreamingRequests = BufferUAVRef;

	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FClearStreamingRequestCount_CS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("ClearStreamingRequestCount"),
		ComputeShader,
		PassParameters,
		FIntVector(1, 1, 1)
	);
}

static void AddPass_UpdateClusterLeafFlags(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef ClusterPageBufferUAV, const TArray<uint32>& PackedUpdates)
{
	const uint32 NumClusterUpdates = PackedUpdates.Num();
	if (NumClusterUpdates == 0u)
	{
		return;
	}

	const uint32 NumUpdatesBufferElements = FMath::RoundUpToPowerOfTwo(NumClusterUpdates);
	FRDGBufferRef UpdatesBuffer = CreateStructuredBuffer(	GraphBuilder, TEXT("Nanite.PackedClusterUpdatesBuffer"), PackedUpdates.GetTypeSize(),
															NumUpdatesBufferElements, PackedUpdates.GetData(), PackedUpdates.Num() * PackedUpdates.GetTypeSize());

	FUpdateClusterLeafFlags_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FUpdateClusterLeafFlags_CS::FParameters>();
	PassParameters->NumClusterUpdates		= NumClusterUpdates;
	PassParameters->PackedClusterUpdates	= GraphBuilder.CreateSRV(UpdatesBuffer);
	PassParameters->ClusterPageBuffer		= ClusterPageBufferUAV;

	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FUpdateClusterLeafFlags_CS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("UpdateClusterLeafFlags"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCount(NumClusterUpdates, 64)
		);
}

// Can't use AddCopyBufferPass because it doesn't support dst==src
static void AddPass_Memcpy(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef UAV, uint32 DstOffset, uint32 SrcOffset, uint32 Length)
{
	check(Length >= NANITE_ROOT_PAGE_GPU_SIZE);
	check(SrcOffset >= DstOffset + Length || DstOffset >= SrcOffset + Length);
	
	check((DstOffset & 15u) == 0u);
	check((SrcOffset & 15u) == 0u);
	check((Length & 15u) == 0u);

	const uint32 NumThreads = Length >> 4;

	FMemcpy_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMemcpy_CS::FParameters>();
	PassParameters->SrcOffset				= SrcOffset;
	PassParameters->DstOffset				= DstOffset;
	PassParameters->NumThreads				= NumThreads;
	PassParameters->ClusterPageBuffer		= UAV;

	auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FMemcpy_CS>();
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("Memcpy"),
		ComputeShader,
		PassParameters,
		FComputeShaderUtils::GetGroupCountWrapped(NumThreads, 64)
		);
}

static void AddPass_Memmove(FRDGBuilder& GraphBuilder, FRDGBufferUAVRef UAV, uint32 DstOffset, uint32 SrcOffset, uint32 Length)
{
	if (DstOffset == SrcOffset)
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "Memmove");
	const uint32 DeltaOffset  = (DstOffset > SrcOffset) ? (DstOffset - SrcOffset) : (SrcOffset - DstOffset);
	const uint32 MaxBlockSize = FMath::Min(FMath::Min(Length, DeltaOffset), 16u << 20);
	const bool	 bReverseCopy = (DstOffset > SrcOffset);

	for (uint32 i = 0; i < Length; i += MaxBlockSize)
	{
		const uint32 BlockSize	= FMath::Min(Length - i, MaxBlockSize);
		const uint32 Offset		= bReverseCopy ? (Length - i - BlockSize) : i;

		AddPass_Memcpy(GraphBuilder, UAV, DstOffset + Offset, SrcOffset + Offset, BlockSize);
	}
}

static int32 SelectTranscodeWaveSize()
{
	const int32 WaveSizeOverride = CVarNaniteStreamingTranscodeWaveSize.GetValueOnRenderThread();

	int32 WaveSize = 0;
	if (WaveSizeOverride != 0 && WaveSizeOverride >= GRHIMinimumWaveSize && WaveSizeOverride <= GRHIMaximumWaveSize && FMath::IsPowerOfTwo(WaveSizeOverride))
	{
		WaveSize = WaveSizeOverride;
	}
	else if (IsRHIDeviceIntel() && 16 >= GRHIMinimumWaveSize && 16 <= GRHIMaximumWaveSize)
	{
		WaveSize = 16;
	}
	else
	{
		WaveSize = GRHIMaximumWaveSize;
	}
	
	return WaveSize;
}

struct FPackedClusterInstallInfo
{
	uint32 LocalPageIndex_LocalClusterIndex;
	uint32 SrcPageOffset;
	uint32 DstPageOffset;
	uint32 PageDependenciesOffset;
};

class FStreamingPageUploader
{
	struct FAddedPageInfo
	{
		FPageKey	GPUPageKey;
		uint32		SrcPageOffset;
		uint32		DstPageOffset;
		uint32		PageDependenciesOffset;
		uint32		NumPageDependencies;
		uint32		ClustersOffset;
		uint32		NumClusters;
		uint32		InstallPassIndex;
	};

	struct FPassInfo
	{
		uint32 NumPages;
		uint32 NumClusters;
	};
public:
	FStreamingPageUploader()
	{
		ResetState();
	}

	void Init(FRDGBuilder& GraphBuilder, uint32 InMaxPages, uint32 InMaxPageBytes, uint32 InMaxStreamingPages)
	{
		ResetState();
		MaxPages = InMaxPages;
		MaxPageBytes = FMath::Max(InMaxPageBytes, 16u);
		MaxStreamingPages = InMaxStreamingPages;

		// Create a new set of buffers if the old set is already queued into RDG.
		if (IsRegistered(GraphBuilder, PageUploadBuffer))
		{
			PageUploadBuffer = nullptr;
			ClusterInstallInfoUploadBuffer = nullptr;
			PageDependenciesBuffer = nullptr;
		}

		const uint32 PageAllocationSize = FMath::RoundUpToPowerOfTwo(MaxPageBytes); //TODO: Revisit po2 rounding once upload buffer refactor lands
		
		// Add EBufferUsageFlags::Dynamic to skip the unneeded copy from upload to VRAM resource on d3d12 RHI
		FRDGBufferDesc BufferDesc = FRDGBufferDesc::CreateByteAddressUploadDesc(PageAllocationSize);
		if (GNaniteStreamingDynamicPageUploadBuffer)
		{
			BufferDesc.Usage |= EBufferUsageFlags::Dynamic;
		}
		
		AllocatePooledBuffer(BufferDesc, PageUploadBuffer, TEXT("Nanite.PageUploadBuffer"));
	
		PageDataPtr = (uint8*)GraphBuilder.RHICmdList.LockBuffer(PageUploadBuffer->GetRHI(), 0, MaxPageBytes, RLM_WriteOnly);
	}

	uint8* Add_GetRef(uint32 PageSize, uint32 NumClusters, uint32 DstPageOffset, const FPageKey& GPUPageKey, const TArray<uint32>& PageDependencies)
	{
		check(IsAligned(PageSize, 4));
		check(IsAligned(DstPageOffset, 4));

		const uint32 PageIndex = AddedPageInfos.Num();

		check(PageIndex < MaxPages);
		check(NextPageByteOffset + PageSize <= MaxPageBytes);

		FAddedPageInfo& Info = AddedPageInfos.AddDefaulted_GetRef();
		Info.GPUPageKey = GPUPageKey;
		Info.SrcPageOffset = NextPageByteOffset;
		Info.DstPageOffset = DstPageOffset;
		Info.PageDependenciesOffset = FlattenedPageDependencies.Num();
		Info.NumPageDependencies = PageDependencies.Num();
		Info.ClustersOffset = NextClusterIndex;
		Info.NumClusters = NumClusters;
		Info.InstallPassIndex = 0xFFFFFFFFu;
		FlattenedPageDependencies.Append(PageDependencies);
		GPUPageKeyToAddedIndex.Add(GPUPageKey, PageIndex);

		uint8* ResultPtr = PageDataPtr + NextPageByteOffset;
		NextPageByteOffset += PageSize;
		NextClusterIndex += NumClusters;
		
		return ResultPtr;
	}

	void Release()
	{
		ClusterInstallInfoUploadBuffer.SafeRelease();
		PageUploadBuffer.SafeRelease();
		PageDependenciesBuffer.SafeRelease();
		ResetState();
	}

	void ResourceUploadTo(FRDGBuilder& GraphBuilder, FRDGBuffer* DstBuffer)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Nanite::Transcode");
		GraphBuilder.RHICmdList.UnlockBuffer(PageUploadBuffer->GetRHI());

		const uint32 NumPages = AddedPageInfos.Num();
		if (NumPages == 0)	// This can end up getting called with NumPages = 0 when NumReadyPages > 0 and all pages early out.
		{
			ResetState();
			return;
		}

		const uint32 ClusterInstallInfoAllocationSize = FMath::RoundUpToPowerOfTwo(NextClusterIndex * sizeof(FPackedClusterInstallInfo));
		if (ClusterInstallInfoAllocationSize > TryGetSize(ClusterInstallInfoUploadBuffer))
		{
			const uint32 BytesPerElement = sizeof(FPackedClusterInstallInfo);

			AllocatePooledBuffer(FRDGBufferDesc::CreateStructuredUploadDesc(BytesPerElement, ClusterInstallInfoAllocationSize / BytesPerElement), ClusterInstallInfoUploadBuffer, TEXT("Nanite.ClusterInstallInfoUploadBuffer"));
		}

		FPackedClusterInstallInfo* ClusterInstallInfoPtr = (FPackedClusterInstallInfo*)GraphBuilder.RHICmdList.LockBuffer(ClusterInstallInfoUploadBuffer->GetRHI(), 0, ClusterInstallInfoAllocationSize, RLM_WriteOnly);

		uint32 PageDependenciesAllocationSize = FMath::RoundUpToPowerOfTwo(FMath::Max(FlattenedPageDependencies.Num(), 4096) * sizeof(uint32));
		if (PageDependenciesAllocationSize > TryGetSize(PageDependenciesBuffer))
		{
			const uint32 BytesPerElement = sizeof(uint32);

			AllocatePooledBuffer(FRDGBufferDesc::CreateStructuredUploadDesc(BytesPerElement, PageDependenciesAllocationSize / BytesPerElement), PageDependenciesBuffer, TEXT("Nanite.PageDependenciesBuffer"));
		}

		uint32* PageDependenciesPtr = (uint32*)GraphBuilder.RHICmdList.LockBuffer(PageDependenciesBuffer->GetRHI(), 0, PageDependenciesAllocationSize, RLM_WriteOnly);
		FMemory::Memcpy(PageDependenciesPtr, FlattenedPageDependencies.GetData(), FlattenedPageDependencies.Num() * sizeof(uint32));
		GraphBuilder.RHICmdList.UnlockBuffer(PageDependenciesBuffer->GetRHI());

		// Split page installs into passes.
		// Every pass adds the pages that no longer have any unresolved dependency.
		// Essentially a naive multi-pass topology sort, but with a low number of passes in practice.
		check(PassInfos.Num() == 0);
		uint32 NumRemainingPages = NumPages;
		uint32 NumClusters = 0;
		uint32 NextSortedPageIndex = 0;
		while (NumRemainingPages > 0)
		{
			const uint32 CurrentPassIndex = PassInfos.Num();
			uint32 NumPassPages = 0;
			uint32 NumPassClusters = 0;
			for(FAddedPageInfo& PageInfo : AddedPageInfos)
			{
				if (PageInfo.InstallPassIndex < CurrentPassIndex)
					continue;	// Page already installed in an earlier pass

				bool bMissingDependency = false;
				for (uint32 i = 0; i < PageInfo.NumPageDependencies; i++)
				{
					const uint32 GPUPageIndex = FlattenedPageDependencies[PageInfo.PageDependenciesOffset + i];
					const FPageKey DependencyGPUPageKey = { PageInfo.GPUPageKey.RuntimeResourceID, GPUPageIndex };
					const uint32* DependencyAddedIndexPtr = GPUPageKeyToAddedIndex.Find(DependencyGPUPageKey);

					// Check if a dependency has not yet been installed.
					// We only need to resolve dependencies in the current batch. Batches are already ordered.
					if (DependencyAddedIndexPtr && AddedPageInfos[*DependencyAddedIndexPtr].InstallPassIndex >= CurrentPassIndex)
					{
						bMissingDependency = true;
						break;
					}
				}

				if (!bMissingDependency)
				{
					PageInfo.InstallPassIndex = CurrentPassIndex;

					// Add cluster infos
					check(PageInfo.NumClusters <= NANITE_MAX_CLUSTERS_PER_PAGE);
					for(uint32 i = 0; i < PageInfo.NumClusters; i++)
					{
						ClusterInstallInfoPtr->LocalPageIndex_LocalClusterIndex = (NextSortedPageIndex << NANITE_MAX_CLUSTERS_PER_PAGE_BITS) | i;
						ClusterInstallInfoPtr->SrcPageOffset					= PageInfo.SrcPageOffset;
						ClusterInstallInfoPtr->DstPageOffset					= PageInfo.DstPageOffset;
						ClusterInstallInfoPtr->PageDependenciesOffset			= PageInfo.PageDependenciesOffset;
						ClusterInstallInfoPtr++;
					}
					NextSortedPageIndex++;
					NumPassPages++;
					NumPassClusters += PageInfo.NumClusters;
				}
			}

			FPassInfo PassInfo;
			PassInfo.NumPages = NumPassPages;
			PassInfo.NumClusters = NumPassClusters;
			PassInfos.Add(PassInfo);
			NumRemainingPages -= NumPassPages;
		}

		GraphBuilder.RHICmdList.UnlockBuffer(ClusterInstallInfoUploadBuffer->GetRHI());

		FRDGBufferSRV* PageUploadBufferSRV = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(PageUploadBuffer));
		FRDGBufferSRV* ClusterInstallInfoUploadBufferSRV = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ClusterInstallInfoUploadBuffer));
		FRDGBufferSRV* PageDependenciesBufferSRV = GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(PageDependenciesBuffer));
		FRDGBufferUAV* DstBufferUAV = GraphBuilder.CreateUAV(DstBuffer);

		// Disable async compute for streaming systems when MGPU is active, to work around GPU hangs
		const bool bAsyncCompute = GSupportsEfficientAsyncCompute && (GNaniteStreamingAsyncCompute != 0) && (GNumExplicitGPUsForRendering == 1);
		
		check(GRHISupportsWaveOperations);

		const uint32 PreferredGroupSize = (uint32)SelectTranscodeWaveSize();

		FTranscodePageToGPU_CS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FTranscodePageToGPU_CS::FGroupSizeDim>(PreferredGroupSize);

		// Independent transcode
		{
			FTranscodePageToGPU_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTranscodePageToGPU_CS::FParameters>();
			PassParameters->ClusterInstallInfoBuffer	= ClusterInstallInfoUploadBufferSRV;
			PassParameters->PageDependenciesBuffer		= PageDependenciesBufferSRV;
			PassParameters->SrcPageBuffer				= PageUploadBufferSRV;
			PassParameters->DstPageBuffer				= DstBufferUAV;
			PassParameters->StartClusterIndex			= 0;
			PassParameters->NumClusters					= NextClusterIndex;
			PassParameters->ZeroUniform					= 0;
			PassParameters->PageConstants				= FIntVector4(0, MaxStreamingPages, 0, 0);

			PermutationVector.Set<FTranscodePageToGPU_CS::FTranscodePassDim>(NANITE_TRANSCODE_PASS_INDEPENDENT);
			auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FTranscodePageToGPU_CS>(PermutationVector);
			
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TranscodePageToGPU Independent (ClusterCount: %u, GroupSize: %u)", NextClusterIndex, PreferredGroupSize),
				bAsyncCompute ? ERDGPassFlags::AsyncCompute : ERDGPassFlags::Compute,
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCountWrapped(NextClusterIndex));
		}

		// Parent-dependent transcode
		const uint32 NumPasses = PassInfos.Num();
		uint32 StartClusterIndex = 0;

		for (uint32 PassIndex = 0; PassIndex < NumPasses; PassIndex++)
		{
			const FPassInfo& PassInfo = PassInfos[PassIndex];

			FTranscodePageToGPU_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTranscodePageToGPU_CS::FParameters>();
			PassParameters->ClusterInstallInfoBuffer	= ClusterInstallInfoUploadBufferSRV;
			PassParameters->PageDependenciesBuffer		= PageDependenciesBufferSRV;
			PassParameters->SrcPageBuffer				= PageUploadBufferSRV;
			PassParameters->DstPageBuffer				= DstBufferUAV;
			PassParameters->StartClusterIndex			= StartClusterIndex;
			PassParameters->NumClusters					= PassInfo.NumClusters;
			PassParameters->ZeroUniform					= 0;
			PassParameters->PageConstants				= FIntVector4(0, MaxStreamingPages, 0, 0);
			
			PermutationVector.Set<FTranscodePageToGPU_CS::FTranscodePassDim>(NANITE_TRANSCODE_PASS_PARENT_DEPENDENT);
			auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FTranscodePageToGPU_CS>(PermutationVector);
			
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TranscodePageToGPU Dependent (ClusterOffset: %u, ClusterCount: %u, GroupSize: %u)", StartClusterIndex, PassInfo.NumClusters, PreferredGroupSize),
				bAsyncCompute ? ERDGPassFlags::AsyncCompute : ERDGPassFlags::Compute,
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCountWrapped(PassInfo.NumClusters));

			StartClusterIndex += PassInfo.NumClusters;
		}	
		Release();
	}
private:
	TRefCountPtr<FRDGPooledBuffer> ClusterInstallInfoUploadBuffer;
	TRefCountPtr<FRDGPooledBuffer> PageUploadBuffer;
	TRefCountPtr<FRDGPooledBuffer> PageDependenciesBuffer;
	uint8*					PageDataPtr;
	uint32					MaxPages;
	uint32					MaxPageBytes;
	uint32					MaxStreamingPages;
	uint32					NextPageByteOffset;
	uint32					NextClusterIndex;
	TArray<FAddedPageInfo>	AddedPageInfos;
	TMap<FPageKey, uint32>	GPUPageKeyToAddedIndex;
	TArray<uint32>			FlattenedPageDependencies;
	TArray<FPassInfo>		PassInfos;
	
	void ResetState()
	{
		PageDataPtr = nullptr;
		MaxPages = 0;
		MaxPageBytes = 0;
		NextPageByteOffset = 0;
		NextClusterIndex = 0;
		AddedPageInfos.Reset();
		GPUPageKeyToAddedIndex.Reset();
		FlattenedPageDependencies.Reset();
		PassInfos.Reset();
	}
};

class FHierarchyDepthManager
{
public:
	FHierarchyDepthManager(uint32 MaxDepth)
	{
		DepthHistogram.SetNumZeroed(MaxDepth + 1);
	}

	void Add(uint32 Depth)
	{
		DepthHistogram[Depth]++;
	}
	void Remove(uint32 Depth)
	{
		uint32& Count = DepthHistogram[Depth];
		check(Count > 0u);
		Count--;
	}

	uint32 CalculateNumLevels() const
	{
		for (int32 Depth = uint32(DepthHistogram.Num() - 1); Depth >= 0; Depth--)
		{
			if (DepthHistogram[Depth] != 0u)
			{
				return uint32(Depth) + 1u;
			}
		}
		return 0u;
	}
private:
	TArray<uint32> DepthHistogram;
};

class FRingBufferAllocator
{
public:
	FRingBufferAllocator(uint32 Size):
		BufferSize(Size)
	{
		Reset();
	}

	void Reset()
	{
		ReadOffset = 0u;
		WriteOffset = 0u;
	#if DO_CHECK
		SizeQueue.Empty();
	#endif
	}

	bool TryAllocate(uint32 Size, uint32& AllocatedOffset)
	{
		if (WriteOffset < ReadOffset)
		{
			if (Size + 1u > ReadOffset - WriteOffset)	// +1 to leave one element free, so we can distinguish between full and empty
			{
				return false;
			}
		}
		else
		{
			// WriteOffset >= ReadOffset
			if (Size + (ReadOffset == 0u ? 1u : 0u) > BufferSize - WriteOffset)
			{
				// Doesn't fit at the end. Try from the beginning
				if (Size + 1u > ReadOffset)
				{
					return false;
				}
				WriteOffset = 0u;
			}
		}

	#if DO_CHECK
		SizeQueue.Enqueue(Size);
	#endif
		AllocatedOffset = WriteOffset;
		WriteOffset += Size;
		check(AllocatedOffset + Size <= BufferSize);
		return true;
	}

	void Free(uint32 Size)
	{
	#if DO_CHECK
		uint32 QueuedSize;
		bool bNonEmpty = SizeQueue.Dequeue(QueuedSize);
		check(bNonEmpty);
		check(QueuedSize == Size);
	#endif
		const uint32 Next = ReadOffset + Size;
		ReadOffset = (Next <= BufferSize) ? Next : Size;
	}
private:
	uint32 BufferSize;
	uint32 ReadOffset;
	uint32 WriteOffset;
#if DO_CHECK
	TQueue<uint32> SizeQueue;
#endif
};

struct FGPUStreamingRequest
{
	uint32	RuntimeResourceID_Magic;
	uint32	PageIndex_NumPages_Magic;
	uint32	Priority_Magic;
};

class FReadbackManager
{
public:
	FReadbackManager(uint32 InNumBuffers):
		NumBuffers(InNumBuffers)
	{
		ReadbackBuffers.SetNum(NumBuffers);
	}

	void PrepareRequestsBuffer(FRDGBuilder& GraphBuilder)
	{
		const uint32 BufferSize = RoundUpToSignificantBits(BufferSizeManager.GetSize(), 2);

		SET_DWORD_STAT(STAT_NaniteStreaming42_ReadbackBufferSize, BufferSize);
	
		if (!RequestsBuffer.IsValid() || RequestsBuffer->Desc.NumElements != BufferSize)
		{
			// Init and clear StreamingRequestsBuffer.
			FRDGBufferDesc Desc = FRDGBufferDesc::CreateStructuredDesc(sizeof(FGPUStreamingRequest), BufferSize);
			Desc.Usage = EBufferUsageFlags(Desc.Usage | BUF_SourceCopy);
			FRDGBufferRef RequestsBufferRef = GraphBuilder.CreateBuffer(Desc, TEXT("Nanite.StreamingRequests"));

			AddPass_ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(RequestsBufferRef));

			RequestsBuffer = GraphBuilder.ConvertToExternalBuffer(RequestsBufferRef);
		}
	}

	FGPUStreamingRequest* LockLatest(uint32& OutNumStreamingRequests)
	{
		OutNumStreamingRequests = 0u;
		check(LatestBuffer == nullptr);

		// Find latest buffer that is ready
		while (NumPendingBuffers > 0)
		{
			if (ReadbackBuffers[NextReadBufferIndex].Buffer->IsReady())
			{
				LatestBuffer = &ReadbackBuffers[NextReadBufferIndex];
				NextReadBufferIndex = (NextReadBufferIndex + 1u) % NumBuffers;
				NumPendingBuffers--;
			}
			else
			{
				break;
			}
		}

		if (LatestBuffer)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(LockBuffer);
			uint32* Ptr = (uint32*)LatestBuffer->Buffer->Lock(LatestBuffer->NumElements * sizeof(FGPUStreamingRequest));
			check(LatestBuffer->NumElements > 0u);
			
			const uint32 NumRequests = Ptr[0];
			BufferSizeManager.Update(NumRequests);

			SET_DWORD_STAT(STAT_NaniteStreaming41_ReadbackSize, NumRequests);
			
			OutNumStreamingRequests = FMath::Min(NumRequests, LatestBuffer->NumElements - 1u);
			return (FGPUStreamingRequest*)Ptr + 1;
		}
		return nullptr;
	}

	void Unlock()
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UnlockBuffer);
		check(LatestBuffer);
		LatestBuffer->Buffer->Unlock();
		LatestBuffer = nullptr;
	}

	void QueueReadback(FRDGBuilder& GraphBuilder)
	{
		if (NumPendingBuffers == NumBuffers)
		{
			// Return when queue is full. It is NOT safe to EnqueueCopy on a buffer that already has a pending copy.
			return;
		}

		const uint32 WriteBufferIndex = (NextReadBufferIndex + NumPendingBuffers) % NumBuffers;
		FReadbackBuffer& ReadbackBuffer = ReadbackBuffers[WriteBufferIndex];

		if (ReadbackBuffer.Buffer == nullptr)
		{
			ReadbackBuffer.Buffer = MakeUnique<FRHIGPUBufferReadback>(TEXT("Nanite.StreamingRequestReadback"));
		}
		ReadbackBuffer.NumElements = RequestsBuffer->Desc.NumElements;

		FRDGBufferRef RDGRequestsBuffer = GraphBuilder.RegisterExternalBuffer(RequestsBuffer);

		AddReadbackBufferPass(GraphBuilder, RDG_EVENT_NAME("Readback"), RDGRequestsBuffer,
			[&GPUReadback = ReadbackBuffer.Buffer, RDGRequestsBuffer](FRDGAsyncTask, FRHICommandList& RHICmdList)
			{
				GPUReadback->EnqueueCopy(RHICmdList, RDGRequestsBuffer->GetRHI(), 0u);
			});

		AddPass_ClearStreamingRequestCount(GraphBuilder, GraphBuilder.CreateUAV(RDGRequestsBuffer));

		NumPendingBuffers++;
		BufferVersion++;
	}

	FRDGBuffer* GetStreamingRequestsBuffer(FRDGBuilder& GraphBuilder) const
	{
		return GraphBuilder.RegisterExternalBuffer(RequestsBuffer);
	}

	uint32 GetBufferVersion() const
	{
		return BufferVersion;
	}

private:
	struct FReadbackBuffer
	{
		TUniquePtr<class FRHIGPUBufferReadback>	Buffer;
		uint32									NumElements = 0u;
	};

	class FBufferSizeManager
	{
	public:
		FBufferSizeManager() :
			CurrentSize((float)GNaniteStreamingGPURequestsBufferMinSize)
		{
		}

		void Update(uint32 NumRequests)
		{
			const uint32 Target = uint32(NumRequests * 1.25f);			// Target 25% headroom

			const bool bOverBudget = Target > CurrentSize;
			const bool bUnderBudget = NumRequests < CurrentSize * 0.5f;	// Only consider shrinking when less than half the buffer is used

			OverBudgetCounter	= bOverBudget  ? (OverBudgetCounter  + 1u) : 0u;
			UnderBudgetCounter	= bUnderBudget ? (UnderBudgetCounter + 1u) : 0u;
			
			if (OverBudgetCounter >= 2u)		// Ignore single frames that are over budget
			{
				CurrentSize = FMath::Max(CurrentSize, Target);
			}
			else if (UnderBudgetCounter >= 30u)	// Only start shrinking when we have been under budget for a while
			{
				CurrentSize *= 0.98f;
			}

			const int32 LimitMinSize = 4u * 1024;
			const int32 LimitMaxSize = 1024u * 1024;
			const int32 MinSize = FMath::Clamp(GNaniteStreamingGPURequestsBufferMinSize, LimitMinSize, LimitMaxSize);
			const int32 MaxSize = FMath::Clamp(GNaniteStreamingGPURequestsBufferMaxSize, MinSize, LimitMaxSize);

			CurrentSize = FMath::Clamp(CurrentSize, (float)MinSize, (float)MaxSize);
		}

		uint32 GetSize()
		{
			return uint32(CurrentSize);
		}
	private:
		float CurrentSize;
		uint32 OverBudgetCounter = 0;
		uint32 UnderBudgetCounter = 0;
	};

	TRefCountPtr<FRDGPooledBuffer>	RequestsBuffer;
	TArray<FReadbackBuffer>			ReadbackBuffers;

	FReadbackBuffer*				LatestBuffer = nullptr;
	uint32							NumBuffers = 0;
	uint32							NumPendingBuffers = 0;
	uint32							NextReadBufferIndex = 0;
	uint32							BufferVersion = 0;

	FBufferSizeManager				BufferSizeManager;
};

class FQualityScalingManager
{
public:
	float Update(float StreamingPoolPercentage)
	{
		const float MinPercentage = FMath::Clamp(GNaniteStreamingQualityScaleMinPoolPercentage, 10.0f, 100.0f);
		const float MaxPercentage = FMath::Clamp(GNaniteStreamingQualityScaleMaxPoolPercentage, MinPercentage, 100.0f);

		const bool bOverBudget = (StreamingPoolPercentage > MaxPercentage);
		const bool bUnderBudget = (StreamingPoolPercentage < MinPercentage);

		OverBudgetCounter = bOverBudget ? (OverBudgetCounter + 1u) : 0u;
		UnderBudgetCounter = bUnderBudget ? (UnderBudgetCounter + 1u) : 0u;

		if (OverBudgetCounter >= 2u)
		{
			// Ignore single frames that could be because of temporary disocclusion.
			// When we are over budget for more than on frame, adjust quality down rapidly.
			Scale *= 0.97f;
		}
		else if (UnderBudgetCounter >= 30u)
		{
			// If we are under budget, slowly start increasing quality again.
			Scale *= 1.01f;
		}

		const float MinScale = FMath::Clamp(GNaniteStreamingQualityScaleMinQuality, 0.1f, 1.0f);
		Scale = FMath::Clamp(Scale, MinScale, 1.0f);
		return Scale;
	}
private:
	float Scale = 1.0f;
	uint32 OverBudgetCounter = 0u;
	uint32 UnderBudgetCounter = 0u;
};

FStreamingManager::FStreamingManager()
#if WITH_EDITOR
	: RequestOwner(nullptr)
#endif
{
}

void FStreamingManager::InitRHI(FRHICommandListBase& RHICmdList)
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);

	HierarchyDepthManager = MakePimpl<FHierarchyDepthManager>(NANITE_MAX_CLUSTER_HIERARCHY_DEPTH);
	ReadbackManager = MakePimpl<FReadbackManager>(4);
	QualityScalingManager = MakePimpl<FQualityScalingManager>();

	UpdatePageConfiguration();

	MaxPendingPages = GNaniteStreamingMaxPendingPages;
	MaxPageInstallsPerUpdate = (uint32)FMath::Min(GNaniteStreamingMaxPageInstallsPerFrame, GNaniteStreamingMaxPendingPages);

	PendingPageStagingMemory.SetNumUninitialized(MaxPendingPages * NANITE_ESTIMATED_MAX_PAGE_DISK_SIZE);
	PendingPageStagingAllocator = MakePimpl<FRingBufferAllocator>(PendingPageStagingMemory.Num());

	ResetStreamingStateCPU();

	PageUploader = MakePimpl<FStreamingPageUploader>();

	const bool bReservedResource = (GRHIGlobals.ReservedResources.Supported && GNaniteStreamingReservedResources);

	FRDGBufferDesc ClusterDataBufferDesc = {};
	if (bReservedResource)
	{
		const uint64 MaxSizeInBytes = uint64(GetMaxPagePoolSizeInMB()) << 20;
		ClusterDataBufferDesc = FRDGBufferDesc::CreateByteAddressDesc(MaxSizeInBytes);
		ClusterDataBufferDesc.Usage |= EBufferUsageFlags::ReservedResource;
	}
	else
	{
		ClusterDataBufferDesc = FRDGBufferDesc::CreateByteAddressDesc(4);
	}

	// Keep non-reserved resource allocations grow only for now to avoid repeating expensive resizes
	Hierarchy.Allocator = FSpanAllocator(true);
	ImposterData.Allocator = FSpanAllocator(true);

	if (!bReservedResource || GNaniteStreamingReservedResourceRootPageGrowOnly != 0)
	{
		ClusterPageData.Allocator = FSpanAllocator(true);
	}
		
	ImposterData.DataBuffer = AllocatePooledBufferCurrentLLMTag(RHICmdList, FRDGBufferDesc::CreateByteAddressDesc(4), TEXT("Nanite.StreamingManager.ImposterData"));
	ClusterPageData.DataBuffer = AllocatePooledBufferCurrentLLMTag(RHICmdList, ClusterDataBufferDesc, TEXT("Nanite.StreamingManager.ClusterPageData"));
	Hierarchy.DataBuffer = AllocatePooledBufferCurrentLLMTag(RHICmdList, FRDGBufferDesc::CreateByteAddressDesc(4), TEXT("Nanite.StreamingManager.HierarchyData"));

#if WITH_EDITOR
	RequestOwner = new FRequestOwner(EPriority::Normal);
#endif
}

void FStreamingManager::ResetStreamingStateCPU()
{
	RegisteredVirtualPages.Empty();
	RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

	RegisteredPages.Empty();
	RegisteredPages.SetNum(MaxStreamingPages);

	RegisteredPageDependencies.Empty();
	RegisteredPageDependencies.SetNum(MaxStreamingPages);

	RegisteredPageIndexToLRU.Empty();
	RegisteredPageIndexToLRU.SetNum(MaxStreamingPages);

	LRUToRegisteredPageIndex.Empty();
	LRUToRegisteredPageIndex.SetNum(MaxStreamingPages);
	for (uint32 i = 0; i < MaxStreamingPages; i++)
	{
		RegisteredPageIndexToLRU[i] = i;
		LRUToRegisteredPageIndex[i] = i;
	}

	ResidentPages.Empty();
	ResidentPages.SetNum(MaxStreamingPages);

	for (FFixupChunk* FixupChunk : ResidentPageFixupChunks)
	{
		FMemory::Free( FixupChunk );
	}
	ResidentPageFixupChunks.Empty();
	ResidentPageFixupChunks.SetNum(MaxStreamingPages);

	ResidentPageMap.Empty();

	PendingPages.Empty();
	PendingPages.SetNum(MaxPendingPages);

	NumPendingPages = 0;
	NextPendingPageIndex = 0;
	ModifiedResources.Empty();

	PendingPageStagingAllocator->Reset();
}

void FStreamingManager::UpdatePageConfiguration()
{
	const uint32 MaxPoolSizeInMB = GetMaxPagePoolSizeInMB();
	const uint32 StreamingPoolSizeInMB = GNaniteStreamingPoolSize;
	if (StreamingPoolSizeInMB >= MaxPoolSizeInMB)
	{
		UE_LOG(LogNaniteStreaming, Fatal, TEXT("Streaming pool size (%dMB) must be smaller than the largest allocation supported by the graphics hardware (%dMB)"), StreamingPoolSizeInMB, MaxPoolSizeInMB);
	}

	const uint32 OldMaxStreamingPages = MaxStreamingPages;
	const uint32 OldNumInitialRootPages = NumInitialRootPages;

	const uint64 MaxRootPoolSizeInMB = MaxPoolSizeInMB - StreamingPoolSizeInMB;
	MaxStreamingPages = uint32((uint64(StreamingPoolSizeInMB) << 20) >> NANITE_STREAMING_PAGE_GPU_SIZE_BITS);
	MaxRootPages = uint32((uint64(MaxRootPoolSizeInMB) << 20) >> NANITE_ROOT_PAGE_GPU_SIZE_BITS);

	check(MaxStreamingPages + MaxRootPages <= NANITE_MAX_GPU_PAGES);
	check((MaxStreamingPages << NANITE_STREAMING_PAGE_MAX_CLUSTERS_BITS) + (MaxRootPages << NANITE_ROOT_PAGE_MAX_CLUSTERS_BITS) <= (1u << NANITE_POOL_CLUSTER_REF_BITS));

	NumInitialRootPages = GNaniteStreamingNumInitialRootPages;
	if (NumInitialRootPages > MaxRootPages)
	{
		if(NumInitialRootPages != PrevNumInitialRootPages || MaxStreamingPages != OldMaxStreamingPages)
		{
			UE_LOG(LogNaniteStreaming, Log, TEXT("r.Nanite.Streaming.NumInitialRootPages clamped from %d to %d.\n"
				"Graphics hardware max buffer size: %dMB, Streaming pool size: %dMB, Max root pool size: %" UINT64_FMT "MB (%d pages)."),
				NumInitialRootPages, MaxRootPages,
				MaxPoolSizeInMB, StreamingPoolSizeInMB, MaxRootPoolSizeInMB, MaxRootPages);
		}
		NumInitialRootPages = MaxRootPages;
	}
	PrevNumInitialRootPages = GNaniteStreamingNumInitialRootPages;
}

void FStreamingManager::ReleaseRHI()
{
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

#if WITH_EDITOR
	delete RequestOwner;
	RequestOwner = nullptr;
#endif

	LLM_SCOPE_BYTAG(Nanite);
	for (FFixupChunk* FixupChunk : ResidentPageFixupChunks)
	{
		FMemory::Free(FixupChunk);
	}

	ImposterData.Release();
	ClusterPageData.Release();
	Hierarchy.Release();
	ReadbackManager.Reset();

	PendingPages.Empty();	// Make sure IO handles are released before IO system is shut down

	PageUploader.Reset();
}

void FStreamingManager::Add( FResources* Resources )
{
	check(Resources != nullptr);	// Needed to make static analysis happy
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);

	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	if (Resources->RuntimeResourceID == INDEX_NONE)
	{
		check(Resources->RootData.Num() > 0);

		const uint32 NumHierarchyNodes = Resources->HierarchyNodes.Num();
		const uint32 NumHierarchyNodeDwords = NumHierarchyNodes * NANITE_HIERARCHY_NODE_SLICE_SIZE_DWORDS;
		const uint32 NumAssemblyTransformDwords = Resources->AssemblyTransforms.Num() * NANITE_ASSEMBLY_TRANSFORM_SIZE_DWORDS;
		const uint32 TotalHierarchyDwords = NumHierarchyNodeDwords + NumAssemblyTransformDwords;

		Resources->HierarchyOffset = Hierarchy.Allocator.Allocate(TotalHierarchyDwords);
		Resources->AssemblyTransformOffset = NumAssemblyTransformDwords > 0 ? Resources->HierarchyOffset + NumHierarchyNodeDwords : MAX_uint32;
		Resources->NumHierarchyNodes = NumHierarchyNodes;
		Resources->NumHierarchyDwords = TotalHierarchyDwords;
		Hierarchy.TotalUpload += TotalHierarchyDwords;
		
		StatNumHierarchyNodes += Resources->NumHierarchyNodes;
		StatPeakHierarchyNodes = FMath::Max(StatPeakHierarchyNodes, StatNumHierarchyNodes);

		INC_DWORD_STAT_BY(STAT_NaniteStreaming00_NaniteResources, 1);
		SET_DWORD_STAT(   STAT_NaniteStreaming02_HierarchyNodes, StatNumHierarchyNodes);
		SET_DWORD_STAT(   STAT_NaniteStreaming03_PeakHierarchyNodes, StatPeakHierarchyNodes);
		INC_DWORD_STAT_BY(STAT_NaniteStreaming06_RootPages, Resources->NumRootPages);

		Resources->RootPageIndex = ClusterPageData.Allocator.Allocate( Resources->NumRootPages );
		if (GNaniteStreamingDynamicallyGrowAllocations == 0 && (uint32)ClusterPageData.Allocator.GetMaxSize() > NumInitialRootPages)
		{
			UE_LOG(LogNaniteStreaming, Fatal, TEXT("Out of root pages. Increase the initial root page allocation (r.Nanite.Streaming.NumInitialRootPages) or allow it to grow dynamically (r.Nanite.Streaming.DynamicallyGrowAllocations)."));
		}
		StatNumRootPages += Resources->NumRootPages;

		StatPeakRootPages = FMath::Max(StatPeakRootPages, StatNumRootPages);
		SET_DWORD_STAT(STAT_NaniteStreaming07_PeakRootPages, StatPeakRootPages);


	#if !NANITE_IMPOSTERS_SUPPORTED
		check(Resources->ImposterAtlas.Num() == 0);
	#endif
		if (GNaniteStreamingImposters && Resources->ImposterAtlas.Num())
		{
			Resources->ImposterIndex = ImposterData.Allocator.Allocate(1);
			if (GNaniteStreamingDynamicallyGrowAllocations == 0 && ImposterData.Allocator.GetMaxSize() > GNaniteStreamingNumInitialImposters)
			{
				UE_LOG(LogNaniteStreaming, Fatal, TEXT("Out of imposters. Increase the initial imposter allocation (r.Nanite.Streaming.NumInitialImposters) or allow it to grow dynamically (r.Nanite.Streaming.DynamicallyGrowAllocations)."));
			}
			ImposterData.TotalUpload++;
			INC_DWORD_STAT_BY( STAT_NaniteStreaming01_Imposters, 1 );
		}

		if ((uint32)Resources->RootPageIndex >= MaxRootPages)
		{
			const uint32 MaxPagePoolSize = GetMaxPagePoolSizeInMB();
			UE_LOG(LogNaniteStreaming, Fatal, TEXT(	"Cannot allocate more root pages %d/%d. Pool resource has grown to maximum size of %dMB.\n"
													"%dMB is spent on streaming data, leaving %dMB for %d root pages."),
													MaxRootPages, MaxRootPages, MaxPagePoolSize, GNaniteStreamingPoolSize, MaxPagePoolSize - GNaniteStreamingPoolSize, MaxRootPages);
		}
		RootPageInfos.SetNum(ClusterPageData.Allocator.GetMaxSize());

		RootPageVersions.SetNumZeroed(FMath::Max(RootPageVersions.Num(), ClusterPageData.Allocator.GetMaxSize()));	// Never shrink, so we never forget versions for root slots that were once allocated.
																													// We need this to filter streaming requests that could still be in flight.

		
		const uint32 NumResourcePages = Resources->PageStreamingStates.Num();
		const uint32 VirtualPageRangeStart = VirtualPageAllocator.Allocate(NumResourcePages);

		RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

		INC_DWORD_STAT_BY(STAT_NaniteStreaming0B_TotalStreamingPages, NumResourcePages - Resources->NumRootPages);

		uint32 RuntimeResourceID;
		{
			FRootPageInfo& RootPageInfo = RootPageInfos[Resources->RootPageIndex];
			uint8& RootPageNextVersion = RootPageVersions[Resources->RootPageIndex];
			// Version root pages so we can disregard invalid streaming requests.
			// TODO: We only need enough versions to cover the frame delay from the GPU, so most of the version bits can be reclaimed.
			RuntimeResourceID = (RootPageNextVersion << NANITE_MAX_GPU_PAGES_BITS) | Resources->RootPageIndex;
			RootPageNextVersion = (RootPageNextVersion + 1u) & MAX_RUNTIME_RESOURCE_VERSIONS_MASK;
		}
		Resources->RuntimeResourceID = RuntimeResourceID;

		for (uint32 i = 0; i < Resources->NumRootPages; i++)
		{
			FRootPageInfo& RootPageInfo			= RootPageInfos[Resources->RootPageIndex + i];
			check(RootPageInfo.Resources == nullptr);
			check(RootPageInfo.RuntimeResourceID == INDEX_NONE);
			check(RootPageInfo.VirtualPageRangeStart == INDEX_NONE);
			check(RootPageInfo.NumClusters == 0u);
			
			RootPageInfo.Resources				= Resources;
			RootPageInfo.RuntimeResourceID		= RuntimeResourceID;
			RootPageInfo.VirtualPageRangeStart	= VirtualPageRangeStart + i;
			RootPageInfo.NumClusters			= 0u;
		}

#if DO_CHECK
		for (uint32 i = 0; i < NumResourcePages; i++)
		{
			check(RegisteredVirtualPages[VirtualPageRangeStart + i] == FVirtualPage());
		}
#endif

		check(Resources->PersistentHash != NANITE_INVALID_PERSISTENT_HASH);
		PersistentHashResourceMap.Add(Resources->PersistentHash, Resources);
		
		PendingAdds.Add( Resources );
		NumResources++;
	}
}

void FStreamingManager::Remove( FResources* Resources )
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);

	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	if (Resources->RuntimeResourceID != INDEX_NONE)
	{
		Hierarchy.Allocator.Free( Resources->HierarchyOffset, Resources->NumHierarchyDwords );
		Resources->HierarchyOffset = INDEX_NONE;

		const uint32 RootPageIndex = Resources->RootPageIndex;
		const uint32 NumRootPages = Resources->NumRootPages;
		ClusterPageData.Allocator.Free( RootPageIndex, NumRootPages);
		Resources->RootPageIndex = INDEX_NONE;

		if (Resources->ImposterIndex != INDEX_NONE)
		{
			ImposterData.Allocator.Free( Resources->ImposterIndex, 1 );
			Resources->ImposterIndex = INDEX_NONE;
			DEC_DWORD_STAT_BY( STAT_NaniteStreaming01_Imposters, 1 );
		}

		StatNumHierarchyNodes -= Resources->NumHierarchyNodes;

		const uint32 NumResourcePages = Resources->PageStreamingStates.Num();
		DEC_DWORD_STAT_BY(STAT_NaniteStreaming0B_TotalStreamingPages, NumResourcePages - NumRootPages);
		DEC_DWORD_STAT_BY(STAT_NaniteStreaming00_NaniteResources, 1);
		SET_DWORD_STAT(   STAT_NaniteStreaming02_HierarchyNodes, StatNumHierarchyNodes);
		DEC_DWORD_STAT_BY(STAT_NaniteStreaming06_RootPages, NumRootPages);
		
		StatNumRootPages -= NumRootPages;
		
		const uint32 VirtualPageRangeStart = RootPageInfos[RootPageIndex].VirtualPageRangeStart;
		for (uint32 i = 0; i < NumRootPages; i++)
		{
			FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex + i];
			RootPageInfo.Resources = nullptr;
			RootPageInfo.RuntimeResourceID = INDEX_NONE;
			RootPageInfo.VirtualPageRangeStart = INDEX_NONE;
			RootPageInfo.NumClusters = 0;

			if (RootPageInfo.MaxHierarchyDepth != 0xFFu)
			{
				HierarchyDepthManager->Remove(RootPageInfo.MaxHierarchyDepth);
				RootPageInfo.MaxHierarchyDepth = 0xFFu;
			}
		}

		// Move all registered pages to the free list. No need to properly uninstall them as they are no longer referenced from the hierarchy.
		for( uint32 PageIndex = NumRootPages; PageIndex < NumResourcePages; PageIndex++ )
		{
			const uint32 VirtualPageIndex = VirtualPageRangeStart + PageIndex;
			const uint32 RegisteredPageIndex = RegisteredVirtualPages[VirtualPageIndex].RegisteredPageIndex;
			if(RegisteredPageIndex != INDEX_NONE)
			{
				RegisteredPages[RegisteredPageIndex] = FRegisteredPage();
				RegisteredPageDependencies[RegisteredPageIndex].Reset();
			}
			RegisteredVirtualPages[VirtualPageIndex] = FVirtualPage();
		}

		VirtualPageAllocator.Free(VirtualPageRangeStart, NumResourcePages);

		Resources->RuntimeResourceID = INDEX_NONE;

		check(Resources->PersistentHash != NANITE_INVALID_PERSISTENT_HASH);
		int32 NumRemoved = PersistentHashResourceMap.Remove(Resources->PersistentHash, Resources);
		check(NumRemoved == 1);
		Resources->PersistentHash = NANITE_INVALID_PERSISTENT_HASH;
		
		PendingAdds.Remove( Resources );
		NumResources--;
	}
}

FResources* FStreamingManager::GetResources(uint32 RuntimeResourceID)
{
	if (RuntimeResourceID != INDEX_NONE)
	{
		const uint32 RootPageIndex = RuntimeResourceID & NANITE_MAX_GPU_PAGES_MASK;
		if (RootPageIndex < (uint32)RootPageInfos.Num())
		{
			FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
			if (RootPageInfo.RuntimeResourceID == RuntimeResourceID)
			{
				return RootPageInfo.Resources;
			}
		}
	}
	return nullptr;
}

FStreamingManager::FRootPageInfo* FStreamingManager::GetRootPage(uint32 RuntimeResourceID)
{
	if (RuntimeResourceID != INDEX_NONE)
	{
		const uint32 RootPageIndex = RuntimeResourceID & NANITE_MAX_GPU_PAGES_MASK;
		if (RootPageIndex < (uint32)RootPageInfos.Num())
		{
			FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
			if (RootPageInfo.RuntimeResourceID == RuntimeResourceID)
			{
				return &RootPageInfo;
			}
		}
	}
	return nullptr;
}

FRDGBuffer* FStreamingManager::GetStreamingRequestsBuffer(FRDGBuilder& GraphBuilder) const
{
	return ReadbackManager->GetStreamingRequestsBuffer(GraphBuilder);
}

FRDGBufferSRV* FStreamingManager::GetHierarchySRV(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(Hierarchy.DataBuffer));
}

FRDGBufferSRV* FStreamingManager::GetClusterPageDataSRV(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer));
}

FRDGBufferSRV* FStreamingManager::GetImposterDataSRV(FRDGBuilder& GraphBuilder) const
{
	return GraphBuilder.CreateSRV(GraphBuilder.RegisterExternalBuffer(ImposterData.DataBuffer));
}

void FStreamingManager::RegisterStreamingPage(uint32 RegisteredPageIndex, const FPageKey& Key)
{
	LLM_SCOPE_BYTAG(Nanite);

	FResources* Resources = GetResources( Key.RuntimeResourceID );
	check( Resources != nullptr );
	check( !Resources->IsRootPage(Key.PageIndex) );
	
	TArray< FPageStreamingState >& PageStreamingStates = Resources->PageStreamingStates;
	FPageStreamingState& PageStreamingState = PageStreamingStates[ Key.PageIndex ];
	
	const uint32 VirtualPageRangeStart = RootPageInfos[Resources->RootPageIndex].VirtualPageRangeStart;

	FRegisteredPageDependencies& Dependencies = RegisteredPageDependencies[RegisteredPageIndex];
	Dependencies.Reset();

	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		uint32 DependencyPageIndex = Resources->PageDependencies[ PageStreamingState.DependenciesStart + i ];
		if( Resources->IsRootPage( DependencyPageIndex ) )
			continue;

		const uint32 DependencyVirtualPageIndex = VirtualPageRangeStart + DependencyPageIndex;
		const uint32 DependencyRegisteredPageIndex = RegisteredVirtualPages[DependencyVirtualPageIndex].RegisteredPageIndex;
		check(DependencyRegisteredPageIndex != INDEX_NONE);
		
		FRegisteredPage& DependencyPage = RegisteredPages[DependencyRegisteredPageIndex];
		check(DependencyPage.RefCount != 0xFF);
		DependencyPage.RefCount++;
		Dependencies.Add(VirtualPageRangeStart + DependencyPageIndex);
	}
	
	FRegisteredPage& RegisteredPage = RegisteredPages[RegisteredPageIndex];
	RegisteredPage = FRegisteredPage();
	RegisteredPage.Key = Key;
	RegisteredPage.VirtualPageIndex = VirtualPageRangeStart + Key.PageIndex;
	
	RegisteredVirtualPages[RegisteredPage.VirtualPageIndex].RegisteredPageIndex = RegisteredPageIndex;
	MoveToEndOfLRUList(RegisteredPageIndex);
}

void FStreamingManager::UnregisterStreamingPage( const FPageKey& Key )
{
	LLM_SCOPE_BYTAG(Nanite);
	
	if( Key.RuntimeResourceID == INDEX_NONE)
	{
		return;
	}

	const FRootPageInfo* RootPage = GetRootPage(Key.RuntimeResourceID);
	check(RootPage);
	const FResources* Resources = RootPage->Resources;
	check( Resources != nullptr );
	check( !Resources->IsRootPage(Key.PageIndex) );

	const uint32 VirtualPageRangeStart = RootPage->VirtualPageRangeStart;

	const uint32 RegisteredPageIndex = RegisteredVirtualPages[VirtualPageRangeStart + Key.PageIndex].RegisteredPageIndex;
	check(RegisteredPageIndex != INDEX_NONE);
	FRegisteredPage& RegisteredPage = RegisteredPages[RegisteredPageIndex];

	// Decrement reference counts of dependencies.
	const TArray< FPageStreamingState >& PageStreamingStates = Resources->PageStreamingStates;
	const FPageStreamingState& PageStreamingState = PageStreamingStates[ Key.PageIndex ];
	for( uint32 i = 0; i < PageStreamingState.DependenciesNum; i++ )
	{
		const uint32 DependencyPageIndex = Resources->PageDependencies[ PageStreamingState.DependenciesStart + i ];
		if( Resources->IsRootPage( DependencyPageIndex ) )
			continue;

		const uint32 DependencyRegisteredPageIndex = RegisteredVirtualPages[VirtualPageRangeStart + DependencyPageIndex].RegisteredPageIndex;
		RegisteredPages[DependencyRegisteredPageIndex].RefCount--;
	}
	check(RegisteredPage.RefCount == 0);

	RegisteredVirtualPages[RegisteredPage.VirtualPageIndex] = FVirtualPage();
	RegisteredPage = FRegisteredPage();
	RegisteredPageDependencies[RegisteredPageIndex].Reset();
}

bool FStreamingManager::ArePageDependenciesCommitted(uint32 RuntimeResourceID, uint32 DependencyPageStart, uint32 DependencyPageNum)
{
	for (uint32 i = 0; i < DependencyPageNum; i++)
	{
		FPageKey DependencyKey = { RuntimeResourceID, DependencyPageStart + i };
		uint32* DependencyStreamingPageIndex = ResidentPageMap.Find(DependencyKey);
		if (DependencyStreamingPageIndex == nullptr || ResidentPages[*DependencyStreamingPageIndex].Key != DependencyKey)	// Is the page going to be committed after this batch and does it already have its fixupchunk loaded?
		{
			return false;
		}
	}
	return true;
}

static uint32 GPUPageIndexToGPUOffset(uint32 MaxStreamingPages, uint32 PageIndex)
{
	return (FMath::Min(PageIndex, MaxStreamingPages) << NANITE_STREAMING_PAGE_GPU_SIZE_BITS) + ((uint32)FMath::Max((int32)PageIndex - (int32)MaxStreamingPages, 0) << NANITE_ROOT_PAGE_GPU_SIZE_BITS);
}

static void ValidateFixupChunk(const FFixupChunk& FixupChunk)
{
	const bool bValid =	FixupChunk.Header.NumClusters > 0 &&
						FixupChunk.Header.NumHierarchyFixups > 0 &&
						FixupChunk.Header.Magic == NANITE_FIXUP_MAGIC;
	if (!bValid)
	{
		UE_LOG(LogNaniteStreaming, Error,
			TEXT("Encountered a corrupt fixup chunk. Magic: %4X NumClusters: %d, NumClusterFixups: %d, NumHierarchyFixups: %d, This should never happen."),
			FixupChunk.Header.Magic,
			FixupChunk.Header.NumClusters,
			FixupChunk.Header.NumClusterFixups,
			FixupChunk.Header.NumHierarchyFixups
		);
	}
}
	

// Applies the fixups required to install/uninstall a page.
// Hierarchy references are patched up and leaf flags of parent clusters are set accordingly.
void FStreamingManager::ApplyFixups( const FFixupChunk& FixupChunk, const FResources& Resources, bool bUninstall )
{
	LLM_SCOPE_BYTAG(Nanite);
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_ApplyFixup);

	ValidateFixupChunk(FixupChunk);
	
	const uint32 RuntimeResourceID = Resources.RuntimeResourceID;
	const uint32 HierarchyOffset = Resources.HierarchyOffset;
	
	// Fixup clusters
	for( uint32 i = 0; i < FixupChunk.Header.NumClusterFixups; i++ )
	{
		const FClusterFixup& Fixup = FixupChunk.GetClusterFixup( i );

		bool bPageDependenciesCommitted = bUninstall || ArePageDependenciesCommitted(RuntimeResourceID, Fixup.GetPageDependencyStart(), Fixup.GetPageDependencyNum());
		if (!bPageDependenciesCommitted)
			continue;
		
		uint32 TargetPageIndex = Fixup.GetPageIndex();
		uint32 TargetGPUPageIndex = INDEX_NONE;
		uint32 NumTargetPageClusters = 0;

		if( Resources.IsRootPage( TargetPageIndex ) )
		{
			TargetGPUPageIndex = MaxStreamingPages + Resources.RootPageIndex + TargetPageIndex;
			NumTargetPageClusters = RootPageInfos[ Resources.RootPageIndex + TargetPageIndex ].NumClusters;
		}
		else
		{
			FPageKey TargetKey = { RuntimeResourceID, TargetPageIndex };
			uint32* TargetResidentPageIndex = ResidentPageMap.Find( TargetKey );

			check( bUninstall || TargetResidentPageIndex);
			if (TargetResidentPageIndex)
			{
				const uint32 GPUPageIndex = *TargetResidentPageIndex;
				FFixupChunk& TargetFixupChunk = *ResidentPageFixupChunks[GPUPageIndex];
				check(ResidentPages[GPUPageIndex].Key == TargetKey);

				NumTargetPageClusters = TargetFixupChunk.Header.NumClusters;
				check(Fixup.GetClusterIndex() < NumTargetPageClusters);

				TargetGPUPageIndex = GPUPageIndex;
			}
		}
		
		if(TargetGPUPageIndex != INDEX_NONE)
		{
			const uint32 ClusterIndex = Fixup.GetClusterIndex();
			AddClusterLeafFlagUpdate(MaxStreamingPages, TargetGPUPageIndex, ClusterIndex, NumTargetPageClusters, false, bUninstall);
		}
	}

	// Fixup hierarchy
	for( uint32 i = 0; i < FixupChunk.Header.NumHierarchyFixups; i++ )
	{
		const FHierarchyFixup& Fixup = FixupChunk.GetHierarchyFixup( i );

		bool bPageDependenciesCommitted = bUninstall || ArePageDependenciesCommitted(RuntimeResourceID, Fixup.GetPageDependencyStart(), Fixup.GetPageDependencyNum());
		if (!bPageDependenciesCommitted)
			continue;

		FPageKey TargetKey = { RuntimeResourceID, Fixup.GetPageIndex() };
		uint32 TargetGPUPageIndex = INDEX_NONE;
		if (!bUninstall)
		{
			if (Resources.IsRootPage(TargetKey.PageIndex))
			{
				TargetGPUPageIndex = MaxStreamingPages + Resources.RootPageIndex + TargetKey.PageIndex;
			}
			else
			{
				uint32* TargetResidentPageIndex = ResidentPageMap.Find(TargetKey);
				check(TargetResidentPageIndex);
				FResidentPage& TargetPage = ResidentPages[*TargetResidentPageIndex];
				check(TargetPage.Key == TargetKey);
				TargetGPUPageIndex = *TargetResidentPageIndex;
			}
		}
		
		// Uninstalls are unconditional. The same uninstall might happen more than once.
		// If this page is getting uninstalled it also means it wont be reinstalled and any split groups can't be satisfied, so we can safely uninstall them.	
		
		uint32 HierarchyNodeIndex = Fixup.GetNodeIndex();
		check( HierarchyNodeIndex < Resources.NumHierarchyNodes );
		uint32 ChildIndex = Fixup.GetChildIndex();
		uint32 ChildStartReference = bUninstall ? 0xFFFFFFFFu : ( ( TargetGPUPageIndex << NANITE_MAX_CLUSTERS_PER_PAGE_BITS ) | Fixup.GetClusterGroupPartStartIndex() );
		uint32 Offset = ( size_t )&( ( (FPackedHierarchyNode*)0 )[ HierarchyNodeIndex ].Misc1[ ChildIndex ].ChildStartReference );

		AddHierarchyUpdate( HierarchyOffset + (Offset / sizeof( uint32 )), ChildStartReference );
	}
}

void FStreamingManager::UninstallGPUPage(uint32 GPUPageIndex, bool bApplyFixup)
{
	FResidentPage& ResidentPage = ResidentPages[GPUPageIndex];

	// Uninstall GPU page
	if (ResidentPage.Key.RuntimeResourceID != INDEX_NONE)
	{
		// Apply fixups to uninstall page. No need to fix up anything if resource is gone.
		FResources* Resources = GetResources(ResidentPage.Key.RuntimeResourceID);
		if (Resources)
		{
			if (bApplyFixup)
			{
				ApplyFixups(*ResidentPageFixupChunks[GPUPageIndex], *Resources, true);
			}

			Resources->NumResidentClusters -= ResidentPageFixupChunks[GPUPageIndex]->Header.NumClusters;
			check(Resources->NumResidentClusters > 0);
			check(Resources->NumResidentClusters <= Resources->NumClusters);
			ModifiedResources.Add(ResidentPage.Key.RuntimeResourceID, Resources->NumResidentClusters);
		}
		HierarchyDepthManager->Remove(ResidentPage.MaxHierarchyDepth);
	}

	ResidentPage.Key.RuntimeResourceID = INDEX_NONE;
}

void FStreamingManager::AddClusterLeafFlagUpdate(uint32 NumStreamingPages, uint32 GPUPageIndex, uint32 ClusterIndex, uint32 NumClusters, bool bReset, bool bUninstall)
{
	const uint32 FlagsOffset = offsetof(FPackedCluster, Flags_NumClusterBoneInfluences);
	const uint32 Offset = GPUPageIndexToGPUOffset(NumStreamingPages, GPUPageIndex) + NANITE_GPU_PAGE_HEADER_SIZE + ((FlagsOffset >> 4) * NumClusters + ClusterIndex) * 16 + (FlagsOffset & 15);
	check((Offset & 3u) == 0);

	uint32 OffsetAndFlags = Offset;
	OffsetAndFlags |= bReset ? 2u : 0u;
	OffsetAndFlags |= bUninstall ? 1u : 0u;

	ClusterLeafFlagUpdates.Add(OffsetAndFlags);
}

void FStreamingManager::FlushClusterLeafFlagUpdates(FRDGBuilder& GraphBuilder, FRDGBuffer* ClusterPageDataBuffer)
{
	AddPass_UpdateClusterLeafFlags(GraphBuilder, GraphBuilder.CreateUAV(ClusterPageDataBuffer), ClusterLeafFlagUpdates);
	ClusterLeafFlagUpdates.Empty();
}

void FStreamingManager::AddHierarchyUpdate(uint32 BufferOffset, uint32 Value)
{
	HierarchyUpdates.Add({ BufferOffset, Value });
}

void FStreamingManager::FlushHierarchyUpdates(FRDGBuilder& GraphBuilder)
{
	if (HierarchyUpdates.Num() > 0)
	{
		Hierarchy.UploadBuffer.Init(GraphBuilder, HierarchyUpdates.Num(), sizeof(uint32), false, TEXT("Nanite.StreamingManager.HierarchyUpload"));
		for (const FHierarchyUpdate& Update : HierarchyUpdates)
		{
			Hierarchy.UploadBuffer.Add(Update.Offset, &Update.Value);
		}
		Hierarchy.UploadBuffer.ResourceUploadTo(GraphBuilder, GraphBuilder.RegisterExternalBuffer(Hierarchy.DataBuffer));
		HierarchyUpdates.Empty();
	}
}

void FStreamingManager::InstallReadyPages( uint32 NumReadyPages )
{
	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::InstallReadyPages);
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_InstallReadyPages);

	if (NumReadyPages == 0)
		return;

	const uint32 StartPendingPageIndex = ( NextPendingPageIndex + MaxPendingPages - NumPendingPages ) % MaxPendingPages;

	struct FUploadTask
	{
		FPendingPage* PendingPage = nullptr;
		uint8* Dst = nullptr;
		const uint8* Src = nullptr;
		uint32 SrcSize = 0;
	};

#if WITH_EDITOR
	TMap<FResources*, const uint8*> ResourceToBulkPointer;
#endif

	TArray<FUploadTask> UploadTasks;
	UploadTasks.AddDefaulted(NumReadyPages);

	// Install ready pages
	{
		// Batched page install:
		// GPU uploads are unordered, so we need to make sure we have no overlapping writes.
		// For actual page uploads, we only upload the last page that ends up on a given GPU page.

		// Fixups are handled with set of UploadBuffers that are executed AFTER page upload.
		// To ensure we don't end up fixing up the same addresses more than once, we only perform the fixup associated with the first uninstall and the last install on a given GPU page.
		// If a page ends up being both installed and uninstalled in the same frame, we only install it to prevent a race.
		// Uninstall fixup depends on StreamingPageFixupChunks that is also updated by installs. To prevent races we perform all uninstalls before installs.
		
		// Calculate first and last Pending Page Index update for each GPU page.
		TMap<uint32, uint32> GPUPageToLastPendingPageIndex;
		for (uint32 i = 0; i < NumReadyPages; i++)
		{
			uint32 PendingPageIndex = (StartPendingPageIndex + i) % MaxPendingPages;
			FPendingPage& PendingPage = PendingPages[PendingPageIndex];
			
			// Update when the GPU page was touched for the last time.
			// This also includes pages from deleted resources. This is intentional as the corresponding uninstall still needs to happen.
			GPUPageToLastPendingPageIndex.Add(PendingPage.GPUPageIndex, PendingPageIndex);
		}

		TSet<FPageKey> BatchNewPageKeys;
		for (auto& Elem : GPUPageToLastPendingPageIndex)
		{
			uint32 GPUPageIndex = Elem.Key;

			// Remove uninstalled pages from streaming map, so we won't try to do uninstall fixup on them.
			FResidentPage& ResidentPage = ResidentPages[GPUPageIndex];
			if (ResidentPage.Key.RuntimeResourceID != INDEX_NONE)
			{
				ResidentPageMap.Remove(ResidentPage.Key);
			}

			// Mark newly installed page
			FPendingPage& PendingPage = PendingPages[Elem.Value];
			BatchNewPageKeys.Add(PendingPage.InstallKey);
		}

		// Uninstall pages
		// We are uninstalling pages in a separate pass as installs will also overwrite the GPU page fixup information we need for uninstalls.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(UninstallFixup);
			for (auto& Elem : GPUPageToLastPendingPageIndex)
			{
				const uint32 GPUPageIndex = Elem.Key;

				// Only uninstall if the page is not going to be installed again to prevent GPU race between install and uninstall
				const bool bApplyFixup = !BatchNewPageKeys.Contains(ResidentPages[GPUPageIndex].Key); 
				UninstallGPUPage(GPUPageIndex, bApplyFixup);
			}
		}

		// Commit to streaming map, so install fixups will happen on all pages
		for (auto& Elem : GPUPageToLastPendingPageIndex)
		{
			uint32 GPUPageIndex = Elem.Key;
			uint32 LastPendingPageIndex = Elem.Value;
			FPendingPage& PendingPage = PendingPages[LastPendingPageIndex];

			FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
			if (Resources)
			{
				ResidentPageMap.Add(PendingPage.InstallKey, GPUPageIndex);
			}
		}

		// Install pages
		// Must be processed in PendingPages order so FFixupChunks are loaded when we need them.
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(InstallReadyPages);
			uint32 NumInstalledPages = 0;
			for (uint32 TaskIndex = 0; TaskIndex < NumReadyPages; TaskIndex++)
			{
				uint32 PendingPageIndex = (StartPendingPageIndex + TaskIndex) % MaxPendingPages;
				FPendingPage& PendingPage = PendingPages[PendingPageIndex];

				FUploadTask& UploadTask = UploadTasks[TaskIndex];
				UploadTask.PendingPage = &PendingPage;

				FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
				uint32 LastPendingPageIndex = GPUPageToLastPendingPageIndex.FindChecked(PendingPages[PendingPageIndex].GPUPageIndex);
				if (PendingPageIndex != LastPendingPageIndex || !Resources)
				{
					continue;	// Skip resource install. Resource no longer exists or page has already been overwritten.
				}

				TArray< FPageStreamingState >& PageStreamingStates = Resources->PageStreamingStates;
				const FPageStreamingState& PageStreamingState = PageStreamingStates[ PendingPage.InstallKey.PageIndex ];
				FResidentPage* ResidentPage = &ResidentPages[ PendingPage.GPUPageIndex ];

				ResidentPageMap.Add(PendingPage.InstallKey, PendingPage.GPUPageIndex);

				const uint8* SrcPtr;
#if WITH_EDITOR
				if(PendingPage.State == FPendingPage::EState::DDC_Ready)
				{
					check(Resources->ResourceFlags & NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC);
					SrcPtr = (const uint8*)PendingPage.SharedBuffer.GetData();
				}
				else if(PendingPage.State == FPendingPage::EState::Memory)
				{
					// Make sure we only lock each resource BulkData once.
					const uint8* BulkDataPtr = ResourceToBulkPointer.FindRef(Resources);
					if (BulkDataPtr)
					{
						SrcPtr = BulkDataPtr + PageStreamingState.BulkOffset;
					}
					else
					{
						FByteBulkData& BulkData = Resources->StreamablePages;
						check(BulkData.IsBulkDataLoaded() && BulkData.GetBulkDataSize() > 0);
						BulkDataPtr = (const uint8*)BulkData.LockReadOnly();
						ResourceToBulkPointer.Add(Resources, BulkDataPtr);
						SrcPtr = BulkDataPtr + PageStreamingState.BulkOffset;
					}
				}
				else
#endif
				{
#if WITH_EDITOR
					check(PendingPage.State == FPendingPage::EState::Disk);
#endif
					SrcPtr = PendingPage.RequestBuffer.GetData();
				}
				
				ValidateFixupChunk(*(const FFixupChunk*)SrcPtr);
				const uint32 FixupChunkSize = ((const FFixupChunk*)SrcPtr)->GetSize();
				FFixupChunk* FixupChunk = (FFixupChunk*)FMemory::Realloc(ResidentPageFixupChunks[PendingPage.GPUPageIndex], FixupChunkSize, sizeof(uint16));	// TODO: Get rid of this alloc. Can we come up with a tight conservative bound, so we could preallocate?
				ResidentPageFixupChunks[PendingPage.GPUPageIndex] = FixupChunk;
				ResidentPage->MaxHierarchyDepth = PageStreamingState.MaxHierarchyDepth;
				HierarchyDepthManager->Add(ResidentPage->MaxHierarchyDepth);

				FMemory::Memcpy(FixupChunk, SrcPtr, FixupChunkSize);

				Resources->NumResidentClusters += FixupChunk->Header.NumClusters;
				check(Resources->NumResidentClusters > 0);
				//check(Resources->NumResidentClusters <= Resources->NumClusters); // Temporary workaround: NumClusters from cooked data is not always correct for Geometry Collections UE-194917
				ModifiedResources.Add(PendingPage.InstallKey.RuntimeResourceID, Resources->NumResidentClusters);

				// Build list of GPU page dependencies
				GPUPageDependencies.Reset();
				if(PageStreamingState.Flags & NANITE_PAGE_FLAG_RELATIVE_ENCODING)
				{
					for (uint32 i = 0; i < PageStreamingState.DependenciesNum; i++)
					{
						const uint32 DependencyPageIndex = Resources->PageDependencies[PageStreamingState.DependenciesStart + i];
						if (Resources->IsRootPage(DependencyPageIndex))
						{
							GPUPageDependencies.Add(MaxStreamingPages + Resources->RootPageIndex + DependencyPageIndex);
						}
						else
						{
							FPageKey DependencyKey = { PendingPage.InstallKey.RuntimeResourceID, DependencyPageIndex };
							uint32* DependencyStreamingPageIndex = ResidentPageMap.Find(DependencyKey);
							check(DependencyStreamingPageIndex != nullptr);
							GPUPageDependencies.Add(*DependencyStreamingPageIndex);
						}
					}
				}
			
				uint32 PageOffset = GPUPageIndexToGPUOffset( MaxStreamingPages, PendingPage.GPUPageIndex );
				uint32 DataSize = PageStreamingState.BulkSize - FixupChunkSize;
				check(NumInstalledPages < MaxPageInstallsPerUpdate);

				const FPageKey GPUPageKey = FPageKey{ PendingPage.InstallKey.RuntimeResourceID, PendingPage.GPUPageIndex };

				UploadTask.PendingPage = &PendingPage;
				UploadTask.Dst = PageUploader->Add_GetRef(DataSize, FixupChunk->Header.NumClusters, PageOffset, GPUPageKey, GPUPageDependencies);
				UploadTask.Src = SrcPtr + FixupChunkSize;
				UploadTask.SrcSize = DataSize;
				NumInstalledPages++;

				// Apply fixups to install page
				ResidentPage->Key = PendingPage.InstallKey;
				ApplyFixups( *FixupChunk, *Resources, false );
			}
		}
	}

	// Upload pages
	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_UploadTask);
		ParallelFor(UploadTasks.Num(), [&UploadTasks](int32 i)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(CopyPageTask);
			const FUploadTask& Task = UploadTasks[i];
		
			if(Task.Dst)	// Dst can be 0 if we skipped install in InstallReadyPages.
			{
				FMemory::Memcpy(Task.Dst, Task.Src, Task.SrcSize);
			}
		#if !DEBUG_TRANSCODE_PAGES_REPEATEDLY
		#if WITH_EDITOR
			Task.PendingPage->SharedBuffer.Reset();
		#else
			check(Task.PendingPage->Request.IsCompleted());
			Task.PendingPage->Request.Reset();
		#endif
		#endif
		});
	}

#if WITH_EDITOR
	// Unlock BulkData
	for (auto it : ResourceToBulkPointer)
	{
		FResources* Resources = it.Key;
		FByteBulkData& BulkData = Resources->StreamablePages;
		BulkData.Unlock();
	}
#endif
}

FRDGBuffer* FStreamingManager::ResizePoolAllocationIfNeeded(FRDGBuilder& GraphBuilder)
{
	const uint32 OldMaxStreamingPages = MaxStreamingPages;

	ClusterPageData.Allocator.Consolidate();
	const uint32 NumRootPages = (uint32)ClusterPageData.Allocator.GetMaxSize();
	const bool bReservedResource = EnumHasAnyFlags(ClusterPageData.DataBuffer->Desc.Usage, EBufferUsageFlags::ReservedResource);

	if (GNaniteStreamingPoolResize != 0)
	{
		UpdatePageConfiguration();
	}

	const bool bAllowGrow = (GNaniteStreamingDynamicallyGrowAllocations != 0);
	const bool bIgnoreInitialRootPages = (GNaniteStreamingReservedResourceIgnoreInitialRootAllocation != 0) && bReservedResource;
	
	uint32 NumAllocatedRootPages;
	if (bReservedResource)
	{
		// Allocate pages in 16MB chunks to reduce the number of page table updates
		const uint32 AllocationGranularityInPages = (16 << 20) / NANITE_ROOT_PAGE_GPU_SIZE;

		NumAllocatedRootPages = bIgnoreInitialRootPages ? 0u : NumInitialRootPages;
		if (NumRootPages > NumAllocatedRootPages)
		{
			NumAllocatedRootPages = FMath::DivideAndRoundUp(NumRootPages, AllocationGranularityInPages) * AllocationGranularityInPages;
			NumAllocatedRootPages = FMath::Min(NumAllocatedRootPages, bAllowGrow ? MaxRootPages : NumInitialRootPages);
		}
	}
	else
	{
		NumAllocatedRootPages = NumInitialRootPages;
		if (NumRootPages > NumInitialRootPages && bAllowGrow)
		{
			NumAllocatedRootPages = FMath::Clamp(RoundUpToSignificantBits(NumRootPages, 2), NumInitialRootPages, MaxRootPages);
		}		
	}

#if DEBUG_ALLOCATION_STRESS_TEST
	NumAllocatedRootPages = NumRootPages;
#endif

	check(NumAllocatedRootPages >= NumRootPages);	// Root pages just don't fit!
	StatNumAllocatedRootPages = NumAllocatedRootPages;

	SET_DWORD_STAT(STAT_NaniteStreaming08_AllocatedRootPages, NumAllocatedRootPages);
	SET_DWORD_STAT(STAT_NaniteStreaming09_RootPageLimit, MaxRootPages);
	SET_FLOAT_STAT(STAT_NaniteStreaming13_AllocatedRootPagesSizeMB, NumAllocatedRootPages * (NANITE_ROOT_PAGE_GPU_SIZE / 1048576.0f));
	
	const uint32 NumAllocatedPages = MaxStreamingPages + NumAllocatedRootPages;
	const uint64 AllocatedPagesSize = (uint64(NumAllocatedRootPages) << NANITE_ROOT_PAGE_GPU_SIZE_BITS) + (uint64(MaxStreamingPages) << NANITE_STREAMING_PAGE_GPU_SIZE_BITS);
	check(NumAllocatedPages <= NANITE_MAX_GPU_PAGES);
	check(AllocatedPagesSize <= (uint64(GetMaxPagePoolSizeInMB()) << 20));

	SET_DWORD_STAT(STAT_NaniteStreaming0A_StreamingPoolPages, MaxStreamingPages);
	SET_FLOAT_STAT(STAT_NaniteStreaming14_StreamingPoolSizeMB, MaxStreamingPages * (NANITE_STREAMING_PAGE_GPU_SIZE / 1048576.0f));
	SET_FLOAT_STAT(STAT_NaniteStreaming12_TotalPoolSizeMB, AllocatedPagesSize / 1048576.0f);
	SET_FLOAT_STAT(STAT_NaniteStreaming15_TotalPoolSizeLimitMB, (float)GetMaxPagePoolSizeInMB());

#if CSV_PROFILER_STATS
	if (ClusterPageData.DataBuffer && AllocatedPagesSize > ClusterPageData.DataBuffer->GetAlignedSize())
	{
		if (!bReservedResource)
		{
			CSV_EVENT(NaniteStreaming, TEXT("GrowPoolAllocation"));
		}
	}
#endif

	FRDGBuffer* ClusterPageDataBuffer = nullptr;

	const bool bResetStreamingState = bClusterPageDataAllocated && ((MaxStreamingPages != OldMaxStreamingPages) || bPendingPoolReset);
	if (bResetStreamingState)
	{
		if (RootPageInfos.Num() > 0)
		{
			// Reset root page leaf flags
			check(ClusterLeafFlagUpdates.Num() == 0);
			for (uint32 RootPageIndex = 0; RootPageIndex < (uint32)RootPageInfos.Num(); RootPageIndex++)
			{
				const FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
				if (RootPageInfo.RuntimeResourceID != INDEX_NONE)
				{
					const uint32 GPUPageIndex = OldMaxStreamingPages + RootPageIndex;
					const uint32 NumClusters = RootPageInfo.NumClusters;
					for (uint32 ClusterIndex = 0; ClusterIndex < NumClusters; ClusterIndex++)
					{
						AddClusterLeafFlagUpdate(OldMaxStreamingPages, GPUPageIndex, ClusterIndex, NumClusters, true, false);
					}
				}
			}

			ClusterPageDataBuffer = GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer);
			FlushClusterLeafFlagUpdates(GraphBuilder, ClusterPageDataBuffer);
		}

		// Uninstall any streaming pages
		for (uint32 i = 0; i < OldMaxStreamingPages; i++)
		{
			UninstallGPUPage(i, false);
		}

		{
			// Update hierarchy. Relocate pointers to root pages and detach any streaming pages.

			TSet<FResources*> ResourcesSet;
			for (FRootPageInfo& RootPage : RootPageInfos)
			{
				if (RootPage.Resources)
				{
					ResourcesSet.Add(RootPage.Resources);
				}
			}

			const uint32 NumNodesPerGroup = 64 / NANITE_MAX_BVH_NODE_FANOUT;
			TArray<FUintVector3> Relocations;
			for (FResources* Resources : ResourcesSet)
			{
				const uint32 NumHierarchyNodes = Resources->NumHierarchyNodes;
				for (uint32 Offset = 0; Offset < NumHierarchyNodes; Offset += NumNodesPerGroup)
				{
					uint32 Num = FMath::Min(NumHierarchyNodes - Offset, NumNodesPerGroup);
					Relocations.Add(FUintVector3(Resources->HierarchyOffset, Offset, Num));
				}
			}

			const FRDGBufferRef RelocationsBuffer = CreateStructuredBuffer(	GraphBuilder, TEXT("RelocationsBuffer"), Relocations.GetTypeSize(),
																			Relocations.Num(), Relocations.GetData(), Relocations.Num() * Relocations.GetTypeSize());

			const FRDGBufferRef HierarchyBuffer = GraphBuilder.RegisterExternalBuffer(Hierarchy.DataBuffer);

			FRelocateHierarchy_CS::FParameters* PassParameters = GraphBuilder.AllocParameters<FRelocateHierarchy_CS::FParameters>();
			PassParameters->OldRootPageStart = OldMaxStreamingPages;
			PassParameters->NewRootPageStart = MaxStreamingPages;
			PassParameters->NumRelocations = Relocations.Num();
			PassParameters->RelocationsBuffer = GraphBuilder.CreateSRV(RelocationsBuffer);
			PassParameters->HierarchyBufferUAV = GraphBuilder.CreateUAV(HierarchyBuffer);

			auto ComputeShader = GetGlobalShaderMap(GMaxRHIFeatureLevel)->GetShader<FRelocateHierarchy_CS>();
			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("RelocateHierarchy"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCountWrapped(Relocations.Num())
			);
		}

		const uint32 RootPagesDataSize = NumRootPages * NANITE_ROOT_PAGE_GPU_SIZE;
		if (bReservedResource)
		{
			// Reserved resource path: Move root pages without using temporary memory and commit/decommit physical pages as needed.
			if (MaxStreamingPages < OldMaxStreamingPages)
			{
				// Smaller allocation: Move root pages down then resize
				ClusterPageDataBuffer = GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer);
				AddPass_Memmove(GraphBuilder, GraphBuilder.CreateUAV(ClusterPageDataBuffer), MaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, OldMaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, RootPagesDataSize);				
				ClusterPageDataBuffer = ResizeByteAddressBufferIfNeededWithCurrentLLMTag(GraphBuilder, ClusterPageData.DataBuffer, AllocatedPagesSize, TEXT("Nanite.StreamingManager.ClusterPageData"));
			}
			else if (MaxStreamingPages > OldMaxStreamingPages)
			{
				// Larger allocation: Resize then move allocation
				ClusterPageDataBuffer = ResizeByteAddressBufferIfNeededWithCurrentLLMTag(GraphBuilder, ClusterPageData.DataBuffer, AllocatedPagesSize, TEXT("Nanite.StreamingManager.ClusterPageData"));
				AddPass_Memmove(GraphBuilder, GraphBuilder.CreateUAV(ClusterPageDataBuffer), MaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, OldMaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, RootPagesDataSize);
			}
		}
		else
		{
			// Non-reserved resource path: Make new allocation and copy root pages over. Temporary peak in memory usage when both allocations need to be live at the same time.

			// TODO: We could lower the theoretical peak memory usage here by copying via a third temporary allocation that is only the size of the root pages.
			//       Investigate if that would even save anything. If RDG overlaps the lifetime of the two buffer ClusterPageData allocations,
			//		 it would actually be worse to introduce a 3rd allocation.
			//		 It might not be worthwhile if reserved resources will be supported on all relevant platforms soon.

			FRDGBuffer* OldClusterPageDataBuffer = GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer);
			ClusterPageDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateByteAddressDesc(AllocatedPagesSize), TEXT("Nanite.StreamingManager.ClusterPageData"));
			AddCopyBufferPass(GraphBuilder, ClusterPageDataBuffer, MaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, OldClusterPageDataBuffer, OldMaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE, RootPagesDataSize);
			ClusterPageData.DataBuffer = GraphBuilder.ConvertToExternalBuffer(ClusterPageDataBuffer);
		}
		
		// Clear cluster page data just to be sure we aren't accidentally depending on stale data
		FMemsetResourceParams MemsetParams = {};
		MemsetParams.Count = MaxStreamingPages * NANITE_STREAMING_PAGE_GPU_SIZE / 4;
		MemsetParams.Value = 0;
		MemsetParams.DstOffset = 0;
		MemsetResource(GraphBuilder, GraphBuilder.CreateUAV(ClusterPageDataBuffer), MemsetParams);
	
		ResetStreamingStateCPU();		
		bPendingPoolReset = false;
	}
	else
	{
		ClusterPageDataBuffer = ResizeByteAddressBufferIfNeededWithCurrentLLMTag(GraphBuilder, ClusterPageData.DataBuffer, AllocatedPagesSize, TEXT("Nanite.StreamingManager.ClusterPageData"));		
		bClusterPageDataAllocated = true;
	}

	RootPageInfos.SetNum(NumAllocatedRootPages);
	
	check(ClusterPageDataBuffer);
	return ClusterPageDataBuffer;
}

void FStreamingManager::ProcessNewResources(FRDGBuilder& GraphBuilder, FRDGBuffer* ClusterPageDataBuffer)
{
	LLM_SCOPE_BYTAG(Nanite);

	if (PendingAdds.Num() == 0)
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::ProcessNewResources);

	// Upload hierarchy for pending resources
	Hierarchy.Allocator.Consolidate();
	const uint32 NumAllocatedHierarchyDwords = FMath::RoundUpToPowerOfTwo(Hierarchy.Allocator.GetMaxSize());
	SET_FLOAT_STAT(STAT_NaniteStreaming11_HiearchySizeMB, NumAllocatedHierarchyDwords * sizeof(uint32) / 1048576.0f);
	FRDGBuffer* HierarchyDataBuffer = ResizeByteAddressBufferIfNeededWithCurrentLLMTag(GraphBuilder, Hierarchy.DataBuffer, NumAllocatedHierarchyDwords * sizeof(uint32), TEXT("Nanite.StreamingManager.Hierarchy"));
	Hierarchy.UploadBuffer.Init(GraphBuilder, Hierarchy.TotalUpload, sizeof(uint32), false, TEXT("Nanite.StreamingManager.HierarchyUpload"));

	FRDGBuffer* ImposterDataBuffer = nullptr;
	const bool bUploadImposters = GNaniteStreamingImposters && ImposterData.TotalUpload > 0;
	if (bUploadImposters)
	{
		check(NANITE_IMPOSTERS_SUPPORTED != 0);
		uint32 WidthInTiles = 12;
		uint32 TileSize = 12;
		uint32 AtlasBytes = FMath::Square( WidthInTiles * TileSize ) * sizeof( uint16 );
		ImposterData.Allocator.Consolidate();
		const uint32 NumAllocatedImposters = FMath::Max( RoundUpToSignificantBits(ImposterData.Allocator.GetMaxSize(), 2), (uint32)GNaniteStreamingNumInitialImposters );
		ImposterDataBuffer = ResizeByteAddressBufferIfNeededWithCurrentLLMTag(GraphBuilder, ImposterData.DataBuffer, NumAllocatedImposters * AtlasBytes, TEXT("Nanite.StreamingManager.ImposterData"));
		ImposterData.UploadBuffer.Init(GraphBuilder, ImposterData.TotalUpload, AtlasBytes, false, TEXT("Nanite.StreamingManager.ImposterDataUpload"));

		SET_FLOAT_STAT(STAT_NaniteStreaming10_ImpostersSizeMB, NumAllocatedImposters * AtlasBytes / 1048576.0f);
	}

	// Calculate total required size
	uint32 TotalPageSize = 0;
	uint32 TotalRootPages = 0;
	for (FResources* Resources : PendingAdds)
	{
		for (uint32 i = 0; i < Resources->NumRootPages; i++)
		{
			TotalPageSize += Resources->PageStreamingStates[i].PageSize;
		}

		TotalRootPages += Resources->NumRootPages;
	}

	FStreamingPageUploader RootPageUploader;
	RootPageUploader.Init(GraphBuilder, TotalRootPages, TotalPageSize, MaxStreamingPages);

	GPUPageDependencies.Reset();

	for (FResources* Resources : PendingAdds)
	{
		Resources->NumResidentClusters = 0;

		for (uint32 LocalPageIndex = 0; LocalPageIndex < Resources->NumRootPages; LocalPageIndex++)
		{
			const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[LocalPageIndex];

			const uint32 RootPageIndex = Resources->RootPageIndex + LocalPageIndex;
			const uint32 GPUPageIndex = MaxStreamingPages + RootPageIndex;

			const uint8* Ptr = Resources->RootData.GetData() + PageStreamingState.BulkOffset;
			const FFixupChunk& FixupChunk = *(FFixupChunk*)Ptr;
			ValidateFixupChunk(FixupChunk);
			const uint32 FixupChunkSize = FixupChunk.GetSize();
			const uint32 NumClusters = FixupChunk.Header.NumClusters;

			const FPageKey GPUPageKey = { Resources->RuntimeResourceID, GPUPageIndex };

			const uint32 PageDiskSize = PageStreamingState.PageSize;
			check(PageDiskSize == PageStreamingState.BulkSize - FixupChunkSize);
			const uint32 PageOffset = GPUPageIndexToGPUOffset(MaxStreamingPages, GPUPageIndex);

			uint8* Dst = RootPageUploader.Add_GetRef(PageDiskSize, NumClusters, PageOffset, GPUPageKey, GPUPageDependencies);

			FMemory::Memcpy(Dst, Ptr + FixupChunkSize, PageDiskSize);

			// Root node should only have fixups that depend on other non-root pages and cannot be satisfied yet.

			// Fixup hierarchy
			for (uint32 i = 0; i < FixupChunk.Header.NumHierarchyFixups; i++)
			{
				const FHierarchyFixup& Fixup = FixupChunk.GetHierarchyFixup(i);
				const uint32 HierarchyNodeIndex = Fixup.GetNodeIndex();
				check(HierarchyNodeIndex < (uint32)Resources->HierarchyNodes.Num());
				const uint32 ChildIndex = Fixup.GetChildIndex();
				const uint32 TargetGPUPageIndex = MaxStreamingPages + Resources->RootPageIndex + Fixup.GetPageIndex();
				const uint32 ChildStartReference = (TargetGPUPageIndex << NANITE_MAX_CLUSTERS_PER_PAGE_BITS) | Fixup.GetClusterGroupPartStartIndex();

				if (Fixup.GetPageDependencyNum() == 0)	// Only install part if it has no other dependencies
				{
					Resources->HierarchyNodes[HierarchyNodeIndex].Misc1[ChildIndex].ChildStartReference = ChildStartReference;
				}
			}

			FRootPageInfo& RootPageInfo = RootPageInfos[RootPageIndex];
			RootPageInfo.RuntimeResourceID = Resources->RuntimeResourceID;
			RootPageInfo.NumClusters = NumClusters;
			RootPageInfo.MaxHierarchyDepth = PageStreamingState.MaxHierarchyDepth;
			HierarchyDepthManager->Add(PageStreamingState.MaxHierarchyDepth);

			Resources->NumResidentClusters += NumClusters; // clusters in root pages are always streamed in
		}

		ModifiedResources.Add(Resources->RuntimeResourceID, Resources->NumResidentClusters);

		const uint32 HierarchyNodeSizeDwords = Resources->HierarchyNodes.Num() * NANITE_HIERARCHY_NODE_SLICE_SIZE_DWORDS;
		const uint32 AssemblyTransformSizeDwords = Resources->AssemblyTransforms.Num() * NANITE_ASSEMBLY_TRANSFORM_SIZE_DWORDS;
		Hierarchy.UploadBuffer.Add(Resources->HierarchyOffset, Resources->HierarchyNodes.GetData(), HierarchyNodeSizeDwords);
		if (AssemblyTransformSizeDwords > 0)
		{
			Hierarchy.UploadBuffer.Add(Resources->HierarchyOffset + HierarchyNodeSizeDwords, Resources->AssemblyTransforms.GetData(), AssemblyTransformSizeDwords);
		}
		if (bUploadImposters && Resources->ImposterAtlas.Num() > 0)
		{
			ImposterData.UploadBuffer.Add(Resources->ImposterIndex, Resources->ImposterAtlas.GetData());
		}

#if !WITH_EDITOR
		// We can't free the CPU data in editor builds because the resource might be kept around and used for cooking later.
		Resources->RootData.Empty();
		Resources->HierarchyNodes.Empty();
		Resources->ImposterAtlas.Empty();
#endif
	}

	{
		Hierarchy.TotalUpload = 0;
		Hierarchy.UploadBuffer.ResourceUploadTo(GraphBuilder, HierarchyDataBuffer);

		RootPageUploader.ResourceUploadTo(GraphBuilder, ClusterPageDataBuffer);

		if (bUploadImposters)
		{
			ImposterData.TotalUpload = 0;
			ImposterData.UploadBuffer.ResourceUploadTo(GraphBuilder, ImposterDataBuffer);
		}
	}

	PendingAdds.Reset();
}

struct FStreamingUpdateParameters
{
	FStreamingManager* StreamingManager = nullptr;
};

class FStreamingUpdateTask
{
public:
	explicit FStreamingUpdateTask(const FStreamingUpdateParameters& InParams) : Parameters(InParams) {}

	FStreamingUpdateParameters Parameters;

	void DoTask(ENamedThreads::Type CurrentThread, const FGraphEventRef& MyCompletionGraphEvent)
	{
		Parameters.StreamingManager->AsyncUpdate();
	}

	static ESubsequentsMode::Type	GetSubsequentsMode()	{ return ESubsequentsMode::TrackSubsequents; }
	ENamedThreads::Type				GetDesiredThread()		{ return ENamedThreads::AnyNormalThreadNormalTask; }
	FORCEINLINE TStatId				GetStatId() const		{ return TStatId(); }
};

uint32 FStreamingManager::DetermineReadyPages(uint32& TotalPageSize)
{
	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::DetermineReadyPages);

	const uint32 StartPendingPageIndex = (NextPendingPageIndex + MaxPendingPages - NumPendingPages) % MaxPendingPages;
	uint32 NumReadyPages = 0;
	
	uint64 UpdateTick = FPlatformTime::Cycles64();
	uint64 DeltaTick = PrevUpdateTick ? UpdateTick - PrevUpdateTick : 0;
	PrevUpdateTick = UpdateTick;

	TotalPageSize = 0;
	// Check how many pages are ready
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CheckReadyPages);

		for( uint32 i = 0; i < NumPendingPages && NumReadyPages < MaxPageInstallsPerUpdate; i++ )
		{
			uint32 PendingPageIndex = ( StartPendingPageIndex + i ) % MaxPendingPages;
			FPendingPage& PendingPage = PendingPages[ PendingPageIndex ];
			bool bFreePageFromStagingAllocator = false;
#if WITH_EDITOR
			if (PendingPage.State == FPendingPage::EState::DDC_Ready)
			{
				if (PendingPage.RetryCount > 0)
				{
					FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
					if (Resources)
					{
						UE_LOG(LogNaniteStreaming, Log, TEXT("Nanite DDC retry succeeded for '%s' (Page %d) after %d attempts."), *Resources->ResourceName, PendingPage.InstallKey.PageIndex, PendingPage.RetryCount);
					}
				}
			}
			else if (PendingPage.State == FPendingPage::EState::DDC_Pending)
			{
				break;
			}
			else if (PendingPage.State == FPendingPage::EState::DDC_Failed)
			{
				FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
				if (Resources)
				{
					// Resource is still there. Retry the request.
					PendingPage.State = FPendingPage::EState::DDC_Pending;
					PendingPage.RetryCount++;
					
					if(PendingPage.RetryCount == 0)	// Only warn on first retry to prevent spam
					{
						UE_LOG(LogNaniteStreaming, Log, TEXT("Nanite DDC request failed for '%s' (Page %d). Retrying..."), *Resources->ResourceName, PendingPage.InstallKey.PageIndex);
					}

					const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[PendingPage.InstallKey.PageIndex];
					FCacheGetChunkRequest Request = BuildDDCRequest(*Resources, PageStreamingState, PendingPageIndex);
					RequestDDCData(MakeArrayView(&Request, 1));
				}
				else
				{
					// Resource is no longer there. Just mark as ready so it will be skipped in InstallReadyPages
					PendingPage.State = FPendingPage::EState::DDC_Ready;
				}
				break;
			}
			else if (PendingPage.State == FPendingPage::EState::Memory)
			{
				// Memory is always ready
			}
			else
#endif
			{
#if WITH_EDITOR
				check(PendingPage.State == FPendingPage::EState::Disk);
#endif
				if (PendingPage.Request.IsCompleted())
				{
					if (!PendingPage.Request.IsOk())
					{
						// Retry if IO request failed for some reason
						FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
						if (Resources)	// If the resource is gone, no need to do anything as the page will be ignored by InstallReadyPages
						{
							const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[PendingPage.InstallKey.PageIndex];
							UE_LOG(LogNaniteStreaming, Warning, TEXT("IO Request failed. RuntimeResourceID: %8X, Offset: %d, Size: %d. Retrying..."), PendingPage.InstallKey.RuntimeResourceID, PageStreamingState.BulkOffset, PageStreamingState.BulkSize);

							TRACE_IOSTORE_METADATA_SCOPE_TAG("NaniteReadyPages");
							FBulkDataBatchRequest::FBatchBuilder Batch = FBulkDataBatchRequest::NewBatch(1);
							Batch.Read(Resources->StreamablePages, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);
							(void)Batch.Issue();
							break;
						}
					}

				#if !DEBUG_TRANSCODE_PAGES_REPEATEDLY
					bFreePageFromStagingAllocator = true;
				#endif
				}
				else
				{
					break;
				}
			}

			if(GNaniteStreamingBandwidthLimit >= 0.0f)
			{
				uint32 SimulatedBytesRemaining = FPlatformTime::ToSeconds64(DeltaTick) * GNaniteStreamingBandwidthLimit * 1048576.0;
				uint32 SimulatedBytesRead = FMath::Min(PendingPage.BytesLeftToStream, SimulatedBytesRemaining);
				PendingPage.BytesLeftToStream -= SimulatedBytesRead;
				SimulatedBytesRemaining -= SimulatedBytesRead;
				if(PendingPage.BytesLeftToStream > 0)
					break;
			}


			if(bFreePageFromStagingAllocator)
			{
				PendingPageStagingAllocator->Free(PendingPage.RequestBuffer.DataSize());
			}

			FResources* Resources = GetResources(PendingPage.InstallKey.RuntimeResourceID);
			if (Resources)
			{
				const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[PendingPage.InstallKey.PageIndex];
				TotalPageSize += PageStreamingState.PageSize;
			}

			NumReadyPages++;
		}
	}
	
	return NumReadyPages;
}

void FStreamingManager::AddPendingExplicitRequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AddPendingExplicitRequests);

	const int32 NumPendingExplicitRequests = PendingExplicitRequests.Num();
	if (NumPendingExplicitRequests == 0)
	{
		return;
	}

	uint32 NumPageRequests = 0;
	int32 Index = 0;
	while (Index < NumPendingExplicitRequests)
	{
		const uint32 ResourcePersistentHash = PendingExplicitRequests[Index++];
			
		// Resolve resource
		TArray<FResources*, TInlineAllocator<16>> MultiMapResult;
		PersistentHashResourceMap.MultiFind(ResourcePersistentHash, MultiMapResult);

		// Keep processing requests from this resource as long as they have the repeat bit set
		bool bRepeat = true;
		while (bRepeat && Index < NumPendingExplicitRequests)
		{
			const uint32 Packed = PendingExplicitRequests[Index++];
			bRepeat = (Packed & 1u) != 0u;
				
			// Add requests to table
			// In the rare event of a collision all resources with the same hash will be requested
			for (const FResources* Resources : MultiMapResult)
			{
				const uint32 PageIndex = (Packed >> 1) & NANITE_MAX_RESOURCE_PAGES_MASK;
				const uint32 Priority = FMath::Min(Packed | ((1 << (NANITE_MAX_RESOURCE_PAGES_BITS + 1)) - 1), NANITE_MAX_PRIORITY_BEFORE_PARENTS);	// Round quantized priority up
				if (PageIndex >= Resources->NumRootPages && PageIndex < (uint32)Resources->PageStreamingStates.Num())
				{
					AddRequest(Resources->RuntimeResourceID, PageIndex, Priority);
					NumPageRequests++;
				}
			}
		}
	}
	PendingExplicitRequests.Reset();

	INC_DWORD_STAT_BY(STAT_NaniteStreaming20_PageRequests, NumPageRequests);
	SET_DWORD_STAT(STAT_NaniteStreaming22_PageRequestsExplicit, NumPageRequests);
}

void FStreamingManager::AddPendingResourcePrefetchRequests()
{
	if (PendingResourcePrefetches.Num() == 0)
	{
		return;
	}
		
	uint32 NumPageRequests = 0;
	for (FResourcePrefetch& Prefetch : PendingResourcePrefetches)
	{
		FResources* Resources = GetResources(Prefetch.RuntimeResourceID);
		if (Resources)
		{
			// Request first MAX_RESOURCE_PREFETCH_PAGES streaming pages of resource
			const uint32 NumRootPages = Resources->NumRootPages;
			const uint32 NumPages = Resources->PageStreamingStates.Num();
			const uint32 EndPage = FMath::Min(NumPages, NumRootPages + MAX_RESOURCE_PREFETCH_PAGES);
			
			NumPageRequests += EndPage - NumRootPages;
			
			for (uint32 PageIndex = NumRootPages; PageIndex < EndPage; PageIndex++)
			{
				const uint32 Priority = NANITE_MAX_PRIORITY_BEFORE_PARENTS - Prefetch.NumFramesUntilRender;	// Prefetching has highest priority. Prioritize requests closer to the deadline higher.
																											// TODO: Calculate appropriate priority based on bounds

				AddRequest(Prefetch.RuntimeResourceID, PageIndex, Priority);
			}
		}
		Prefetch.NumFramesUntilRender--;	// Keep the request alive until projected first render
	}

	INC_DWORD_STAT_BY(STAT_NaniteStreaming20_PageRequests, NumPageRequests);
	SET_DWORD_STAT(STAT_NaniteStreaming23_PageRequestsPrefetch, NumPageRequests);

	// Remove requests that are past the rendering deadline
	PendingResourcePrefetches.RemoveAll([](const FResourcePrefetch& Prefetch) { return Prefetch.NumFramesUntilRender == 0; });
}

void FStreamingManager::BeginAsyncUpdate(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::BeginAsyncUpdate);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, NaniteStreaming, "Nanite::Streaming");
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteStreaming);

	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_BeginAsyncUpdate);
	
	check(!AsyncState.bUpdateActive);
	AsyncState = FAsyncState {};
	AsyncState.bUpdateActive = true;

	VirtualPageAllocator.Consolidate();
	RegisteredVirtualPages.SetNum(VirtualPageAllocator.GetMaxSize());

	FRDGBuffer* ClusterPageDataBuffer = ResizePoolAllocationIfNeeded(GraphBuilder);
	ProcessNewResources(GraphBuilder, ClusterPageDataBuffer);

	CSV_CUSTOM_STAT(NaniteStreaming, RootAllocationMB, StatNumAllocatedRootPages * (NANITE_ROOT_PAGE_GPU_SIZE / 1048576.0f), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(NaniteStreaming, RootDataSizeMB, ClusterPageData.Allocator.GetMaxSize() * (NANITE_ROOT_PAGE_GPU_SIZE / 1048576.0f), ECsvCustomStatOp::Set);

	uint32 TotalPageSize;
	AsyncState.NumReadyPages = DetermineReadyPages(TotalPageSize);
	if (AsyncState.NumReadyPages > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AllocBuffers);
		// Prepare buffers for upload
		const uint32 NumPages = AsyncState.NumReadyPages;
		PageUploader->Init(GraphBuilder, NumPages, TotalPageSize, MaxStreamingPages);
		
		check(HierarchyUpdates.Num() == 0);
		check(ClusterLeafFlagUpdates.Num() == 0);
	}
	
	AsyncState.GPUStreamingRequestsPtr = ReadbackManager->LockLatest(AsyncState.NumGPUStreamingRequests);
	ReadbackManager->PrepareRequestsBuffer(GraphBuilder);


	// Start async processing
	FStreamingUpdateParameters Parameters;
	Parameters.StreamingManager = this;

	check(AsyncTaskEvents.IsEmpty());
	if (GNaniteStreamingAsync)
	{
		AsyncTaskEvents.Add(TGraphTask<FStreamingUpdateTask>::CreateTask().ConstructAndDispatchWhenReady(Parameters));
	}
	else
	{
		AsyncUpdate();
	}
}

#if NANITE_SANITY_CHECK_STREAMING_REQUESTS
void FStreamingManager::SanityCheckStreamingRequests(const FGPUStreamingRequest* StreamingRequestsPtr, const uint32 NumStreamingRequests)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(SanityCheckRequests);
	uint32 PrevFrameNibble = ~0u;
	for (uint32 Index = 0; Index < NumStreamingRequests; Index++)
	{
		const FGPUStreamingRequest& GPURequest = StreamingRequestsPtr[Index];

		// Validate request magics
		if ((GPURequest.RuntimeResourceID_Magic & 0x30) != 0x10 ||
			(GPURequest.PageIndex_NumPages_Magic & 0x30) != 0x20 ||
			(GPURequest.Priority_Magic & 0x30) != 0x30)
		{
			UE_LOG(LogNaniteStreaming, Fatal, TEXT("Validation of Nanite streaming request failed! The magic doesn't match. This likely indicates an issue with the GPU readback."));
		}

		// Validate that requests are from the same frame
		const uint32 FrameNibble0 = GPURequest.RuntimeResourceID_Magic & 0xF;
		const uint32 FrameNibble1 = GPURequest.PageIndex_NumPages_Magic & 0xF;
		const uint32 FrameNibble2 = GPURequest.Priority_Magic & 0xF;
		if (FrameNibble0 != FrameNibble1 || FrameNibble0 != FrameNibble2 || FrameNibble1 != FrameNibble2 || (PrevFrameNibble != ~0u && FrameNibble0 != PrevFrameNibble))
		{
			UE_LOG(LogNaniteStreaming, Fatal, TEXT("Validation of Nanite streaming request failed! Single readback has data from multiple frames. Is there a race condition on the readback, a missing streaming update or is GPUScene being updated mid-frame?"));
		}
		PrevFrameNibble = FrameNibble0;

		const uint32 NumPages = (GPURequest.PageIndex_NumPages_Magic >> NANITE_STREAMING_REQUEST_MAGIC_BITS) & NANITE_MAX_GROUP_PARTS_MASK;
		const uint32 PageStartIndex = GPURequest.PageIndex_NumPages_Magic >> (NANITE_STREAMING_REQUEST_MAGIC_BITS + NANITE_MAX_GROUP_PARTS_BITS);

		if (NumPages == 0)
		{
			UE_LOG(LogNaniteStreaming, Fatal, TEXT("Validation of Nanite streaming request failed! Request range is empty."));
		}

		FResources* Resources = GetResources(GPURequest.RuntimeResourceID_Magic >> NANITE_STREAMING_REQUEST_MAGIC_BITS);
		if (Resources)
		{
			// Check that request page range is within the resource limits
			// Resource could have been uninstalled in the meantime, which is ok. The request is ignored.
			// We don't have to worry about RuntimeResourceIDs being reused because MAX_RUNTIME_RESOURCE_VERSIONS is high enough to never have two resources with the same ID in flight.
			const uint32 MaxPageIndex = PageStartIndex + NumPages - 1;
			if (MaxPageIndex >= (uint32)Resources->PageStreamingStates.Num())
			{
				UE_LOG(LogNaniteStreaming, Fatal, TEXT("Validation of Nanite streaming request failed! Page range out of bounds. Start: %d Num: %d Total: %d"), PageStartIndex, NumPages, Resources->PageStreamingStates.Num());
			}
		}
	}
}
#endif

bool FStreamingManager::AddRequest(uint32 RuntimeResourceID, uint32 PageIndex, uint32 VirtualPageIndex, uint32 Priority)
{
	check(Priority != 0u);

	FVirtualPage& VirtualPage = RegisteredVirtualPages[VirtualPageIndex];
	if (VirtualPage.RegisteredPageIndex != INDEX_NONE)
	{
		if (VirtualPage.Priority == 0u)
		{
			RequestedRegisteredPages.Add(VirtualPageIndex);
		}
	}
	else
	{
		if (VirtualPage.Priority == 0u)
		{
			RequestedNewPages.Add(FNewPageRequest{ FPageKey{ RuntimeResourceID, PageIndex }, VirtualPageIndex});
		}
	}

	const bool bUpdatedPriority = Priority > VirtualPage.Priority;
	VirtualPage.Priority = bUpdatedPriority ? Priority : VirtualPage.Priority;
	return bUpdatedPriority;
}

bool FStreamingManager::AddRequest(uint32 RuntimeResourceID, uint32 PageIndex, uint32 Priority)
{
	const FRootPageInfo* RootPageInfo = GetRootPage(RuntimeResourceID);
	if (RootPageInfo)
	{
		return AddRequest(RuntimeResourceID, PageIndex, RootPageInfo->VirtualPageRangeStart + PageIndex, Priority);
	}
	return false;
}


void FStreamingManager::AddPendingGPURequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(AddPendingGPURequests);
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_ProcessGPURequests);

	// Update priorities
	const uint32 NumStreamingRequests = AsyncState.NumGPUStreamingRequests;
	if (NumStreamingRequests == 0)
	{
		return;
	}

	const FGPUStreamingRequest* StreamingRequestsPtr = AsyncState.GPUStreamingRequestsPtr;
#if NANITE_SANITY_CHECK_STREAMING_REQUESTS
	SanityCheckStreamingRequests(StreamingRequestsPtr, NumStreamingRequests);
#endif
	const FGPUStreamingRequest* StreamingRequestsEndPtr = StreamingRequestsPtr + NumStreamingRequests;

	do
	{
		const FGPUStreamingRequest& GPURequest = *StreamingRequestsPtr;
#if NANITE_SANITY_CHECK_STREAMING_REQUESTS
		const uint32 RuntimeResourceID = (GPURequest.RuntimeResourceID_Magic >> NANITE_STREAMING_REQUEST_MAGIC_BITS);
		const uint32 NumPages = (GPURequest.PageIndex_NumPages_Magic >> NANITE_STREAMING_REQUEST_MAGIC_BITS) & NANITE_MAX_GROUP_PARTS_MASK;
		const uint32 FirstPageIndex = GPURequest.PageIndex_NumPages_Magic >> (NANITE_STREAMING_REQUEST_MAGIC_BITS + NANITE_MAX_GROUP_PARTS_BITS);
		const uint32 Priority = GPURequest.Priority_Magic & ~NANITE_STREAMING_REQUEST_MAGIC_MASK;
#else
		const uint32 RuntimeResourceID = GPURequest.RuntimeResourceID_Magic;
		const uint32 NumPages = GPURequest.PageIndex_NumPages_Magic & NANITE_MAX_GROUP_PARTS_MASK;
		const uint32 FirstPageIndex = GPURequest.PageIndex_NumPages_Magic >> NANITE_MAX_GROUP_PARTS_BITS;
		const uint32 Priority = GPURequest.Priority_Magic;
#endif
		
		check(Priority != 0u && Priority <= NANITE_MAX_PRIORITY_BEFORE_PARENTS);

		FRootPageInfo* RootPageInfo = GetRootPage(RuntimeResourceID);
		if (RootPageInfo)
		{
			auto ProcessPage = [this](uint32 RuntimeResourceID, uint32 PageIndex, uint32 VirtualPageIndex, uint32 Priority) {
				FVirtualPage& VirtualPage = RegisteredVirtualPages[VirtualPageIndex];
				if (VirtualPage.RegisteredPageIndex != INDEX_NONE)
				{
					if (VirtualPage.Priority == 0u)
					{
						RequestedRegisteredPages.Add(VirtualPageIndex);
					}
				}
				else
				{
					if (VirtualPage.Priority == 0u)
					{
						RequestedNewPages.Add(FNewPageRequest{ FPageKey{ RuntimeResourceID, PageIndex }, VirtualPageIndex });
					}
				}
				RegisteredVirtualPages[VirtualPageIndex].Priority = FMath::Max(RegisteredVirtualPages[VirtualPageIndex].Priority, Priority);	// TODO: Preserve old behavior. We should redo priorities to accumulation
			};

			const uint32 VirtualPageRangeStart = RootPageInfo->VirtualPageRangeStart;
			ProcessPage(RuntimeResourceID, FirstPageIndex, VirtualPageRangeStart + FirstPageIndex, Priority);	// Manually peel off first iteration for performance
			for (uint32 i = 1; i < NumPages; i++)
			{
				const uint32 PageIndex = FirstPageIndex + i;
				const uint32 VirtualPageIndex = VirtualPageRangeStart + PageIndex;
				ProcessPage(RuntimeResourceID, PageIndex, VirtualPageIndex, Priority);
			}
		}
	} while (++StreamingRequestsPtr < StreamingRequestsEndPtr);

	INC_DWORD_STAT_BY(STAT_NaniteStreaming20_PageRequests, NumStreamingRequests);
	SET_DWORD_STAT(STAT_NaniteStreaming21_PageRequestsGPU, NumStreamingRequests);
}

void FStreamingManager::AddParentNewRequestsRecursive(const FResources& Resources, uint32 RuntimeResourceID, uint32 PageIndex, uint32 VirtualPageRangeStart, uint32 Priority)
{
	checkSlow(Priority < MAX_uint32);
	const uint32 NextPriority = Priority + 1u;

	const FPageStreamingState& PageStreamingState = Resources.PageStreamingStates[PageIndex];
	for (uint32 i = 0; i < PageStreamingState.DependenciesNum; i++)
	{
		const uint32 DependencyPageIndex = Resources.PageDependencies[PageStreamingState.DependenciesStart + i];
		if (!Resources.IsRootPage(DependencyPageIndex))
		{
			if (AddRequest(RuntimeResourceID, DependencyPageIndex, VirtualPageRangeStart + DependencyPageIndex, NextPriority))
			{
				AddParentNewRequestsRecursive(Resources, RuntimeResourceID, DependencyPageIndex, VirtualPageRangeStart, NextPriority);
			}
		}
	}
}

void FStreamingManager::AddParentRegisteredRequestsRecursive(uint32 RegisteredPageIndex, uint32 Priority)
{
	checkSlow(Priority < MAX_uint32);
	const uint32 NextPriority = Priority + 1u;
	
	const FRegisteredPageDependencies& Dependencies = RegisteredPageDependencies[RegisteredPageIndex];
	for (uint32 DependencyVirtualPageIndex : Dependencies)
	{
		FVirtualPage& DependencyVirtualPage = RegisteredVirtualPages[DependencyVirtualPageIndex];

		if (DependencyVirtualPage.Priority == 0u)
		{
			RequestedRegisteredPages.Add(DependencyVirtualPageIndex);
		}
		
		if (NextPriority > DependencyVirtualPage.Priority)
		{
			DependencyVirtualPage.Priority = NextPriority;
			AddParentRegisteredRequestsRecursive(DependencyVirtualPage.RegisteredPageIndex, NextPriority);
		}
	}
}

// Add implicit requests for any parent pages that were not already referenced
void FStreamingManager::AddParentRequests()
{
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_AddParentRequests);
	
	// Process new pages first as they might add references to already registered pages.
	// An already registered page will never have a dependency on a new page.
	if (RequestedNewPages.Num() > 0)
	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_AddParentNewRequests);
		const uint32 NumInitialRequests = RequestedNewPages.Num();
		for (uint32 i = 0; i < NumInitialRequests; i++)
		{
			FNewPageRequest Request = RequestedNewPages[i];	// Needs to be a copy as the array can move
			checkSlow(RegisteredVirtualPages[Request.VirtualPageIndex].RegisteredPageIndex == INDEX_NONE);

			FRootPageInfo* RootPage = GetRootPage(Request.Key.RuntimeResourceID);
			const uint32 Priority = RegisteredVirtualPages[Request.VirtualPageIndex].Priority;
			AddParentNewRequestsRecursive(*RootPage->Resources, Request.Key.RuntimeResourceID, Request.Key.PageIndex, RootPage->VirtualPageRangeStart, Priority);	//Make it non-recursive
		}
	}

	if (RequestedRegisteredPages.Num() > 0)
	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_AddParentRegisteredRequests);
		const uint32 NumInitialRequests = RequestedRegisteredPages.Num();
		for (uint32 i = 0; i < NumInitialRequests; i++)
		{
			const uint32 VirtualPageIndex = RequestedRegisteredPages[i];
			const FVirtualPage& VirtualPage = RegisteredVirtualPages[VirtualPageIndex];

			checkSlow(VirtualPage.Priority <= NANITE_MAX_PRIORITY_BEFORE_PARENTS);
			const uint32 NextPriority = VirtualPage.Priority + 1u;
			const FRegisteredPageDependencies& Dependencies = RegisteredPageDependencies[VirtualPage.RegisteredPageIndex];
			for (uint32 DependencyVirtualPageIndex : Dependencies)
			{
				FVirtualPage& DependencyVirtualPage = RegisteredVirtualPages[DependencyVirtualPageIndex];

				if (DependencyVirtualPage.Priority == 0u)
				{
					RequestedRegisteredPages.Add(DependencyVirtualPageIndex);
				}

				if (NextPriority > DependencyVirtualPage.Priority)
				{
					DependencyVirtualPage.Priority = NextPriority;
					AddParentRegisteredRequestsRecursive(DependencyVirtualPage.RegisteredPageIndex, NextPriority);
				}
			}
		}
	}
}

void FStreamingManager::MoveToEndOfLRUList(uint32 RegisteredPageIndex)
{
	uint32& LRUIndex = RegisteredPageIndexToLRU[RegisteredPageIndex];
	check(LRUIndex != INDEX_NONE);
	check((LRUToRegisteredPageIndex[LRUIndex] & LRU_INDEX_MASK) == RegisteredPageIndex);

	LRUToRegisteredPageIndex[LRUIndex] = INDEX_NONE;
	LRUIndex = LRUToRegisteredPageIndex.Num();
	LRUToRegisteredPageIndex.Add(RegisteredPageIndex | LRU_FLAG_REFERENCED_THIS_UPDATE);
}

void FStreamingManager::CompactLRU()
{
	//TODO: Make it so uninstalled pages are moved to the front of the queue immediately
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_CompactLRU);
	uint32 WriteIndex = 0;
	const uint32 LRUBufferLength = LRUToRegisteredPageIndex.Num();
	for (uint32 i = 0; i < LRUBufferLength; i++)
	{
		const uint32 Entry = LRUToRegisteredPageIndex[i];
		if (Entry != INDEX_NONE)
		{
			const uint32 RegisteredPageIndex = Entry & LRU_INDEX_MASK;
			LRUToRegisteredPageIndex[WriteIndex] = RegisteredPageIndex;
			RegisteredPageIndexToLRU[RegisteredPageIndex] = WriteIndex;
			WriteIndex++;
		}
	}
	check(WriteIndex == MaxStreamingPages);
	LRUToRegisteredPageIndex.SetNum(WriteIndex);
#if DO_CHECK
	VerifyLRU();
#endif
}

void FStreamingManager::VerifyLRU()
{
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_VerifyLRU);

	check(RegisteredPageIndexToLRU.Num() == MaxStreamingPages);
	check(LRUToRegisteredPageIndex.Num() == MaxStreamingPages);

	TBitArray<> ReferenceMap;
	ReferenceMap.Init(false, MaxStreamingPages);
	for (uint32 RegisteredPageIndex = 0; RegisteredPageIndex < MaxStreamingPages; RegisteredPageIndex++)
	{
		const uint32 LRUIndex = RegisteredPageIndexToLRU[RegisteredPageIndex];

		check(!ReferenceMap[LRUIndex]);
		ReferenceMap[LRUIndex] = true;

		check(LRUToRegisteredPageIndex[LRUIndex] == RegisteredPageIndex);
	}
}

void FStreamingManager::SelectHighestPriorityPagesAndUpdateLRU(uint32 MaxSelectedPages)
{
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_SelectHighestPriority);

	const auto StreamingRequestPriorityPredicate = [](const FStreamingRequest& A, const FStreamingRequest& B)
	{
		return A.Priority > B.Priority;
	};

	PrioritizedRequestsHeap.Reset();

	for (const FNewPageRequest& NewPageRequest : RequestedNewPages)
	{
		FStreamingRequest StreamingRequest;
		StreamingRequest.Key = NewPageRequest.Key;
		StreamingRequest.Priority = RegisteredVirtualPages[NewPageRequest.VirtualPageIndex].Priority;
			
		PrioritizedRequestsHeap.Push(StreamingRequest);
	}

	const uint32 NumNewPageRequests = PrioritizedRequestsHeap.Num();
	const uint32 NumUniqueRequests = RequestedRegisteredPages.Num() + RequestedNewPages.Num();

	SET_DWORD_STAT(STAT_NaniteStreaming27_PageRequestsNew, NumNewPageRequests);
	CSV_CUSTOM_STAT(NaniteStreamingDetail, NewStreamingDataSizeMB, NumNewPageRequests * (NANITE_STREAMING_PAGE_GPU_SIZE / 1048576.0f), ECsvCustomStatOp::Set);

	StatVisibleSetSize = NumUniqueRequests;

	StatStreamingPoolPercentage = MaxStreamingPages ? NumUniqueRequests / float(MaxStreamingPages) * 100.0f : 0.0f;
	QualityScaleFactor = QualityScalingManager->Update(StatStreamingPoolPercentage);

	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_Heapify);
		PrioritizedRequestsHeap.Heapify(StreamingRequestPriorityPredicate);
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_UpdateLRU);
		for (const uint32 VirtualPageIndex : RequestedRegisteredPages)
		{
			const uint32 RegisteredPageIndex = RegisteredVirtualPages[VirtualPageIndex].RegisteredPageIndex;
			MoveToEndOfLRUList(RegisteredPageIndex);
		}
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_ClearReferencedArray);
		for (const uint32 VirtualPageIndex : RequestedRegisteredPages)
		{
			RegisteredVirtualPages[VirtualPageIndex].Priority = 0;
		}

		for (const FNewPageRequest& NewPageRequest : RequestedNewPages)
		{
			RegisteredVirtualPages[NewPageRequest.VirtualPageIndex].Priority = 0;
		}
	}

#if DO_CHECK
	for (const FVirtualPage& Page : RegisteredVirtualPages)
	{
		check(Page.Priority == 0);
	}
#endif
	
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(SelectStreamingPages);
		while ((uint32)SelectedPages.Num() < MaxSelectedPages && PrioritizedRequestsHeap.Num() > 0)
		{
			FStreamingRequest SelectedRequest;
			PrioritizedRequestsHeap.HeapPop(SelectedRequest, StreamingRequestPriorityPredicate, EAllowShrinking::No);

			FResources* Resources = GetResources(SelectedRequest.Key.RuntimeResourceID);
			if (Resources)
			{
				const uint32 NumResourcePages = (uint32)Resources->PageStreamingStates.Num();
				if (SelectedRequest.Key.PageIndex < NumResourcePages)
				{
					SelectedPages.Push(SelectedRequest.Key);
				}
				else
				{
					checkf(false, TEXT("Reference to page index that is out of bounds: %d / %d. "
						"This could be caused by GPUScene corruption or issues with the GPU readback."),
						SelectedRequest.Key.PageIndex, NumResourcePages);
				}
			}
		}
		check((uint32)SelectedPages.Num() <= MaxSelectedPages);
	}
}

void FStreamingManager::AsyncUpdate()
{
	LLM_SCOPE_BYTAG(Nanite);
	SCOPED_NAMED_EVENT(FStreamingManager_AsyncUpdate, FColor::Cyan);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::AsyncUpdate);
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_AsyncUpdate);

	check(AsyncState.bUpdateActive);
	InstallReadyPages(AsyncState.NumReadyPages);
	MaxHierarchyLevels = HierarchyDepthManager->CalculateNumLevels();
	SET_DWORD_STAT(STAT_NaniteStreaming04_MaxHierarchyLevels, MaxHierarchyLevels);

	const uint32 StartTime = FPlatformTime::Cycles();


	if (AsyncState.GPUStreamingRequestsPtr)
	{
		RequestedRegisteredPages.Reset();
		RequestedNewPages.Reset();
	
		{
			SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_ProcessRequests);

			SET_DWORD_STAT(STAT_NaniteStreaming20_PageRequests, 0);

			AddPendingGPURequests();
		#if WITH_EDITOR
			RecordGPURequests();
		#endif
			AddPendingExplicitRequests();
			AddPendingResourcePrefetchRequests();
			AddParentRequests();

			SET_DWORD_STAT(STAT_NaniteStreaming25_PageRequestsUnique, RequestedRegisteredPages.Num() + RequestedNewPages.Num());
			SET_DWORD_STAT(STAT_NaniteStreaming26_PageRequestsRegistered, RequestedRegisteredPages.Num());
			SET_DWORD_STAT(STAT_NaniteStreaming27_PageRequestsNew, RequestedNewPages.Num());
		}

		// NOTE: Requests can still contain references to resources that are no longer resident.
		const uint32 MaxSelectedPages = MaxPendingPages - NumPendingPages;
		SelectedPages.Reset();
		SelectHighestPriorityPagesAndUpdateLRU(MaxSelectedPages);

		uint32 NumLegacyRequestsIssued = 0;

		if( !SelectedPages.IsEmpty() )
		{
		#if WITH_EDITOR
			TArray<FCacheGetChunkRequest> DDCRequests;
			DDCRequests.Reserve(MaxSelectedPages);
		#endif

			FBulkDataBatchRequest::FBatchBuilder Batch = FBulkDataBatchRequest::NewBatch(SelectedPages.Num());
			bool bIssueIOBatch = false;
			float TotalIORequestSizeMB = 0.0f;

			// Register Pages
			{
				TRACE_CPUPROFILER_EVENT_SCOPE(RegisterPages);

				int32 NextLRUTestIndex = 0;
				for( const FPageKey& SelectedKey : SelectedPages )
				{
					FResources* Resources = GetResources(SelectedKey.RuntimeResourceID);
					check(Resources);
					FByteBulkData& BulkData = Resources->StreamablePages;
#if WITH_EDITOR
					const bool bDiskRequest = !(Resources->ResourceFlags & NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC) && !BulkData.IsBulkDataLoaded();
#else
					const bool bDiskRequest = true;
#endif

					const bool bLegacyRequest = bDiskRequest && !BulkData.IsUsingIODispatcher();
					if (bLegacyRequest && NumLegacyRequestsIssued == MAX_LEGACY_REQUESTS_PER_UPDATE)
					{
						break;
					}

					FRegisteredPage* Page = nullptr;					
					while(NextLRUTestIndex < LRUToRegisteredPageIndex.Num())
					{
						const uint32 Entry = LRUToRegisteredPageIndex[NextLRUTestIndex++];
						if (Entry == INDEX_NONE || (Entry & LRU_FLAG_REFERENCED_THIS_UPDATE) != 0)
						{
							continue;
						}

						const uint32 RegisteredPageIndex = Entry & LRU_INDEX_MASK;
						FRegisteredPage* CandidatePage = &RegisteredPages[RegisteredPageIndex];
						if (CandidatePage && CandidatePage->RefCount == 0)
						{
							Page = CandidatePage;
							break;
						}
					}

					if (!Page)
					{
						break;	// Couldn't find a free page. Abort.
					}

					const FPageStreamingState& PageStreamingState = Resources->PageStreamingStates[SelectedKey.PageIndex];
					check(!Resources->IsRootPage(SelectedKey.PageIndex));

					FPendingPage& PendingPage = PendingPages[NextPendingPageIndex];
					PendingPage = FPendingPage();

#if WITH_EDITOR
					if(!bDiskRequest)
					{
						if(Resources->ResourceFlags & NANITE_RESOURCE_FLAG_STREAMING_DATA_IN_DDC)
						{
							DDCRequests.Add(BuildDDCRequest(*Resources, PageStreamingState, NextPendingPageIndex));
							PendingPage.State = FPendingPage::EState::DDC_Pending;
						}
						else
						{
							PendingPage.State = FPendingPage::EState::Memory;
						}
					}
					else
#endif
					{
						uint32 AllocatedOffset;
						if (!PendingPageStagingAllocator->TryAllocate(PageStreamingState.BulkSize, AllocatedOffset))
						{
							// Staging ring buffer full. Postpone any remaining pages to next frame.
							// UE_LOG(LogNaniteStreaming, Verbose, TEXT("This should be a rare event."));
							break;
						}
						TRACE_IOSTORE_METADATA_SCOPE_TAG("NaniteGPU");
						uint8* Dst = PendingPageStagingMemory.GetData() + AllocatedOffset;
						PendingPage.RequestBuffer = FIoBuffer(FIoBuffer::Wrap, Dst, PageStreamingState.BulkSize);
						Batch.Read(BulkData, PageStreamingState.BulkOffset, PageStreamingState.BulkSize, AIOP_Low, PendingPage.RequestBuffer, PendingPage.Request);
						bIssueIOBatch = true;

						if (bLegacyRequest)
						{
							NumLegacyRequestsIssued++;
						}	
#if WITH_EDITOR
						PendingPage.State = FPendingPage::EState::Disk;
#endif
					}

					UnregisterStreamingPage(Page->Key);

					TotalIORequestSizeMB += PageStreamingState.BulkSize * (1.0f / 1048576.0f);

					PendingPage.InstallKey = SelectedKey;
					const uint32 GPUPageIndex = uint32(Page - RegisteredPages.GetData());
					PendingPage.GPUPageIndex = GPUPageIndex;

					NextPendingPageIndex = ( NextPendingPageIndex + 1 ) % MaxPendingPages;
					NumPendingPages++;

					PendingPage.BytesLeftToStream = PageStreamingState.BulkSize;

					RegisterStreamingPage(GPUPageIndex, SelectedKey);
				}
			}

			INC_FLOAT_STAT_BY(STAT_NaniteStreaming40_IORequestSizeMB, TotalIORequestSizeMB);
				
			CSV_CUSTOM_STAT(NaniteStreamingDetail, IORequestSizeMB, TotalIORequestSizeMB, ECsvCustomStatOp::Set);
			CSV_CUSTOM_STAT(NaniteStreamingDetail, IORequestSizeMBps, TotalIORequestSizeMB / FPlatformTime::ToSeconds(StartTime - StatPrevUpdateTime), ECsvCustomStatOp::Set);

#if WITH_EDITOR
			if (DDCRequests.Num() > 0)
			{
				RequestDDCData(DDCRequests);
				DDCRequests.Empty();
			}
#endif

			if (bIssueIOBatch)
			{
				// Issue batch
				TRACE_CPUPROFILER_EVENT_SCOPE(FIoBatch::Issue);
				(void)Batch.Issue();
			}
		}

		CompactLRU();

#if !WITH_EDITOR
		// Issue warning if we end up taking the legacy path
		static const bool bUsingPakFiles = FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")) != nullptr;
		if (NumLegacyRequestsIssued > 0 && bUsingPakFiles)
		{
			static bool bHasWarned = false;
			if(!bHasWarned)
			{
				UE_LOG(LogNaniteStreaming, Warning, TEXT(	"PERFORMANCE WARNING: Nanite is issuing IO requests using the legacy IO path. Expect slower streaming and higher CPU overhead. "
															"To avoid this penalty make sure iostore is enabled, it is supported by the platform, and that resources are built with -iostore."));
				bHasWarned = true;
			}
		}
#endif
	}
	
	StatPrevUpdateTime = StartTime;
	CSV_CUSTOM_STAT(NaniteStreamingDetail, StreamingPoolSizeMB, MaxStreamingPages * (NANITE_STREAMING_PAGE_GPU_SIZE / 1048576.0f), ECsvCustomStatOp::Set);

	const float VisibleStreamingDataSizeMB = StatVisibleSetSize * (NANITE_STREAMING_PAGE_GPU_SIZE / 1048576.0f);
	SET_FLOAT_STAT(STAT_NaniteStreaming30_VisibleStreamingDataSizeMB, VisibleStreamingDataSizeMB);
	CSV_CUSTOM_STAT(NaniteStreamingDetail, VisibleStreamingDataSizeMB, VisibleStreamingDataSizeMB, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(NaniteStreamingDetail, AsyncUpdateMs, 1000.0f * FPlatformTime::ToSeconds(FPlatformTime::Cycles() - StartTime), ECsvCustomStatOp::Set);

	SET_FLOAT_STAT(STAT_NaniteStreaming31_VisibleStreamingPoolPercentage, StatStreamingPoolPercentage);
	SET_FLOAT_STAT(STAT_NaniteStreaming32_VisibleStreamingQualityScale, QualityScaleFactor);
}

void FStreamingManager::EndAsyncUpdate(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);
	TRACE_CPUPROFILER_EVENT_SCOPE(FStreamingManager::EndAsyncUpdate);
	
	RDG_EVENT_SCOPE_STAT(GraphBuilder, NaniteStreaming, "Nanite::EndAsyncUpdate");
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteStreaming);

	RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());
	SCOPE_CYCLE_COUNTER(STAT_NaniteStreaming_EndAsyncUpdate);

	check(AsyncState.bUpdateActive);

	// Wait for async processing to finish
	if (GNaniteStreamingAsync)
	{
		check(!AsyncTaskEvents.IsEmpty());
		FTaskGraphInterface::Get().WaitUntilTasksComplete(AsyncTaskEvents, ENamedThreads::GetRenderThread_Local());
	}

	AsyncTaskEvents.Empty();

	if (AsyncState.GPUStreamingRequestsPtr)
	{
		ReadbackManager->Unlock();
	}

	// Issue GPU copy operations
	if (AsyncState.NumReadyPages > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UploadPages);

		const FRDGBufferRef ClusterPageDataBuffer = GraphBuilder.RegisterExternalBuffer(ClusterPageData.DataBuffer);
		PageUploader->ResourceUploadTo(GraphBuilder, ClusterPageDataBuffer);
		FlushHierarchyUpdates(GraphBuilder);
		FlushClusterLeafFlagUpdates(GraphBuilder, ClusterPageDataBuffer);

	#if !DEBUG_TRANSCODE_PAGES_REPEATEDLY
		NumPendingPages -= AsyncState.NumReadyPages;
	#endif
	}

	AsyncState.bUpdateActive = false;
}

void FStreamingManager::SubmitFrameStreamingRequests(FRDGBuilder& GraphBuilder)
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);
	if (!DoesPlatformSupportNanite(GMaxRHIShaderPlatform))
	{
		return;
	}

	LLM_SCOPE_BYTAG(Nanite);

	RDG_EVENT_SCOPE_STAT(GraphBuilder, NaniteReadback, "Nanite::Readback");
	RDG_GPU_STAT_SCOPE(GraphBuilder, NaniteReadback);

	ReadbackManager->QueueReadback(GraphBuilder);
}

bool FStreamingManager::IsAsyncUpdateInProgress()
{
	return AsyncState.bUpdateActive;
}

void FStreamingManager::PrefetchResource(const FResources* Resources, uint32 NumFramesUntilRender)
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);
	check(Resources);
	if (GNaniteStreamingPrefetch)
	{
		FResourcePrefetch Prefetch;
		Prefetch.RuntimeResourceID		= Resources->RuntimeResourceID;
		Prefetch.NumFramesUntilRender	= FMath::Min(NumFramesUntilRender, 30u);		// Make sure invalid values doesn't cause the request to stick around forever
		PendingResourcePrefetches.Add(Prefetch);
	}
}

void FStreamingManager::RequestNanitePages(TArrayView<uint32> RequestData)
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);
	if (GNaniteStreamingExplicitRequests)
	{
		PendingExplicitRequests.Append(RequestData.GetData(), RequestData.Num());
	}
}

uint32 FStreamingManager::GetStreamingRequestsBufferVersion() const
{
	return ReadbackManager->GetBufferVersion();
}

#if WITH_EDITOR
uint64 FStreamingManager::GetRequestRecordBuffer(TArray<uint32>& OutRequestData)
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);
	if (PageRequestRecordHandle == (uint64)-1)
	{
		return (uint64)-1;
	}

	const uint64 Ret = PageRequestRecordHandle;
	PageRequestRecordHandle = (uint64)-1;
	if (PageRequestRecordMap.Num() == 0)
	{
		OutRequestData.Empty();
		return Ret;
	}

	// Resolve requests and convert to persistent resource IDs
	TArray<FStreamingRequest> Requests;
	Requests.Reserve(PageRequestRecordMap.Num());
	for (const TPair<FPageKey, uint32>& MapEntry : PageRequestRecordMap)
	{
		FResources* Resources = GetResources(MapEntry.Key.RuntimeResourceID);
		if (Resources)
		{	
			Requests.Add(FStreamingRequest { FPageKey { Resources->PersistentHash, MapEntry.Key.PageIndex }, MapEntry.Value } );
		}
	}
	PageRequestRecordMap.Reset();

	Requests.Sort();

	// Count unique resources
	uint32 NumUniqueResources = 0;
	{
		uint64 PrevPersistentHash = NANITE_INVALID_PERSISTENT_HASH;
		for (const FStreamingRequest& Request : Requests)
		{
			if (Request.Key.RuntimeResourceID != PrevPersistentHash)
			{
				NumUniqueResources++;
			}
			PrevPersistentHash = Request.Key.RuntimeResourceID;
		}
	}
	
	// Write packed requests
	// A request consists of two DWORDs. A resource DWORD and a pageindex/priority/repeat DWORD.
	// The repeat bit indicates if the next request is to the same resource, so the resource DWORD can be omitted.
	// As there are often many requests per resource, this encoding can safe upwards of half of the total DWORDs.
	{
		const uint32 NumOutputDwords = NumUniqueResources + Requests.Num();
		OutRequestData.SetNum(NumOutputDwords);
		uint32 WriteIndex = 0;
		uint64 PrevResourceID = ~0ull;
		for (const FStreamingRequest& Request : Requests)
		{
			check(Request.Key.PageIndex < NANITE_MAX_RESOURCE_PAGES);
			if (Request.Key.RuntimeResourceID != PrevResourceID)
			{
				OutRequestData[WriteIndex++] = Request.Key.RuntimeResourceID;
			}
			else
			{
				OutRequestData[WriteIndex - 1] |= 1;	// Mark resource repeat bit in previous packed dword
 			}
			PrevResourceID = Request.Key.RuntimeResourceID;

			const uint32 QuantizedPriority = Request.Priority >> (NANITE_MAX_RESOURCE_PAGES_BITS + 1);	// Exact priority doesn't matter, so just quantize it to fit
			const uint32 Packed = (QuantizedPriority << (NANITE_MAX_RESOURCE_PAGES_BITS + 1)) | (Request.Key.PageIndex << 1);	// Lowest bit is resource repeat bit
			OutRequestData[WriteIndex++] = Packed;
		}

		check(WriteIndex == NumOutputDwords);
	}
	
	return Ret;
}

void FStreamingManager::SetRequestRecordBuffer(uint64 Handle)
{
	check(IsInRenderingThread());
	check(!AsyncState.bUpdateActive);
	PageRequestRecordHandle = Handle;
	PageRequestRecordMap.Empty();
}

void FStreamingManager::RecordGPURequests()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(RecordGPURequests);
	if (PageRequestRecordHandle != (uint64)-1)
	{
		auto UpdateKeyPriority = [this](const FPageKey& Key, uint32 Priority)
		{
			uint32* PriorityPtr = PageRequestRecordMap.Find(Key);
			if (PriorityPtr)
				*PriorityPtr = FMath::Max(*PriorityPtr, Priority);
			else
				PageRequestRecordMap.Add(Key, Priority);
		};

		for (uint32 VirtualPageIndex : RequestedRegisteredPages)
		{
			const FVirtualPage& VirtualPage = RegisteredVirtualPages[VirtualPageIndex];
			const FRegisteredPage& RegisteredPage = RegisteredPages[VirtualPage.RegisteredPageIndex];
			UpdateKeyPriority(RegisteredPage.Key, VirtualPage.Priority);
		}

		for (const FNewPageRequest& Request : RequestedNewPages)
		{
			const FVirtualPage& VirtualPage = RegisteredVirtualPages[Request.VirtualPageIndex];
			UpdateKeyPriority(Request.Key, VirtualPage.Priority);
		}
	}
}

FCacheGetChunkRequest FStreamingManager::BuildDDCRequest(const FResources& Resources, const FPageStreamingState& PageStreamingState, const uint32 PendingPageIndex)
{
	FCacheKey Key;
	Key.Bucket = FCacheBucket(TEXT("StaticMesh"));
	Key.Hash = Resources.DDCKeyHash;
	check(!Resources.DDCRawHash.IsZero());

	FCacheGetChunkRequest Request;
	Request.Id			= NaniteValueId;
	Request.Key			= Key;
	Request.RawOffset	= PageStreamingState.BulkOffset;
	Request.RawSize		= PageStreamingState.BulkSize;
	Request.RawHash		= Resources.DDCRawHash;
	Request.UserData	= PendingPageIndex;
	return Request;
}

void FStreamingManager::RequestDDCData(TConstArrayView<FCacheGetChunkRequest> DDCRequests)
{
	FRequestBarrier Barrier(*RequestOwner);	// This is a critical section on the owner. It does not constrain ordering
	GetCache().GetChunks(DDCRequests, *RequestOwner,
		[this](FCacheGetChunkResponse&& Response)
		{
			const uint32 PendingPageIndex = (uint32)Response.UserData;
			FPendingPage& PendingPage = PendingPages[PendingPageIndex];

			if (Response.Status == EStatus::Ok)
			{
				PendingPage.SharedBuffer = MoveTemp(Response.RawData);
				PendingPage.State = FPendingPage::EState::DDC_Ready;
			}
			else
			{
				PendingPage.State = FPendingPage::EState::DDC_Failed;
			}
		});
}

#endif // WITH_EDITOR

TGlobalResource< FStreamingManager > GStreamingManager;

} // namespace Nanite