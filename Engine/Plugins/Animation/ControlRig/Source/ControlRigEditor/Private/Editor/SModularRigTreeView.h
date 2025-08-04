// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Views/STreeView.h"
#include "ModularRig.h"
#include "Editor/SRigHierarchyTreeView.h"

class SSearchBox;
class SModularRigTreeView;
class SModularRigModelItem;
class FModularRigTreeElement;


DECLARE_DELEGATE_RetVal(const UModularRig*, FOnGetModularRigTreeRig);
DECLARE_DELEGATE_OneParam(FOnModularRigTreeRequestDetailsInspection, const FName&);
DECLARE_DELEGATE_RetVal_TwoParams(FName, FOnModularRigTreeRenameElement, const FName& /*OldModuleName*/, const FName& /*NewName*/);
DECLARE_DELEGATE_TwoParams(FOnModularRigTreeResolveConnector, const FRigElementKey& /*Connector*/, const TArray<FRigElementKey>& /*Targets*/);
DECLARE_DELEGATE_OneParam(FOnModularRigTreeDisconnectConnector, const FRigElementKey& /*Connector*/);
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FOnModularRigTreeVerifyElementNameChanged, const FName& /*OldModuleName*/, const FName& /*NewName*/, FText& /*OutErrorMessage*/);
DECLARE_DELEGATE_RetVal_OneParam(bool, FOnModularRigTreeAlwaysShowConnector, const FName& /*ConnectorName*/);

typedef STreeView<TSharedPtr<FModularRigTreeElement>>::FOnMouseButtonClick FOnModularRigTreeMouseButtonClick;
typedef STreeView<TSharedPtr<FModularRigTreeElement>>::FOnMouseButtonDoubleClick FOnModularRigTreeMouseButtonDoubleClick;
typedef STableRow<TSharedPtr<FModularRigTreeElement>>::FOnCanAcceptDrop FOnModularRigTreeCanAcceptDrop;
typedef STableRow<TSharedPtr<FModularRigTreeElement>>::FOnAcceptDrop FOnModularRigTreeAcceptDrop;
typedef STreeView<TSharedPtr<FModularRigTreeElement>>::FOnSelectionChanged FOnModularRigTreeSelectionChanged;

struct CONTROLRIGEDITOR_API FModularRigTreeDelegates
{
public:
	
	FOnGetModularRigTreeRig OnGetModularRig;
	FOnModularRigTreeMouseButtonClick OnMouseButtonClick;
	FOnModularRigTreeMouseButtonDoubleClick OnMouseButtonDoubleClick;
	FOnDragDetected OnDragDetected;
	FOnModularRigTreeCanAcceptDrop OnCanAcceptDrop;
	FOnModularRigTreeAcceptDrop OnAcceptDrop;
	FOnContextMenuOpening OnContextMenuOpening;
	FOnModularRigTreeRequestDetailsInspection OnRequestDetailsInspection;
	FOnModularRigTreeRenameElement OnRenameElement;
	FOnModularRigTreeVerifyElementNameChanged OnVerifyModuleNameChanged;
	FOnModularRigTreeResolveConnector OnResolveConnector;
	FOnModularRigTreeDisconnectConnector OnDisconnectConnector;
	FOnModularRigTreeSelectionChanged OnSelectionChanged;
	FOnModularRigTreeAlwaysShowConnector OnAlwaysShowConnector;
	
	FModularRigTreeDelegates()
	{
	}

	const UModularRig* GetModularRig() const
	{
		if(OnGetModularRig.IsBound())
		{
			return OnGetModularRig.Execute();
		}
		return nullptr;
	}

	FName HandleRenameElement(const FName& OldModuleName, const FName& NewName) const
	{
		if(OnRenameElement.IsBound())
		{
			return OnRenameElement.Execute(OldModuleName, NewName);
		}
		return OldModuleName;
	}

	bool HandleVerifyElementNameChanged(const FName& OldModuleName, const FName& NewName, FText& OutErrorMessage) const
	{
		if(OnVerifyModuleNameChanged.IsBound())
		{
			return OnVerifyModuleNameChanged.Execute(OldModuleName, NewName, OutErrorMessage);
		}
		return false;
	}

	bool HandleResolveConnector(const FRigElementKey& InConnector, const TArray<FRigElementKey>& InTargets)
	{
		if(OnResolveConnector.IsBound())
		{
			OnResolveConnector.Execute(InConnector, InTargets);
			return true;
		}
		return false;
	}

