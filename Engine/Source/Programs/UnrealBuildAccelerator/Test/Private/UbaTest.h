// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UbaStringBuffer.h"

#define CHECK_TRUE(x) \
	if (!(x)) \
		return logger.Error(TC("Failed %s (%s:%u)"), TC(#x), TC("") __FILE__, __LINE__);


namespace uba
{
	class LoggerWithWriter;
	class ProcessHandle;
	class SessionServer;
	struct ProcessStartInfo;

	using RunProcessFunction = Function<ProcessHandle(const ProcessStartInfo&)>;
	using TestSessionFunction = Function<bool(LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)>;
	bool RunLocal(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, bool enableDetour = true);
	bool RunRemote(LoggerWithWriter& logger, const StringBufferBase& testRootDir, const TestSessionFunction& testFunc, bool deleteAll = true, bool serverShouldListen = true);
	void GetTestAppPath(LoggerWithWriter& logger, StringBufferBase& out);
	const tchar* GetSystemApplication();
	const tchar* GetSystemArguments();
}