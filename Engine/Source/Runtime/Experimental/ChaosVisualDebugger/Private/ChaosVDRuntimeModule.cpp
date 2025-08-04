// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosVDRuntimeModule.h"

#include "Modules/ModuleManager.h"

#if WITH_CHAOS_VISUAL_DEBUGGER

#include "Containers/Array.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Internationalization.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"

#if WITH_EDITOR
#include "Misc/ScopedSlowTask.h"
#endif

#include "ChaosVDRecordingDetails.h"
#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/TraceAuxiliary.h"

IMPLEMENT_MODULE(FChaosVDRuntimeModule, ChaosVDRuntime);

DEFINE_LOG_CATEGORY_STATIC( LogChaosVDRuntime, Log, All );

#define LOCTEXT_NAMESPACE "ChaosVisualDebugger"

FAutoConsoleCommand ChaosVDStartRecordingCommand(
	TEXT("p.Chaos.StartVDRecording"),
	TEXT("Turn on the recording of debugging data"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		UE_AUTORTFM_ONCOMMIT(Args)
		{
			FChaosVDRuntimeModule::Get().StartRecording(Args);
		};
	})
);

FAutoConsoleCommand StopVDStartRecordingCommand(
	TEXT("p.Chaos.StopVDRecording"),
	TEXT("Turn off the recording of debugging data"),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		UE_AUTORTFM_ONCOMMIT(Args)
		{
			FChaosVDRuntimeModule::Get().StopRecording();
		};
	})
);

static FAutoConsoleVariable CVarChaosVDGTimeBetweenFullCaptures(
	TEXT("p.Chaos.VD.TimeBetweenFullCaptures"),
	10,
	TEXT("Time interval in seconds after which a full capture (not only delta changes) should be recorded"));

static FAutoConsoleVariable CVarChaosVDMaxTimeToWaitForDisconnect(
	TEXT("p.Chaos.VD.MaxTimeToWaitForDisconnectSeconds"),
	5.0f,
	TEXT("Max time to wait after attempting to stop an active trace session. After that time has passed if we are still connected, CVD will continue and eventually error out."));

FChaosVDRecordingStateChangedDelegate FChaosVDRuntimeModule::RecordingStartedDelegate = FChaosVDRecordingStateChangedDelegate();
FChaosVDRecordingStateChangedDelegate FChaosVDRuntimeModule::RecordingStopDelegate = FChaosVDRecordingStateChangedDelegate();
FChaosVDRecordingStartFailedDelegate FChaosVDRuntimeModule::RecordingStartFailedDelegate = FChaosVDRecordingStartFailedDelegate();
FChaosVDCaptureRequestDelegate FChaosVDRuntimeModule::PerformFullCaptureDelegate = FChaosVDCaptureRequestDelegate();
FTransactionallySafeRWLock FChaosVDRuntimeModule::DelegatesRWLock = FTransactionallySafeRWLock();

FChaosVDRuntimeModule& FChaosVDRuntimeModule::Get()
{
	return FModuleManager::Get().LoadModuleChecked<FChaosVDRuntimeModule>(TEXT("ChaosVDRuntime"));
}

bool FChaosVDRuntimeModule::IsLoaded()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("ChaosVDRuntime"));
}

void FChaosVDRuntimeModule::StartupModule()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("StartCVDRecording")))
	{
		TArray<FString, TInlineAllocator<1>> CVDOptions;

		{
			FString CVDHostAddress;
			if (FParse::Value(FCommandLine::Get(), TEXT("CVDHost="), CVDHostAddress))
			{
				CVDOptions.Emplace(MoveTemp(CVDHostAddress));
			}
		}
        
        StartRecording(CVDOptions);
	}
	else
	{
		
#if UE_TRACE_ENABLED
		UE::Trace::ToggleChannel(TEXT("ChaosVDChannel"), false);
#endif

	}
}

void FChaosVDRuntimeModule::ShutdownModule()
{
	if (bIsRecording)
	{
		StopRecording();
	}

	FTraceAuxiliary::OnTraceStopped.RemoveAll(this);
}

