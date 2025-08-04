// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/VVMCell.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMDebugger.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMGlobalTrivialEmergentTypePtr.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMReturnSlot.h"
#include "VerseVM/VVMTaskNativeHook.h"
#include "VerseVM/VVMTree.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMWriteBarrier.h"

namespace Verse
{
struct FOp;
struct VFailureContext;

struct VTask : VValueObject
	, TIntrusiveTree<VTask>
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VValueObject);
	COREUOBJECT_API static TGlobalHeapPtr<VEmergentType> EmergentType;

	// A task is "running" when it is associated with a frame on the native stack.
	// This includes a running interpreter (even if it is just on the `YieldTask` chain), and native
	// functions like `CancelChildren`.
	// Running tasks can only be resumed by falling through a sequence of yields and native returns.
	// This is independent of `Phase`, as both active and cancelling tasks may suspend.
	bool bRunning{true};

	// See the note on CancelImpl.
	enum class EPhase : int8
	{
		Active,
		CancelRequested,
		CancelStarted,
		CancelUnwind,
		Canceled,
	};
	EPhase Phase{EPhase::Active};

	// To be run on resume or unwind. May point back to the resumer.
	TWriteBarrier<VTaskNativeHook> NativeDefer;

	// Currently tracked suspended native Await() calls.
	// Invoked in FIFO order when this task completes.
	TWriteBarrier<VTaskNativeHook> NativeAwaitsHead;
	TWriteBarrier<VTaskNativeHook> NativeAwaitsTail;

	// Where execution should continue when resuming.
	FOp* ResumePC{nullptr};
	TWriteBarrier<VFrame> ResumeFrame;
	VReturnSlot ResumeSlot; // May point into ResumeFrame or one of its ancestors.

	// Where execution should continue when suspending.
	FOp* YieldPC;
	TWriteBarrier<VFrame> YieldFrame;
	TWriteBarrier<VTask> YieldTask;

	// Where execution should continue when complete.
	TWriteBarrier<VValue> Result;
	TWriteBarrier<VTask> LastAwait;
	TWriteBarrier<VTask> LastCancel;

	// Links for the containing LastCancel or LastAwait list.
	TWriteBarrier<VTask> PrevTask;
	TWriteBarrier<VTask> NextTask;

	COREUOBJECT_API static void BindStruct(FAllocationContext Context, VClass& TaskClass);
	COREUOBJECT_API static void BindStructTrivial(FAllocationContext Context);

	static VTask& New(FAllocationContext Context, FOp* YieldPC, VFrame* YieldFrame, VTask* YieldTask, VTask* Parent)
	{
		VEmergentType& TaskEmergentType = *EmergentType;
		uint64 NumIndexedFields = TaskEmergentType.Shape->NumIndexedFields;
		VTask& Result = *new (AllocateCell(Context, StaticCppClassInfo, NumIndexedFields)) VTask(Context, TaskEmergentType, YieldPC, YieldFrame, YieldTask, Parent);
		if (FDebugger* Debugger = GetDebugger())
		{
			Debugger->AddTask(Context, Result);
		}
		return Result;
	}

	COREUOBJECT_API FOpResult::EKind Resume(FRunningContext Context, VValue ResumeArgument);
	COREUOBJECT_API FOpResult::EKind Unwind(FRunningContext Context);

	bool Active() const { return Phase < EPhase::CancelStarted && !Result; }
	bool Completed() const { return !!Result; }

	COREUOBJECT_API static FOpResult ActiveImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult CompletedImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult CancelingImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult CanceledImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult UnsettledImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult SettledImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult UninterruptedImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult InterruptedImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);

	COREUOBJECT_API static FOpResult AwaitImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);
	COREUOBJECT_API static FOpResult CancelImpl(FRunningContext Context, VValue Scope, VNativeFunction::Args Arguments);

	bool RequestCancel(FRunningContext Context);
	bool CancelChildren(FRunningContext Context);

	void Suspend(FAccessContext Context)
	{
		bRunning = false;
	}

	void Resume(FAccessContext Context)
	{
		bRunning = true;
	}

	void Park(FAccessContext Context, TWriteBarrier<VTask>& LastTask)
	{
		V_DIE_IF(PrevTask || NextTask);
		if (LastTask)
		{
			PrevTask.Set(Context, LastTask.Get());
			LastTask->NextTask.Set(Context, this);
		}
		LastTask.Set(Context, this);
	}

	void Unpark(FAccessContext Context, TWriteBarrier<VTask>& LastTask)
	{
		if (LastTask.Get() == this)
		{
			V_DIE_IF(NextTask);
			LastTask.Set(Context, PrevTask.Get());
		}
		if (PrevTask)
		{
			V_DIE_UNLESS(PrevTask->NextTask.Get() == this);
			PrevTask->NextTask.Set(Context, NextTask.Get());
		}
		if (NextTask)
		{
			V_DIE_UNLESS(NextTask->PrevTask.Get() == this);
			NextTask->PrevTask.Set(Context, PrevTask.Get());
		}
		PrevTask.Reset();
		NextTask.Reset();
	}

	template <
		typename FunctorType
			UE_REQUIRES(
				!TIsTFunction<std::decay_t<FunctorType>>::Value && std::is_invocable_r_v<void, std::decay_t<FunctorType>, FAccessContext, VTask*>)>
	void Defer(FAllocationContext Context, FunctorType&& Func)
	{
		// This function expects to be run in the open
		V_DIE_IF(AutoRTFM::IsClosed());
		V_DIE_IF(NativeDefer);

		NativeDefer.Set(Context, VTaskNativeHook::New(Context, Func));
	}

	void ClearDefer()
	{
		NativeDefer.Reset();
	}

	template <
		typename FunctorType
			UE_REQUIRES(
				!TIsTFunction<std::decay_t<FunctorType>>::Value && std::is_invocable_r_v<void, std::decay_t<FunctorType>, FAccessContext, VTask*>)>
	void Await(FRunningContext Context, FunctorType&& Func)
	{
		Await(Context, VTaskNativeHook::New(Context, Func));
	}

	COREUOBJECT_API void ExecNativeDefer(FAccessContext Context);
	COREUOBJECT_API void ExecNativeAwaits(FAccessContext Context);

	struct FCallerSpec
	{
		FOp* PC;
		VFrame* Frame;
		VRestValue* ReturnSlot;
	};
	COREUOBJECT_API static FCallerSpec MakeFrameForSpawn(FAllocationContext Context);

	COREUOBJECT_API static void InitializeGlobals(FAllocationContext Context);

	static void SerializeLayout(FAllocationContext Context, VTask*& This, FStructuredArchiveVisitor& Visitor);
	void SerializeImpl(FAllocationContext Context, FStructuredArchiveVisitor& Visitor);

