// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/OnlineServices.h"
#include "Online/OnlineBase.h"

#include "Online/OnlineServicesLog.h"
#include "Online/OnlineServicesRegistry.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"

DEFINE_LOG_CATEGORY(LogOnlineServices);

namespace UE::Online {

int32 GetBuildUniqueId()
{
	static bool bStaticCheck = false;
	static int32 BuildId = 0;
	static bool bUseBuildIdOverride = false;
	static int32 BuildIdOverride = 0;

	if (!bStaticCheck)
	{
		bStaticCheck = true;
		FString BuildIdOverrideCommandLineString;
		if (FParse::Value(FCommandLine::Get(), TEXT("BuildIdOverride="), BuildIdOverrideCommandLineString))
		{
			BuildIdOverride = FCString::Atoi(*BuildIdOverrideCommandLineString);
		}
		if (BuildIdOverride != 0)
		{
			bUseBuildIdOverride = true;
		}
		else
		{
			if (!GConfig->GetBool(TEXT("OnlineServices"), TEXT("bUseBuildIdOverride"), bUseBuildIdOverride, GEngineIni))
			{
				UE_LOG(LogOnlineServices, Warning, TEXT("Missing bUseBuildIdOverride= in [OnlineServices] of DefaultEngine.ini"));
			}

			if (!GConfig->GetInt(TEXT("OnlineServices"), TEXT("BuildIdOverride"), BuildIdOverride, GEngineIni))
			{
				UE_LOG(LogOnlineServices, Warning, TEXT("Missing BuildIdOverride= in [OnlineServices] of DefaultEngine.ini"));
			}
		}

		if (bUseBuildIdOverride == false)
		{
			// Removed old hashing code to use something more predictable and easier to override for when
			// it's necessary to force compatibility with an older build
			BuildId = FNetworkVersion::GetNetworkCompatibleChangelist();
		}
		else
		{
			BuildId = BuildIdOverride;
		}
		
		// use a cvar so it can be modified at runtime
		TAutoConsoleVariable<int32>& CVarBuildIdOverride = GetBuildIdOverrideCVar();
		CVarBuildIdOverride->Set(BuildId);
	}

	return GetBuildIdOverrideCVar()->GetInt();
}

bool IsLoaded(EOnlineServices OnlineServices, FName InstanceName, FName InstanceConfigName)
{
	return FOnlineServicesRegistry::Get().IsLoaded(OnlineServices, InstanceName, InstanceConfigName);
}

TSharedPtr<IOnlineServices> GetServices(EOnlineServices OnlineServices, FName InstanceName, FName InstanceConfigName)
{
	return FOnlineServicesRegistry::Get().GetNamedServicesInstance(OnlineServices, InstanceName, InstanceConfigName);
}

void DestroyService(EOnlineServices OnlineServices, FName InstanceName, FName InstanceConfigName)
{
	FOnlineServicesRegistry::Get().DestroyNamedServicesInstance(OnlineServices, InstanceName, InstanceConfigName);
}

void DestroyAllNamedServices(EOnlineServices OnlineServices)
{
	FOnlineServicesRegistry::Get().DestroyAllNamedServicesInstances(OnlineServices);
}

void DestroyAllServicesWithName(FName InstanceName)
{
	FOnlineServicesRegistry::Get().DestroyAllServicesInstancesWithName(InstanceName);
}

/* UE::Online */ }
