// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/IConsoleManager.h"

#include "PCGCommon.generated.h"

#define UE_API PCG_API

using FPCGTaskId = uint64;
static const FPCGTaskId InvalidPCGTaskId = (uint64)-1;

using FPCGPinId = uint64;

class IPCGElement;
class IPCGGraphExecutionSource;
class UPCGComponent;
class UPCGGraph;
struct FPCGContext;
struct FPCGStack;
struct FPCGStackContext;

namespace PCGLog
{
	/** Returns the component's owner name. In the case of commandlets, owner full object path will be returned. */
	FString GetExecutionSourceName(const IPCGGraphExecutionSource* InExecutionSource, bool bUseLabel = false, FString InDefaultName = TEXT("MISSINGOWNER"));
}

namespace PCGPinIdHelpers
{
	/** Pin active bitmask stored in uint64, so 64 flags available. */
	constexpr int PinActiveBitmaskSize = 64;

	/** There are 64 pin flags available, however we use flag 63 as a special pin-less ID for task dependencies that don't have associated pins. */
	constexpr int MaxOutputPins = PinActiveBitmaskSize - 1;

	/** Convert node ID and pin index to a unique pin ID. */
	FPCGPinId NodeIdAndPinIndexToPinId(FPCGTaskId NodeId, uint64 PinIndex);

	/** Create a pin ID from a node ID alone. Used for task inputs that don't have associated pins. */
	FPCGPinId NodeIdToPinId(FPCGTaskId NodeId);

	/** Adjust the pin ID to incorporate the given node ID offset. */
	FPCGPinId OffsetNodeIdInPinId(FPCGPinId PinId, uint64 NodeIDOffset);

	/** Extract node ID from the given pin ID. */
	FPCGTaskId GetNodeIdFromPinId(FPCGPinId PinId);

	/** Extract pin index from unique pin ID. */
	uint64 GetPinIndexFromPinId(FPCGPinId PinId);
}

namespace PCGPointCustomPropertyNames
{
	bool IsCustomPropertyName(FName Name);

	const FName ExtentsName = TEXT("Extents");
	const FName LocalCenterName = TEXT("LocalCenter");
	const FName PositionName = TEXT("Position");
	const FName RotationName = TEXT("Rotation");
	const FName ScaleName = TEXT("Scale");
	const FName LocalSizeName = TEXT("LocalSize");
	const FName ScaledLocalSizeName = TEXT("ScaledLocalSize");
}

// Value names need to match EPCGPointProperties
UENUM(meta = (Bitflags))
enum class EPCGPointNativeProperties : uint32
{
	None = 0 UMETA(Hidden),
	Transform = 1 << 0,
	Density = 1 << 1,
	BoundsMin = 1 << 2,
	BoundsMax = 1 << 3,
	Color = 1 << 4,
	Steepness = 1 << 5,
	Seed = 1 << 6,
	MetadataEntry = 1 << 7,
	All = Transform | Density | BoundsMin | BoundsMax | Color | Steepness | Seed | MetadataEntry,
};
ENUM_CLASS_FLAGS(EPCGPointNativeProperties);

UENUM(meta = (Bitflags))
enum class EPCGChangeType : uint32
{
	None = 0,
	Cosmetic = 1 << 0,
	Settings = 1 << 1,
	Input = 1 << 2,
	Edge = 1 << 3,
	Node = 1 << 4,
	Structural = 1 << 5,
	/** Anything related to generation grids - changing grid size or adding/removing grid size nodes. */
	GenerationGrid = 1 << 6,
	/** Change to any shader source code. */
	ShaderSource = 1 << 7,
	/** Changes in the graph customization that will impact the editor. */
	GraphCustomization = 1 << 8,
};
ENUM_CLASS_FLAGS(EPCGChangeType);

// Bitmask containing the various data types supported in PCG. Note that this enum cannot be a blueprint type because
// enums have to be uint8 for blueprint, and we already use more than 8 bits in the bitmask.
// This is why we have a parallel enum just below that must match on a name basis 1:1 to allow the make/break functions to work properly
// in blueprint.
// WARNING: Please be mindful that combination of flags that are not explicitly defined there won't be serialized correctly, inducing data loss.
UENUM(meta = (Bitflags))
enum class EPCGDataType : uint32
{
	None = 0 UMETA(Hidden),
	Point = 1 << 1,

