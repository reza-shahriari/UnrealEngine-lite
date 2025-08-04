// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class HierarchyTableRuntime : ModuleRules
{
	public HierarchyTableRuntime(ReadOnlyTargetRules Target) : base(Target)
	{
		ShortName = "HTRun";

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine"
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
            {
			}
		);
	}
}
