// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ProfilingDebugging/CsvProfilerConfig.h"
#include "AutoRTFM.h"
#include "Misc/MTTransactionallySafeAccessDetector.h"
#include "MassEntityTypes.h"
#include "MassEntityUtils.h"
#include "MassEntityManager.h"
#include "MassCommands.generated.h"

/**
 * Enum used by MassBatchCommands to declare their "type". This data is later used to group commands so that command 
 * effects are applied in a controllable fashion 
 * Important: if changed make sure to update FMassCommandBuffer::Flush.CommandTypeOrder as well
 */
UENUM()
enum class EMassCommandOperationType : uint8
{
	None,				// default value. Commands marked this way will be always executed last. Programmers are encouraged to instead use one of the meaningful values below.
	Create,				// signifies commands performing entity creation
	Add,				// signifies commands adding fragments or tags to entities
	Remove,				// signifies commands removing fragments or tags from entities
	ChangeComposition,	// signifies commands both adding and removing fragments and/or tags from entities
	Set,				// signifies commands setting values to pre-existing fragments. The fragments might be added if missing,
						// depending on specific command, so this group will always be executed after the Add group
	Destroy,			// signifies commands removing entities
	MAX
};

enum class EMassCommandCheckTime : bool
{
	RuntimeCheck = true,
	CompileTimeCheck = false
};

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
#	define DEBUG_NAME(Name) , FName(TEXT(Name))
#	define DEBUG_NAME_PARAM(Name) , const FName InDebugName = TEXT(Name)
#	define FORWARD_DEBUG_NAME_PARAM , InDebugName
#else
#	define DEBUG_NAME(Name)
#	define DEBUG_NAME_PARAM(Name)
#	define FORWARD_DEBUG_NAME_PARAM
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

namespace UE::Mass::Utils
{
	template<typename BitSetType, EMassCommandCheckTime CheckTime, typename... TTypes>
	BitSetType ConstructBitSet()
	{
		if constexpr (CheckTime == EMassCommandCheckTime::RuntimeCheck)
		{
			return BitSetType({ TTypes::StaticStruct()... });
		}
		else
		{
			BitSetType Result;
			UE::Mass::TMultiTypeList<TTypes...>::PopulateBitSet(Result);
			return Result;
		}
	}

	template<EMassCommandCheckTime CheckTime, typename... TTypes>
	FMassFragmentBitSet ConstructFragmentBitSet()
	{
		return ConstructBitSet<FMassFragmentBitSet, CheckTime, TTypes...>();
	}

	template<EMassCommandCheckTime CheckTime, typename... TTypes>
	FMassTagBitSet ConstructTagBitSet()
	{
		return ConstructBitSet<FMassTagBitSet, CheckTime, TTypes...>();
	}
} // namespace UE::Mass::Utils

struct FMassBatchedCommand
{
	FMassBatchedCommand() = default;
	explicit FMassBatchedCommand(EMassCommandOperationType OperationType)
		: OperationType(OperationType)
	{}
#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	FMassBatchedCommand(EMassCommandOperationType OperationType, FName DebugName)
		: OperationType(OperationType)
		, DebugName(DebugName)
	{}
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	virtual ~FMassBatchedCommand() { Reset(); }

	virtual void Execute(FMassEntityManager& EntityManager) const = 0;
	virtual void Reset()
	{
		bHasWork = false;
	}

	bool HasWork() const { return bHasWork; }
	EMassCommandOperationType GetOperationType() const { return OperationType; }
	
	template<typename T>
	UE_AUTORTFM_ALWAYS_OPEN
	static uint32 GetCommandIndex()
	{
		static const uint32 ThisTypesStaticIndex = CommandsCounter++;
		return ThisTypesStaticIndex;
	}

	virtual SIZE_T GetAllocatedSize() const = 0;

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	virtual int32 GetNumOperationsStat() const = 0;
	FName GetFName() const { return DebugName; }
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

protected:
	// @todo note for reviewers - I could use an opinion if having a virtual function per-command would be a more 
	// preferable way of asking commands if there's anything to do.
	bool bHasWork = false;
	EMassCommandOperationType OperationType = EMassCommandOperationType::None;

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	FName DebugName;
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

private:
	static MASSENTITY_API std::atomic<uint32> CommandsCounter;
};

struct FMassBatchedEntityCommand : public FMassBatchedCommand
{
	using Super = FMassBatchedCommand;

