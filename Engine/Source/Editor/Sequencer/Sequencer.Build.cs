// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Sequencer : ModuleRules
{
	public Sequencer(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[] {
				"TimeManagement",
				}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"AppFramework", 
				"ApplicationCore",
				"AssetRegistry",
				"CinematicCamera",
				"ContentBrowser",
				"Core", 
				"CoreUObject", 
				"CurveEditor",
				"InputCore",
				"Engine", 
				"Slate", 
				"SlateCore",
				"SceneOutliner",
				"SequencerCore",
				"EditorStyle",
				"EditorFramework",
				"UnrealEd", 
				"MovieScene", 
				"MovieSceneTracks", 
				"MovieSceneTools", 
				"MovieSceneCapture", 
				"MovieSceneCaptureDialog", 
				"EditorWidgets", 
				"SequencerWidgets",
				"BlueprintGraph",
				"LevelSequence",
				"GraphEditor",
				"PropertyEditor",
				"ViewportInteraction",
				"SerializedRecorderInterface",
				"SubobjectDataInterface",
				"ToolMenus",
				"ToolWidgets",
				"TypedElementFramework",
				"TypedElementRuntime",
				"UniversalObjectLocator",
				"UniversalObjectLocatorEditor",
				"RenderCore"
				}
			);

		CircularlyReferencedDependentModules.AddRange(
			new string[] {
				"ViewportInteraction",
				"MovieSceneCaptureDialog",
				"UniversalObjectLocatorEditor",
				}
			);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
				"LevelEditor",
				"DesktopPlatform",
				}
			);

		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"PropertyEditor",
				"SceneOutliner",
				"CurveEditor",
				"Analytics",
				"SequencerWidgets"
				}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[] {
				"LevelEditor",
				"MainFrame",
				}
			);

		CircularlyReferencedDependentModules.Add("MovieSceneTools");
	}
}