int32 FChaosVDRuntimeModule::GenerateUniqueID()
{
	int32 NewID = 0;
	UE_AUTORTFM_OPEN
	{
		NewID = LastGeneratedID++;
	};

	return NewID;
}

FString FChaosVDRuntimeModule::GetLastRecordingFileNamePath() const
{
	return LastRecordingFileNamePath;
}

FChaosVDTraceDetails FChaosVDRuntimeModule::GetCurrentTraceSessionDetails() const
{
	FChaosVDTraceDetails Details;
	if (bool bIsConnected = FTraceAuxiliary::IsConnected(Details.SessionGuid, Details.TraceGuid))
	{
		Details.TraceTarget = FTraceAuxiliary::GetTraceDestinationString();
		Details.Mode = FTraceAuxiliary::GetConnectionType() == FTraceAuxiliary::EConnectionType::File ? EChaosVDRecordingMode::File : EChaosVDRecordingMode::Live;
		Details.bIsConnected = bIsConnected;
		return Details;
	}

	return Details;
}

void FChaosVDRuntimeModule::StopTrace()
{
	bRequestedStop = true;
	FTraceAuxiliary::Stop();
}

void FChaosVDRuntimeModule::GenerateRecordingFileName(FString& OutFileName)
{
	FStringFormatOrderedArguments NameArgs { FString(FApp::GetProjectName()), LexToString(FApp::GetBuildTargetType()), FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")) };
	
	OutFileName = FString::Format(TEXT("ChaosVD-{0}-{1}-{2}.utrace"), NameArgs);
}

bool FChaosVDRuntimeModule::RequestFullCapture(float DeltaTime)
{
	// Full capture intervals are clamped to be no lower than 1 sec
	UE::TReadScopeLock ReadLock (DelegatesRWLock);
	PerformFullCaptureDelegate.Broadcast(EChaosVDFullCaptureFlags::Particles);
	return true;
}

bool FChaosVDRuntimeModule::RecordingTimerTick(float DeltaTime)
{
	if (bIsRecording)
	{
		AccumulatedRecordingTime += DeltaTime;
	}
	
	return true;
}

void FChaosVDRuntimeModule::StartRecording(TConstArrayView<FString> Args)
{
	if (bIsRecording)
	{
		return;
	}

	// Start Listening for Trace Stopped events, in case Trace is stopped outside our control so we can gracefully stop CVD recording and log a warning 
	FTraceAuxiliary::OnTraceStopped.AddRaw(this, &FChaosVDRuntimeModule::HandleTraceStopRequest);

	// Start with a generic Failure reason
	FText FailureReason = LOCTEXT("SeeLogsForErrorDetailsText","Please see the logs for more details...");

#if UE_TRACE_ENABLED

	// Other tools could bee using trace
	// This is aggressive but until Trace supports multi-sessions, just take over.
	if (FTraceAuxiliary::IsConnected())
	{
		UE_LOG(LogChaosVDRuntime, Log, TEXT("[%s] There is an active trace session. attempting to disconnect..."), ANSI_TO_TCHAR(__FUNCTION__));

		//TODO: We should make the wait async like we do whe we attempt to connect to a live session
		if (FTraceAuxiliary::Stop() && WaitForTraceSessionDisconnect())
		{
			UE_LOG(LogChaosVDRuntime, Log, TEXT("[%s] Successful disconnect attempt!."), ANSI_TO_TCHAR(__FUNCTION__));
		}
		else
		{
			FailureReason = LOCTEXT("FailedToStopActiveRecordingErrorMessage", "Failed to Stop active Trace Session.");
		}
	}

	SaveAndDisabledCurrentEnabledTraceChannels();

	EnableRequiredTraceChannels();

	FTraceAuxiliary::FOptions TracingOptions;
	TracingOptions.bExcludeTail = true;

	if (Args.Num() == 0 || Args[0] == TEXT("File"))
	{
		LastRecordingFileNamePath.Empty();
		GenerateRecordingFileName(LastRecordingFileNamePath);

		UE_LOG(LogChaosVDRuntime, Log, TEXT("[%s] Generated trace file name [%s]"), ANSI_TO_TCHAR(__FUNCTION__), *LastRecordingFileNamePath);

		bIsRecording = FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *LastRecordingFileNamePath, nullptr, &TracingOptions);

		LastRecordingFileNamePath = bIsRecording ? FTraceAuxiliary::GetTraceDestinationString() : TEXT("");
		
		CurrentRecordingMode = EChaosVDRecordingMode::File;
	}
	else if(Args[0] == TEXT("Server"))
	{
		const FString Target = Args.IsValidIndex(1) ? Args[1] : TEXT("127.0.0.1");

		LastRecordingFileNamePath = Target;

		bIsRecording = FTraceAuxiliary::Start(
		FTraceAuxiliary::EConnectionType::Network,
		*Target,
		nullptr, &TracingOptions);
		
		CurrentRecordingMode = EChaosVDRecordingMode::Live;
	}
	else
	{
		FailureReason = LOCTEXT("WrongCommandArgumentsError", "The start recording command was called with invalid arguments");
	}
#endif
	
	AccumulatedRecordingTime = 0.0f;

	if (ensure(bIsRecording))
	{
		{
			UE::TReadScopeLock ReadLock(DelegatesRWLock);
			RecordingStartedDelegate.Broadcast();
		}
		
		constexpr int32 MinAllowedTimeInSecondsBetweenCaptures = 1;
		int32 ConfiguredTimeBetweenCaptures = CVarChaosVDGTimeBetweenFullCaptures->GetInt();

		ensureAlwaysMsgf(ConfiguredTimeBetweenCaptures > MinAllowedTimeInSecondsBetweenCaptures,
			TEXT("The minimum allowed time interval between full captures is [%d] seconds, but [%d] seconds were configured. Clamping to [%d] seconds"),
			MinAllowedTimeInSecondsBetweenCaptures, ConfiguredTimeBetweenCaptures, MinAllowedTimeInSecondsBetweenCaptures);

		FullCaptureRequesterHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FChaosVDRuntimeModule::RequestFullCapture),
			FMath::Clamp(ConfiguredTimeBetweenCaptures, MinAllowedTimeInSecondsBetweenCaptures, TNumericLimits<int32>::Max()));

		RecordingTimerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FChaosVDRuntimeModule::RecordingTimerTick));
	}
	else
	{
		UE_LOG(LogChaosVDRuntime, Error, TEXT("[%s] Failed to start CVD recording | Reason: [%s]"), ANSI_TO_TCHAR(__FUNCTION__), *FailureReason.ToString());

#if WITH_EDITOR
		FMessageDialog::Open(EAppMsgType::Ok, FText::FormatOrdered(LOCTEXT("StartRecordingFailedMessage", "Failed to start CVD recording. \n\n{0}"), FailureReason));
#endif

		{
			UE::TReadScopeLock ReadLock(DelegatesRWLock);
			RecordingStartFailedDelegate.Broadcast(FailureReason);
		}

		CurrentRecordingMode = EChaosVDRecordingMode::Invalid;
	}
}