	FMassBatchedEntityCommand() = default;
	explicit FMassBatchedEntityCommand(EMassCommandOperationType OperationType DEBUG_NAME_PARAM("BatchedEntityCommand"))
		: Super(OperationType FORWARD_DEBUG_NAME_PARAM)
	{}

	void Add(FMassEntityHandle Entity)
	{
		UE_MT_SCOPED_WRITE_ACCESS(EntitiesAccessDetector);
		TargetEntities.Add(Entity);
		bHasWork = true;
	}

	void Add(TConstArrayView<FMassEntityHandle> Entities)
	{
		UE_MT_SCOPED_WRITE_ACCESS(EntitiesAccessDetector);
		TargetEntities.Append(Entities.GetData(), Entities.Num());
		bHasWork = true;
	}

	void Add(TArray<FMassEntityHandle>&& Entities)
	{
		UE_MT_SCOPED_WRITE_ACCESS(EntitiesAccessDetector);
		TargetEntities.Append(Forward<TArray<FMassEntityHandle>>(Entities));
		bHasWork = true;
	}

protected:
	virtual SIZE_T GetAllocatedSize() const
	{
		return TargetEntities.GetAllocatedSize();
	}

	virtual void Reset() override
	{
		TargetEntities.Reset();
		Super::Reset();
	}

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	virtual int32 GetNumOperationsStat() const override { return TargetEntities.Num(); }
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

	UE_MT_DECLARE_TS_RW_ACCESS_DETECTOR(EntitiesAccessDetector); 
	TArray<FMassEntityHandle> TargetEntities;
};

//-----------------------------------------------------------------------------
// Entity destruction
//-----------------------------------------------------------------------------
struct FMassCommandDestroyEntities : public FMassBatchedEntityCommand
{
	using Super = FMassBatchedEntityCommand;

	FMassCommandDestroyEntities()
		: Super(EMassCommandOperationType::Destroy DEBUG_NAME("DestroyEntities"))
	{
	}

protected:
	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandDestroyEntities_Execute);

		TArray<FMassArchetypeEntityCollection> EntityCollectionsToDestroy;
		UE::Mass::Utils::CreateEntityCollections(EntityManager, TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates, EntityCollectionsToDestroy);
		EntityManager.BatchDestroyEntityChunks(EntityCollectionsToDestroy);
	}
};

//-----------------------------------------------------------------------------
// Simple fragment composition change
//-----------------------------------------------------------------------------
template<EMassCommandCheckTime CheckTime, typename... TTypes>
struct FMassCommandAddFragmentsInternal : public FMassBatchedEntityCommand
{
	using Super = FMassBatchedEntityCommand;
	FMassCommandAddFragmentsInternal()
		: Super(EMassCommandOperationType::Add DEBUG_NAME("AddFragments"))
		, FragmentsAffected(UE::Mass::Utils::ConstructFragmentBitSet<CheckTime, TTypes...>())
	{}

protected:
	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandAddFragments_Execute);
		TArray<FMassArchetypeEntityCollection> EntityCollections;
		UE::Mass::Utils::CreateEntityCollections(EntityManager, TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates, EntityCollections);
		EntityManager.BatchChangeFragmentCompositionForEntities(EntityCollections, FragmentsAffected, FMassFragmentBitSet());
	}
	FMassFragmentBitSet FragmentsAffected;
};

template<typename... TTypes>
using FMassCommandAddFragments = FMassCommandAddFragmentsInternal<EMassCommandCheckTime::CompileTimeCheck, TTypes...>;

template<EMassCommandCheckTime CheckTime, typename... TTypes>
struct FMassCommandRemoveFragmentsInternal : public FMassBatchedEntityCommand
{
	using Super = FMassBatchedEntityCommand;
	FMassCommandRemoveFragmentsInternal()
		: Super(EMassCommandOperationType::Remove DEBUG_NAME("RemoveFragments"))
		, FragmentsAffected(UE::Mass::Utils::ConstructFragmentBitSet<CheckTime, TTypes...>())
	{}

protected:
	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandRemoveFragments_Execute);
		TArray<FMassArchetypeEntityCollection> EntityCollections;
		UE::Mass::Utils::CreateEntityCollections(EntityManager, TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates, EntityCollections);
		EntityManager.BatchChangeFragmentCompositionForEntities(EntityCollections, FMassFragmentBitSet(), FragmentsAffected);
	}
	FMassFragmentBitSet FragmentsAffected;
};

template<typename... TTypes>
using FMassCommandRemoveFragments = FMassCommandRemoveFragmentsInternal<EMassCommandCheckTime::CompileTimeCheck, TTypes...>;

