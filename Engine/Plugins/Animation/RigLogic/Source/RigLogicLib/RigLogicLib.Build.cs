// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class RigLogicLib : ModuleRules
	{
		public RigLogicLib(ReadOnlyTargetRules Target) : base(Target)
		{
			IWYUSupport = IWYUSupport.None;

			PublicDependencyModuleNames.AddRange(
				new string[]
				{
						"Core",
						"CoreUObject",
						"Engine",
						"ControlRig"
				}
			);

			if (Target.Type == TargetType.Editor)
			{
				PublicDependencyModuleNames.Add("UnrealEd");
				PublicDependencyModuleNames.Add("EditorFramework");
			}

			Type = ModuleType.CPlusPlus;

			PrivateDefinitions.Add("RL_BUILD_WITH_ZYX_ROTATION_ORDER=1");

			if (Target.LinkType != TargetLinkType.Monolithic)
			{
				PrivateDefinitions.Add("RL_BUILD_SHARED=1");
				PublicDefinitions.Add("RL_SHARED=1");
			}

			if (Target.Platform == UnrealTargetPlatform.Win64)
			{
				PrivateDefinitions.Add("TRIMD_CUSTOM_WINDOWS_H=\"WindowsPlatformUE.h\"");
				PrivateDefinitions.Add("TRIO_CUSTOM_WINDOWS_H=\"WindowsPlatformUE.h\"");
				PrivateDefinitions.Add("TRIO_WINDOWS_FILE_MAPPING_AVAILABLE=1");
			}

			if (Target.Platform == UnrealTargetPlatform.Linux || Target.Platform == UnrealTargetPlatform.LinuxArm64)
			{
				PrivateDefinitions.Add("TRIO_MREMAP_AVAILABLE=1");
			}

			if (Target.Platform == UnrealTargetPlatform.Linux ||
					Target.Platform == UnrealTargetPlatform.LinuxArm64 ||
					Target.Platform == UnrealTargetPlatform.Mac)
			{
				PrivateDefinitions.Add("TRIO_MMAP_AVAILABLE=1");
			}

			PrivateDefinitions.Add("RL_AUTODETECT_SSE=1");
			PrivateDefinitions.Add("RL_AUTODETECT_HALF_FLOATS=1");
		}
	}
}