	Spline = 1 << 2,
	LandscapeSpline = 1 << 3,
	PolyLine = Spline | LandscapeSpline UMETA(DisplayName = "Curve"),

	Landscape = 1 << 4,
	Texture = 1 << 5,
	RenderTarget = 1 << 6,
	// VirtualTexture is not a subtype of BaseTexture because they share no common functionality, particularly with respect to sampling.
	VirtualTexture = 1 << 12,
	BaseTexture = Texture | RenderTarget UMETA(Tooltip = "Common base type for both textures and render targets."),
	Surface = Landscape | BaseTexture | VirtualTexture,

	Volume = 1 << 7,
	Primitive = 1 << 8,
	DynamicMesh = 1 << 10,

	StaticMeshResource = 1 << 11,

	/** Simple concrete data. */
	Concrete = Point | PolyLine | Surface | Volume | Primitive | DynamicMesh,

	/** Boolean operations like union, difference, intersection. */
	Composite = 1 << 9 UMETA(Hidden),

	/** Combinations of concrete data and/or boolean operations. */
	Spatial = Composite | Concrete,

	/** Data that represent resources/assets. */
	Resource = StaticMeshResource UMETA(Hidden),

	/** Proxy for data that was created on the GPU and not necessarily read back to CPU. */
	ProxyForGPU = 1 << 26 UMETA(Hidden),

	Param = 1 << 27 UMETA(DisplayName = "Attribute Set"),

	// Combination of Param and Point, necessary for named-based serialization of enums.
	PointOrParam = Point | Param UMETA(DisplayName = "Point or Attribute Set"),

	Settings = 1 << 28 UMETA(Hidden),
	Other = 1 << 29,
	Any = (1 << 30) - 1
};
ENUM_CLASS_FLAGS(EPCGDataType);

// As discussed just before, a parallel version for "exclusive" (as in only type) of the EPCGDataType enum. Needed for blueprint compatibility.
UENUM(BlueprintType, meta=(DisplayName="PCG Data Type"))
enum class EPCGExclusiveDataType : uint8
{
	None = 0 UMETA(Hidden),
	Point,
	Spline,
	LandscapeSpline,
	PolyLine UMETA(DisplayName = "Curve"),
	Landscape,
	Texture,
	RenderTarget,
	VirtualTexture,
	BaseTexture UMETA(Hidden),
	Surface,
	Volume,
	Primitive,
	Concrete,
	Composite UMETA(Hidden),
	Spatial,
	Param UMETA(DisplayName = "Attribute Set"),
	Settings UMETA(Hidden),
	Other,
	Any,
	PointOrParam UMETA(DisplayName = "Point or Attribute Set"),
	DynamicMesh,
	StaticMeshResource,
	Resource UMETA(Hidden)
};

UENUM(meta=(DisplayName="PCG Container Type"))
enum class EPCGContainerType : uint8
{
	Element = 0,
	None = Element,
	Array,
	Map,
	Set
};

namespace PCGValueConstants
{
	constexpr int32 DefaultSeed = 42;
}

namespace PCGPinConstants
{
	const FName DefaultInputLabel = TEXT("In");
	const FName DefaultOutputLabel = TEXT("Out");
	const FName DefaultParamsLabel = TEXT("Overrides");
	UE_DEPRECATED(5.6, "Please use 'DefaultExecutionDependencyLabel' instead.")
	const FName DefaultDependencyOnlyLabel = TEXT("Dependency Only");
	const FName DefaultExecutionDependencyLabel = TEXT("Execution Dependency");

	const FName DefaultInFilterLabel = TEXT("InsideFilter");
	const FName DefaultOutFilterLabel = TEXT("OutsideFilter");

namespace Private
{
	const FName OldDefaultParamsLabel = TEXT("Params");
}

namespace Icons
{
	const FName LoopPinIcon = TEXT("GraphEditor.Macro.Loop_16x");
	const FName FeedbackPinIcon = TEXT("GraphEditor.GetSequenceBinding");
}

#if WITH_EDITOR
namespace Tooltips
{
	const FText ExecutionDependencyTooltip = NSLOCTEXT("PCGCommon", "ExecutionDependencyPinTooltip", "Data passed to this pin will be used to order execution but will otherwise not contribute to the results of this node.");
}
#endif // WITH_EDITOR
} // namespace PCGPinConstants

namespace PCGNodeConstants
{
namespace Icons
{
	const FName CompactNodeConvert = TEXT("PCG.Node.Compact.Convert");
	const FName CompactNodeFilter = TEXT("PCG.Node.Compact.Filter");
}
}

