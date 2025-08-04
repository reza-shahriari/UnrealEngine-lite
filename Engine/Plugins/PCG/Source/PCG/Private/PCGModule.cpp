// Copyright Epic Games, Inc. All Rights Reserved.

#include "PCGModule.h"

#include "PCGContext.h"
#include "PCGElement.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGPointData.h"
#include "Data/PCGSplineData.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
#include "Elements/PCGDifferenceElement.h"
#include "Tests/Determinism/PCGDeterminismNativeTests.h"
#include "Tests/Determinism/PCGDifferenceDeterminismTest.h"

#include "ISettingsModule.h"
#include "ShaderCore.h"
#include "ShowFlags.h"
#include "Misc/Paths.h"
#endif

#define LOCTEXT_NAMESPACE "FPCGModule"

namespace PCGModule
{
	FPCGModule* PCGModulePtr = nullptr;
}

FPCGModule& FPCGModule::GetPCGModuleChecked()
{
	return PCGModule::PCGModulePtr ? *PCGModule::PCGModulePtr : FModuleManager::GetModuleChecked<FPCGModule>(TEXT("PCG"));
}

bool FPCGModule::IsPCGModuleLoaded()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("PCG"));
}

void FPCGModule::StartupModule()
{
#if WITH_EDITOR
	PCGDeterminismTests::FNativeTestRegistry::Create();

	RegisterNativeElementDeterminismTests();

	FEngineShowFlags::RegisterCustomShowFlag(PCGEngineShowFlags::Debug, /*DefaultEnabled=*/true, EShowFlagGroup::SFG_Developer, LOCTEXT("ShowFlagDisplayName", "PCG Debug"));

	const FString PluginShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("PCG"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/PCG"), PluginShaderDir);
#endif // WITH_EDITOR

	// Cache for fast access
	check(!PCGModule::PCGModulePtr);
	PCGModule::PCGModulePtr = this;

	// Registering accessor methods
	AttributeAccessorFactory.RegisterDefaultMethods();
	AttributeAccessorFactory.RegisterMethods<UPCGBasePointData>(UPCGBasePointData::GetPointAccessorMethods());
	// @todo_pcg: Eventually remove the UPCGPointData method registration because the UPCGBasePointData accessors are compatible
	AttributeAccessorFactory.RegisterMethods<UPCGPointData>(UPCGPointData::GetPointAccessorMethods());
	AttributeAccessorFactory.RegisterMethods<UPCGSplineData>(UPCGSplineData::GetSplineAccessorMethods());

	// Register onto the PreExit, because we need the class to be still valid to remove them from the mapping
	FCoreDelegates::OnPreExit.AddRaw(this, &FPCGModule::PreExit);

	TickDelegateHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FPCGModule::Tick));
}

void FPCGModule::ShutdownModule()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickDelegateHandle);

	FCoreDelegates::OnPreExit.RemoveAll(this);

	PCGModule::PCGModulePtr = nullptr;
}

void FPCGModule::PreExit()
{
	// Unregistering accessor methods
	AttributeAccessorFactory.UnregisterMethods<UPCGSplineData>();
	AttributeAccessorFactory.UnregisterMethods<UPCGPointData>();
	AttributeAccessorFactory.UnregisterDefaultMethods();
	AttributeAccessorFactory.UnregisterMethods<UPCGBasePointData>();

#if WITH_EDITOR
	DeregisterNativeElementDeterminismTests();

	PCGDeterminismTests::FNativeTestRegistry::Destroy();
#endif // WITH_EDITOR
}

void FPCGModule::ExecuteNextTick(TFunction<void()> TickFunction)
{
	FScopeLock Lock(&ExecuteNextTickLock);
	ExecuteNextTicks.Add(TickFunction);
}

bool FPCGModule::Tick(float DeltaTime)
{
	TArray<TFunction<void()>> LocalExecuteNextTicks;

	{
		FScopeLock Lock(&ExecuteNextTickLock);
		LocalExecuteNextTicks = MoveTemp(ExecuteNextTicks);
	}

	for (TFunction<void()>& LocalExecuteNextTick : LocalExecuteNextTicks)
	{
		LocalExecuteNextTick();
	}

	return true;
}

#if WITH_EDITOR
void FPCGModule::RegisterNativeElementDeterminismTests()
{
	PCGDeterminismTests::FNativeTestRegistry::RegisterTestFunction(UPCGDifferenceSettings::StaticClass(), PCGDeterminismTests::DifferenceElement::RunTestSuite);
	// TODO: Add other native test functions
}

void FPCGModule::DeregisterNativeElementDeterminismTests()
{
	PCGDeterminismTests::FNativeTestRegistry::DeregisterTestFunction(UPCGDifferenceSettings::StaticClass());
}
#endif // WITH_EDITOR

IMPLEMENT_MODULE(FPCGModule, PCG);

PCG_API DEFINE_LOG_CATEGORY(LogPCG);

void PCGLog::LogErrorOnGraph(const FText& InMsg, const FPCGContext* InContext)
{
	if (InContext)
	{
		PCGE_LOG_C(Error, GraphAndLog, InContext, InMsg);
	}
	else
	{
		UE_LOG(LogPCG, Error, TEXT("%s"), *InMsg.ToString());
	}
}

void PCGLog::LogWarningOnGraph(const FText& InMsg, const FPCGContext* InContext)
{
	if (InContext)
	{
		PCGE_LOG_C(Warning, GraphAndLog, InContext, InMsg);
	}
	else
	{
		UE_LOG(LogPCG, Warning, TEXT("%s"), *InMsg.ToString());
	}
}

#undef LOCTEXT_NAMESPACE
