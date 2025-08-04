// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class MetasoundEditor : ModuleRules
	{
		public MetasoundEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateDependencyModuleNames.AddRange
			(
				new string[]
				{
					"AssetTools",
					"AppFramework",
					"AudioExtensions",
					"AudioSynesthesia",
					"ContentBrowser",
					"ContentBrowserAssetDataSource",
					"ContentBrowserData",
					"EditorWidgets",
					"Kismet",
					"KismetWidgets",
					"MetasoundEngine",
					"MetasoundFrontend",
					"MetasoundGenerator",
					"MetasoundGraphCore",
					"MetasoundStandardNodes",
					"SignalProcessing",
					"SubobjectEditor",
					"ToolMenus",
					"ToolWidgets",
					"WaveTable",
					"WaveTableEditor",
					"UMG"

				}
			);

			PublicDependencyModuleNames.AddRange
			(
				new string[]
				{
					"ApplicationCore",
					"AudioAnalyzer",
					"AudioEditor",
					"AudioMixer",
					"AudioWidgets",
					"AssetDefinition",
					"ClassViewer",
					"ContentBrowser",
					"Core",
					"CoreUObject",
					"DetailCustomizations",
					"EditorFramework",
					"EditorStyle",
					"EditorSubsystem",
					"Engine",
					"GraphEditor",
					"InputCore",
					"PropertyEditor",
					"RenderCore",
					"Slate",
					"SlateCore",
					"StructUtilsEditor",
					"UnrealEd"
				}
			);
		}
	}
}
