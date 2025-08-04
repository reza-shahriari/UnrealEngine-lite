// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaFile.h"
#include "UbaNetworkServer.h"
#include "UbaScheduler.h"
#include "UbaSessionServer.h"
#include "UbaTest.h"

namespace uba
{
	bool TestLocalSchedule(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				SchedulerCreateInfo info(session);
				Scheduler scheduler(info);

				ProcessStartInfo processInfo;
				processInfo.application = GetSystemApplication();
				processInfo.workingDir = workingDir;
				processInfo.arguments = GetSystemArguments();

				EnqueueProcessInfo epi(processInfo);
				scheduler.EnqueueProcess(epi);
				scheduler.Start();

				u32 queued, activeLocal, activeRemote, finished;
				do { scheduler.GetStats(queued, activeLocal, activeRemote, finished); } while (finished != 1);

				scheduler.Stop();
				return true;
			});
	}

	bool TestLocalScheduleReuse(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunLocal(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				SchedulerCreateInfo info(session);
				info.enableProcessReuse = true;
				Scheduler scheduler(info);

				StringBuffer<> testApp;
				GetTestAppPath(logger, testApp);

				ProcessStartInfo processInfo;
				processInfo.application = testApp.data;
				processInfo.workingDir = workingDir;
				processInfo.arguments = TC("-reuse");

				EnqueueProcessInfo epi(processInfo);
				scheduler.EnqueueProcess(epi);
				scheduler.Start();

				u32 queued, activeLocal, activeRemote, finished;
				do { scheduler.GetStats(queued, activeLocal, activeRemote, finished); } while (finished != 1);

				scheduler.Stop();
				return true;
			});
	}

	bool TestRemoteScheduleReuse(LoggerWithWriter& logger, const StringBufferBase& testRootDir)
	{
		return RunRemote(logger, testRootDir, [](LoggerWithWriter& logger, SessionServer& session, const tchar* workingDir, const RunProcessFunction& runProcess)
			{
				SchedulerCreateInfo info(session);
				info.enableProcessReuse = true;
				info.maxLocalProcessors = 0;
				Scheduler scheduler(info);

				StringBuffer<> testApp;
				GetTestAppPath(logger, testApp);

				ProcessStartInfo processInfo;
				processInfo.application = testApp.data;
				processInfo.workingDir = workingDir;
				processInfo.arguments = TC("-reuse");

				EnqueueProcessInfo epi(processInfo);
				scheduler.EnqueueProcess(epi);
				scheduler.Start();

				u32 queued, activeLocal, activeRemote, finished;
				do { scheduler.GetStats(queued, activeLocal, activeRemote, finished); } while (finished != 1);

				session.GetServer().DisconnectClients(); // Must make sure all is disconnected since scheduler goes out of scope

				scheduler.Stop();
				return true;
			});
	}
}
