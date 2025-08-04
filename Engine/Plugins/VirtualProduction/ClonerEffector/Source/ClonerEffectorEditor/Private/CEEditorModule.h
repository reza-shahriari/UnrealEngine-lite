// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleManager.h"

class FCEEditorThrottleManager;

class FCEEditorModule : public IModuleInterface
{
public:
	//~ Begin IModuleInterface
    virtual void StartupModule() override;
    virtual void ShutdownModule() override;
	//~ End IModuleInterface

protected:
	FDelegateHandle ClonerTrackCreateEditorHandle;
	TSharedPtr<FCEEditorThrottleManager> ThrottleManager;
};