//-----------------------------------------------------------------------------
// Simple tag composition change
//-----------------------------------------------------------------------------
struct FMassCommandChangeTags : public FMassBatchedEntityCommand
{
	using Super = FMassBatchedEntityCommand;
	FMassCommandChangeTags()
		: Super(EMassCommandOperationType::ChangeComposition DEBUG_NAME("ChangeTags"))
	{}

	FMassCommandChangeTags(EMassCommandOperationType OperationType, FMassTagBitSet TagsToAdd, FMassTagBitSet TagsToRemove DEBUG_NAME_PARAM("ChangeTags"))
		: Super(OperationType FORWARD_DEBUG_NAME_PARAM)
		, TagsToAdd(TagsToAdd)
		, TagsToRemove(TagsToRemove)
	{}

protected:
	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandChangeTags_Execute);
		TArray<FMassArchetypeEntityCollection> EntityCollections;
		UE::Mass::Utils::CreateEntityCollections(EntityManager, TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates, EntityCollections);

		EntityManager.BatchChangeTagsForEntities(EntityCollections, TagsToAdd, TagsToRemove);
	}

	virtual SIZE_T GetAllocatedSize() const override
	{
		return TagsToAdd.GetAllocatedSize() + TagsToRemove.GetAllocatedSize() + Super::GetAllocatedSize();
	}

	FMassTagBitSet TagsToAdd;
	FMassTagBitSet TagsToRemove;
};

template<EMassCommandCheckTime CheckTime, typename... TTypes>
struct FMassCommandAddTagsInternal : public FMassCommandChangeTags
{
	using Super = FMassCommandChangeTags;
	FMassCommandAddTagsInternal()
		: Super(
			EMassCommandOperationType::Add, 
			UE::Mass::Utils::ConstructTagBitSet<CheckTime, TTypes...>(),
			{} 
			DEBUG_NAME("AddTags"))
	{}
};

template<typename T>
using FMassCommandAddTag = FMassCommandAddTagsInternal<EMassCommandCheckTime::CompileTimeCheck, T>;

template<typename... TTypes>
using FMassCommandAddTags = FMassCommandAddTagsInternal<EMassCommandCheckTime::CompileTimeCheck, TTypes...>;

template<EMassCommandCheckTime CheckTime, typename... TTypes>
struct FMassCommandRemoveTagsInternal : public FMassCommandChangeTags
{
	using Super = FMassCommandChangeTags;
	FMassCommandRemoveTagsInternal()
		: Super(
			EMassCommandOperationType::Remove, 
			{}, 
			UE::Mass::Utils::ConstructTagBitSet<CheckTime, TTypes...>()
			DEBUG_NAME("RemoveTags"))
	{}
};

template<typename T>
using FMassCommandRemoveTag = FMassCommandRemoveTagsInternal<EMassCommandCheckTime::CompileTimeCheck, T>;

template<typename... TTypes>
using FMassCommandRemoveTags = FMassCommandRemoveTagsInternal<EMassCommandCheckTime::CompileTimeCheck, TTypes...>;

template<EMassCommandCheckTime CheckTime, typename TOld, typename TNew>
struct FMassCommandSwapTagsInternal : public FMassCommandChangeTags
{
	using Super = FMassCommandChangeTags;
	FMassCommandSwapTagsInternal()
		: Super(
			EMassCommandOperationType::ChangeComposition,
			UE::Mass::Utils::ConstructTagBitSet<CheckTime, TNew>(),
			UE::Mass::Utils::ConstructTagBitSet<CheckTime, TOld>()
			DEBUG_NAME("SwapTags"))
	{}
};

template<typename TOld, typename TNew>
using FMassCommandSwapTags = FMassCommandSwapTagsInternal<EMassCommandCheckTime::CompileTimeCheck, TOld, TNew>;

//-----------------------------------------------------------------------------
// Struct Instances adding and setting
//-----------------------------------------------------------------------------
template<typename... TOthers>
struct FMassCommandAddFragmentInstances : public FMassBatchedEntityCommand
{
	using Super = FMassBatchedEntityCommand;

	FMassCommandAddFragmentInstances(EMassCommandOperationType OperationType = EMassCommandOperationType::Set DEBUG_NAME_PARAM("AddFragmentInstanceList"))
		: Super(EMassCommandOperationType::Set FORWARD_DEBUG_NAME_PARAM)
		, FragmentsAffected(UE::Mass::Utils::ConstructFragmentBitSet<EMassCommandCheckTime::CompileTimeCheck, TOthers...>())
	{}

