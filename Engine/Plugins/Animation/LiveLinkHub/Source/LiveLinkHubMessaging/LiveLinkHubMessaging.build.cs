// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class LiveLinkHubMessaging : ModuleRules
	{
		public LiveLinkHubMessaging(ReadOnlyTargetRules Target) : base(Target)
		{
			PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"TimeManagement",
				"LiveLinkMessageBusFramework"
			});

			PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"LiveLink",
				"LiveLinkInterface",
				"Messaging",
				"SlateCore",
			});

			if (Target.bCompileAgainstEditor)
			{
				PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Slate",
					"UnrealEd"
				});
			}
		}
	}
}
