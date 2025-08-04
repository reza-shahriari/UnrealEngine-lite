// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System;

public class GeForceNOWWrapper : ModuleRules
{
	public GeForceNOWWrapper(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicSystemIncludePaths.Add(
			Target.UEThirdPartySourceDirectory + "NVIDIA/GeForceNOW/include"
		);

		StaticAnalyzerDisabledCheckers.Add("unix.Malloc");

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"GeForceNOW",
				"Slate"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"SlateCore"
			}
		);

		bool bNVGFN = Target.Type != TargetRules.TargetType.Server
				   && Target.Type != TargetRules.TargetType.Program
				   && Target.Configuration != UnrealTargetConfiguration.Unknown
				   && Target.Configuration != UnrealTargetConfiguration.Debug
				   && Target.Platform == UnrealTargetPlatform.Win64
				   && Target.Architecture != UnrealArch.Arm64;
				   
		PublicDefinitions.Add("NV_GEFORCENOW=" + (bNVGFN ? 1 : 0));
	}
}