	void Add(FMassEntityHandle Entity, TOthers... InFragments)
	{
		Super::Add(Entity);
		Fragments.Add(InFragments...);
	}

protected:
	virtual void Reset() override
	{
		Fragments.Reset();
		Super::Reset();
	}

	virtual SIZE_T GetAllocatedSize() const override
	{
		return Super::GetAllocatedSize() + Fragments.GetAllocatedSize() + FragmentsAffected.GetAllocatedSize();
	}

	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandAddFragmentInstances_Execute);

		TArray<FStructArrayView> GenericMultiArray;
		GenericMultiArray.Reserve(Fragments.GetNumArrays());
		Fragments.GetAsGenericMultiArray(GenericMultiArray);

		TArray<FMassArchetypeEntityCollectionWithPayload> EntityCollections;
		FMassArchetypeEntityCollectionWithPayload::CreateEntityRangesWithPayload(EntityManager, TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates
			, FMassGenericPayloadView(GenericMultiArray), EntityCollections);

		EntityManager.BatchAddFragmentInstancesForEntities(EntityCollections, FragmentsAffected);
	}

	mutable UE::Mass::TMultiArray<TOthers...> Fragments;
	const FMassFragmentBitSet FragmentsAffected;
};

template<typename... TOthers>
struct FMassCommandBuildEntity : public FMassCommandAddFragmentInstances<TOthers...>
{
	using Super = FMassCommandAddFragmentInstances<TOthers...>;

	FMassCommandBuildEntity()
		: Super(EMassCommandOperationType::Create DEBUG_NAME("BuildEntity"))
	{
	}

protected:

	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandBuildEntity_Execute);

		TArray<FStructArrayView> GenericMultiArray;
		GenericMultiArray.Reserve(Super::Fragments.GetNumArrays());
		Super::Fragments.GetAsGenericMultiArray(GenericMultiArray);

		TArray<FMassArchetypeEntityCollectionWithPayload> EntityCollections;
		FMassArchetypeEntityCollectionWithPayload::CreateEntityRangesWithPayload(EntityManager, Super::TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates
			, FMassGenericPayloadView(GenericMultiArray), EntityCollections);

		check(EntityCollections.Num() <= 1);
		if (EntityCollections.Num())
		{
			EntityManager.BatchBuildEntities(EntityCollections[0], Super::FragmentsAffected, FMassArchetypeSharedFragmentValues());
		}
	}
};

/** 
 * Note: that TSharedFragmentValues is always expected to be FMassArchetypeSharedFragmentValues, but is declared as 
 *	template's param to maintain uniform command adding interface via FMassCommandBuffer.PushCommand. 
 *	PushCommands received all input params in one `typename...` list and as such cannot be easily split up to reason about.
 */
template<typename TSharedFragmentValues, typename... TOthers>
struct FMassCommandBuildEntityWithSharedFragments : public FMassBatchedCommand
{
	using Super = FMassBatchedCommand;

	FMassCommandBuildEntityWithSharedFragments()
		: Super(EMassCommandOperationType::Create DEBUG_NAME("FMassCommandBuildEntityWithSharedFragments"))
		, FragmentsAffected(UE::Mass::Utils::ConstructFragmentBitSet<EMassCommandCheckTime::CompileTimeCheck, TOthers...>())
	{}

	void Add(FMassEntityHandle Entity, FMassArchetypeSharedFragmentValues&& InSharedFragments, TOthers... InFragments)
	{
		InSharedFragments.Sort();

		// Compute hash before adding to the map since evaluation order is not guaranteed
		// and MoveTemp will invalidate InSharedFragments
		const uint32 Hash = GetTypeHash(InSharedFragments);

		FPerSharedFragmentsHashData& Instance = Data.FindOrAdd(Hash, MoveTemp(InSharedFragments));
		Instance.Fragments.Add(InFragments...);
		Instance.TargetEntities.Add(Entity);

		bHasWork = true;
	}

protected:
	virtual SIZE_T GetAllocatedSize() const override
	{
		SIZE_T TotalSize = 0;
		for (const auto& KeyValue : Data)
		{
			TotalSize += KeyValue.Value.GetAllocatedSize();
		}
		TotalSize += Data.GetAllocatedSize();
		TotalSize += FragmentsAffected.GetAllocatedSize();
		return TotalSize;
	}

