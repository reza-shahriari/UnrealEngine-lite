// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Async/LockTags.h"
#include "CoreTypes.h"
#include <atomic>

#define UE_API CORE_API

namespace UE
{

/**
 * A one-byte mutex that is not fair and does not support recursive locking.
 * Note: Changes to this class should also be ported to FExternalMutex.
 */
class FMutex final
{
public:
	constexpr FMutex() = default;

	/** Construct in a locked state. Avoids an expensive compare-and-swap at creation time. */
	inline explicit FMutex(FAcquireLock)
		: State(IsLockedFlag)
	{
	}

	FMutex(const FMutex&) = delete;
	FMutex& operator=(const FMutex&) = delete;

	inline bool IsLocked() const
	{
		return (State.load(std::memory_order_relaxed) & IsLockedFlag);
	}
	
	inline bool TryLock()
	{
		uint8 Expected = State.load(std::memory_order_relaxed);
		return !(Expected & IsLockedFlag) &&
			State.compare_exchange_strong(Expected, Expected | IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed);
	}

	inline void Lock()
	{
		uint8 Expected = 0;
		if (LIKELY(State.compare_exchange_weak(Expected, IsLockedFlag, std::memory_order_acquire, std::memory_order_relaxed)))
		{
			return;
		}
		LockSlow();
	}

	inline void Unlock()
	{
		if constexpr (bUnlockImmediately)
		{
			// Unlock immediately to allow other threads to acquire the lock while this thread looks for a thread to wake.
			const uint8 LastState = State.fetch_sub(IsLockedFlag, std::memory_order_release);
			if (LIKELY(!(LastState & MayHaveWaitingLockFlag)))
			{
				return;
			}
			WakeWaitingThread();
		}
		else
		{
			uint8 Expected = IsLockedFlag;
			if (LIKELY(State.compare_exchange_weak(Expected, 0, std::memory_order_release, std::memory_order_relaxed)))
			{
				return;
			}
			WakeWaitingThread();
		}
	}

private:
	UE_API void LockSlow();
	UE_API void WakeWaitingThread();

	static constexpr bool bUnlockImmediately = true;

	static constexpr uint8 IsLockedFlag = 1 << 0;
	static constexpr uint8 MayHaveWaitingLockFlag = 1 << 1;

	std::atomic<uint8> State = 0;
};

} // UE

#undef UE_API
