// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class ChaosOutfitAssetDataflowNodes : ModuleRules
{
	public ChaosOutfitAssetDataflowNodes(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Chaos",
				"ChaosClothAssetEngine",
				"ChaosOutfitAssetEngine",
				"DataflowCore",
				"DataflowNodes",
				"Engine",
				"MeshResizingCore",
			}
		);
	}
}
