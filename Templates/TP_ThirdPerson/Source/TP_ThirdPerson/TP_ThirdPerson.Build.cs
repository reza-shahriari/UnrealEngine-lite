// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TP_ThirdPerson : ModuleRules
{
	public TP_ThirdPerson(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"EnhancedInput",
			"AIModule",
			"StateTreeModule",
			"GameplayStateTreeModule",
			"UMG"
		});

		PrivateDependencyModuleNames.AddRange(new string[] { });

		PublicIncludePaths.AddRange(new string[] {
			"TP_ThirdPerson",
			"TP_ThirdPerson/Variant_Platforming",
			"TP_ThirdPerson/Variant_Combat",
			"TP_ThirdPerson/Variant_Combat/AI",
			"TP_ThirdPerson/Variant_SideScrolling",
			"TP_ThirdPerson/Variant_SideScrolling/Gameplay",
			"TP_ThirdPerson/Variant_SideScrolling/AI"
		});

		// Uncomment if you are using Slate UI
		// PrivateDependencyModuleNames.AddRange(new string[] { "Slate", "SlateCore" });

		// Uncomment if you are using online features
		// PrivateDependencyModuleNames.Add("OnlineSubsystem");

		// To include OnlineSubsystemSteam, add it to the plugins section in your uproject file with the Enabled attribute set to true
	}
}
