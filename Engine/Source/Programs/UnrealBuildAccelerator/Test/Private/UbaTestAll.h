// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaFile.h"
#include "UbaStringBuffer.h"
#include "UbaTest.h"

namespace uba
{

#if 0
	#define UBA_TEST_EXTRA \
		UBA_TEST(TestSharedReservedMemory)
#else
	#define UBA_TEST_EXTRA
#endif


#define UBA_ALLPLATFORM_TESTS \
		UBA_TEST(TestTime) \
		UBA_TEST(TestEvents) \
		UBA_TEST(TestPaths) \
		UBA_TEST(TestFiles) \
		UBA_TEST(TestTraverseDir) \
		UBA_TEST(TestOverlappedIO) \
		UBA_TEST(TestMemoryBlock) \
		UBA_TEST(TestParseArguments) \
		UBA_TEST(TestBinaryWriter) \
		UBA_TEST(TestSockets) \
		UBA_TEST(TestClientServer) \
		UBA_TEST(TestClientServer2) \
		UBA_TEST(TestClientServerMem) \
		UBA_TEST(TestStorage) \
		UBA_TEST(TestRemoteStorageStore) \
		UBA_TEST(TestRemoteStorageFetch) \
		UBA_TEST(TestDetouredTestApp) \
		UBA_TEST(TestRemoteDetouredTestApp) \
		UBA_TEST(TestCompactPathTable) \
		UBA_TEST(TestCompactCasKeyTable) \
		UBA_TEST(TestCacheEntry) \
		UBA_TEST(TestHashTable) \
		UBA_TEST(TestLoadConfig) \
		UBA_TEST(TestSaveConfig) \
		UBA_TEST(TestCrypto) \
		UBA_TEST(TestBinDependencies) \
		UBA_TEST(TestRootPaths) \
		UBA_TEST(TestRegisterChanges) \
		UBA_TEST(TestRegisterChangesRemote) \
		UBA_TEST(TestDetouredClang) \
		UBA_TEST(TestFileMappingBuffer) \
		UBA_TEST(TestRemoteDirectoryTable) \
		UBA_TEST(TestThreads) \
		UBA_TEST_EXTRA \

#define UBA_POSIX_TESTS \


#define UBA_NONMAC_TESTS \
		UBA_TEST(TestMultipleDetouredProcesses) \
		UBA_TEST(TestLogLines) \
		UBA_TEST(TestLogLinesNoDetour) \
		UBA_TEST(TestLocalSchedule) \
		UBA_TEST(TestLocalScheduleReuse) \
		UBA_TEST(TestRemoteScheduleReuse) \
		UBA_TEST(TestCacheClientAndServer) \
		UBA_TEST(TestRemoteDetouredClang) \

#define UBA_WINDOWS_TESTS \
		UBA_NONMAC_TESTS \
		UBA_TEST(TestKnownSystemFiles) \
		UBA_TEST(TestCustomService) \
		UBA_TEST(TestStdOutLocal) \
		UBA_TEST(TestStdOutViaCmd) \
		UBA_TEST(TestVolumeCache) \
		UBA_TEST(TestDependencyCrawler) \


#define UBA_LINUX_TESTS \
		UBA_NONMAC_TESTS \
		UBA_POSIX_TESTS \
		UBA_TEST(TestDetouredTouch) \
		UBA_TEST(TestDetouredPopen) \


#define UBA_MAC_TESTS \
		UBA_POSIX_TESTS




#if !PLATFORM_WINDOWS
#undef UBA_WINDOWS_TESTS
#define UBA_WINDOWS_TESTS
#endif

#if !PLATFORM_LINUX
#undef UBA_LINUX_TESTS
#define UBA_LINUX_TESTS
#endif

#if !PLATFORM_MAC
#undef  UBA_MAC_TESTS
#define UBA_MAC_TESTS
#endif

#define UBA_TESTS \
		UBA_ALLPLATFORM_TESTS \
		UBA_WINDOWS_TESTS \
		UBA_LINUX_TESTS \
		UBA_MAC_TESTS \

	#define UBA_TEST(x) bool x(LoggerWithWriter& logger, const StringBufferBase& testRootDir);
	UBA_TESTS
	#undef UBA_TEST


	#define UBA_TEST(x) \
		if (!filter || Contains(TC(#x), filter)) \
		{ \
			logger.Info(TC("Running %s..."), TC(#x)); \
			if (!x(testLogger, testRootDir)) \
				return logger.Error(TC("  %s failed"), TC(#x)); \
			logger.Info(TC("  %s success!"),  TC(#x)); \
		}

	bool RunTests(int argc, tchar* argv[])
	{
		LoggerWithWriter logger(g_consoleLogWriter, TC(""));

		FilteredLogWriter filteredWriter(g_consoleLogWriter, LogEntryType_Warning);
		LoggerWithWriter testLogger(filteredWriter, TC("   "));
		//LoggerWithWriter& testLogger = logger;

		StringBuffer<512> testRootDir;

		#if PLATFORM_WINDOWS
		StringBuffer<> temp;
		temp.count = GetTempPathW(temp.capacity, temp.data);
		testRootDir.count = GetLongPathNameW(temp.data, testRootDir.data, testRootDir.capacity);
		testRootDir.EnsureEndsWithSlash().Append(L"UbaTest");
		#else
		testRootDir.count = GetFullPathNameW("~/UbaTest", testRootDir.capacity, testRootDir.data, nullptr);
		#endif
		DeleteAllFiles(logger, testRootDir.data, false);
		CreateDirectoryW(testRootDir.data);
		testRootDir.EnsureEndsWithSlash();

		logger.Info(TC("Running tests (Test rootdir: %s)"), testRootDir.data);

		const tchar* filter = nullptr;
		if (argc > 1)
			filter = argv[1];

		//UBA_TEST(TestStress) // This can not be submitted.. it depends on CoordinatorHorde and credentials
		//UBA_TEST(TestStdOutRemote) // This can not be submitted.. depends on a running UbaAgent
		UBA_TESTS

		logger.Info(TC("Tests finished successfully!"));
		Sleep(2000);

		return true;
	}
}