// Metadata used by PCG
namespace PCGObjectMetadata
{
	const FName Overridable = TEXT("PCG_Overridable");
	const FName NotOverridable = TEXT("PCG_NotOverridable");
	const FName OverrideAliases = TEXT("PCG_OverrideAliases");
	const FName DiscardPropertySelection = TEXT("PCG_DiscardPropertySelection");
	const FName DiscardExtraSelection = TEXT("PCG_DiscardExtraSelection");
	const FName EnumMetadataDomain = TEXT("PCG_MetadataDomain");

	// Metadata usable in UPROPERTY for customizing the behavior when displaying the property in a property panel or graph node
	enum
	{
		/// [PropertyMetadata] Indicates that the property is overridable by params.
		PCG_Overridable,

		/// [PropertyMetadata] Indicates that the property is not-overridable by params. Used in structs to hide some parameters
		PCG_NotOverridable,

		/// [PropertyMetadata] Extra names to match for a given property.
		PCG_OverrideAliases,

		/// [PropertyMetadata] For FPCGAttributePropertySelector, won't display the point property items in the dropdown
		PCG_DiscardPropertySelection,

		/// [PropertyMetadata] For FPCGAttributePropertySelector, won't display the extra property items in the dropdown
		PCG_DiscardExtraSelection,
	};

	// Metadata usable in UENUM for customizing the entry in the attribute property selector
	enum
	{
		/// [EnumMetadata] Specify the domain for this entry.
		PCG_MetadataDomain
	};
}

namespace PCGFeatureSwitches
{
	extern PCG_API TAutoConsoleVariable<bool> CVarCheckSamplerMemory;
	extern PCG_API TAutoConsoleVariable<float> CVarSamplerMemoryThreshold;

	namespace Helpers
	{
		/** Checks the cvar for allowed physical and virtual memory ratio to be used with samplers. */
		PCG_API uint64 GetAvailableMemoryForSamplers();
	}
}

namespace PCGSystemSwitches
{
#if WITH_EDITOR
	extern PCG_API TAutoConsoleVariable<bool> CVarPausePCGExecution;
	extern TAutoConsoleVariable<bool> CVarGlobalDisableRefresh;
	extern TAutoConsoleVariable<bool> CVarDirtyLoadAsPreviewOnLoad;
	extern TAutoConsoleVariable<bool> CVarForceDynamicGraphDispatch;
#endif

	extern PCG_API TAutoConsoleVariable<bool> CVarReleaseTransientResourcesEarly;
	extern PCG_API TAutoConsoleVariable<bool> CVarPCGDebugDrawGeneratedCells;
}

/** Describes space referential for operations that create data */
UENUM(BlueprintType)
enum class EPCGCoordinateSpace : uint8
{
	World UMETA(DisplayName = "Global"),
	OriginalComponent,
	LocalComponent
};

UENUM(BlueprintType)
enum class EPCGStringMatchingOperator : uint8
{
	/** Will return a match only if the two strings compared are the same */
	Equal,
	/** Will return a match if the first string contains the second */
	Substring,
	/** Will return a match if the first string matches the pattern defined by the second (including wildcards) */
	Matches
};

