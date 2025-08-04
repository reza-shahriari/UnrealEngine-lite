// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "IRewindDebugger.h"
#include "IRewindDebuggerExtension.h"

// Rewind debugger extension for Chooser support

class FRewindDebuggerChooser : public IRewindDebuggerExtension
{
public:
	FRewindDebuggerChooser();
	virtual ~FRewindDebuggerChooser() {};

	virtual void Update(float DeltaTime, IRewindDebugger* RewindDebugger) override;
	virtual FString GetName() { return TEXT("ChooserDebugger"); }
};
