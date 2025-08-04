// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	UObjectArray.h: Unreal object array
=============================================================================*/

#pragma once

#include "AutoRTFM.h"
#include "HAL/ThreadSafeCounter.h"
#include "Containers/LockFreeList.h"
#include "Misc/ScopeLock.h"
#include "Misc/TransactionallySafeCriticalSection.h"
#include "UObject/GarbageCollectionGlobals.h"
#include "UObject/UObjectBase.h"

#if WITH_VERSE_VM || defined(__INTELLISENSE__)
namespace Verse
{
	struct VCell;
}
#endif

#ifndef UE_PACK_FUOBJECT_ITEM
#	define UE_ENABLE_FUOBJECT_ITEM_PACKING 0
#elif PLATFORM_CPU_ARM_FAMILY && FORCE_ANSI_ALLOCATOR
//	disable packing on ARM64 is we use Ansi allocator as it might use MTE or HWASan which uses top byte of a pointer and this feature will discard that
#	define UE_ENABLE_FUOBJECT_ITEM_PACKING 0
#else
#	define UE_ENABLE_FUOBJECT_ITEM_PACKING UE_PACK_FUOBJECT_ITEM
#endif

/**
* Controls whether the number of available elements is being tracked in the ObjObjects array.
* By default it is only tracked in WITH_EDITOR builds as it adds a small amount of tracking overhead
*/
#if !defined(UE_GC_TRACK_OBJ_AVAILABLE)
#define UE_GC_TRACK_OBJ_AVAILABLE UE_DEPRECATED_MACRO(5.2, "The UE_GC_TRACK_OBJ_AVAILABLE macro has been deprecated because it is no longer necessary.") 1
#endif

namespace UE::GC::Private
{
	class FGCFlags;
}

/**
* Single item in the UObject array.
*/
struct FUObjectItem
{
	friend class FUObjectArray;
	friend class UE::GC::Private::FGCFlags;

#if !UE_ENABLE_FUOBJECT_ITEM_PACKING
public:
	union
	{
		// Pointer to the allocated object
		UE_DEPRECATED(5.6, "Use GetObject() and SetObject() to access Object.")
		class UObjectBase* Object = nullptr;
		uint32 ObjectPtrLow;	// this one is used as a dummy for natvis only an will be removed once packing will be enabled by default
	};
#else
private:
	union
	{
		// Stores lower 32 bits of UObject pointer shifted by 3 to the left as all our allocations are at leas 8 bytes aligned and lower 3 bits will always be 0
		uint32 ObjectPtrLow = 0;
		uint32 Object;	// this one is used as a dummy for natvis only an will be removed once packing will be enabled by default
	};
#endif

private:
	// Currently we assume UObjects are aligned by 8 bytes, that gives us 3 lower bits as zeros that we can discard.
	// This will give us total 45 bits in a pointer that we pack into a int32 and the remaining 13 bits we pack with Flags
	// EInternalObjectFlags_MinFlagBitIndex at the time of writing this is 14 and we have only 1 bit left in the EInternalObjectFlags for future use
	// We can increase UObject alignment to 16 bytes to get one more bit and reduce the overall addressable virtual memory range to get more bits if necessary
	constexpr static int32 UObjectAlignment = 8;
	constexpr static int32 UObjectPtrTrailingZeroes = FMath::CountTrailingZerosConstExpr(UObjectAlignment);
	static_assert(int(EInternalObjectFlags_MinFlagBitIndex) >= 48 - 32 - UObjectPtrTrailingZeroes, "We need at least 13 bits to pack higher bits of a UObject pointer into Flags");
	constexpr static int32 FlagsMask = 0xFFFFFFFF << int(EInternalObjectFlags_MinFlagBitIndex);
	constexpr static int32 PtrMask = ~FlagsMask;

	// Stores EInternalObjectFlags (and higher 13 bits of the UObject pointer packed together if UE_PACK_FUOBJECT_ITEM is set to 1)
	// These can only be changed via Set* and Clear* functions
	int32 Flags;

public:
	// UObject Owner Cluster Index
	int32 ClusterRootIndex;	
	// Weak Object Pointer Serial number associated with the object
	int32 SerialNumber;
	// RefCount associated with the object preventing its destruction.
	int32 RefCount;

#if UE_WITH_REMOTE_OBJECT_HANDLE
	// Globally unique id of this object
	FRemoteObjectId RemoteId;
#endif

#if STATS || ENABLE_STATNAMEDEVENTS_UOBJECT
	/** Stat id of this object, 0 if nobody asked for it yet */
	mutable TStatId StatID;

#if ENABLE_STATNAMEDEVENTS_UOBJECT
	mutable PROFILER_CHAR* StatIDStringStorage;
#endif
#endif // STATS || ENABLE_STATNAMEDEVENTS

	FUObjectItem()
		: Flags(0)
		, ClusterRootIndex(0)
		, SerialNumber(0)
		, RefCount(0)
#if ENABLE_STATNAMEDEVENTS_UOBJECT
		, StatIDStringStorage(nullptr)
#endif
	{
	}
	~FUObjectItem()
	{
#if ENABLE_STATNAMEDEVENTS_UOBJECT
		if (PROFILER_CHAR* Storage = StatIDStringStorage)
		{
			AutoRTFM::PopOnAbortHandler(Storage);
			delete[] Storage;
		}
#endif
	}

	// Non-copyable
	FUObjectItem(FUObjectItem&&) = delete;
	FUObjectItem(const FUObjectItem&) = delete;
	FUObjectItem& operator=(FUObjectItem&&) = delete;
	FUObjectItem& operator=(const FUObjectItem&) = delete;

	FORCEINLINE class UObjectBase* GetObject() const
	{
#if UE_ENABLE_FUOBJECT_ITEM_PACKING
		const uintptr_t Obj = (uintptr_t(Flags & PtrMask) << (32 + UObjectPtrTrailingZeroes)) | (uintptr_t(ObjectPtrLow) << UObjectPtrTrailingZeroes);
		return (class UObjectBase*)Obj;
#else
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		return Object;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	}

	FORCEINLINE void SetObject(class UObjectBase* InObject)
	{
#if UE_ENABLE_FUOBJECT_ITEM_PACKING
		const uint32 ObjectPtrHi = ((uintptr_t)InObject & 0xFFFF00000000ULL) >> (32 + UObjectPtrTrailingZeroes);
		Flags |= ObjectPtrHi;
		ObjectPtrLow = ((uintptr_t)InObject >> UObjectPtrTrailingZeroes) & 0xFFFFFFFF;
#else
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Object = InObject;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
	}

	FORCEINLINE void SetOwnerIndex(int32 OwnerIndex)
	{
		ClusterRootIndex = OwnerIndex;
	}

	FORCEINLINE int32 GetOwnerIndex() const
	{
		return ClusterRootIndex;
	}

	/** Encodes the cluster index in the ClusterRootIndex variable */
	FORCEINLINE void SetClusterIndex(int32 ClusterIndex)
	{
		ClusterRootIndex = -ClusterIndex - 1;
	}