/** Describes one or more target execution grids. */
UENUM(meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class EPCGHiGenGrid : uint32
{
	Uninitialized = 0 UMETA(Hidden),

	// NOTE: When adding new grids, increment PCGHiGenGrid::NumGridValues below
	Grid4 = 4 UMETA(DisplayName = "400"),
	Grid8 = 8 UMETA(DisplayName = "800"),
	Grid16 = 16 UMETA(DisplayName = "1600"),
	Grid32 = 32 UMETA(DisplayName = "3200"),
	Grid64 = 64 UMETA(DisplayName = "6400"),
	Grid128 = 128 UMETA(DisplayName = "12800"),
	Grid256 = 256 UMETA(DisplayName = "25600"),
	Grid512 = 512 UMETA(DisplayName = "51200"),
	Grid1024 = 1024 UMETA(DisplayName = "102400"),
	Grid2048 = 2048 UMETA(DisplayName = "204800"),
	Grid4096 = 4096 UMETA(Hidden),
	Grid8192 = 8192 UMETA(Hidden),
	Grid16384 = 16384 UMETA(Hidden),
	Grid32768 = 32768 UMETA(Hidden),
	Grid65536 = 65536 UMETA(Hidden),
	Grid131072 = 131072 UMETA(Hidden),
	Grid262144 = 262144 UMETA(Hidden),
	Grid524288 = 524288 UMETA(Hidden),
	Grid1048576 = 1048576 UMETA(Hidden),
	Grid2097152 = 2097152 UMETA(Hidden),
	Grid4194304 = 4194304 UMETA(Hidden),

	GridMin = Grid4 UMETA(Hidden),
	GridMax = Grid4194304 UMETA(Hidden),

	// Should execute once rather than executing on any grid
	Unbounded = 1u << 31,
};
ENUM_CLASS_FLAGS(EPCGHiGenGrid);

namespace PCGHiGenGrid
{
	// Number of unique values of EPCGHiGenGrid, const so it can be used for the inline allocator below.
	constexpr uint32 NumGridValues = 13;

	// Alias for array which is allocated on the stack (we have a strong idea of the max required elements).
	using FSizeArray = TArray<uint32, TInlineAllocator<PCGHiGenGrid::NumGridValues>>;

	PCG_API bool IsValidGridSize(uint32 InGridSize);
	PCG_API bool IsValidGrid(EPCGHiGenGrid InGrid);
	PCG_API bool IsValidGridOrUninitialized(EPCGHiGenGrid InGrid);
	PCG_API uint32 GridToGridSize(EPCGHiGenGrid InGrid);
	PCG_API EPCGHiGenGrid GridSizeToGrid(uint32 InGridSize);

	PCG_API uint32 UninitializedGridSize();
	PCG_API uint32 UnboundedGridSize();
}

UENUM()
enum class EPCGAttachOptions : uint32
{
	NotAttached UMETA(Tooltip="Actor will not be attached to the target actor nor placed in an actor folder"),
	Attached UMETA(Tooltip="Actor will be attached to the target actor in the given node"),
	InFolder UMETA(Tooltip="Actor will be placed in an actor folder containing the name of the target actor."),
	InGraphFolder UMETA(Tooltip="Actor will be placed in a folder named after the top graph it was generated from."),
	InGeneratedFolder UMETA(Tooltip="Actor will be placed in the PCG_Generated folder.")
};

UENUM()
enum class EPCGEditorDirtyMode : uint8
{
	Normal UMETA(Tooltip="Normal editing mode where generation changes (generation, cleanup) dirty the component and its resources."),
	Preview UMETA(Tooltip="Editing mode where generation changes (generation, cleanup, resources) on the component will not trigger any dirty state, but will also not save any of the generated resources. Also represents the state after loading from the Load as Preview edit mode, where this will hold the last saved generation until a new generation is triggered."),
	LoadAsPreview UMETA(Tooltip="Acts as the normal editing mode until the next load of the component, at which state it acts as-if-transient, namely that any further generation changes will not dirty the component.")
};

USTRUCT(BlueprintType)
struct FPCGRuntimeGenerationRadii
{
	GENERATED_BODY()

public:
	/** Get the runtime generation radius for the given grid size. */
	UE_API double GetGenerationRadiusFromGrid(EPCGHiGenGrid Grid) const;

	/** Compute the runtime cleanup radius for the given grid size. */
	UE_API double GetCleanupRadiusFromGrid(EPCGHiGenGrid Grid) const;

	static constexpr double DefaultGenerationRadiusMultiplier = 2.0;
	static constexpr double DefaultCleanupRadiusMultiplier = 1.1;

	/** The distance (in centimeters) at which the component will be considered for generation by the RuntimeGenerationScheduler. For partitioned components, this also acts as the unbounded generation radius. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius = PCGHiGenGrid::UnboundedGridSize() * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius400 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid4) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius800 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid8) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius1600 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid16) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius3200 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid32) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius6400 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid64) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius12800 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid128) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius25600 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid256) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius51200 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid512) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius102400 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid1024) * DefaultGenerationRadiusMultiplier;

	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation")
	double GenerationRadius204800 = PCGHiGenGrid::GridToGridSize(EPCGHiGenGrid::Grid2048) * DefaultGenerationRadiusMultiplier;

	/** Multiplier on the GenerationRadius to control the distance at which runtime generated components will be cleaned up. Applied per grid size. */
	UPROPERTY(BlueprintReadWrite, EditAnywhere, Category = "Runtime Generation", meta = (UIMin = "1.0", ClampMin = "1.0"))
	double CleanupRadiusMultiplier = DefaultCleanupRadiusMultiplier;
};

struct FInstancedPropertyBag;

namespace PCGDelegates
{
#if WITH_EDITOR
	/** Callback to hook in the UI to detect property bag changes, so the UI is reset and does not try to read in garbage memory. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnInstanceLayoutChanged, const FInstancedPropertyBag& /*Instance*/);
	extern PCG_API FOnInstanceLayoutChanged OnInstancedPropertyBagLayoutChanged;
#endif
}

