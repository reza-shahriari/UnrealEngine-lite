// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once
#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"
#include "Toolkits/IToolkit.h"
#include "TG_Exporter.h"
class IAssetTools;
class IAssetTypeActions;
class ITG_Editor;
class UTextureGraph;
class UTextureGraphInstance;
class FTG_EditorGraphNodeFactory;
class FTG_EditorGraphPanelPinFactory;
typedef TArray<TSharedPtr<IAssetTypeActions>> AssetTypeActionsArray;
extern const FName TG_EditorAppIdentifier;
extern const FName TG_InstanceEditorAppIdentifier;

class TEXTUREGRAPHEDITOR_API FTextureGraphEditorModule : public IModuleInterface
{
private:
	/** All created asset type actions.  Cached here so that we can unregister them during shutdown. */
	AssetTypeActionsArray			CreatedAssetTypeActions;
	/** Delegate to run the Tick method in charge of running TextureGraphEngine update*/
	FTickerDelegate					TickDelegate;
	/** Handle of the delegate to run the Tick method in charge of running TextureGraphEngine update*/
	FTSTicker::FDelegateHandle		TickDelegateHandle;
	TUniquePtr<FTG_ExporterUtility>		TG_Exporter;

protected:
	TSharedPtr<FTG_EditorGraphNodeFactory> GraphNodeFactory;
	TSharedPtr<FTG_EditorGraphPanelPinFactory>	GraphPanelPinFactory;
public:
	/** IModuleInterface implementation */
	virtual void					StartupModule() override;
	virtual void					ShutdownModule() override;

	
	virtual void					StartTextureGraphEngine();
	virtual void					ShutdownTextureGraphEngine();
	bool							Tick(float deltaTime);
	void							RegisterAssetTypeAction(IAssetTools& AssetTools, TSharedRef<IAssetTypeActions> Action);
	void							UnRegisterAllAssetTypeActions();
	static TSharedRef<ITG_Editor>	CreateTextureGraphEditor(const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UTextureGraph* InTextureGraph);
	static TSharedRef<ITG_Editor>	CreateTextureGraphInstanceEditor(const EToolkitMode::Type Mode, const TSharedPtr< class IToolkitHost >& InitToolkitHost, UTextureGraphInstance* InTextureGraphInstance);

	/** Returns a reference to the Blueprint Debugger state object */
	const TUniquePtr<FTG_ExporterUtility>& GetTextureExporter() const { return TG_Exporter; }
};
