// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

// Nomad tab window to hold the widget with the list of Changesets, see SPlasticSourceControlChangesetsWidget
class FPlasticSourceControlChangesetsWindow
{
public:
	void Register();
	void Unregister();

	void OpenTab();

private:
	TSharedRef<class SDockTab> OnSpawnTab(const class FSpawnTabArgs& SpawnTabArgs);

	TSharedPtr<class SWidget> CreateChangesetsWidget();
};