	bool HandleDisconnectConnector(const FRigElementKey& InConnector)
	{
		if(OnDisconnectConnector.IsBound())
		{
			OnDisconnectConnector.Execute(InConnector);
			return true;
		}
		return false;
	}

	void HandleSelectionChanged(TSharedPtr<FModularRigTreeElement> Selection, ESelectInfo::Type SelectInfo)
	{
		if(bSuspendSelectionDelegate)
		{
			return;
		}
		TGuardValue<bool> Guard(bSuspendSelectionDelegate, true);
		(void)OnSelectionChanged.ExecuteIfBound(Selection, SelectInfo);
	}

	bool ShouldAlwaysShowConnector(const FName& InConnectorName) const
	{
		if(OnAlwaysShowConnector.IsBound())
		{
			return OnAlwaysShowConnector.Execute(InConnectorName);
		}
		return false;
	}

private:

	bool bSuspendSelectionDelegate = false;

	friend class SModularRigTreeView;
};


/** An item in the tree */
class FModularRigTreeElement : public TSharedFromThis<FModularRigTreeElement>
{
public:
	FModularRigTreeElement(const FString& InKey, TWeakPtr<SModularRigTreeView> InTreeView, bool bInIsPrimary);

public:
	/** Element Data to display */
	FString Key;
	bool bIsPrimary;
	FName ModuleName;
	FString ConnectorName;
	FName ShortName;
	TArray<TSharedPtr<FModularRigTreeElement>> Children;

	TSharedRef<ITableRow> MakeTreeRowWidget(const TSharedRef<STableViewBase>& InOwnerTable, TSharedRef<FModularRigTreeElement> InRigTreeElement, TSharedPtr<SModularRigTreeView> InTreeView, bool bPinned);

	void RequestRename();

	void RefreshDisplaySettings(const UModularRig* InModularRig);

	TPair<const FSlateBrush*, FSlateColor> GetBrushAndColor(const UModularRig* InModularRig);

	/** Delegate for when the context menu requests a rename */
	DECLARE_DELEGATE(FOnRenameRequested);
	FOnRenameRequested OnRenameRequested;

	static TMap<FSoftObjectPath, TSharedPtr<FSlateBrush>> IconPathToBrush;

	/** The brush to use when rendering an icon */
	const FSlateBrush* IconBrush;

	/** The color to use when rendering an icon */
	FSlateColor IconColor;

	/** The color to use when rendering the label text */
	FSlateColor TextColor;
};

class SModularRigModelItem : public SMultiColumnTableRow<TSharedPtr<FModularRigTreeElement>>
{
public:
	
	void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& OwnerTable, TSharedRef<FModularRigTreeElement> InRigTreeElement, TSharedPtr<SModularRigTreeView> InTreeView, bool bPinned);
	bool OnConnectorTargetChanged(TArray<FRigElementKey> InTargets, const FRigElementKey InConnectorKey);

	void OnNameCommitted(const FText& InText, ETextCommit::Type InCommitType) const;
	bool OnVerifyNameChanged(const FText& InText, FText& OutErrorMessage);

	virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& ColumnName) override;

private:
	TWeakPtr<FModularRigTreeElement> WeakRigTreeElement;
 	FModularRigTreeDelegates Delegates;
	TSharedPtr<SSearchableRigHierarchyTreeView> ConnectorComboBox;
	TSharedPtr<SButton> ResetConnectorButton;
	TSharedPtr<SButton> UseSelectedButton;
	TSharedPtr<SButton> SelectElementButton;
	FRigElementKey ConnectorKey;
	TOptional<FModularRigResolveResult> ConnectorMatches;

	FText GetName(bool bUseShortName) const;
	FText GetItemTooltip() const;
	TArray<FRigElementKey> GetTargetKeys() const;

	friend class SModularRigTreeView; 
};

class SModularRigTreeView : public STreeView<TSharedPtr<FModularRigTreeElement>>
{
public:

	static const FName Column_Module;
	static const FName Column_Tags;
	static const FName Column_Connector;
	static const FName Column_Buttons;

