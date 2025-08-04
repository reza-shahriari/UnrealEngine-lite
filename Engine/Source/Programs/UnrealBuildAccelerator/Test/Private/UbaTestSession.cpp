// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaConfig.h"
#include "UbaFileAccessor.h"
#include "UbaNetworkBackendTcp.h"
#include "UbaProcess.h"
#include "UbaSessionServer.h"
#include "UbaSessionClient.h"
#include "UbaStorageServer.h"
#include "UbaStorageClient.h"
#include "UbaTest.h"

namespace uba
{
	bool RunLocal(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, bool enableDetour)
	{
		LogWriter& logWriter = logger.m_writer;

		StringBuffer<MaxPath> rootDir;
		rootDir.Append(testRootDir).Append(TCV("Uba"));
		if (!DeleteAllFiles(logger, rootDir.data))
			return false;

		StorageCreateInfo storageInfo(rootDir.data, logWriter);
		storageInfo.casCapacityBytes = 1024ull * 1024 * 1024;
		StorageImpl storage(storageInfo);

		bool ctorSuccess = true;
		NetworkServer server(ctorSuccess, { logWriter });

		SessionServerCreateInfo sessionServerInfo(storage, server, logWriter);
		sessionServerInfo.checkMemory = false;
		sessionServerInfo.rootDir = rootDir.data;

		#if UBA_DEBUG
		sessionServerInfo.logToFile = true;
		#endif

		SessionServer session(sessionServerInfo);

		StringBuffer<MaxPath> workingDir;
		workingDir.Append(testRootDir).Append(TCV("WorkingDir"));
		if (!DeleteAllFiles(logger, workingDir.data))
			return false;

		if (!storage.CreateDirectory(workingDir.data))
			return false;
		if (!DeleteAllFiles(logger, workingDir.data, false))
			return false;
		workingDir.EnsureEndsWithSlash();
		return testFunc(logger, session, workingDir.data, [&](const ProcessStartInfo& pi) { return session.RunProcess(pi, true, enableDetour); });
	}

	using TestServerClientSessionFunction = Function<bool(LoggerWithWriter& logger, const StringView& workingDir, SessionServer& sessionServer, SessionClient& sessionClient)>;
	bool SetupServerClientSession(LoggerWithWriter& logger, const StringBufferBase& testRootDir, bool deleteAll, bool serverShouldListen, const TestServerClientSessionFunction& testFunc)
	{
		LogWriter& logWriter = logger.m_writer;
		NetworkBackendTcp tcpBackend(logWriter);

		bool ctorSuccess = true;
		NetworkServer server(ctorSuccess, { logWriter });
		NetworkClient client(ctorSuccess, { logWriter });

		StringBuffer<MaxPath> rootDir;
		rootDir.Append(testRootDir).Append(TCV("Uba"));
		if (deleteAll && !DeleteAllFiles(logger, rootDir.data))
			return false;

		StorageServerCreateInfo storageServerInfo(server, rootDir.data, logWriter);
		storageServerInfo.casCapacityBytes = 1024ull * 1024 * 1024;
		auto& storageServer = *new StorageServer(storageServerInfo);
		auto ssg = MakeGuard([&]() { delete &storageServer; });

		SessionServerCreateInfo sessionServerInfo(storageServer, server, logWriter);
		sessionServerInfo.checkMemory = false;
		sessionServerInfo.rootDir = rootDir.data;

		#if UBA_DEBUG
		sessionServerInfo.logToFile = true;
		sessionServerInfo.remoteLogEnabled = true;
		#endif

		auto& sessionServer = *new SessionServer(sessionServerInfo);
		auto ssg2 = MakeGuard([&]() { delete &sessionServer; });

		auto sg = MakeGuard([&]() { server.DisconnectClients(); });

		sessionServer.SetRemoteProcessReturnedEvent([](Process& p) { p.Cancel(true); });

		Config clientConfig;
		clientConfig.AddTable(TC("Storage")).AddValue(TC("CheckExistsOnServer"), true);
		server.SetClientsConfig(clientConfig);

		u16 port = 1356;

		if (serverShouldListen)
		{
			if (!server.StartListen(tcpBackend, port))
				return logger.Error(TC("Failed to listen"));
			if (!client.Connect(tcpBackend, TC("127.0.0.1"), port))
				return logger.Error(TC("Failed to connect"));
			if (!client.Connect(tcpBackend, TC("127.0.0.1"), port))
				return logger.Error(TC("Failed to connect"));
		}
		else
		{
			if (!client.StartListen(tcpBackend, port))
				return logger.Error(TC("Failed to listen"));
			if (!server.AddClient(tcpBackend, TC("127.0.0.1"), port))
				return logger.Error(TC("Failed to connect"));
			while (server.HasConnectInProgress())
				Sleep(1);
		}

		auto disconnectGuard = MakeGuard([&]() { client.Disconnect(); });

		Config config;
		if (!client.FetchConfig(config))
			return false;

		rootDir.Append(TCV("Client"));
		if (deleteAll && !DeleteAllFiles(logger, rootDir.data))
			return false;

		StorageClientCreateInfo storageClientInfo(client, rootDir.data);
		storageClientInfo.Apply(config);
		auto& storageClient = *new StorageClient(storageClientInfo);
		auto scg = MakeGuard([&]() { delete &storageClient; });

		SessionClientCreateInfo sessionClientInfo(storageClient, client, logWriter);
		sessionClientInfo.rootDir = rootDir.data;
		sessionClientInfo.allowKeepFilesInMemory = false;

		#if UBA_DEBUG
		sessionClientInfo.logToFile = true;
		#endif

		auto& sessionClient = *new SessionClient(sessionClientInfo);
		auto scg2 = MakeGuard([&]() { delete &sessionClient; });

		auto cg = MakeGuard([&]() { sessionClient.Stop(); disconnectGuard.Execute(); });

		StringBuffer<MaxPath> workingDir;
		workingDir.Append(testRootDir).Append(TCV("WorkingDir"));
		if (deleteAll && !DeleteAllFiles(logger, workingDir.data))
			return false;
		if (!storageServer.CreateDirectory(workingDir.data))
			return false;
		if (deleteAll && !DeleteAllFiles(logger, workingDir.data, false))
			return false;

		storageClient.Start();
		sessionClient.Start();


		workingDir.EnsureEndsWithSlash();
		return testFunc(logger, workingDir, sessionServer, sessionClient);
	}

