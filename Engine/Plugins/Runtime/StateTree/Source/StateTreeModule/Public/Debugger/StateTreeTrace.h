// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_STATETREE_TRACE

#include "Trace/Trace.h"
#include "Containers/ContainersFwd.h"
#include "Containers/UnrealString.h"
#include "Containers/StringView.h"


struct FStateTreeExecutionFrame;
class UStateTree;
struct FStateTreeDataView;
struct FStateTreeActiveStates;
struct FStateTreeInstanceDebugId;
struct FStateTreeIndex16;
struct FStateTreeStateHandle;
struct FStateTreeTransitionSource;
enum class EStateTreeStateSelectionBehavior : uint8;
enum class EStateTreeRunStatus : uint8;
enum class EStateTreeTraceEventType : uint8;
enum class EStateTreeUpdatePhase : uint8;
namespace ELogVerbosity { enum Type : uint8; }

UE_TRACE_CHANNEL_EXTERN(StateTreeDebugChannel, STATETREEMODULE_API)

namespace UE::StateTreeTrace
{

/** Struct allowing StateTree nodes to add (or replace) a custom string to the trace event */
struct FNodeCustomDebugData
{
	enum class EMergePolicy: uint8
	{
		Unset,
		Append,
		Override
	};

	FNodeCustomDebugData() = default;

	FNodeCustomDebugData(FStringView TraceDebuggerString, const EMergePolicy MergePolicy): TraceDebuggerString(TraceDebuggerString)
	, MergePolicy(MergePolicy)
	{
	}

	FNodeCustomDebugData(FNodeCustomDebugData&& Other)
		: TraceDebuggerString(MoveTemp(Other.TraceDebuggerString))
		, MergePolicy(Other.MergePolicy)
	{
		Other.Reset();
	}

	FNodeCustomDebugData& operator=(FNodeCustomDebugData&& Other)
	{
		if (this == &Other)
		{
			return *this;
		}
		TraceDebuggerString = MoveTemp(Other.TraceDebuggerString);
		MergePolicy = Other.MergePolicy;
		Other.Reset();
		return *this;
	}

	bool IsSet() const
	{
		return MergePolicy != EMergePolicy::Unset && !TraceDebuggerString.IsEmpty();
	}

	void Reset()
	{
		TraceDebuggerString.Reset();
		MergePolicy = EMergePolicy::Unset;
	}

	bool ShouldOverrideDataView() const
	{
		return MergePolicy == EMergePolicy::Override;
	}
	bool ShouldAppendToDataView() const
	{
		return MergePolicy == EMergePolicy::Append;
	}

	FStringView GetTraceDebuggerString() const
	{
		return TraceDebuggerString;
	}

private:
	FString TraceDebuggerString;
	EMergePolicy MergePolicy = EMergePolicy::Unset;
};

void RegisterGlobalDelegates();
void UnregisterGlobalDelegates();
UE_DEPRECATED(5.6, "This method will no longer be exposed publicly.")
FStateTreeIndex16 FindOrAddDebugIdForAsset(const UStateTree* StateTree);
void OutputPhaseScopeEvent(FStateTreeInstanceDebugId InstanceId, EStateTreeUpdatePhase Phase, EStateTreeTraceEventType EventType, FStateTreeStateHandle StateHandle);
void OutputAssetDebugIdEvent(const UStateTree* StateTree, FStateTreeIndex16 AssetDebugId);
void OutputInstanceLifetimeEvent(FStateTreeInstanceDebugId InstanceId, const UStateTree* StateTree, const TCHAR* InstanceName, EStateTreeTraceEventType EventType);
void OutputInstanceAssetEvent(FStateTreeInstanceDebugId InstanceId, const UStateTree* StateTree);
void OutputInstanceFrameEvent(FStateTreeInstanceDebugId InstanceId, const FStateTreeExecutionFrame* Frame);
void OutputLogEventTrace(FStateTreeInstanceDebugId InstanceId, ELogVerbosity::Type Verbosity, const TCHAR* Fmt, ...);
void OutputStateEventTrace(FStateTreeInstanceDebugId InstanceId, FStateTreeStateHandle StateHandle, EStateTreeTraceEventType EventType);
void OutputTaskEventTrace(FStateTreeInstanceDebugId InstanceId, FNodeCustomDebugData&& DebugData, FStateTreeIndex16 TaskIdx, FStateTreeDataView DataView, EStateTreeTraceEventType EventType, EStateTreeRunStatus Status);
void OutputEvaluatorEventTrace(FStateTreeInstanceDebugId InstanceId, FNodeCustomDebugData&& DebugData, FStateTreeIndex16 EvaluatorIdx, FStateTreeDataView DataView, EStateTreeTraceEventType EventType);
void OutputConditionEventTrace(FStateTreeInstanceDebugId InstanceId, FNodeCustomDebugData&& DebugData, FStateTreeIndex16 ConditionIdx, FStateTreeDataView DataView, EStateTreeTraceEventType EventType);
void OutputTransitionEventTrace(FStateTreeInstanceDebugId InstanceId, FStateTreeTransitionSource TransitionSource, EStateTreeTraceEventType EventType);
void OutputActiveStatesEventTrace(FStateTreeInstanceDebugId InstanceId, TConstArrayView<FStateTreeExecutionFrame> ActiveFrames);

} // UE::StateTreeTrace