	virtual void Execute(FMassEntityManager& EntityManager) const 
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassCommandBuildEntityWithSharedFragments_Execute);

		constexpr int FragmentTypesCount = UE::Mass::TMultiTypeList<TOthers...>::Ordinal + 1;
		TArray<FStructArrayView> GenericMultiArray;
		GenericMultiArray.Reserve(FragmentTypesCount);

		for (auto It : Data)
		{			
			It.Value.Fragments.GetAsGenericMultiArray(GenericMultiArray);

			TArray<FMassArchetypeEntityCollectionWithPayload> EntityCollections;
			FMassArchetypeEntityCollectionWithPayload::CreateEntityRangesWithPayload(EntityManager, It.Value.TargetEntities, FMassArchetypeEntityCollection::FoldDuplicates
				, FMassGenericPayloadView(GenericMultiArray), EntityCollections);
			checkf(EntityCollections.Num() <= 1, TEXT("We expect TargetEntities to only contain archetype-less entities, ones that need to be \'build\'"));

			if (EntityCollections.Num())
			{
				EntityManager.BatchBuildEntities(EntityCollections[0], FragmentsAffected, It.Value.SharedFragmentValues);
			}

			GenericMultiArray.Reset();
		}
	}

	virtual void Reset() override
	{
		Data.Reset();
		Super::Reset();
	}

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	virtual int32 GetNumOperationsStat() const override
	{
		int32 TotalCount = 0;
		for (const auto& KeyValue : Data)
		{
			TotalCount += KeyValue.Value.TargetEntities.Num();
		}
		return TotalCount;
	}
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

	FMassFragmentBitSet FragmentsAffected;

	struct FPerSharedFragmentsHashData
	{
		FPerSharedFragmentsHashData(FMassArchetypeSharedFragmentValues&& InSharedFragmentValues)
			: SharedFragmentValues(MoveTemp(InSharedFragmentValues))
		{	
		}

		SIZE_T GetAllocatedSize() const
		{
			return TargetEntities.GetAllocatedSize() + Fragments.GetAllocatedSize() + SharedFragmentValues.GetAllocatedSize();
		}

		TArray<FMassEntityHandle> TargetEntities;
		mutable UE::Mass::TMultiArray<TOthers...> Fragments;
		FMassArchetypeSharedFragmentValues SharedFragmentValues;
	};

	TMap<uint32, FPerSharedFragmentsHashData> Data;
};

//-----------------------------------------------------------------------------
// Commands that really can't know the types at compile time
//-----------------------------------------------------------------------------
template<EMassCommandOperationType OpType>
struct FMassDeferredCommand : public FMassBatchedCommand
{
	using Super = FMassBatchedCommand;
	using FExecFunction = TFunction<void(FMassEntityManager& EntityManager)>;

	FMassDeferredCommand()
		: Super(OpType DEBUG_NAME("BatchedDeferredCommand"))
	{}

	void Add(FExecFunction&& ExecFunction)
	{
		DeferredFunctions.Add(MoveTemp(ExecFunction));
		bHasWork = true;
	}

	void Add(const FExecFunction& ExecFunction)
	{
		DeferredFunctions.Add(ExecFunction);
		bHasWork = true;
	}

protected:
	virtual SIZE_T GetAllocatedSize() const
	{
		return DeferredFunctions.GetAllocatedSize();
	}

	virtual void Execute(FMassEntityManager& EntityManager) const override
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(MassDeferredCommand_Execute);

		for (const FExecFunction& ExecFunction : DeferredFunctions)
		{
			ExecFunction(EntityManager);
		}
	}

	virtual void Reset() override
	{
		DeferredFunctions.Reset();
		Super::Reset();
	}

#if CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG
	virtual int32 GetNumOperationsStat() const override
	{
		return DeferredFunctions.Num();
	}
#endif // CSV_PROFILER_STATS || WITH_MASSENTITY_DEBUG

	TArray<FExecFunction> DeferredFunctions;
};

using FMassDeferredCreateCommand = FMassDeferredCommand<EMassCommandOperationType::Create>;
using FMassDeferredAddCommand = FMassDeferredCommand<EMassCommandOperationType::Add>;
using FMassDeferredRemoveCommand = FMassDeferredCommand<EMassCommandOperationType::Remove>;
using FMassDeferredChangeCompositionCommand = FMassDeferredCommand<EMassCommandOperationType::ChangeComposition>;
using FMassDeferredSetCommand = FMassDeferredCommand<EMassCommandOperationType::Set>;
using FMassDeferredDestroyCommand = FMassDeferredCommand<EMassCommandOperationType::Destroy>;

#undef DEBUG_NAME
#undef DEBUG_NAME_PARAM
#undef FORWARD_DEBUG_NAME_PARAM