UENUM()
enum class EPCGNodeTitleType : uint8
{
	/** The full title, may be multiple lines. */
	FullTitle,
	/** More concise, single line title. */
	ListView,
};

namespace PCGQualityHelpers
{
	constexpr int32 NumPins = 6;
	const FName PinLabelDefault = TEXT("Default");
	const FName PinLabelLow = TEXT("Low");
	const FName PinLabelMedium = TEXT("Medium");
	const FName PinLabelHigh = TEXT("High");
	const FName PinLabelEpic = TEXT("Epic");
	const FName PinLabelCinematic = TEXT("Cinematic");

	/** Get the pin label associated with the current 'pcg.Quality' value. If the quality level is invalid, it will return the default pin label. */
	PCG_API FName GetQualityPinLabel();
}

USTRUCT(meta=(Deprecated = "5.5"))
struct UE_DEPRECATED(5.5, "FPCGPartitionActorRecord is deprecated.") FPCGPartitionActorRecord
{
	GENERATED_BODY()
		
	/** Unique ID for the grid this actor belongs to. */
	UPROPERTY(VisibleAnywhere, Category = Debug)
	FGuid GridGuid;

	/** The grid size this actor lives on. */
	UPROPERTY(VisibleAnywhere, Category = Debug)
	uint32 GridSize = 0;

	/** The specific grid cell this actor lives in. */
	UPROPERTY(VisibleAnywhere, Category = Debug)
	FIntVector GridCoords = FIntVector::ZeroValue;

	bool operator==(const FPCGPartitionActorRecord& InOther) const;
	friend uint32 GetTypeHash(const FPCGPartitionActorRecord& In);
};

UENUM(BlueprintType)
enum class EPCGDensityMergeOperation : uint8
{
	/** D = B */
	Set,
	/** D = A */
	Ignore,
	/** D = min(A, B) */
	Minimum,
	/** D = max(A, B) */
	Maximum,
	/** D = A + B */
	Add,
	/** D = A - B */
	Subtract,
	/** D = A * B */
	Multiply,
	/** D = A / B */
	Divide
};

UENUM(BlueprintType)
enum class EPCGGenerationStatus : uint8
{
	Completed,
	Aborted
};

using FPCGElementPtr = TSharedPtr<IPCGElement, ESPMode::ThreadSafe>;

struct FPCGScheduleGraphParams
{
	FPCGScheduleGraphParams(
		UPCGGraph* InGraph,
		IPCGGraphExecutionSource* InExecutionSource,
		FPCGElementPtr InPreGraphElement,
		FPCGElementPtr InInputElement,
		TArray<FPCGTaskId> InExternalDependencies,
		const FPCGStack* InFromStack,
		bool bInAllowHierarchicalGeneration)
		: Graph(InGraph)
		, ExecutionSource(InExecutionSource)
		, PreGraphElement(InPreGraphElement)
		, InputElement(InInputElement)
		, ExternalDependencies(MoveTemp(InExternalDependencies))
		, FromStack(InFromStack)
		, bAllowHierarchicalGeneration(bInAllowHierarchicalGeneration)
	{
	}

	// Graph to execute
	UPCGGraph* Graph = nullptr;

	// PCG execution source associated with this task. Can be null.
	IPCGGraphExecutionSource* ExecutionSource = nullptr;

	// First task to run.
	FPCGElementPtr PreGraphElement;

	// Task to run as an input to the provided graph.
	FPCGElementPtr InputElement;
	
	// PreGraph Task dependencies (will wait on those to finish before executing anything).
	TArray<FPCGTaskId> ExternalDependencies;
	
	// When scheduling sub-graphs, this is the parent execution stack.
	const FPCGStack* FromStack;
	
	// If graph is allowed to use hierarchical generation.
	bool bAllowHierarchicalGeneration = true;
};