#define TRACE_STATETREE_INSTANCE_EVENT(InstanceID, StateTree, InstanceName, EventType) \
	UE::StateTreeTrace::OutputInstanceLifetimeEvent(InstanceID, StateTree, InstanceName, EventType);

#define TRACE_STATETREE_INSTANCE_FRAME_EVENT(InstanceID, Frame) \
	UE::StateTreeTrace::OutputInstanceFrameEvent(InstanceID, Frame);

#define TRACE_STATETREE_PHASE_EVENT(InstanceID, Phase, EventType, StateHandle) \
	UE::StateTreeTrace::OutputPhaseScopeEvent(InstanceID, Phase, EventType, StateHandle); \

#define TRACE_STATETREE_LOG_EVENT(InstanceId, TraceVerbosity, Format, ...) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputLogEventTrace(InstanceId, ELogVerbosity::TraceVerbosity, Format, ##__VA_ARGS__); \
	}

#define TRACE_STATETREE_STATE_EVENT(InstanceId, StateHandle, EventType) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputStateEventTrace(InstanceId, StateHandle, EventType); \
	}

#define TRACE_STATETREE_TASK_EVENT(InstanceId, DebugData, TaskIdx, DataView, EventType, Status) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputTaskEventTrace(InstanceId, DebugData, TaskIdx, DataView, EventType, Status); \
	}

#define TRACE_STATETREE_EVALUATOR_EVENT(InstanceId, DebugData, EvaluatorIdx, DataView, EventType) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputEvaluatorEventTrace(InstanceId, DebugData, EvaluatorIdx, DataView, EventType); \
	}

#define TRACE_STATETREE_CONDITION_EVENT(InstanceId, DebugData, ConditionIdx, DataView, EventType) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputConditionEventTrace(InstanceId, DebugData, ConditionIdx, DataView, EventType); \
	}

#define TRACE_STATETREE_TRANSITION_EVENT(InstanceId, TransitionIdx, EventType) \
	if (UE_TRACE_CHANNELEXPR_IS_ENABLED(StateTreeDebugChannel)) \
	{ \
		UE::StateTreeTrace::OutputTransitionEventTrace(InstanceId, TransitionIdx, EventType); \
	}

#define TRACE_STATETREE_ACTIVE_STATES_EVENT(InstanceId, ActivateFrames) \
		UE::StateTreeTrace::OutputActiveStatesEventTrace(InstanceId, ActivateFrames);

#else //WITH_STATETREE_TRACE

#define TRACE_STATETREE_INSTANCE_EVENT(...)
#define TRACE_STATETREE_INSTANCE_FRAME_EVENT(...)
#define TRACE_STATETREE_PHASE_EVENT(...)
#define TRACE_STATETREE_LOG_EVENT(...)
#define TRACE_STATETREE_STATE_EVENT(...)
#define TRACE_STATETREE_TASK_EVENT(...)
#define TRACE_STATETREE_EVALUATOR_EVENT(...)
#define TRACE_STATETREE_CONDITION_EVENT(...)
#define TRACE_STATETREE_TRANSITION_EVENT(...)
#define TRACE_STATETREE_ACTIVE_STATES_EVENT(...)

#endif // WITH_STATETREE_TRACE