	/** Decodes the cluster index from the ClusterRootIndex variable */
	FORCEINLINE int32 GetClusterIndex() const
	{
		checkSlow(ClusterRootIndex < 0);
		return -ClusterRootIndex - 1;
	}

	FORCEINLINE int32 GetSerialNumber() const
	{
		return SerialNumber;
	}

#if UE_WITH_REMOTE_OBJECT_HANDLE
	FORCEINLINE FRemoteObjectId GetRemoteId() const
	{
		return RemoteId;
	}
#endif // UE_WITH_REMOTE_OBJECT_HANDLE

	FORCEINLINE void SetFlags(EInternalObjectFlags FlagsToSet)
	{
		ThisThreadAtomicallySetFlag(FlagsToSet);
	}

	FORCEINLINE EInternalObjectFlags GetFlags() const
	{
#if UE_ENABLE_FUOBJECT_ITEM_PACKING
		return EInternalObjectFlags(GetFlagsInternal() & FlagsMask);
#else
		return EInternalObjectFlags(GetFlagsInternal());
#endif
	}

	FORCEINLINE void ClearFlags(EInternalObjectFlags FlagsToClear)
	{
		ThisThreadAtomicallyClearedFlag(FlagsToClear);
	}

	/**
	 * Uses atomics to clear the specified flag(s). GC internal version
	 * @param FlagsToClear
	 * @return True if this call cleared the flag, false if it has been cleared by another thread.
	 */
	UE_DEPRECATED(5.5, "ThisThreadAtomicallyClearedFlag_ForGC() can only be used by the garbage collector. Use ThisThreadAtomicallyClearedFlag instead.")
	FORCEINLINE bool ThisThreadAtomicallyClearedFlag_ForGC(EInternalObjectFlags FlagToClear)
	{
		return AtomicallyClearFlag_ForGC(FlagToClear);
	}

	/**
	 * Uses atomics to clear the specified flag(s).
	 * @param FlagsToClear
	 * @return True if this call cleared the flag, false if it has been cleared by another thread.
	 */
	FORCEINLINE bool ThisThreadAtomicallyClearedFlag(EInternalObjectFlags FlagToClear)
	{
		checkf((int32(FlagToClear) & ~int32(EInternalObjectFlags_AllFlags)) == 0, TEXT("%d is not a valid internal flag value"), int32(FlagToClear));
		bool Result = false;
		UE_AUTORTFM_OPEN
		{
			FlagToClear &= ~EInternalObjectFlags_ReachabilityFlags; // reachability flags can only be cleared by GC through *_ForGC functions
			FlagToClear &= ~EInternalObjectFlags::RefCounted; // refcounted flag is internal and must only be cleared internally by AddRef/ReleaseRef.
			if (!!(FlagToClear & EInternalObjectFlags_RootFlags))
			{
				Result = ClearRootFlags(FlagToClear);
			}
			else
			{
				Result = AtomicallyClearFlag_ForGC(FlagToClear);
			}
		};

		return Result;
	}

	/**
	 * Uses atomics to set the specified flag(s). GC internal version.
	 * @param FlagToSet
	 * @return True if this call set the flag, false if it has been set by another thread.
	 */
	UE_DEPRECATED(5.5, "ThisThreadAtomicallySetFlag_ForGC() can only be used by the garbage collector. Use ThisThreadAtomicallySetFlag instead.")
	FORCEINLINE bool ThisThreadAtomicallySetFlag_ForGC(EInternalObjectFlags FlagToSet)
	{
		return AtomicallySetFlag_ForGC(FlagToSet);
	}

	/**
	 * Uses atomics to set the specified flag(s)
	 * @param FlagToSet
	 * @return True if this call set the flag, false if it has been set by another thread.
	 */
	FORCEINLINE bool ThisThreadAtomicallySetFlag(EInternalObjectFlags FlagToSet)
	{
		checkf((int32(FlagToSet) & ~int32(EInternalObjectFlags_AllFlags)) == 0, TEXT("%d is not a valid internal flag value"), int32(FlagToSet));
		bool Result = false;
		UE_AUTORTFM_OPEN
		{
			FlagToSet &= ~EInternalObjectFlags_ReachabilityFlags; // reachability flags can only be cleared by GC through *_ForGC functions
			FlagToSet &= ~EInternalObjectFlags::RefCounted; // refcounted flag is internal and must only be set by AddRef/ReleaseRef.
			if (!!(FlagToSet & EInternalObjectFlags_RootFlags))
			{
				Result = SetRootFlags(FlagToSet);
			}
			else
			{
				Result = AtomicallySetFlag_ForGC(FlagToSet);
			}
		};
		return Result;
	}

	FORCEINLINE bool HasAnyFlags(EInternalObjectFlags InFlags) const
	{
		return !!(GetFlagsInternal() & int32(InFlags));
	}

	FORCEINLINE bool HasAllFlags(EInternalObjectFlags InFlags) const
	{
		return (GetFlagsInternal() & int32(InFlags)) == int32(InFlags);
	}

	UE_DEPRECATED(5.5, "SetUnreachable() can only be used by the garbage collector.")
	FORCEINLINE void SetUnreachable()
	{
		AtomicallySetFlag_ForGC(EInternalObjectFlags::Unreachable);
	}
	UE_DEPRECATED(5.5, "SetMaybeUnreachable() can only be used by the garbage collector.")
	COREUOBJECT_API void SetMaybeUnreachable();

	UE_DEPRECATED(5.5, "ClearUnreachable() can only be used by the garbage collector.")
	FORCEINLINE void ClearUnreachable()
	{
		AtomicallyClearFlag_ForGC(EInternalObjectFlags::Unreachable);
	}
	FORCEINLINE bool IsUnreachable() const
	{
		return !!(GetFlagsInternal() & int32(EInternalObjectFlags::Unreachable));
	}

	UE_DEPRECATED(5.5, "IsMaybeUnreachable() can only be used by the garbage collector.")
	COREUOBJECT_API bool IsMaybeUnreachable() const;

	UE_DEPRECATED(5.5, "ThisThreadAtomicallyClearedRFUnreachable() can only be used by the garbage collector.")
	FORCEINLINE bool ThisThreadAtomicallyClearedRFUnreachable()
	{
		return AtomicallyClearFlag_ForGC(EInternalObjectFlags::Unreachable);
	}
	FORCEINLINE void SetGarbage()
	{
		AtomicallySetFlag_ForGC(EInternalObjectFlags::Garbage);
	}
	FORCEINLINE void ClearGarbage()
	{
		AtomicallyClearFlag_ForGC(EInternalObjectFlags::Garbage);
	}
	FORCEINLINE bool IsGarbage() const
	{
		return !!(GetFlagsInternal() & int32(EInternalObjectFlags::Garbage));
	}

	UE_DEPRECATED(5.4, "SetPendingKill() should no longer be used. Use SetGarbage() instead.")
	FORCEINLINE void SetPendingKill()
	{
		SetGarbage();
	}
	UE_DEPRECATED(5.4, "ClearPendingKill() should no longer be used. Use ClearGarbage() instead.")
	FORCEINLINE void ClearPendingKill()
	{
		ClearGarbage();
	}
	UE_DEPRECATED(5.4, "IsPendingKill() should no longer be used. Use IsGarbage() instead.")
	FORCEINLINE bool IsPendingKill() const
	{
		return IsGarbage();
	}

