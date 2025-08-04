// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "Editor/RigVMEditorStyle.h"

class FRigVMEditorCommands : public TCommands<FRigVMEditorCommands>
{
public:
	FRigVMEditorCommands() : TCommands<FRigVMEditorCommands>
	(
		"RigVMBlueprint",
		NSLOCTEXT("Contexts", "RigVM", "RigVM Blueprint"),
		NAME_None, // "MainFrame" // @todo Fix this crash
		FRigVMEditorStyle::Get().GetStyleSetName() // Icon Style Set
	)
	{}

	/** Compile the blueprint */
	TSharedPtr<FUICommandInfo> Compile;
	TSharedPtr<FUICommandInfo> SaveOnCompile_Never;
	TSharedPtr<FUICommandInfo> SaveOnCompile_SuccessOnly;
	TSharedPtr<FUICommandInfo> SaveOnCompile_Always;
	TSharedPtr<FUICommandInfo> JumpToErrorNode;

	/** Edit global options */
	TSharedPtr<FUICommandInfo> EditGlobalOptions;
	TSharedPtr<FUICommandInfo> EditClassDefaults;
	
	/** Deletes the selected items and removes their nodes from the graph. */
	TSharedPtr< FUICommandInfo > DeleteItem;

	/** Toggle Execute the Graph */
	TSharedPtr< FUICommandInfo > ExecuteGraph;

	/** Toggle Auto Compilation in the Graph */
	TSharedPtr< FUICommandInfo > AutoCompileGraph;

	/** Toggle between this and the last event queue */
	TSharedPtr< FUICommandInfo > ToggleEventQueue;

	/** Toggle between Release and Debug execution mode */
	TSharedPtr< FUICommandInfo > ToggleExecutionMode;

	/** Compile and run the optimized rig, ignoring any debug data */
	TSharedPtr< FUICommandInfo > ReleaseMode;

	/** Compile and run the unoptimized rig, ignoring any debug data */
	TSharedPtr< FUICommandInfo > DebugMode;
	
	/** Resume the execution of the graph when halted at a breakpoint */
	TSharedPtr< FUICommandInfo > ResumeExecution;

	/** Focuses on the node currently being debugged */
	TSharedPtr< FUICommandInfo > ShowCurrentStatement;

	/** Steps to the next node in the execution graph (at the same graph level) when halted at a breakpoint */
	TSharedPtr< FUICommandInfo > StepOver;

	/** Steps into the collapsed/function node, when halted at a breakpoint */
	TSharedPtr< FUICommandInfo > StepInto;

	/** Steps out of the collapsed/function node, when halted at a breakpoint */
	TSharedPtr< FUICommandInfo > StepOut;

	/** Frames the selected nodes */
	TSharedPtr< FUICommandInfo > FrameSelection;

	/** Swap Function (Asset) */
	TSharedPtr< FUICommandInfo > SwapFunctionWithinAsset;

	/** Swap Function (Project) */
	TSharedPtr< FUICommandInfo > SwapFunctionAcrossProject;

	/** Swap Asset References */
	TSharedPtr< FUICommandInfo > SwapAssetReferences;

	/**
	 * Initialize commands
	 */
	virtual void RegisterCommands() override;
};