	bool RunRemote(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, bool deleteAll, bool serverShouldListen)
	{
		return SetupServerClientSession(logger, testRootDir, deleteAll, serverShouldListen,
			[&](LoggerWithWriter& logger, const StringView& workingDir, SessionServer& sessionServer, SessionClient& sessionClient)
			{
				return testFunc(logger, sessionServer, workingDir.data, [&](const ProcessStartInfo& pi) { return sessionServer.RunProcessRemote(pi); });
			});
	}

	void GetTestAppPath(LoggerWithWriter& logger, StringBufferBase& out)
	{
		GetDirectoryOfCurrentModule(logger, out);
		out.EnsureEndsWithSlash();
		out.Append(IsWindows ? TC("UbaTestApp.exe") : TC("UbaTestApp"));
	}

	bool CreateTextFile(StringBufferBase& outPath, LoggerWithWriter& logger, const tchar* workingDir, const tchar* fileName, const char* text)
	{
		outPath.Clear().Append(workingDir).EnsureEndsWithSlash().Append(fileName);
		FileAccessor fr(logger, outPath.data);
		if (!fr.CreateWrite())
			return false;
		fr.Write(text, strlen(text) + 1);
		return fr.Close();
	}

	bool RunTestApp(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
	{
		StringBuffer<MaxPath> testApp;
		GetTestAppPath(logger, testApp);

		StringBuffer<MaxPath> fileR;
		if (!CreateTextFile(fileR, logger, workingDir, TC("FileR.h"), "Foo"))
			return false;

		{
			StringBuffer<MaxPath> dir;
			dir.Append(workingDir).Append(TCV("Dir1"));
			if (!CreateDirectoryW(dir.data))
				return logger.Error(TC("Failed to create dir %s"), dir.data);

			dir.Clear().Append(workingDir).Append(TCV("Dir2"));
			if (!CreateDirectoryW(dir.data))
				return logger.Error(TC("Failed to create dir %s"), dir.data);
			dir.EnsureEndsWithSlash().Append(TCV("Dir3"));
			if (!CreateDirectoryW(dir.data))
				return logger.Error(TC("Failed to create dir %s"), dir.data);
			dir.EnsureEndsWithSlash().Append(TCV("Dir4"));
			if (!CreateDirectoryW(dir.data))
				return logger.Error(TC("Failed to create dir %s"), dir.data);
			dir.EnsureEndsWithSlash().Append(TCV("Dir5"));
			if (!CreateDirectoryW(dir.data))
				return logger.Error(TC("Failed to create dir %s"), dir.data);
		}

		ProcessStartInfo processInfo;
		processInfo.application = testApp.data;
		processInfo.workingDir = workingDir;
		processInfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type)
			{
				LoggerWithWriter(g_consoleLogWriter, TC("")).Info(line);
			};

		ProcessHandle process = runProcess(processInfo);
		if (!process.WaitForExit(100000))
			return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
		u32 exitCode = process.GetExitCode();

		if (exitCode != 0)
		{
			for (auto& logLine : process.GetLogLines())
				logger.Error(logLine.text.c_str());
			return logger.Error(TC("UbaTestApp returned exit code %u"), exitCode);
		}

		{
			StringBuffer<MaxPath> fileW2;
			fileW2.Append(workingDir).Append(TCV("FileW2"));
			if (!FileExists(logger, fileW2.data))
				return logger.Error(TC("Can't find file %s"), fileW2.data);
		}
		{
			StringBuffer<MaxPath> fileWF;
			fileWF.Append(workingDir).Append(TCV("FileWF"));
			if (!FileExists(logger, fileWF.data))
				return logger.Error(TC("Can't find file %s"), fileWF.data);
		}
		return true;
	}

#if PLATFORM_MAC
	bool ExecuteCommand(LoggerWithWriter& logger, const tchar* command, StringBufferBase& commandOutput)
	{
		FILE* fpCommand = popen(command, "r");
		if (fpCommand == nullptr || fgets(commandOutput.data, commandOutput.capacity, fpCommand) == nullptr || pclose(fpCommand) != 0)
		{
			logger.Warning("Failed to get an Xcode from xcode-select");
			return false;
		}

		commandOutput.count = strlen(commandOutput.data);
		while (isspace(commandOutput.data[commandOutput.count-1]))
		{
			commandOutput.data[commandOutput.count-1] = 0;
			commandOutput.count--;
		}
		return true;
	}
#endif