	SLATE_BEGIN_ARGS(SModularRigTreeView)
		: _AutoScrollEnabled(false)
		, _FilterText()
		, _ShowSecondaryConnectors(false)
		, _ShowOptionalConnectors(false)
		, _ShowUnresolvedConnectors(true)
	{}
		SLATE_ARGUMENT( TSharedPtr<SHeaderRow>, HeaderRow )
		SLATE_ARGUMENT(FModularRigTreeDelegates, RigTreeDelegates)
		SLATE_ARGUMENT(bool, AutoScrollEnabled)
		SLATE_ATTRIBUTE(FText, FilterText)
		SLATE_ATTRIBUTE(bool, ShowSecondaryConnectors)
		SLATE_ATTRIBUTE(bool, ShowOptionalConnectors)
		SLATE_ATTRIBUTE(bool, ShowUnresolvedConnectors)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SModularRigTreeView() {}

	/** Performs auto scroll */
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	bool bRequestRenameSelected = false;

	/** Save a snapshot of the internal map that tracks item expansion before tree reconstruction */
	void SaveAndClearSparseItemInfos()
	{
		// Only save the info if there is something to save (do not overwrite info with an empty map)
		if (!SparseItemInfos.IsEmpty())
		{
			OldSparseItemInfos = SparseItemInfos;
		}
		ClearExpandedItems();
	}

	/** Restore the expansion infos map from the saved snapshot after tree reconstruction */
	void RestoreSparseItemInfos(TSharedPtr<FModularRigTreeElement> ItemPtr)
	{
		for (const auto& Pair : OldSparseItemInfos)
		{
			if (Pair.Key->Key == ItemPtr->Key)
			{
				// the SparseItemInfos now reference the new element, but keep the same expansion state
				SparseItemInfos.Add(ItemPtr, Pair.Value);
				break;
			}
		}
	}


	TSharedPtr<FModularRigTreeElement> FindElement(const FString& InElementKey);
	static TSharedPtr<FModularRigTreeElement> FindElement(const FString& InElementKey, TSharedPtr<FModularRigTreeElement> CurrentItem);
	bool AddElement(FString InKey, FString InParentKey = FString(), bool bApplyFilterText = true);
	bool AddElement(const FRigModuleInstance* InElement, bool bApplyFilterText);
	void AddSpacerElement();
	bool ReparentElement(const FString InKey, const FString InParentKey);
	void RefreshTreeView(bool bRebuildContent = true);
	TSharedRef<ITableRow> MakeTableRowWidget(TSharedPtr<FModularRigTreeElement> InItem, const TSharedRef<STableViewBase>& OwnerTable, bool bPinned);
	void HandleGetChildrenForTree(TSharedPtr<FModularRigTreeElement> InItem, TArray<TSharedPtr<FModularRigTreeElement>>& OutChildren);

	TArray<FName> GetSelectedModuleNames() const;
	void SetSelection(const TArray<TSharedPtr<FModularRigTreeElement>>& InSelection);
	const TArray<TSharedPtr<FModularRigTreeElement>>& GetRootElements() const { return RootElements; }
	FModularRigTreeDelegates& GetRigTreeDelegates() { return Delegates; }

	/** Given a position, return the item under that position. If nothing is there, return null. */
	const TSharedPtr<FModularRigTreeElement>* FindItemAtPosition(FVector2D InScreenSpacePosition) const;

private:

	/** A temporary snapshot of the SparseItemInfos in STreeView, used during RefreshTreeView() */
	TSparseItemMap OldSparseItemInfos;

	/** Backing array for tree view */
	TArray<TSharedPtr<FModularRigTreeElement>> RootElements;
	
	/** A map for looking up items based on their key */
	TMap<FString, TSharedPtr<FModularRigTreeElement>> ElementMap;

	/** A map for looking up a parent based on their key */
	TMap<FString, FString> ParentMap;

	FModularRigTreeDelegates Delegates;

	bool bAutoScrollEnabled;
	FVector2D LastMousePosition;
	double TimeAtMousePosition;

	TAttribute<FText> FilterText;
	TAttribute<bool> ShowSecondaryConnectors;
	TAttribute<bool> ShowOptionalConnectors;
	TAttribute<bool> ShowUnresolvedConnectors;

	friend class SModularRigModel;
};

class SSearchableModularRigTreeView : public SCompoundWidget
{
public:

	SLATE_BEGIN_ARGS(SSearchableModularRigTreeView) {}
		SLATE_ARGUMENT(FModularRigTreeDelegates, RigTreeDelegates)
		SLATE_ARGUMENT(FText, InitialFilterText)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SSearchableModularRigTreeView() {}
	TSharedRef<SModularRigTreeView> GetTreeView() const { return TreeView.ToSharedRef(); }

private:

	TSharedPtr<SModularRigTreeView> TreeView;
};