	FORCEINLINE void SetRootSet()
	{
		ThisThreadAtomicallySetFlag(EInternalObjectFlags::RootSet);
	}
	FORCEINLINE void ClearRootSet()
	{
		ThisThreadAtomicallyClearedFlag(EInternalObjectFlags::RootSet);
	}
	FORCEINLINE bool IsRootSet() const
	{
		return !!(GetFlagsInternal() & int32(EInternalObjectFlags::RootSet));
	}

	FORCEINLINE int32 GetRefCount() const
	{
		return RefCount;
	}

	void AddRef()
	{
		UE_AUTORTFM_OPEN
		{
			FPlatformAtomics::InterlockedIncrement(&RefCount);
			if ((GetFlags() & EInternalObjectFlags::RefCounted) != EInternalObjectFlags::RefCounted)
			{
				SetRootFlags(EInternalObjectFlags::RefCounted);
			}
		};

		// If the transaction is aborted we need to remember to release the reference we added in the open!
		UE_AUTORTFM_ONABORT(this)
		{
			this->ReleaseRef();
		};
	}

	void ReleaseRef()
	{
		UE_AUTORTFM_OPEN
		{
			// This alone is not thread-safe as we may race with AddRef and in that case we don't want ClearRootFlags to apply.
			// We fix this by validating that the refcount is still 0 while inside the root locks in ClearRootFlags.
			const int32 NewRefCount = FPlatformAtomics::InterlockedDecrement(&RefCount);
			check(NewRefCount >= 0);
			if (NewRefCount == 0)
			{
				ClearRootFlags(EInternalObjectFlags::RefCounted);
			}
		};

		// If the transaction is aborted we need to remember to re-add the reference we released in the open!
		// Note: this is different to how we handle ref counts in general in UE. Elsewhere we will eagerly
		// increment ref counts, but delay decrement. This is because `0` is normally the 'destroy the object'
		// trigger. But for UObject's `0` just clears the `RefCounted` flag, and since GC cannot run between
		// when we clear the flag and potentially re-add the flag below, the GC couldn't observe the state
		// that the object wasn't being actively ref-counted and thus could be collected. We do it differently
		// here because a bunch of the UObject systems depend on the ref count being correct after calls to
		// add/release.
		UE_AUTORTFM_ONABORT(this)
		{
			this->AddRef();
		};
	}

#if STATS || ENABLE_STATNAMEDEVENTS_UOBJECT
	COREUOBJECT_API void CreateStatID() const;
#endif

	// Mark this object item as Reachable and clear MaybeUnreachable flag. For GC use only.
	UE_DEPRECATED(5.5, "FastMarkAsReachableInterlocked_ForGC() can only be used by the garbage collector.")
	COREUOBJECT_API void FastMarkAsReachableInterlocked_ForGC();

	// Mark this object item as Reachable and clear ReachableInCluster and MaybeUnreachable flags. For GC use only.
	UE_DEPRECATED(5.5, "FastMarkAsReachableAndClearReachaleInClusterInterlocked_ForGC() can only be used by the garbage collector.")
	COREUOBJECT_API void FastMarkAsReachableAndClearReachaleInClusterInterlocked_ForGC();

	/**
	 * Mark this object item as Reachable and clear MaybeUnreachable flag. Only thread-safe for concurrent clear, not concurrent set+clear. Don't use during mark phase. For GC use only.
	 * @return True if this call cleared MaybeUnreachable flag, false if it has been cleared by another thread.
	 */
	UE_DEPRECATED(5.5, "MarkAsReachableInterlocked_ForGC() can only be used by the garbage collector.")
	COREUOBJECT_API bool MarkAsReachableInterlocked_ForGC();

	UE_DEPRECATED(5.5, "OffsetOfFlags() can only be used by the garbage collector.")
	FORCEINLINE static constexpr ::size_t OffsetOfFlags()
	{
		return offsetof(FUObjectItem, Flags);
	}

private:
	FORCEINLINE int32 GetFlagsInternal() const
	{
		return FPlatformAtomics::AtomicRead_Relaxed((int32*)&Flags);
	}
	
	FORCEINLINE int32 GetRefCountInternal() const
	{
		return FPlatformAtomics::AtomicRead_Relaxed((int32*)&RefCount);
	}

	COREUOBJECT_API bool SetRootFlags(EInternalObjectFlags FlagsToSet);
	COREUOBJECT_API bool ClearRootFlags(EInternalObjectFlags FlagsToClear);

	/**
	 * Uses atomics to set the specified flag(s). GC internal version.
	 * @param FlagToSet
	 * @return True if this call set the flag, false if it has been set by another thread.
	 */
	FORCEINLINE bool AtomicallySetFlag_ForGC(EInternalObjectFlags FlagToSet)
	{
		static_assert(sizeof(int32) == sizeof(Flags), "Flags must be 32-bit for atomics.");
		bool bIChangedIt = false;
		while (1)
		{
			int32 StartValue = GetFlagsInternal();
			if ((StartValue & int32(FlagToSet)) == int32(FlagToSet))
			{
				break;
			}
			int32 NewValue = StartValue | int32(FlagToSet);
			if ((int32)FPlatformAtomics::InterlockedCompareExchange((int32*)&Flags, NewValue, StartValue) == StartValue)
			{
				bIChangedIt = true;
				break;
			}
		}
		return bIChangedIt;
	}

	/**
	 * Uses atomics to clear the specified flag(s). GC internal version
	 * @param FlagsToClear
	 * @return True if this call cleared the flag, false if it has been cleared by another thread.
	 */
	FORCEINLINE bool AtomicallyClearFlag_ForGC(EInternalObjectFlags FlagToClear)
	{
		static_assert(sizeof(int32) == sizeof(Flags), "Flags must be 32-bit for atomics.");
		bool bIChangedIt = false;
		while (1)
		{
			int32 StartValue = GetFlagsInternal();
			if (!(StartValue & int32(FlagToClear)))
			{
				break;
			}
			int32 NewValue = StartValue & ~int32(FlagToClear);
			if ((int32)FPlatformAtomics::InterlockedCompareExchange((int32*)&Flags, NewValue, StartValue) == StartValue)
			{
				bIChangedIt = true;
				break;
			}
		}
		return bIChangedIt;
	}
};

namespace UE::UObjectArrayPrivate
{
	COREUOBJECT_API void FailMaxUObjectCountExceeded(const int32 MaxUObjects, const int32 NewUObjectCount);

	FORCEINLINE void CheckUObjectLimitReached(const int32 NumUObjects, const int32 MaxUObjects, const int32 NewUObjectCount)
	{
		if ((NumUObjects + NewUObjectCount) > MaxUObjects)
		{
			FailMaxUObjectCountExceeded(MaxUObjects, NewUObjectCount);
		}
	}
};


/**
* Fixed size UObject array.
*/
class FFixedUObjectArray
{
	/** Static primary table to chunks of pointers **/
	TSAN_ATOMIC(FUObjectItem*) Objects;
	/** Number of elements we currently have **/
	TSAN_ATOMIC(int32) MaxElements;
	/** Current number of UObject slots */
	TSAN_ATOMIC(int32) NumElements;

public:

