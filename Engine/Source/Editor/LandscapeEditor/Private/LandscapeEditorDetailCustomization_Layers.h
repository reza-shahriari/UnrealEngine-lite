// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "LandscapeEditorDetailCustomization_Base.h"
#include "LandscapeEdMode.h"
#include "LandscapeEditLayer.h"
#include "Layout/Visibility.h"
#include "Layout/Margin.h"
#include "Styling/SlateColor.h"
#include "Styling/SlateBrush.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SWidget.h"
#include "Widgets/Layout/SBorder.h"
#include "IDetailCustomNodeBuilder.h"
#include "IDetailCustomization.h"
#include "AssetThumbnail.h"
#include "Framework/SlateDelegates.h"

class FDetailWidgetRow;
class IDetailChildrenBuilder;
class IDetailLayoutBuilder;
class SDragAndDropVerticalBox;
class SInlineEditableTextBlock;
class ALandscapeBlueprintBrushBase;
class ULandscapeLayerInfoObject;
class FMenuBuilder;

/**
 * Slate widgets customizer for the layers list in the Landscape Editor
 */

class FLandscapeEditorDetailCustomization_Layers : public FLandscapeEditorDetailCustomization_Base
{
public:
	/** Makes a new instance of this detail layout class for a specific detail view requesting it */
	static TSharedRef<IDetailCustomization> MakeInstance();

	/** IDetailCustomization interface */
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailBuilder) override;

protected:
	static bool ShouldShowLayersErrorMessageTip();
	static FText GetLayersErrorMessageText();
};

class FLandscapeEditorCustomNodeBuilder_Layers : public IDetailCustomNodeBuilder, public TSharedFromThis<FLandscapeEditorCustomNodeBuilder_Layers>
{
public:
	FLandscapeEditorCustomNodeBuilder_Layers(TSharedRef<FAssetThumbnailPool> ThumbnailPool);
	~FLandscapeEditorCustomNodeBuilder_Layers();

	virtual void SetOnRebuildChildren( FSimpleDelegate InOnRegenerateChildren ) override;
	virtual void GenerateHeaderRowContent( FDetailWidgetRow& NodeRow ) override;
	virtual void GenerateChildContent( IDetailChildrenBuilder& ChildrenBuilder ) override;
	virtual void Tick( float DeltaTime ) override {}
	virtual bool RequiresTick() const override { return false; }
	virtual bool InitiallyCollapsed() const override { return false; }
	virtual FName GetName() const override { return "Layers"; }

protected:
	TSharedRef<FAssetThumbnailPool> ThumbnailPool;

	static class FEdModeLandscape* GetEditorMode();

	TSharedPtr<SWidget> GenerateRow(int32 InLayerIndex);

	// Drag/Drop handling
	int32 SlotIndexToLayerIndex(int32 SlotIndex);
	FReply HandleDragDetected(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent, int32 SlotIndex, SVerticalBox::FSlot* Slot);
	TOptional<SDragAndDropVerticalBox::EItemDropZone> HandleCanAcceptDrop(const FDragDropEvent& DragDropEvent, SDragAndDropVerticalBox::EItemDropZone DropZone, int32 SlotIndex, SVerticalBox::FSlot* Slot);
	FReply HandleAcceptDrop(FDragDropEvent const& DragDropEvent, SDragAndDropVerticalBox::EItemDropZone DropZone, int32 SlotIndex, SVerticalBox::FSlot* Slot);

	bool IsLayerSelected(int32 LayerIndex) const;
	void OnLayerSelectionChanged(int32 LayerIndex);
	TSharedPtr<SWidget> OnLayerContextMenuOpening(int32 InLayerIndex);

	FText GetNumLayersText() const;

	bool CanCreateLayer(FText& OutReason) const;
	void CreateLayer();

	const FSlateBrush* GetEditLayerIconBrush(int32 InLayerIndex) const;

	bool CanClearTargetLayerOnLayer(int32 InLayerIndex, ULandscapeLayerInfoObject* InLayerInfo, FText& OutReason) const;
	void ClearTargetLayerOnLayer(int32 InLayerIndex, ULandscapeLayerInfoObject* InLayerInfo);
	bool CanClearLayer(int32 InLayerIndex, FText& OutReason) const;
	bool CanClearTargetLayersOnLayer(int32 InLayerIndex, ELandscapeClearMode InClearMode, FText& OutReason) const;
	void ClearTargetLayersOnLayer(int32 InLayerIndex, ELandscapeClearMode InClearMode);

