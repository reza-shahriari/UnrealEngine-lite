// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Modules/ModuleInterface.h"
#include "MuCO/ICustomizableObjectEditorModule.h"


/** Interface only accessible from the Customizable Object Editor module.
 *
 * TODO Move to private. Currently can not be moved due to MutableValidation module. */
class ICustomizableObjectEditorModulePrivate : public ICustomizableObjectEditorModule
{
public:
	static ICustomizableObjectEditorModulePrivate* Get()
	{
		// Prevent access to this module if the game is being played (in Standalone mode for example)
		if (IsRunningGame())
		{
			return nullptr;
		}
		
		return FModuleManager::LoadModulePtr<ICustomizableObjectEditorModulePrivate>(MODULE_NAME_COE);
	}
	
	static ICustomizableObjectEditorModulePrivate& GetChecked()
	{
		check(!IsRunningGame()) // This module is editor-only. DO NOT try to access it during gameplay
		return FModuleManager::LoadModuleChecked<ICustomizableObjectEditorModulePrivate>(MODULE_NAME_COE);
	}
	
	virtual void EnqueueCompileRequest(const TSharedRef<FCompilationRequest>& InCompilationRequest, bool bForceRequest = false) = 0;
};