	FFixedUObjectArray()
		: Objects(nullptr)
		, MaxElements(0)
		, NumElements(0)
	{
	}

	~FFixedUObjectArray()
	{
		delete [] Objects;
	}

	/**
	* Expands the array so that Element[Index] is allocated. New pointers are all zero.
	* @param Index The Index of an element we want to be sure is allocated
	**/
	void PreAllocate(int32 InMaxElements)
	{
		check(!Objects);
		Objects = new FUObjectItem[InMaxElements];
		MaxElements = InMaxElements;
	}

	int32 AddSingle()
	{
		int32 Result = NumElements;
		UE::UObjectArrayPrivate::CheckUObjectLimitReached(NumElements, MaxElements, 1);
		check(Result == NumElements);
		++NumElements;
		FPlatformMisc::MemoryBarrier();
		check(Objects[Result].GetObject() == nullptr);
		return Result;
	}

	int32 AddRange(int32 Count)
	{
		int32 Result = NumElements + Count - 1;
		UE::UObjectArrayPrivate::CheckUObjectLimitReached(NumElements, MaxElements, Count);
		check(Result == (NumElements + Count - 1));
		NumElements += Count;
		FPlatformMisc::MemoryBarrier();
		check(Objects[Result].GetObject() == nullptr);
		return Result;
	}

	FORCEINLINE FUObjectItem const* GetObjectPtr(int32 Index) const
	{
		check(Index >= 0 && Index < NumElements);
		return &Objects[Index];
	}

	FORCEINLINE FUObjectItem* GetObjectPtr(int32 Index)
	{
		check(Index >= 0 && Index < NumElements);
		return &Objects[Index];
	}

	/**
	* Return the number of elements in the array
	* Thread safe, but you know, someone might have added more elements before this even returns
	* @return	the number of elements in the array
	**/
	FORCEINLINE int32 Num() const
	{
		return NumElements;
	}

	/**
	* Return the number max capacity of the array
	* Thread safe, but you know, someone might have added more elements before this even returns
	* @return	the maximum number of elements in the array
	**/
	FORCEINLINE int32 Capacity() const
	{
		return MaxElements;
	}

	/**
	* Return if this index is valid
	* Thread safe, if it is valid now, it is valid forever. Other threads might be adding during this call.
	* @param	Index	Index to test
	* @return	true, if this is a valid
	**/
	FORCEINLINE bool IsValidIndex(int32 Index) const
	{
		return Index < Num() && Index >= 0;
	}
	/**
	* Return a reference to an element
	* @param	Index	Index to return
	* @return	a reference to the pointer to the element
	* Thread safe, if it is valid now, it is valid forever. This might return nullptr, but by then, some other thread might have made it non-nullptr.
	**/
	FORCEINLINE FUObjectItem const& operator[](int32 Index) const
	{
		FUObjectItem const* ItemPtr = GetObjectPtr(Index);
		check(ItemPtr);
		return *ItemPtr;
	}

	FORCEINLINE FUObjectItem& operator[](int32 Index)
	{
		FUObjectItem* ItemPtr = GetObjectPtr(Index);
		check(ItemPtr);
		return *ItemPtr;
	}

	/**
	* Return a naked pointer to the fundamental data structure for debug visualizers.
	**/
	UObjectBase*** GetRootBlockForDebuggerVisualizers()
	{
		return nullptr;
	}
};

/**
* Simple array type that can be expanded without invalidating existing entries.
* This is critical to thread safe FNames.
* @param ElementType Type of the pointer we are storing in the array
* @param MaxTotalElements absolute maximum number of elements this array can ever hold
* @param ElementsPerChunk how many elements to allocate in a chunk
**/
class FChunkedFixedUObjectArray
{
	enum
	{
		NumElementsPerChunk = 64 * 1024,
	};

	/** Primary table to chunks of pointers **/
	FUObjectItem** Objects;
	/** If requested, a contiguous memory where all objects are allocated **/
	FUObjectItem* PreAllocatedObjects;
	/** Maximum number of elements **/
	TSAN_ATOMIC(int32) MaxElements;
	/** Number of elements we currently have **/
	TSAN_ATOMIC(int32) NumElements;
	/** Maximum number of chunks **/
	int32 MaxChunks;
	/** Number of chunks we currently have **/
	TSAN_ATOMIC(int32) NumChunks;

	static constexpr bool bFUObjectItemIsPacked = UE_ENABLE_FUOBJECT_ITEM_PACKING;


	/**
	* Allocates new chunk for the array
	**/
	void ExpandChunksToIndex(int32 Index)
	{
		check(Index >= 0 && Index < MaxElements);
		int32 ChunkIndex = Index / NumElementsPerChunk;
		while (ChunkIndex >= NumChunks)
		{
			// add a chunk, and make sure nobody else tries
			FUObjectItem** Chunk = &Objects[NumChunks];
			FUObjectItem* NewChunk = new FUObjectItem[NumElementsPerChunk];
			if (FPlatformAtomics::InterlockedCompareExchangePointer((void**)Chunk, NewChunk, nullptr))
			{
				// someone else beat us to the add, we don't support multiple concurrent adds
				check(0);
			}
			else
			{
				NumChunks++;
				check(NumChunks <= MaxChunks);
			}
		}
		check(ChunkIndex < NumChunks && Objects[ChunkIndex]); // should have a valid pointer now
	}
    
public:

	/** Constructor : Probably not thread safe **/
	FChunkedFixedUObjectArray()
		: Objects(nullptr)
		, PreAllocatedObjects(nullptr)
		, MaxElements(0)
		, NumElements(0)
		, MaxChunks(0)
		, NumChunks(0)
	{
	}

	~FChunkedFixedUObjectArray()
	{
		if (!PreAllocatedObjects)
		{
			for (int32 ChunkIndex = 0; ChunkIndex < MaxChunks; ++ChunkIndex)
			{
				delete[] Objects[ChunkIndex];
			}
		}
		else
		{
			delete[] PreAllocatedObjects;
		}
		delete[] Objects;
	}

	/**
	* Expands the array so that Element[Index] is allocated. New pointers are all zero.
	* @param Index The Index of an element we want to be sure is allocated
	**/
	void PreAllocate(int32 InMaxElements, bool bPreAllocateChunks)
	{
		check(!Objects);
		MaxChunks = InMaxElements / NumElementsPerChunk + 1;
		MaxElements = MaxChunks * NumElementsPerChunk;
		Objects = new FUObjectItem*[MaxChunks];
		FMemory::Memzero(Objects, sizeof(FUObjectItem*) * MaxChunks);
		if (bPreAllocateChunks)
		{
			// Fully allocate all chunks as contiguous memory
			PreAllocatedObjects = new FUObjectItem[MaxElements];
			for (int32 ChunkIndex = 0; ChunkIndex < MaxChunks; ++ChunkIndex)
			{
				Objects[ChunkIndex] = PreAllocatedObjects + ChunkIndex * NumElementsPerChunk;
			}
			NumChunks = MaxChunks;
		}
	}

