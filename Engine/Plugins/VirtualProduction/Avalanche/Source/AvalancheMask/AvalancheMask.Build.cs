// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class AvalancheMask : ModuleRules
{
	public AvalancheMask(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"AvalancheModifiers",
				"AvalancheShapes",
				"Core",
                "CoreUObject",
                "GeometryMask",
            });

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"ActorModifierCore",
				"ActorModifier",
				"AssetRegistry",
				"Avalanche",
				"AvalancheCore",
				"AvalancheText",
				"DeveloperSettings",
				"DynamicMaterial",
				"Engine",
				"GeometryFramework",
				"MediaPlate",
				"Slate",
				"SlateCore",
				"Text3D",
			});

        if (Target.bBuildEditor == true)
        {
            PrivateDependencyModuleNames.AddRange(
                new string[] {
	                "AssetTools",
	                "DynamicMaterialEditor",
	                "Projects",
                    "UnrealEd",
                });
        }
    }
}
