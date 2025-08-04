// Copyright Epic Games, Inc. All Rights Reserved.

#include "Text3DModule.h"

#include "Logs/Text3DLogs.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY(LogText3D);

IMPLEMENT_MODULE(FText3DModule, Text3D);

FText3DModule::FText3DModule()
{
	FreeTypeLib = nullptr;
}

void FText3DModule::StartupModule()
{
	FT_Init_FreeType(&FreeTypeLib);
}

void FText3DModule::ShutdownModule()
{
	FT_Done_FreeType(FreeTypeLib);
	FreeTypeLib = nullptr;
}

FT_Library FText3DModule::GetFreeTypeLibrary()
{
	const FText3DModule& Instance = FModuleManager::LoadModuleChecked<FText3DModule>("Text3D");
	return Instance.FreeTypeLib;
}