void FChaosVDRuntimeModule::StopRecording()
{
	if (!bIsRecording)
	{
		UE_LOG(LogChaosVDRuntime, Warning, TEXT("[%s] Attempted to stop recorded when there is no CVD recording active."), ANSI_TO_TCHAR(__FUNCTION__));
		return;
	}
	
	FTraceAuxiliary::OnTraceStopped.RemoveAll(this);

#if UE_TRACE_ENABLED
	RestoreTraceChannelsToPreRecordingState();

	StopTrace();
#endif

	if (FullCaptureRequesterHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(FullCaptureRequesterHandle);

		FullCaptureRequesterHandle.Reset();
	}
	
	if (RecordingTimerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RecordingTimerHandle);
		RecordingTimerHandle.Reset();
	}
	
	bIsRecording = false;
	AccumulatedRecordingTime = 0.0f;

	{
		UE::TReadScopeLock ReadLock(DelegatesRWLock);
		RecordingStopDelegate.Broadcast();
	}
}

void FChaosVDRuntimeModule::HandleTraceStopRequest(FTraceAuxiliary::EConnectionType TraceType, const FString& TraceDestination)
{
	if (bIsRecording)
	{
		if (!ensure(bRequestedStop))
		{
			UE_LOG(LogChaosVDRuntime, Warning, TEXT("Trace Recording has been stopped unexpectedly"));

#if WITH_EDITOR
			FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("UnexpectedStopMessage", "Trace recording has been stopped unexpectedly. CVD cannot continue with the recording session... "));
#endif
		}

		StopRecording();
	}

	bRequestedStop = false;
}