	bool RunClang(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
	{
		StringBuffer<MaxPath> sourceFile;
		sourceFile.Append(workingDir).Append(TCV("Code.cpp"));
		FileAccessor codeFile(logger, sourceFile.data);
		if (!codeFile.CreateWrite())
			return false;
		char code[] = "#include <stdio.h>\n int main() { printf(\"Hello world\\n\"); return 0; }";
		if (!codeFile.Write(code, sizeof(code) - 1))
			return false;
		if (!codeFile.Close())
			return false;

#if PLATFORM_WINDOWS
		const tchar* clangPath = TC("c:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\VC\\Tools\\Llvm\\bin\\clang-cl.exe");
#elif PLATFORM_MAC
		StringBuffer<MaxPath> xcodePath;
		if (!ExecuteCommand(logger, "/usr/bin/xcrun --find clang++", xcodePath))
			return true;
		const tchar* clangPath = xcodePath.data;
#else
		const tchar* clangPath = TC("/usr/bin/clang++");
#endif

		if (!FileExists(logger, clangPath)) // Skipping if clang is not installed.
			return true;

		ProcessStartInfo processInfo;
		processInfo.application = clangPath;

		StringBuffer<MaxPath> args;

#if PLATFORM_WINDOWS
		args.Append("/Brepro ");
#elif PLATFORM_MAC
		StringBuffer<MaxPath> xcodeSDKPath;
		if (!ExecuteCommand(logger, "xcrun --show-sdk-path", xcodeSDKPath))
			return true;
		args.Append("-isysroot ");
		args.Append(xcodeSDKPath.data).Append(' ');
#endif
		args.Append(TCV("-o code Code.cpp"));

		processInfo.arguments = args.data;

		processInfo.workingDir = workingDir;
		//processInfo.logFile = TC("/home/honk/RunClang.log");
		ProcessHandle process = runProcess(processInfo);
		if (!process.WaitForExit(40000))
			return logger.Error(TC("clang++ timed out"));
		u32 exitCode = process.GetExitCode();
		if (exitCode != 0)
			return logger.Error(TC("clang++ returned exit code %u"), exitCode);
		return true;
	}

	bool RunCustomService(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
	{
		bool gotMessage = false;
		session.RegisterCustomService([&](uba::Process& process, const void* recv, u32 recvSize, void* send, u32 sendCapacity)
			{
				gotMessage = true;
				//wprintf(L"GOT MESSAGE: %.*s\n", recvSize / 2, (const wchar_t*)recv);
				const wchar_t* hello = L"Hello response from server";
				u64 helloBytes = wcslen(hello) * 2;
				memcpy(send, hello, helloBytes);
				return u32(helloBytes);
			});

		StringBuffer<> testApp;
		GetTestAppPath(logger, testApp);

		ProcessStartInfo processInfo;
		processInfo.application = testApp.data;
		processInfo.workingDir = workingDir;
		processInfo.arguments = TC("Whatever");
		ProcessHandle process = runProcess(processInfo);
		if (!process.WaitForExit(10000))
			return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
		u32 exitCode = process.GetExitCode();

		if (exitCode != 0)
			return logger.Error(TC("UbaTestApp returned exit code %u"), exitCode);

		if (!gotMessage)
			return logger.Error(TC("Never got message from UbaTestApp"));

		return true;
	}

	// NOTE: This test is dependent on the UbaTestApp<Platform>
	// The purpose of this test is to validate that the platform specific detours are
	// working as expected.
	// Before running the actual UbaTestApp, RunLocal calls through a variety of functions
	// that sets up the various UbaSession Servers, Clients, etc. It creates some temporary
	// directories, e.g. Dir1 and eventually call ProcessImpl::InternalCreateProcess.
	// InternalCreateProcess will setup the shared memory, inject the Detour library
	// and setup any other necessary environment variables, and spawn the actual process
	// (in this case the UbaTestApp)
	// Once UbaTestApp has started, it will first check and validate that the detour library
	// is in the processes address space. With the detour in place, the test app will
	// exercise various file functions which will actually go through our detour library.
	bool TestDetouredTestApp(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, RunTestApp);
	}

	bool TestRemoteDetouredTestApp(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunRemote(logger, testRootDir, RunTestApp);
	}

	bool TestCustomService(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunRemote(logger, testRootDir, RunCustomService);
	}

	bool TestDetouredClang(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, RunClang);
	}