	/**
	* Return the number of elements in the array
	* Thread safe, but you know, someone might have added more elements before this even returns
	* @return	the number of elements in the array
	**/
	FORCEINLINE int32 Num() const
	{
		return NumElements;
	}

	/**
	* Return the number max capacity of the array
	* Thread safe, but you know, someone might have added more elements before this even returns
	* @return	the maximum number of elements in the array
	**/
	FORCEINLINE int32 Capacity() const
	{
		return MaxElements;
	}

	/**
	* Return if this index is valid
	* Thread safe, if it is valid now, it is valid forever. Other threads might be adding during this call.
	* @param	Index	Index to test
	* @return	true, if this is a valid
	**/
	FORCEINLINE bool IsValidIndex(int32 Index) const
	{
		return Index < Num() && Index >= 0;
	}

	/**
	* Return a pointer to the pointer to a given element
	* @param Index The Index of an element we want to retrieve the pointer-to-pointer for
	**/
	FORCEINLINE_DEBUGGABLE FUObjectItem const* GetObjectPtr(int32 Index) const
	{
		const uint32 ChunkIndex = (uint32)Index / NumElementsPerChunk;
		const uint32 WithinChunkIndex = (uint32)Index % NumElementsPerChunk;
		checkf(IsValidIndex(Index), TEXT("IsValidIndex(%d)"), Index);
		checkf(ChunkIndex < (uint32)NumChunks, TEXT("ChunkIndex (%d) < NumChunks (%d)"), ChunkIndex, (int32)NumChunks);
		checkf(Index < MaxElements, TEXT("Index (%d) < MaxElements (%d)"), Index, (int32)MaxElements);
		FUObjectItem* Chunk = Objects[ChunkIndex];
		check(Chunk);
		return Chunk + WithinChunkIndex;
	}
	FORCEINLINE_DEBUGGABLE FUObjectItem* GetObjectPtr(int32 Index)
	{
		const uint32 ChunkIndex = (uint32)Index / NumElementsPerChunk;
		const uint32 WithinChunkIndex = (uint32)Index % NumElementsPerChunk;
		checkf(IsValidIndex(Index), TEXT("IsValidIndex(%d)"), Index);
		checkf(ChunkIndex < (uint32)NumChunks, TEXT("ChunkIndex (%d) < NumChunks (%d)"), ChunkIndex, (int32)NumChunks);
		checkf(Index < MaxElements, TEXT("Index (%d) < MaxElements (%d)"), Index, (int32)MaxElements);
		FUObjectItem* Chunk = Objects[ChunkIndex];
		check(Chunk);
		return Chunk + WithinChunkIndex;
	}

	FORCEINLINE_DEBUGGABLE void PrefetchObjectPtr(int32 Index) const
	{
		const uint32 ChunkIndex = (uint32)Index / NumElementsPerChunk;
		const uint32 WithinChunkIndex = (uint32)Index % NumElementsPerChunk;
		const FUObjectItem* Chunk = Objects[ChunkIndex];
		FPlatformMisc::Prefetch(Chunk + WithinChunkIndex);
	}

	/**
	* Return a reference to an element
	* @param	Index	Index to return
	* @return	a reference to the pointer to the element
	* Thread safe, if it is valid now, it is valid forever. This might return nullptr, but by then, some other thread might have made it non-nullptr.
	**/
	FORCEINLINE FUObjectItem const& operator[](int32 Index) const
	{
		FUObjectItem const* ItemPtr = GetObjectPtr(Index);
		check(ItemPtr);
		return *ItemPtr;
	}
	FORCEINLINE FUObjectItem& operator[](int32 Index)
	{
		FUObjectItem* ItemPtr = GetObjectPtr(Index);
		check(ItemPtr);
		return *ItemPtr;
	}

	int32 AddRange(int32 NumToAdd)
	{
		int32 Result = NumElements;
		UE::UObjectArrayPrivate::CheckUObjectLimitReached(Result, MaxElements, NumToAdd);
		ExpandChunksToIndex(Result + NumToAdd - 1);
		NumElements += NumToAdd;
		return Result;
	}

	int32 AddSingle()
	{
		return AddRange(1);
	}

	/**
	* Return a naked pointer to the fundamental data structure for debug visualizers.
	**/
	FUObjectItem*** GetRootBlockForDebuggerVisualizers()
	{
		return nullptr;
	}
    
    int64 GetAllocatedSize() const
    {
        return MaxChunks * sizeof(FUObjectItem*) + NumChunks * NumElementsPerChunk * sizeof(FUObjectItem);
    }
};

/***
*
* FUObjectArray replaces the functionality of GObjObjects and UObject::Index
*
* Note the layout of this data structure is mostly to emulate the old behavior and minimize code rework during code restructure.
* Better data structures could be used in the future, for example maybe all that is needed is a TSet<UObject *>
* One has to be a little careful with this, especially with the GC optimization. I have seen spots that assume
* that non-GC objects come before GC ones during iteration.
*
**/
class FUObjectArray
{
	friend class UObject;
	friend COREUOBJECT_API UObject* StaticAllocateObject(const UClass*, UObject*, FName, EObjectFlags, EInternalObjectFlags, bool, bool*, UPackage*, int32, FRemoteObjectId, class FGCReconstructionGuard*);

private:
	/**
	 * Reset the serial number from the game thread to invalidate all weak object pointers to it
	 *
	 * @param Object to reset
	 */
	COREUOBJECT_API void ResetSerialNumber(UObjectBase* Object);

public:

	enum ESerialNumberConstants
	{
		START_SERIAL_NUMBER = 1000,
	};

	/**
	 * Base class for UObjectBase create class listeners
	 */
	class FUObjectCreateListener
	{
	public:
		virtual ~FUObjectCreateListener() {}
		/**
		* Provides notification that a UObjectBase has been added to the uobject array
		 *
		 * @param Object object that has been added
		 * @param Index	index of object that is being added
		 */
		virtual void NotifyUObjectCreated(const class UObjectBase *Object, int32 Index)=0;

		/**
		 * Called when UObject Array is being shut down, this is where all listeners should be removed from it 
		 */
		virtual void OnUObjectArrayShutdown()=0;
	};

	/**
	 * Base class for UObjectBase delete class listeners
	 */
	class FUObjectDeleteListener
	{
	public:
		virtual ~FUObjectDeleteListener() {}

		/**
		 * Provides notification that a UObjectBase has been removed from the uobject array
		 *
		 * @param Object object that has been destroyed
		 * @param Index	index of object that is being deleted
		 */
		virtual void NotifyUObjectDeleted(const class UObjectBase *Object, int32 Index)=0;

		/**
		 * Called when UObject Array is being shut down, this is where all listeners should be removed from it
		 */
		virtual void OnUObjectArrayShutdown() = 0;

		/**
		 * Returns the size of heap memory allocated internally by this listener
		 */
		virtual SIZE_T GetAllocatedSize() const
		{
			return 0;
		}
	};

	/**
	 * Constructor, initializes to no permanent object pool
	 */
	COREUOBJECT_API FUObjectArray();