bool FChaosVDRuntimeModule::WaitForTraceSessionDisconnect()
{
	float MaxWaitTime = CVarChaosVDMaxTimeToWaitForDisconnect->GetFloat();
	float CurrentWaitTime = 0.0f;

#if WITH_EDITOR
	FScopedSlowTask DisconnectAttemptSlowTask(MaxWaitTime, LOCTEXT("DisconnectAttemptMessage", " Active Trace Session detected, attempting to disconnect ..."));

	constexpr bool bShowCancelButton = false;
	constexpr bool bAllowInPIE = true;
	DisconnectAttemptSlowTask.MakeDialog(bShowCancelButton, bAllowInPIE);
#endif

	while (CurrentWaitTime < MaxWaitTime)
	{
		constexpr float WaitInterval = 0.1f;
		FPlatformProcess::Sleep(0.1f);

		if (!FTraceAuxiliary::IsConnected())
		{
			return true;
		}

		// We don't need to be precise for this, we can just accumulate the wait
		CurrentWaitTime += WaitInterval;

#if WITH_EDITOR
		DisconnectAttemptSlowTask.EnterProgressFrame(CurrentWaitTime);
#endif
	}

	return FTraceAuxiliary::IsConnected();
}

void FChaosVDRuntimeModule::SaveAndDisabledCurrentEnabledTraceChannels()
{
	// Until we support allowing other channels, indicate in the logs that we are disabling everything else
	UE_LOG(LogChaosVDRuntime, Log, TEXT("[%s] Disabling additional trace channels..."), ANSI_TO_TCHAR(__FUNCTION__));

#if UE_TRACE_ENABLED
	OriginalTraceChannelsState.Reset();

	// Disable any enabled additional channel
	UE::Trace::EnumerateChannels([](const ANSICHAR* ChannelName, bool bEnabled, void* SavedTraceChannelsPtr)
	{
		TMap<FString, bool>* SavedTraceChannels = static_cast<TMap<FString, bool>*>(SavedTraceChannelsPtr);
		FString ChannelNameFString(ChannelName);
		SavedTraceChannels->Add(ChannelNameFString, bEnabled);
		if (bEnabled)
		{
			UE::Trace::ToggleChannel(ChannelNameFString.GetCharArray().GetData(), false);
		}
	}
	, &OriginalTraceChannelsState);
#endif
}

void FChaosVDRuntimeModule::RestoreTraceChannelsToPreRecordingState()
{
#if UE_TRACE_ENABLED
	UE_LOG(LogChaosVDRuntime, Log, TEXT("[%s] Restoring trace channels state..."), ANSI_TO_TCHAR(__FUNCTION__));

	for (const TPair<FString, bool>& ChannelWithState : OriginalTraceChannelsState)
	{
		UE::Trace::ToggleChannel(GetData(ChannelWithState.Key), ChannelWithState.Value); 
	}
	
	OriginalTraceChannelsState.Reset();
#endif
}

void FChaosVDRuntimeModule::EnableRequiredTraceChannels()
{
#if UE_TRACE_ENABLED
	UE::Trace::ToggleChannel(TEXT("ChaosVDChannel"), true); 
	UE::Trace::ToggleChannel(TEXT("Frame"), true);
	UE::Trace::ToggleChannel(TEXT("Log"), true);
#endif
}

#undef LOCTEXT_NAMESPACE
#else

IMPLEMENT_MODULE(FDefaultModuleImpl, ChaosVDRuntime);

#endif