	bool TestRemoteDetouredClang(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		// Run twice to test LoadCasTable/SaveCasTable etc
		if (!RunRemote(logger, testRootDir, RunClang))
			return false;
		return RunRemote(logger, testRootDir, RunClang, false);
	}

	bool TestDetouredTouch(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				StringBuffer<> file;
				file.Append(workingDir).Append(TCV("TouchFile.h"));
				FileAccessor fr(logger, file.data);

				CHECK_TRUE(fr.CreateWrite());
				CHECK_TRUE(fr.Write("Foo", 4));
				CHECK_TRUE(fr.Close());
				FileInformation oldInfo;
				CHECK_TRUE(GetFileInformation(oldInfo, logger, file.data));

				Sleep(100);

				ProcessStartInfo processInfo;
				processInfo.application = TC("/usr/bin/touch");
				processInfo.workingDir = workingDir;
				processInfo.arguments = file.data;
				processInfo.logFile = TC("/home/honk/Touch.log");
				ProcessHandle process = runProcess(processInfo);
				if (!process.WaitForExit(10000))
					return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
				u32 exitCode = process.GetExitCode();
				if (exitCode != 0)
					return false;

				FileInformation newInfo;
				CHECK_TRUE(GetFileInformation(newInfo, logger, file.data));
				if (newInfo.lastWriteTime == oldInfo.lastWriteTime)
					return logger.Error(TC("File time not changed after touch"));
				return true;
			});
	}

	bool TestDetouredPopen(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		#if PLATFORM_LINUX
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				StringBuffer<> testApp;
				GetTestAppPath(logger, testApp);

				ProcessStartInfo processInfo;
				processInfo.application = testApp.data;
				processInfo.workingDir = workingDir;
				processInfo.arguments = "-popen";
				processInfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type)
					{
						LoggerWithWriter(g_consoleLogWriter, TC("")).Info(line);
					};

				ProcessHandle process = runProcess(processInfo);
				if (!process.WaitForExit(100000))
					return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
				u32 exitCode = process.GetExitCode();

				if (exitCode != 0)
				{
					for (auto& logLine : process.GetLogLines())
						logger.Error(logLine.text.c_str());
					return logger.Error(TC("UbaTestApp returned exit code %u"), exitCode);
				}
				return true;
			});
		#else
		return true;
		#endif
	}

	const tchar* GetSystemApplication()
	{
		#if PLATFORM_WINDOWS
		return TC("c:\\windows\\system32\\ping.exe");
		#elif PLATFORM_LINUX
		return TC("/usr/bin/zip");
		#else
		return TC("/sbin/zip");
		#endif
	}

	const tchar* GetSystemArguments()
	{
		#if PLATFORM_WINDOWS
		return TC("-n 1 localhost");
		#else
		return TC("-help");
		#endif
	}

	const tchar* GetSystemExpectedLogLine()
	{
		#if PLATFORM_WINDOWS
		return TC("Pinging ");
		#else
		return TC("zip [-options]");
		#endif
	}

	bool TestMultipleDetouredProcesses(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				ProcessStartInfo processInfo;
				processInfo.application = GetSystemApplication();
				processInfo.workingDir = workingDir;
				processInfo.arguments = GetSystemArguments();
				//processInfo.logFile = TC("e:\\temp\\ttt\\LogFile.log");
				Vector<ProcessHandle> processes;

				for (u32 i=0; i!=50; ++i)
					processes.push_back(runProcess(processInfo));

				for (auto& process : processes)
				{
					if (!process.WaitForExit(10000))
						return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
					u32 exitCode = process.GetExitCode();
					if (exitCode != 0)
						return logger.Error(TC("UbaTestApp exited with code %u"), exitCode);
				}

				return true;
			});
	}

	bool RunSystemApplicationAndLookForLog(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
	{
		ProcessStartInfo processInfo;
		processInfo.application = GetSystemApplication();
		processInfo.workingDir = workingDir;
		processInfo.arguments = GetSystemArguments();

		bool foundPingString = false;
		processInfo.logLineUserData = &foundPingString;
		processInfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type)
			{
				*(bool*)userData |= Contains(line, GetSystemExpectedLogLine());
			};

		ProcessHandle process = runProcess(processInfo);

		if (!process.WaitForExit(10000))
			return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
		u32 exitCode = process.GetExitCode();
		if (exitCode != 0)
			return logger.Error(TC("Got exit code %u"), exitCode);
		if (!foundPingString)
			return logger.Error(TC("Did not log string containing \"%s\""), GetSystemExpectedLogLine());
		return true;
	}

	bool TestLogLines(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, RunSystemApplicationAndLookForLog);
	}

	bool TestLogLinesNoDetour(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, RunSystemApplicationAndLookForLog, false);
	}

	bool CheckAttributes(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
	{
		StringBuffer<MaxPath> testApp;
		GetTestAppPath(logger, testApp);
		ProcessStartInfo processInfo;
		processInfo.application = testApp.data;
		processInfo.workingDir = workingDir;
		processInfo.logLineFunc = [](void* userData, const tchar* line, u32 length, LogEntryType type)
			{
				LoggerWithWriter(g_consoleLogWriter, TC("")).Info(line);
			};

		auto GetAttributes = [&](const StringView& file) -> u32
			{
				StringBuffer<> arg(TC("-GetFileAttributes="));
				arg.Append(file);
				processInfo.arguments = arg.data;
				ProcessHandle process = runProcess(processInfo);
				if (!process.WaitForExit(100000))
					return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
				u32 exitCode =  process.GetExitCode();
				return exitCode == 255 ? INVALID_FILE_ATTRIBUTES : exitCode;
			};

		MemoryBlock temp;
		DirectoryTable dirTable(temp);
		dirTable.Init(session.GetDirectoryTableMemory(), 0, 0);

		CHECK_TRUE(session.RefreshDirectory(workingDir, true));
		CHECK_TRUE(session.RefreshDirectory(workingDir));
		CHECK_TRUE(dirTable.EntryExists(ToView(workingDir)) == DirectoryTable::Exists_Maybe);
		dirTable.ParseDirectoryTable(session.GetDirectoryTableSize());
		CHECK_TRUE(dirTable.EntryExists(ToView(workingDir), true) == DirectoryTable::Exists_Yes);

		StringBuffer<MaxPath> sourceFile;
		sourceFile.Append(workingDir).Append(TCV("Code.cpp"));

		CHECK_TRUE(GetAttributes(sourceFile) == INVALID_FILE_ATTRIBUTES);
		FileAccessor codeFile(logger, sourceFile.data);
		CHECK_TRUE(codeFile.CreateWrite());
		CHECK_TRUE(codeFile.Close());
		CHECK_TRUE(session.RegisterNewFile(sourceFile.data));
		CHECK_TRUE(GetAttributes(sourceFile) != INVALID_FILE_ATTRIBUTES);

		CHECK_TRUE(dirTable.EntryExists(sourceFile) == DirectoryTable::Exists_No);
		dirTable.ParseDirectoryTable(session.GetDirectoryTableSize());
		CHECK_TRUE(dirTable.EntryExists(sourceFile) == DirectoryTable::Exists_Yes);

		StringBuffer<MaxPath> newDir;
		newDir.Append(workingDir).Append(TCV("NewDir"));
		StringBuffer<MaxPath> newDirAndSlash(newDir);
		newDirAndSlash.Append('/');

		CHECK_TRUE(GetAttributes(newDir) == INVALID_FILE_ATTRIBUTES);
		CHECK_TRUE(CreateDirectoryW(newDir.data));
		CHECK_TRUE(session.RegisterNewFile(newDir.data));
		CHECK_TRUE(dirTable.EntryExists(newDir) == DirectoryTable::Exists_No);
		dirTable.ParseDirectoryTable(session.GetDirectoryTableSize());
		CHECK_TRUE(dirTable.EntryExists(newDir) == DirectoryTable::Exists_Yes);
		CHECK_TRUE(GetAttributes(newDir) != INVALID_FILE_ATTRIBUTES);
		CHECK_TRUE(GetAttributes(newDirAndSlash) != INVALID_FILE_ATTRIBUTES);

		StringBuffer<MaxPath> newDir2;
		newDir2.Append(workingDir).Append(TCV("NewDir2"));
		CHECK_TRUE(CreateDirectoryW(newDir2.data));
		CHECK_TRUE(GetAttributes(newDir2) == INVALID_FILE_ATTRIBUTES);
		CHECK_TRUE(session.RefreshDirectory(workingDir))
		CHECK_TRUE(GetAttributes(newDir2) != INVALID_FILE_ATTRIBUTES);
		dirTable.ParseDirectoryTable(session.GetDirectoryTableSize());
		CHECK_TRUE(dirTable.EntryExists(newDir2) == DirectoryTable::Exists_Yes);

		return true;
	}

	bool TestRegisterChanges(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, CheckAttributes);
	}

	bool TestRegisterChangesRemote(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunRemote(logger, testRootDir, CheckAttributes);
	}

	bool TestSharedReservedMemory(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				StringBuffer<MaxPath> testApp;
				GetTestAppPath(logger, testApp);

				ProcessStartInfo processInfo;
				processInfo.application = testApp.data;
				processInfo.workingDir = workingDir;
				processInfo.arguments = TC("-sleep=100000");
				Vector<ProcessHandle> processes;

				for (u32 i=0; i!=128; ++i)
					processes.push_back(runProcess(processInfo));

				for (auto& process : processes)
				{
					if (!process.WaitForExit(100000))
						return logger.Error(TC("UbaTestApp did not exit in 10 seconds"));
					u32 exitCode = process.GetExitCode();
					if (exitCode != 0)
						return false;
				}

				return true;
			});
	}


	bool TestRemoteDirectoryTable(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
#if 0
		return SetupServerClientSession(logger, testRootDir, true, true,
			[&](LoggerWithWriter& logger, const StringView& workingDir, SessionServer& sessionServer, SessionClient& sessionClient)
			{
				u32 attributes;
				#if PLATFORM_WINDOWS
				CHECK_TRUE(sessionClient.Exists(TCV("c:\\"), attributes));
				CHECK_TRUE(sessionClient.Exists(TCV("c:\\windows"), attributes));
				CHECK_TRUE(IsDirectory(attributes));
				CHECK_TRUE(!sessionClient.Exists(TCV("q:\\"), attributes))
				CHECK_TRUE(!sessionClient.Exists(TCV("r:\\foo"), attributes))
				#else
				CHECK_TRUE(!sessionClient.Exists(TCV("/ergewrgergreg"), attributes));
				CHECK_TRUE(!sessionClient.Exists(TCV("/ergergreg/h5r6tyh"), attributes));
				#endif
				return true;
			});
#else
		return true;
#endif
	}

}
