// Copyright Epic Games, Inc. All Rights Reserved.

#include "ChaosVDGenericDebugDrawSettings.h"

#include "ChaosVDSettingsManager.h"
#include "Utils/ChaosVDUserInterfaceUtils.h"


void UChaosVDGenericDebugDrawSettings::SetDataVisualizationFlags(EChaosVDGenericDebugDrawVisualizationFlags NewFlags)
{
	if (UChaosVDGenericDebugDrawSettings* Settings = FChaosVDSettingsManager::Get().GetSettingsObject<UChaosVDGenericDebugDrawSettings>())
	{
		Settings->DebugDrawFlags = static_cast<uint32>(NewFlags);
		Settings->BroadcastSettingsChanged();
	}
}

EChaosVDGenericDebugDrawVisualizationFlags UChaosVDGenericDebugDrawSettings::GetDataVisualizationFlags()
{
	if (UChaosVDGenericDebugDrawSettings* Settings = FChaosVDSettingsManager::Get().GetSettingsObject<UChaosVDGenericDebugDrawSettings>())
	{
		return static_cast<EChaosVDGenericDebugDrawVisualizationFlags>(Settings->DebugDrawFlags);
	}

	return EChaosVDGenericDebugDrawVisualizationFlags::None;
}

bool UChaosVDGenericDebugDrawSettings::CanVisualizationFlagBeChangedByUI(uint32 Flag)
{
	return Chaos::VisualDebugger::Utils::ShouldVisFlagBeEnabledInUI(Flag, DebugDrawFlags, EChaosVDGenericDebugDrawVisualizationFlags::EnableDraw);
}
