// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "SGraphNode.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Text/STextBlock.h"
#include "RigVMModel/RigVMPin.h"
#include "RigVMBlueprint.h"
#include "Overrides/SOverrideStatusWidget.h"

class URigVMEdGraphNode;
class STableViewBase;
class SOverlay;
class SGraphPin;
class UEdGraphPin;

class RIGVMEDITOR_API SRigVMGraphNode : public SGraphNode
{
public:
	SLATE_BEGIN_ARGS(SRigVMGraphNode)
		: _GraphNodeObj(nullptr)
		{}

	SLATE_ARGUMENT(URigVMEdGraphNode*, GraphNodeObj)

	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// SGraphNode interface
	virtual TSharedRef<SWidget> CreateTitleWidget(TSharedPtr<SNodeTitle> InNodeTitle) override;

	virtual void EndUserInteraction() const override;
	virtual void MoveTo(const FVector2f& NewPosition, FNodeSet& NodeFilter, bool bMarkDirty = true) override;
	virtual void AddPin( const TSharedRef<SGraphPin>& PinToAdd ) override;
	virtual void CreateStandardPinWidget(UEdGraphPin* CurPin) override;
	virtual void SetDefaultTitleAreaWidget(TSharedRef<SOverlay> DefaultTitleAreaWidget) override
	{
		TitleAreaWidget = DefaultTitleAreaWidget;
	}
	virtual const FSlateBrush * GetNodeBodyBrush() const override;
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;

	virtual TSharedRef<SWidget> CreateNodeContentArea() override;
	virtual void GetOverlayBrushes(bool bSelected, const FVector2f& WidgetSize, TArray<FOverlayBrushInfo>& Brushes) const override;
	virtual void GetNodeInfoPopups(FNodeInfoContext* Context, TArray<FGraphInformationPopupInfo>& Popups) const override;
	virtual TArray<FOverlayWidgetInfo> GetOverlayWidgets(bool bSelected, const FVector2f& WidgetSize) const override;

	virtual void RefreshErrorInfo() override;
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;
	virtual bool IsHidingPinWidgets() const override { return UseLowDetailNodeContent(); }
	virtual bool UseLowDetailPinNames() const override;
	virtual void UpdateGraphNode() override;
	void UpdateStandardNode();
	void UpdateCompactNode();

	void CreateAddPinButton();

	void CreateWorkflowWidgets();

	/** Callback function executed when Add pin button is clicked */
	virtual FReply OnAddPin() override;

protected:

	bool UseLowDetailNodeContent() const;

	FVector2D GetLowDetailDesiredSize() const;

	EVisibility GetTitleVisibility() const;
	EVisibility GetArrayPlusButtonVisibility(URigVMPin* InModelPin) const;

	FText GetPinLabel(TWeakPtr<SGraphPin> GraphPin) const;

	virtual TOptional<FSlateColor> GetHighlightColor(const SGraphPin* InGraphPin) const override;
	FSlateColor GetVariableLabelTextColor(TWeakObjectPtr<URigVMFunctionReferenceNode> FunctionReferenceNode, FName InVariableName) const;
	FText GetVariableLabelTooltipText(TWeakObjectPtr<URigVMBlueprint> InBlueprint, FName InVariableName) const;

	FReply HandleAddArrayElement(FString InModelPinPath);

	void HandleNodeTitleDirtied();
	void HandleNodePinsChanged();
	void HandleNodeBeginRemoval();

	FText GetInstructionCountText() const;
	FText GetInstructionDurationText() const;

	TSharedRef<SWidget> OnOverrideWidgetMenu() const;

protected:

	int32 GetNodeTopologyVersion() const;
	EVisibility GetPinVisibility(int32 InPinInfoIndex, bool bAskingForSubPin) const;
	const FSlateBrush * GetExpanderImage(int32 InPinInfoIndex, bool bLeft, bool bHovered) const;
	FReply OnExpanderArrowClicked(int32 InPinInfoIndex);
	void HandleModifiedEvent(ERigVMGraphNotifType InNotifType, URigVMGraph* InGraph, UObject* InSubject);
	virtual void UpdatePinTreeView();
	ERigVMNodeDefaultValueOverrideState::Type GetPinDefaultValueOverrideState() const;
	bool IsCategoryPin(const SGraphPin* InGraphPin) const;
	EOverrideWidgetStatus::Type GetOverrideStatus() const;

	/** Cached widget title area */
	TSharedPtr<SOverlay> TitleAreaWidget;

	int32 NodeErrorType;

	TSharedPtr<SImage> VisualDebugIndicatorWidget;
	TSharedPtr<STextBlock> InstructionCountTextBlockWidget;
	TSharedPtr<STextBlock> InstructionDurationTextBlockWidget;
	TSharedPtr<SOverrideStatusWidget> OverrideStatusWidget; 

	static const FSlateBrush* CachedImg_CR_Pin_Connected;
	static const FSlateBrush* CachedImg_CR_Pin_Disconnected;

	/** Cache the node title so we can invalidate it */
	TSharedPtr<SNodeTitle> NodeTitle;

	TWeakObjectPtr<URigVMBlueprint> Blueprint;

	FVector2D LastHighDetailSize;

	struct FPinInfo
	{
		int32 Index;
		int32 ParentIndex;
		bool bIsCategoryPin;
		bool bHasChildren;
		bool bHideInputWidget;
		bool bIsContainer;
		int32 Depth;
		FString Identifier;
		TSharedPtr<SGraphPin> InputPinWidget;
		TSharedPtr<SGraphPin> OutputPinWidget;
		bool bExpanded;
		bool bAutoHeight;
		bool bShowOnlySubPins;
	};

	
	/**
	 * Simple tagging metadata
	 */
	class FPinInfoMetaData : public ISlateMetaData
	{
	public:
		SLATE_METADATA_TYPE(FPinInfoMetaData, ISlateMetaData)

		FPinInfoMetaData(const FString& InCPPType, const FString& InBoundVariableName)
		: CPPType(InCPPType)
		, BoundVariableName(InBoundVariableName)
		{}

		FString CPPType;
		FString BoundVariableName;
	};
	
	TArray<FPinInfo> PinInfos;
	TWeakObjectPtr<URigVMNode> ModelNode;

	// Pins to keep after calling HandleNodePinsChanged. We recycle these pins in
	// CreateStandardPinWidget.
	TMap<const UEdGraphPin *, TSharedRef<SGraphPin>> PinsToKeep;

	// Delayed pin deletion. To deal with the fact that pin deletion cannot occur until we
	// have re-generated the pin list. SRigVMGraphNode has already relinquished them
	// but we still have a pointer to them in our pin widget.
	TSet<UEdGraphPin *> PinsToDelete;
};
