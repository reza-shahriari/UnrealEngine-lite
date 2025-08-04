// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"
#include "AutoRTFMTesting.h"
#include "CoreMinimal.h"
#include "Misc/EventPool.h"
#include "Misc/LazySingleton.h"
#include "RequiredProgramMainCPPInclude.h" // required for ue programs
#include "HAL/MallocLeakDetection.h"

IMPLEMENT_APPLICATION(AutoRTFMTests, "AutoRTFMTests");

#define CATCH_AMALGAMATED_CUSTOM_MAIN
#include "catch_amalgamated.cpp"

class FListener final : public Catch::EventListenerBase
{
public:
	using Catch::EventListenerBase::EventListenerBase;

	void testCaseStarting(const Catch::TestCaseInfo&) override
	{
		FMallocLeakDetection::Get().SetAllocationCollection(true);
	}

	void testCaseEnded(const Catch::TestCaseStats&) override
	{
		FMallocLeakDetection::Get().SetAllocationCollection(false);
	}

	void testRunEnded(const Catch::TestRunStats&) override
	{
	}
};

CATCH_REGISTER_LISTENER(FListener)

void AutoRTFM::Testing::AssertionFailure(const char* Expression, const char* File, int Line)
{
	FAIL(File << ":" << Line << ": " << Expression);
}

// Checks for memory leaks. Returns true if no leaks were found, otherwise all
// leaks are printed to stderr and false is returned.
bool CheckNoMemoryLeaks();

int RunTests(int ArgC, const char* ArgV[])
{
	{
		// measure_environment lazily allocates an Environment pointer on first
		// call, and holds this in a static variable. Pre-allocate this with the
		// leak detector disabled to prevent this being reported as a leak.
		MALLOCLEAK_IGNORE_SCOPE();
		Catch::Benchmark::Detail::measure_environment<Catch::Benchmark::default_clock>();
	}

	Catch::Session Session;

	bool bNoRetry = false;
	bool bRetryNestedToo = false;
	bool bEnableBenchmarks = false;

	Session.cli(Session.cli()
		| Catch::Clara::Opt(bNoRetry)["--no-retry"]
		| Catch::Clara::Opt(bRetryNestedToo)["--retry-nested-too"]
		| Catch::Clara::Opt(bEnableBenchmarks)["--enable-benchmarks"]);

	{
		const int Result = Session.applyCommandLine(ArgC, ArgV);

		if (!bEnableBenchmarks)
		{
			constexpr int FakeArgC = 2;
			const char* const FakeArgV[FakeArgC] = { ArgV[0], "--skip-benchmarks" };
			Session.applyCommandLine(FakeArgC, FakeArgV);
		}

		if (0 != Result)
		{
			return Result;
		}
	}

	const TCHAR* CommandLine = TEXT("-Multiprocess -LogCmds=\"LogCsvProfiler off, LogStreaming off, LogUObjectGlobals off, LogPackageName off, LogAutoRTFM warning\" -AsyncLoadingThread");
	GEngineLoop.PreInit(CommandLine); // Note: Initializes AutoRTFM
	GLog->SetCurrentThreadAsPrimaryThread();

	if (bRetryNestedToo)
	{
		AutoRTFM::ForTheRuntime::SetRetryTransaction(AutoRTFM::ForTheRuntime::EAutoRTFMRetryTransactionState::RetryNestedToo);
	}
	else if (bNoRetry)
	{
		AutoRTFM::ForTheRuntime::SetRetryTransaction(AutoRTFM::ForTheRuntime::EAutoRTFMRetryTransactionState::NoRetry);
	}
	else
	{
		// Otherwise default to just retrying the parent transaction.
		AutoRTFM::ForTheRuntime::SetRetryTransaction(AutoRTFM::ForTheRuntime::EAutoRTFMRetryTransactionState::RetryNonNested);
	}

	// Enable AutoRTFM.
	AutoRTFM::ForTheRuntime::SetAutoRTFMRuntime(AutoRTFM::ForTheRuntime::EAutoRTFMEnabledState::AutoRTFM_EnabledByDefault);

	// By default, crash on an internal abort to keep testing honest.
	AutoRTFM::ForTheRuntime::SetInternalAbortAction(AutoRTFM::ForTheRuntime::EAutoRTFMInternalAbortActionState::Crash);

	// Unexpected memory validation errors should be full assertions, without validation throttling.
	AutoRTFM::ForTheRuntime::SetMemoryValidationLevel(AutoRTFM::EMemoryValidationLevel::Error);
	AutoRTFM::ForTheRuntime::SetMemoryValidationThrottlingEnabled(false);

	// Don't print memory validation stats.
	AutoRTFM::ForTheRuntime::SetMemoryValidationStatisticsEnabled(false);

	FModuleManager::Get().StartProcessingNewlyLoadedObjects();

	int Result = Session.run();

	FPlatformMisc::RequestExit(false);

	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /* bPerformFullPurge */ true);

	FEngineLoop::AppPreExit();
	FModuleManager::Get().UnloadModulesAtShutdown();
	FEngineLoop::AppExit();
	TLazySingleton<TEventPool<EEventMode::AutoReset>>::Get().EmptyPool();
	TLazySingleton<TEventPool<EEventMode::ManualReset>>::Get().EmptyPool();

	return Result;
}

int main(int ArgC, const char* ArgV[])
{
	int Result = RunTests(ArgC, ArgV);
	
#if 0 // Memory leak detection disabled - see FORT-794390
	if (0 == Result && !CheckNoMemoryLeaks() )
	{
		Result = -1;
	}
#endif

	return Result;
}

bool CheckNoMemoryLeaks()
{
	class FOutputDeviceStderr final : public FOutputDevice
	{
	public:
		void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const class FName& Category) override
		{
			std::cerr << reinterpret_cast<const char*>(StringCast<UTF8CHAR>(V).Get()) << std::endl;
		}
	};

	FOutputDeviceStderr OutputDevice;
	FMallocLeakReportOptions Options;
	Options.OutputDevice = &OutputDevice;
	int32 NumLeaks = FMallocLeakDetection::Get().DumpOpenCallstacks(TEXT("AutoRTFMTests"), Options);
	if (NumLeaks > 0)
	{
		std::cerr << NumLeaks << " memory leaks detected" << std::endl;
		return false;
	}

	return true;
}