	/**
	 * Allocates and initializes the permanent object pool
	 *
	 * @param MaxUObjects maximum number of UObjects that can ever exist in the array
	 * @param MaxObjectsNotConsideredByGC number of objects in the permanent object pool
	 */
	COREUOBJECT_API void AllocateObjectPool(int32 MaxUObjects, int32 MaxObjectsNotConsideredByGC, bool bPreAllocateObjectArray);

	/**
	 * Disables the disregard for GC optimization.
	 *
	 */
	COREUOBJECT_API void DisableDisregardForGC();

	/**
	* If there's enough slack in the disregard pool, we can re-open it and keep adding objects to it
	*/
	COREUOBJECT_API void OpenDisregardForGC();

	/**
	 * After the initial load, this closes the disregard pool so that new object are GC-able
	 */
	COREUOBJECT_API void CloseDisregardForGC();

	/** Returns true if the disregard for GC pool is open */
	bool IsOpenForDisregardForGC() const
	{
		return OpenForDisregardForGC;
	}

	/**
	 * indicates if the disregard for GC optimization is active
	 *
	 * @return true if MaxObjectsNotConsideredByGC is greater than zero; this indicates that the disregard for GC optimization is enabled
	 */
	bool DisregardForGCEnabled() const 
	{ 
		return MaxObjectsNotConsideredByGC > 0;
	}

	/**
	 * Adds a uobject to the global array which is used for uobject iteration
	 *
	 * @param	Object Object to allocate an index for
	 * @param	InitialFlags Flags to set in the object array before the object pointer becomes visible to other threads. 
	 * @param	AlreadyAllocatedIndex already allocated internal index to use, negative value means allocate a new index
	 * @param	SerialNumber serial number to use
	 */
	COREUOBJECT_API void AllocateUObjectIndex(class UObjectBase* Object, EInternalObjectFlags InitialFlags, int32 AlreadyAllocatedIndex = -1, int32 SerialNumber = 0, FRemoteObjectId RemoteId = FRemoteObjectId());

	/**
	 * Returns a UObject index top to the global uobject array
	 *
	 * @param Object object to free
	 */
	COREUOBJECT_API void FreeUObjectIndex(class UObjectBase* Object);

	/**
	 * Returns the index of a UObject. Be advised this is only for very low level use.
	 *
	 * @param Object object to get the index of
	 * @return index of this object
	 */
	FORCEINLINE int32 ObjectToIndex(const class UObjectBase* Object) const
	{
		return Object->InternalIndex;
	}

	/**
	 * Returns the UObject corresponding to index. Be advised this is only for very low level use.
	 *
	 * @param Index index of object to return
	 * @return Object at this index
	 */
	FORCEINLINE FUObjectItem* IndexToObject(int32 Index)
	{
		check(Index >= 0);
		if (Index < ObjObjects.Num())
		{
			return const_cast<FUObjectItem*>(&ObjObjects[Index]);
		}
		return nullptr;
	}

	FORCEINLINE FUObjectItem* IndexToObjectUnsafeForGC(int32 Index)
	{
		return const_cast<FUObjectItem*>(&ObjObjects[Index]);
	}

	FORCEINLINE FUObjectItem* IndexToObject(int32 Index, bool bEvenIfGarbage)
	{
		FUObjectItem* ObjectItem = IndexToObject(Index);
		if (ObjectItem && ObjectItem->GetObject())
		{
			if (!bEvenIfGarbage && ObjectItem->HasAnyFlags(EInternalObjectFlags::Garbage))
			{
				ObjectItem = nullptr;
			}
		}
		return ObjectItem;
	}

	FORCEINLINE FUObjectItem* ObjectToObjectItem(const UObjectBase* Object)
	{
		FUObjectItem* ObjectItem = IndexToObject(Object->InternalIndex);
		return ObjectItem;
	}

	FORCEINLINE bool IsValid(FUObjectItem* ObjectItem, bool bEvenIfGarbage)
	{
		if (ObjectItem)
		{
			return bEvenIfGarbage ? !ObjectItem->IsUnreachable() : !(ObjectItem->HasAnyFlags(EInternalObjectFlags::Unreachable | EInternalObjectFlags::Garbage));
		}
		return false;
	}

	FORCEINLINE FUObjectItem* IndexToValidObject(int32 Index, bool bEvenIfGarbage)
	{
		FUObjectItem* ObjectItem = IndexToObject(Index);
		return IsValid(ObjectItem, bEvenIfGarbage) ? ObjectItem : nullptr;
	}

	FORCEINLINE bool IsValid(int32 Index, bool bEvenIfGarbage)
	{
		// This method assumes Index points to a valid object.
		FUObjectItem* ObjectItem = IndexToObject(Index);
		return IsValid(ObjectItem, bEvenIfGarbage);
	}

	FORCEINLINE bool IsStale(FUObjectItem* ObjectItem, bool bIncludingGarbage)
	{
		// This method assumes ObjectItem is valid.
		return bIncludingGarbage ? (ObjectItem->HasAnyFlags(EInternalObjectFlags::Unreachable | EInternalObjectFlags::Garbage)) : (ObjectItem->IsUnreachable());
	}

	FORCEINLINE bool IsStale(int32 Index, bool bIncludingGarbage)
	{
		// This method assumes Index points to a valid object.
		FUObjectItem* ObjectItem = IndexToObject(Index);
		if (ObjectItem)
		{
			return IsStale(ObjectItem, bIncludingGarbage);
		}
		return true;
	}

	/** Returns the index of the first object outside of the disregard for GC pool */
	FORCEINLINE int32 GetFirstGCIndex() const
	{
		return ObjFirstGCIndex;
	}

	/**
	 * Adds a new listener for object creation
	 *
	 * @param Listener listener to notify when an object is deleted
	 */
	COREUOBJECT_API void AddUObjectCreateListener(FUObjectCreateListener* Listener);

	/**
	 * Removes a listener for object creation
	 *
	 * @param Listener listener to remove
	 */
	COREUOBJECT_API void RemoveUObjectCreateListener(FUObjectCreateListener* Listener);

	/**
	 * Adds a new listener for object deletion
	 *
	 * @param Listener listener to notify when an object is deleted
	 */
	COREUOBJECT_API void AddUObjectDeleteListener(FUObjectDeleteListener* Listener);

	/**
	 * Removes a listener for object deletion
	 *
	 * @param Listener listener to remove
	 */
	COREUOBJECT_API void RemoveUObjectDeleteListener(FUObjectDeleteListener* Listener);

	/**
	 * Removes an object from delete listeners
	 *
	 * @param Object to remove from delete listeners
	 */
	COREUOBJECT_API void RemoveObjectFromDeleteListeners(UObjectBase* Object);

	/**
	 * Checks if a UObject pointer is valid
	 *
	 * @param	Object object to test for validity
	 * @return	true if this index is valid
	 */
	COREUOBJECT_API bool IsValid(const UObjectBase* Object) const;

	/** Checks if the object index is valid. */
	FORCEINLINE bool IsValidIndex(const UObjectBase* Object) const 
	{ 
		return ObjObjects.IsValidIndex(Object->InternalIndex);
	}