struct FPCGScheduleGenericParams
{
	FPCGScheduleGenericParams(TFunction<bool(FPCGContext*)> InOperation, IPCGGraphExecutionSource* InExecutionSource)
		: Operation(MoveTemp(InOperation)), ExecutionSource(InExecutionSource)
	{
	}

	FPCGScheduleGenericParams(
		TFunction<bool(FPCGContext*)> InOperation, 
		IPCGGraphExecutionSource* InExecutionSource, 
		const TArray<FPCGTaskId>& InExecutionDependencies, 
		const TArray<FPCGTaskId>& InDataDependencies, 
		bool bInSupportBasePointDataInput)
		: Operation(MoveTemp(InOperation))
		, ExecutionSource(InExecutionSource)
		, ExecutionDependencies(InExecutionDependencies)
		, DataDependencies(InDataDependencies)
		, bSupportBasePointDataInput(bInSupportBasePointDataInput)
	{
	}
		
	FPCGScheduleGenericParams(
		TFunction<bool(FPCGContext*)> InOperation, 
		TFunction<void(FPCGContext*)> InAbortOperation, 
		IPCGGraphExecutionSource* InExecutionSource,
		const TArray<FPCGTaskId>& InExecutionDependencies, 
		const TArray<FPCGTaskId>& InDataDependencies = {}, 
		bool bInSupportBasePointDataInput = false)
		: Operation(MoveTemp(InOperation))
		, ExecutionSource(InExecutionSource)
		, AbortOperation(MoveTemp(InAbortOperation))
		, ExecutionDependencies(InExecutionDependencies)
		, DataDependencies(InDataDependencies)
		, bSupportBasePointDataInput(bInSupportBasePointDataInput)
	{
	}

	// Callback that takes a Context as argument and returns true if the task is done, false otherwise
	TFunction<bool(FPCGContext*)> Operation;

	// PCG execution source associated with this task. Can be null.
	IPCGGraphExecutionSource* ExecutionSource = nullptr;

	// Callback that is called if the task is aborted (cancelled) before fully executed.
	TFunction<void(FPCGContext*)> AbortOperation;

	// Task will wait on these tasks to execute and won't take their output data as input.
	TArray<FPCGTaskId> ExecutionDependencies;

	// Task will wait on these tasks to execute and will take their output data as input.
	TArray<FPCGTaskId> DataDependencies;

	// When true, generic element will not convert input to UPCGPointData, this is false by default to preserver backward compatibility.
	bool bSupportBasePointDataInput = false;

	// When false, generic element can be executed outside of the game thread.
	bool bCanExecuteOnlyOnMainThread = true;
};

/** A handle to a call stack which holds onto the stack memory. */
struct FPCGStackHandle
{
	FPCGStackHandle() = default;
	UE_API FPCGStackHandle(TSharedPtr<const FPCGStackContext> InStackContext, int32 InStackIndex);

	UE_API bool IsValid() const;

	UE_API const FPCGStack* GetStack() const;

private:
	TSharedPtr<const FPCGStackContext> StackContext;

	int32 StackIndex = INDEX_NONE;
};

// Enable to debug if some PCG Elements are creating some data that should be pre-created by the graph PreGraph element
#define PCG_EXECUTION_CACHE_VALIDATION_ENABLED 0

#if PCG_EXECUTION_CACHE_VALIDATION_ENABLED

#define PCG_EXECUTION_CACHE_VALIDATION_CREATE_SCOPE(PCGComponent) TGuardValue<bool> ValidationCreateScope(PCGComponent->bCanCreateExecutionCache, true);
#define PCG_EXECUTION_CACHE_VALIDATION_CREATE_ORIGINAL_SCOPE(PCGComponent) TGuardValue<bool> ValidationCreateOriginalScope(PCGComponent->GetOriginalComponent()->bCanCreateExecutionCache, PCGComponent->bCanCreateExecutionCache);
#define PCG_EXECUTION_CACHE_VALIDATION_CHECK(PCGComponent) ensureAlways(PCGComponent->bCanCreateExecutionCache || PCGComponent->CurrentGenerationTask == InvalidPCGTaskId);

#else

#define PCG_EXECUTION_CACHE_VALIDATION_CREATE_SCOPE(PCGComponent)
#define PCG_EXECUTION_CACHE_VALIDATION_CREATE_ORIGINAL_SCOPE(PCGComponent)
#define PCG_EXECUTION_CACHE_VALIDATION_CHECK(PCGComponent)

#endif // PCG_EXECUTION_CACHE_VALIDATION_ENABLED

#undef UE_API
