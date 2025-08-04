// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "Editor/SModularRigTreeView.h"
#include "ControlRigBlueprint.h"
#include "DragAndDrop/GraphNodeDragDropOp.h"
#include "Editor/RigVMNewEditor.h"
#include "ControlRigDragOps.h"

class SModularRigModel;
class IControlRigBaseEditor;
class SSearchBox;
class FUICommandList;
class URigVMBlueprint;
class UControlRig;
struct FAssetData;
class FMenuBuilder;
class UToolMenu;
struct FToolMenuContext;

/** Widget allowing editing of a control rig's structure */
class SModularRigModel : public SCompoundWidget, public FEditorUndoClient
{
public:
	SLATE_BEGIN_ARGS(SModularRigModel) {}
	SLATE_END_ARGS()

	~SModularRigModel();

	void Construct(const FArguments& InArgs, TSharedRef<IControlRigBaseEditor> InControlRigEditor);

	IControlRigBaseEditor* GetControlRigEditor() const
	{
		if(ControlRigEditor.IsValid())
		{
			return ControlRigEditor.Pin().Get();
		}
		return nullptr;
	}

private:

	void OnEditorClose(IControlRigBaseEditor* InEditor, UControlRigBlueprint* InBlueprint);

	/** Bind commands that this widget handles */
	void BindCommands();

	/** SWidget interface */
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent);
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;

	/** Rebuild the tree view */
	void RefreshTreeView(bool bRebuildContent = true);

	/** Returns all selected items */
	TArray<TSharedPtr<FModularRigTreeElement>> GetSelectedItems() const;

	/** Return all selected keys */
	TArray<FString> GetSelectedKeys() const;

	/** Create a new item */
	void HandleNewItem();
	void HandleNewItem(UClass* InClass, const FName& InParentModuleName);

	/** Rename item */
	bool CanRenameModule() const;
	void HandleRenameModule();
	FName HandleRenameModule(const FName& InOldModuleName, const FName& InNewName);
	bool HandleVerifyNameChanged(const FName& InOldModuleName, const FName& InNewName, FText& OutErrorMessage);

	/** Delete items */
	void HandleDeleteModules();
	void HandleDeleteModules(const TArray<FName>& InModuleNames);

	/** Reparent items */
	void HandleReparentModules(const TArray<FName>& InModuleNames, const FName& InParentModuleName);

	/** Mirror items */
	void HandleMirrorModules();
	void HandleMirrorModules(const TArray<FName>& InModuleNames);

	/** Reresolve items */
	void HandleReresolveModules();
	void HandleReresolveModules(const TArray<FName>& InModuleNames);

	/** Swap module class for items */
	bool CanSwapModules() const;
	void HandleSwapClassForModules();
	void HandleSwapClassForModules(const TArray<FName>& InModuleNames);

	/** Copy & paste module settings */
	bool CanCopyModuleSettings() const;
	void HandleCopyModuleSettings();
	bool CanPasteModuleSettings() const;
	void HandlePasteModuleSettings();

	/** Resolve connector */
	void HandleConnectorResolved(const FRigElementKey& InConnector, const TArray<FRigElementKey>& InTargets);

	/** UnResolve connector */
	void HandleConnectorDisconnect(const FRigElementKey& InConnector);

	/** Set Selection Changed */
	void HandleSelectionChanged(TSharedPtr<FModularRigTreeElement> Selection, ESelectInfo::Type SelectInfo);

	/** Returns true if a given connector should always be shown */
	bool ShouldAlwaysShowConnector(const FName& InConnectorName) const;

	TSharedPtr< SWidget > CreateContextMenuWidget();
	void OnItemClicked(TSharedPtr<FModularRigTreeElement> InItem);
	void OnItemDoubleClicked(TSharedPtr<FModularRigTreeElement> InItem);
	
	// FEditorUndoClient
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

	// reply to a drag operation
	FReply OnDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);

	// reply to a drop operation on item
	TOptional<EItemDropZone> OnCanAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone DropZone, TSharedPtr<FModularRigTreeElement> TargetItem);
	FReply OnAcceptDrop(const FDragDropEvent& DragDropEvent, EItemDropZone DropZone, TSharedPtr<FModularRigTreeElement> TargetItem);

	// SWidget Overrides
	virtual FReply OnDrop(const FGeometry& MyGeometry, const FDragDropEvent& DragDropEvent) override;

	static const FName ContextMenuName;
	static void CreateContextMenu();
	UToolMenu* GetContextMenu();
	TSharedPtr<FUICommandList> GetContextMenuCommands() const;
	
	/** Our owning control rig editor */
	TWeakPtr<IControlRigBaseEditor> ControlRigEditor;

	/** Tree view widget */
	TSharedPtr<SModularRigTreeView> TreeView;
	TSharedPtr<SHeaderRow> HeaderRowWidget;

	TWeakObjectPtr<UControlRigBlueprint> ControlRigBlueprint;
	TWeakObjectPtr<UModularRig> ControlRigBeingDebuggedPtr;
	
	/** Command list we bind to */
	TSharedPtr<FUICommandList> CommandList;

	bool IsSingleSelected() const;
	
	UModularRig* GetModularRig() const;
	UModularRig* GetDefaultModularRig() const;
	const UModularRig* GetModularRigForTreeView() const { return GetModularRig(); }
	void OnRequestDetailsInspection(const FName& InModuleName);
	void HandlePreCompileModularRigs(URigVMBlueprint* InBlueprint);
	void HandlePostCompileModularRigs(URigVMBlueprint* InBlueprint);
	void OnModularRigModified(EModularRigNotification InNotif, const FRigModuleReference* InModule);
	void OnHierarchyModified(ERigHierarchyNotification InNotif, URigHierarchy* InHierarchy, const FRigNotificationSubject& InSubject);

	void HandleRefreshEditorFromBlueprint(URigVMBlueprint* InBlueprint);
	void HandleSetObjectBeingDebugged(UObject* InObject);

	TSharedRef<SWidget> OnGetOptionsMenu();
	void OnFilterTextChanged(const FText& SearchText);

	bool bShowSecondaryConnectors;
	bool bShowOptionalConnectors;
	bool bShowUnresolvedConnectors;
	FText FilterText;

	TSharedPtr<SSearchBox> FilterBox;
	bool bIsPerformingSelection;
	bool bKeepCurrentEditedConnectors;
	TSet<FName> CurrentlyEditedConnectors;

public:

	friend class FModularRigTreeElement;
	friend class SModularRigModelItem;
	friend class UControlRigBlueprintEditorLibrary;
};