	/**
	 * Returns true if this object is "disregard for GC"...same results as the legacy RF_DisregardForGC flag
	 *
	 * @param Object object to get for disregard for GC
	 * @return true if this object si disregard for GC
	 */
	FORCEINLINE bool IsDisregardForGC(const class UObjectBase* Object)
	{
		return Object->InternalIndex <= ObjLastNonGCIndex;
	}
	/**
	 * Returns the size of the global UObject array, some of these might be unused
	 *
	 * @return	the number of UObjects in the global array
	 */
	FORCEINLINE int32 GetObjectArrayNum() const 
	{ 
		return ObjObjects.Num();
	}

	/**
	 * Returns the size of the global UObject array minus the number of permanent objects
	 *
	 * @return	the number of UObjects in the global array
	 */
	FORCEINLINE int32 GetObjectArrayNumMinusPermanent() const 
	{ 
		return ObjObjects.Num() - (ObjLastNonGCIndex + 1);
	}

	/**
	 * Returns the number of permanent objects
	 *
	 * @return	the number of permanent objects
	 */
	FORCEINLINE int32 GetObjectArrayNumPermanent() const 
	{ 
		return ObjLastNonGCIndex + 1;
	}

	/**
	 * Returns the number of actual object indices that are claimed (the total size of the global object array minus
	 * the number of available object array elements
	 *
	 * @return	The number of objects claimed
	 */
	int32 GetObjectArrayNumMinusAvailable() const
	{
		return ObjObjects.Num() - ObjAvailableList.Num();
	}

	/**
	* Returns the estimated number of object indices available for allocation
	*/
	int32 GetObjectArrayEstimatedAvailable() const
	{
		return ObjObjects.Capacity() - GetObjectArrayNumMinusAvailable();
	}

	/**
	* Returns the estimated number of object indices available for allocation
	*/
	int32 GetObjectArrayCapacity() const
	{
		return ObjObjects.Capacity();
	}

	/**
	 * Clears some internal arrays to get rid of false memory leaks
	 */
	COREUOBJECT_API void ShutdownUObjectArray();

	/**
	* Given a UObject index return the serial number. If it doesn't have a serial number, give it one. Threadsafe.
	* @param Index - UObject Index
	* @return - the serial number for this UObject
	*/
	COREUOBJECT_API int32 AllocateSerialNumber(int32 Index);

	/**
	* Given a UObject index return the serial number. If it doesn't have a serial number, return 0. Threadsafe.
	* @param Index - UObject Index
	* @return - the serial number for this UObject
	*/
	FORCEINLINE int32 GetSerialNumber(int32 Index)
	{
		FUObjectItem* ObjectItem = IndexToObject(Index);
		checkSlow(ObjectItem);
		return ObjectItem->GetSerialNumber();
	}

#if UE_WITH_REMOTE_OBJECT_HANDLE

	/**
	* Given a UObject index return the remote id.
	* @param Index - UObject Index
	* @return - the remote id for this UObject
	*/
	FORCEINLINE FRemoteObjectId GetRemoteId(int32 Index)
	{
		FUObjectItem* ObjectItem = IndexToObject(Index);
		checkSlow(ObjectItem);
		return ObjectItem->GetRemoteId();
	}
#endif

	/** Locks the internal object array mutex */
	void LockInternalArray() const
	{
#if THREADSAFE_UOBJECTS
		ObjObjectsCritical.Lock();
#else
		check(IsInGameThread());
#endif
	}

	/** Unlocks the internal object array mutex */
	void UnlockInternalArray() const
	{
#if THREADSAFE_UOBJECTS
		ObjObjectsCritical.Unlock();
#endif
	}

	/**
	 * Low level iterator.
	 */
	class TIterator
	{
	public:
		enum EEndTagType
		{
			EndTag
		};

		/**
		 * Constructor
		 *
		 * @param	InArray				the array to iterate on
		 * @param	bOnlyGCedObjects	if true, skip all of the permanent objects
		 */
		TIterator( const FUObjectArray& InArray, bool bOnlyGCedObjects = false ) :	
			Array(InArray),
			Index(-1),
			CurrentObject(nullptr)
		{
			if (bOnlyGCedObjects)
			{
				Index = Array.ObjLastNonGCIndex;
			}
			Advance();
		}

		/**
		 * Constructor
		 *
		 * @param	InArray				the array to iterate on
		 * @param	bOnlyGCedObjects	if true, skip all of the permanent objects
		 */
		TIterator( EEndTagType, const TIterator& InIter ) :	
			Array (InIter.Array),
			Index(Array.ObjObjects.Num())
		{
		}

		/**
		 * Iterator advance
		 */
		FORCEINLINE void operator++()
		{
			Advance();
		}

		bool operator==(const TIterator& Rhs) const { return Index == Rhs.Index; }
		bool operator!=(const TIterator& Rhs) const { return Index != Rhs.Index; }

		/** Conversion to "bool" returning true if the iterator is valid. */
		FORCEINLINE explicit operator bool() const
		{ 
			return !!CurrentObject;
		}
		/** inverse of the "bool" operator */
		FORCEINLINE bool operator !() const 
		{
			return !(bool)*this;
		}

		FORCEINLINE int32 GetIndex() const
		{
			return Index;
		}

	protected:

		/**
		 * Dereferences the iterator with an ordinary name for clarity in derived classes
		 *
		 * @return	the UObject at the iterator
		 */
		FORCEINLINE FUObjectItem* GetObject() const
		{ 
			return CurrentObject;
		}
		/**
		 * Iterator advance with ordinary name for clarity in subclasses
		 * @return	true if the iterator points to a valid object, false if iteration is complete
		 */
		FORCEINLINE bool Advance()
		{
			//@todo UE check this for LHS on Index on consoles
			FUObjectItem* NextObject = nullptr;
			CurrentObject = nullptr;
			while(++Index < Array.GetObjectArrayNum())
			{
				NextObject = const_cast<FUObjectItem*>(&Array.ObjObjects[Index]);
				if (NextObject->GetObject())
				{
					CurrentObject = NextObject;
					return true;
				}
			}
			return false;
		}

		/** Gets the array this iterator iterates over */
		const FUObjectArray& GetIteratedArray() const
		{
			return Array;
		}

	private:
		/** the array that we are iterating on, probably always GUObjectArray */
		const FUObjectArray& Array;
		/** index of the current element in the object array */
		int32 Index;
		/** Current object */
		mutable FUObjectItem* CurrentObject;
	};

private:

	//typedef TStaticIndirectArrayThreadSafeRead<UObjectBase, 8 * 1024 * 1024 /* Max 8M UObjects */, 16384 /* allocated in 64K/128K chunks */ > TUObjectArray;
	typedef FChunkedFixedUObjectArray TUObjectArray;

	// note these variables are left with the Obj prefix so they can be related to the historical GObj versions

	/** First index into objects array taken into account for GC.							*/
	int32 ObjFirstGCIndex;
	/** Index pointing to last object created in range disregarded for GC.					*/
	int32 ObjLastNonGCIndex;
	/** Maximum number of objects in the disregard for GC Pool */
	int32 MaxObjectsNotConsideredByGC;

