// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

class FHierarchyTableAnimationEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	
	virtual void ShutdownModule() override;

private:
	TArray<TWeakObjectPtr<UScriptStruct>> BuiltinTableTypes;
	TArray<TWeakObjectPtr<UScriptStruct>> BuiltinElementTypes;
};