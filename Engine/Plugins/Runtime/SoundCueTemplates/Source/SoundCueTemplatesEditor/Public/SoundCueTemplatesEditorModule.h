// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"
#include "Modules/ModuleInterface.h"

SOUNDCUETEMPLATESEDITOR_API DECLARE_LOG_CATEGORY_EXTERN(SoundCueTemplatesEditor, Log, All);

class FSoundCueTemplatesEditorModule :	public IModuleInterface
{
public:
	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