	/** If true this is the intial load and we should load objects int the disregarded for GC range.	*/
	bool OpenForDisregardForGC;
	/** Array of all live objects.											*/
	TUObjectArray ObjObjects;
	/** Synchronization object for all live objects.											*/
	mutable FTransactionallySafeCriticalSection ObjObjectsCritical;
	/** Available object indices.											*/
	TArray<int32> ObjAvailableList;

	/**
	 * Array of things to notify when a UObjectBase is created
	 */
	TArray<FUObjectCreateListener* > UObjectCreateListeners;
	/**
	 * Array of things to notify when a UObjectBase is destroyed
	 */
	TArray<FUObjectDeleteListener* > UObjectDeleteListeners;
#if THREADSAFE_UOBJECTS
	mutable FTransactionallySafeCriticalSection UObjectDeleteListenersCritical;
#endif

	/** Current primary serial number **/
	FThreadSafeCounter	PrimarySerialNumber;

	/** If set to false object indices won't be recycled to the global pool and can be explicitly reused when creating new objects */
	bool bShouldRecycleObjectIndices = true;

public:

	/** INTERNAL USE ONLY: gets the internal FUObjectItem array */
	TUObjectArray& GetObjectItemArrayUnsafe()
	{
		return ObjObjects;
	}
    
	const TUObjectArray& GetObjectItemArrayUnsafe() const
	{
		return ObjObjects;
	}

    SIZE_T GetAllocatedSize() const
    {
		UE::TScopeLock ObjListLock(ObjObjectsCritical);
#if THREADSAFE_UOBJECTS
		UE::TScopeLock ListenersLock(UObjectDeleteListenersCritical);
#endif
        return ObjObjects.GetAllocatedSize() + ObjAvailableList.GetAllocatedSize() + UObjectCreateListeners.GetAllocatedSize() + UObjectDeleteListeners.GetAllocatedSize();
    }

	SIZE_T GetDeleteListenersAllocatedSize(int32* OutNumListeners = nullptr) const
	{
#if THREADSAFE_UOBJECTS
		UE::TScopeLock ListenersLock(UObjectDeleteListenersCritical);
#endif
		SIZE_T AllocatedSize = 0;
		for (FUObjectDeleteListener* Listener : UObjectDeleteListeners)
		{
			AllocatedSize += Listener->GetAllocatedSize();
		}
		if (OutNumListeners)
		{
			*OutNumListeners = UObjectDeleteListeners.Num();
		}
		return AllocatedSize;
	}

	COREUOBJECT_API void DumpUObjectCountsToLog() const;
};

/** UObject cluster. Groups UObjects into a single unit for GC. */
struct FUObjectCluster
{
	FUObjectCluster()
		: RootIndex(INDEX_NONE)
		, bNeedsDissolving(false)
	{}

	/** Root object index */
	int32 RootIndex;
	/** Objects that belong to this cluster */
	TArray<int32> Objects;
	/** Other clusters referenced by this cluster */
	TArray<int32> ReferencedClusters;
	/** Objects that could not be added to the cluster but still need to be referenced by it */
	TArray<int32> MutableObjects;
	/** List of clusters that direcly reference this cluster. Used when dissolving a cluster. */
	TArray<int32> ReferencedByClusters;
#if WITH_VERSE_VM || defined(__INTELLISENSE__)
	/** All verse cells are considered mutable.  They will just be added directly to verse gc when the cluster is marked */
	TArray<Verse::VCell*> MutableCells;
#endif

	/** Cluster needs dissolving, probably due to PendingKill reference */
	bool bNeedsDissolving;
};

class FUObjectClusterContainer
{
	/** List of all clusters */
	TArray<FUObjectCluster> Clusters;
	/** List of available cluster indices */
	TArray<int32> FreeClusterIndices;
	/** Number of allocated clusters */
	int32 NumAllocatedClusters;
	/** Clusters need dissolving, probably due to PendingKill reference */
	bool bClustersNeedDissolving;

	/** Dissolves a cluster */
	COREUOBJECT_API void DissolveCluster(FUObjectCluster& Cluster);

public:

	COREUOBJECT_API FUObjectClusterContainer();

	FORCEINLINE FUObjectCluster& operator[](int32 Index)
	{
		checkf(Index >= 0 && Index < Clusters.Num(), TEXT("Cluster index %d out of range [0, %d]"), Index, Clusters.Num());
		return Clusters[Index];
	}

	/** Returns an index to a new cluster */
	COREUOBJECT_API int32 AllocateCluster(int32 InRootObjectIndex);

	/** Frees the cluster at the specified index */
	COREUOBJECT_API void FreeCluster(int32 InClusterIndex);

	/**
	* Gets the cluster the specified object is a root of or belongs to.
	* @Param ClusterRootOrObjectFromCluster Root cluster object or object that belongs to a cluster
	*/
	COREUOBJECT_API FUObjectCluster* GetObjectCluster(UObjectBaseUtility* ClusterRootOrObjectFromCluster);


	/** 
	 * Dissolves a cluster and all clusters that reference it 
	 * @Param ClusterRootOrObjectFromCluster Root cluster object or object that belongs to a cluster
	 */
	COREUOBJECT_API void DissolveCluster(UObjectBaseUtility* ClusterRootOrObjectFromCluster);

	/** 
	 * Dissolve all clusters marked for dissolving 
	 * @param bForceDissolveAllClusters if true, dissolves all clusters even if they're not marked for dissolving
	 */
	COREUOBJECT_API void DissolveClusters(bool bForceDissolveAllClusters = false);

	/** Dissolve the specified cluster and all clusters that reference it */
	COREUOBJECT_API void DissolveClusterAndMarkObjectsAsUnreachable(FUObjectItem* RootObjectItem);

	/*** Returns the minimum cluster size as specified in ini settings */
	COREUOBJECT_API int32 GetMinClusterSize() const;

	/** Gets the clusters array (for internal use only!) */
	TArray<FUObjectCluster>& GetClustersUnsafe() 
	{ 
		return Clusters;  
	}

	/** Returns the number of currently allocated clusters */
	int32 GetNumAllocatedClusters() const
	{
		return NumAllocatedClusters;
	}

	/** Lets the FUObjectClusterContainer know some clusters need dissolving */
	void SetClustersNeedDissolving()
	{
		bClustersNeedDissolving = true;
	}
	
	/** Checks if any clusters need dissolving */
	bool ClustersNeedDissolving() const
	{
		return bClustersNeedDissolving;
	}
};

/** Global UObject allocator							*/
extern COREUOBJECT_API FUObjectArray GUObjectArray;
extern COREUOBJECT_API FUObjectClusterContainer GUObjectClusters;

/**
	* Static version of IndexToObject for use with TWeakObjectPtr.
	*/
struct FIndexToObject
{
	static FORCEINLINE class UObjectBase* IndexToObject(int32 Index, bool bEvenIfGarbage)
	{
		FUObjectItem* ObjectItem = GUObjectArray.IndexToObject(Index, bEvenIfGarbage);
		return ObjectItem ? ObjectItem->GetObject() : nullptr;
	}
};

namespace verse
{
COREUOBJECT_API bool CanAllocateUObjects();
}