	bool CanRenameLayerTo(const FText& NewText, FText& OutErrorMessage, int32 InLayerIndex);
	bool CanRenameLayer(int32 InLayerIndex, FText& OutReason) const;
	void RenameLayer(int32 InLayerIndex);

	bool CanDeleteLayer(int32 InLayerIndex, FText& OutReason) const;
	void DeleteLayer(int32 InLayerIndex);

	bool CanCollapseLayer(int32 InLayerIndex, FText& OutReason) const;
	void CollapseLayer(int32 InLayerIndex);

	bool CanExecuteCustomLayerAction(int32 InLayerIndex, const ULandscapeEditLayerBase::FEditLayerAction& InCustomLayerAction, FText& OutReason) const;
	void ExecuteCustomLayerAction(int32 InLayerIndex, const ULandscapeEditLayerBase::FEditLayerAction& InCustomLayerAction);

	void ShowOnlySelectedLayer(int32 InLayerIndex);
	void ShowAllLayers();
	void SetLayerName(const FText& InText, ETextCommit::Type InCommitType, int32 InLayerIndex);
	FText GetLayerText(int32 InLayerIndex) const;
	FSlateColor GetLayerTextColor(int32 InLayerIndex) const;
	FText GetLayerDisplayName(int32 InLayerIndex) const;
	EVisibility GetLayerAlphaVisibility(int32 InLayerIndex) const;
	TSharedPtr<IToolTip> GetEditLayerTypeTooltip(int32 InLayerIndex) const;
	TSubclassOf<ULandscapeEditLayerBase> PickEditLayerClass() const;

	TOptional<float> GetLayerAlpha(int32 InLayerIndex) const;
	float GetLayerAlphaMinValue() const;
	bool CanSetLayerAlpha(int32 InLayerIndex, FText& OutReason) const;
	void SetLayerAlpha(float InAlpha, int32 InLayerIndex, bool bCommit);
	
	bool CanToggleVisibility(int32 InLayerIndex, FText& OutReason) const;
	FReply OnToggleVisibility(int32 InLayerIndex);
	const FSlateBrush* GetVisibilityBrushForLayer(int32 InLayerIndex) const;
	
	void OnSetInspectedDetailsToEditLayer(int32 InLayerIndex) const;

	FReply OnToggleLock(int32 InLayerIndex);
	const FSlateBrush* GetLockBrushForLayer(int32 InLayerIndex) const;

	// Add Existing BP Brush (Brushes with no landscape actor assigned)
	void FillUnassignedBrushMenu(FMenuBuilder& MenuBuilder, TArray<ALandscapeBlueprintBrushBase*> Brushes, int32 InLayerIndex);
	void AssignBrushToEditLayer(ALandscapeBlueprintBrushBase* Brush, int32 InLayerIndex);

	void FillClearTargetLayerMenu(FMenuBuilder& MenuBuilder, int32 InLayerIndex, TArray<ULandscapeLayerInfoObject*> InUsedLayerInfos);
	void FillClearLayerMenu(FMenuBuilder& MenuBuilder, int32 InLayerIndex);

private:

	/** Widgets for displaying and editing the layer name */
	TArray< TSharedPtr< SInlineEditableTextBlock > > InlineTextBlocks;

	int32 CurrentSlider;
};

class FLandscapeListElementDragDropOp : public FDragAndDropVerticalBoxOp
{
public:
	DRAG_DROP_OPERATOR_TYPE(FLandscapeListElementDragDropOp, FDragAndDropVerticalBoxOp)

	TSharedPtr<SWidget> WidgetToShow;

	static TSharedRef<FLandscapeListElementDragDropOp> New(int32 InSlotIndexBeingDragged, SVerticalBox::FSlot* InSlotBeingDragged, TSharedPtr<SWidget> InWidgetToShow);

public:
	virtual ~FLandscapeListElementDragDropOp() {}

	virtual TSharedPtr<SWidget> GetDefaultDecorator() const override;
};