private:
	enum class EEndTaskProcedureRegister : uint32
	{
		TaskResult = FRegisterIndex::PARAMETER_START,
		__MAX
	};

	VTask(FAllocationContext Context, VEmergentType& TaskEmergentType, FOp* YieldPC, VFrame* YieldFrame, VTask* YieldTask, VTask* Parent)
		: VValueObject(Context, TaskEmergentType)
		, TIntrusiveTree(Context, Parent)
		, ResumeSlot(Context, nullptr)
		, YieldPC(YieldPC)
		, YieldFrame(Context, YieldFrame)
		, YieldTask(Context, YieldTask)
	{
	}

	COREUOBJECT_API void Await(FAccessContext Context, VTaskNativeHook& Hook);
};

// A counting semaphore with room for a single waiting task. Used for structured concurrency.
struct VSemaphore : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);
	COREUOBJECT_API static TGlobalTrivialEmergentTypePtr<&StaticCppClassInfo> GlobalTrivialEmergentType;

	int32 Count{0};
	TWriteBarrier<VTask> Await;

	static VSemaphore& New(FAllocationContext Context)
	{
		return *new (Context.AllocateFastCell(sizeof(VSemaphore))) VSemaphore(Context);
	}

private:
	VSemaphore(FAllocationContext Context)
		: VCell(Context, &GlobalTrivialEmergentType.Get(Context))
	{
	}
};
} // namespace Verse

#endif