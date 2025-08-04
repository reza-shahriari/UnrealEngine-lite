// Copyright Epic Games, Inc. All Rights Reserved.

#include "SMaterialEditorViewportToolBar.h"
#include "Widgets/Layout/SBorder.h"
#include "Styling/AppStyle.h"
#include "MaterialEditorActions.h"
#include "MaterialEditorViewportToolbarSections.h"
#include "PreviewProfileController.h"

#define LOCTEXT_NAMESPACE "MaterialEditorViewportToolBar"

///////////////////////////////////////////////////////////
// SMaterialEditorViewportPreviewShapeToolBar

void SMaterialEditorViewportPreviewShapeToolBar::Construct(const FArguments& InArgs, TSharedPtr<class SMaterialEditor3DPreviewViewport> InViewport)
{
	// Force this toolbar to have small icons, as the preview panel is only small so we have limited space
	const bool bForceSmallIcons = true;
	FToolBarBuilder ToolbarBuilder(InViewport->GetCommandList(), FMultiBoxCustomization::None, nullptr, bForceSmallIcons);

	// Use a custom style
	ToolbarBuilder.SetStyle(&FAppStyle::Get(), "LegacyViewportMenu");
	ToolbarBuilder.SetLabelVisibility(EVisibility::Collapsed);
	ToolbarBuilder.SetIsFocusable(false);
	
	ToolbarBuilder.BeginSection("Preview");
	{
		ToolbarBuilder.AddToolBarButton(FMaterialEditorCommands::Get().SetCylinderPreview);
		ToolbarBuilder.AddToolBarButton(FMaterialEditorCommands::Get().SetSpherePreview);
		ToolbarBuilder.AddToolBarButton(FMaterialEditorCommands::Get().SetPlanePreview);
		ToolbarBuilder.AddToolBarButton(FMaterialEditorCommands::Get().SetCubePreview);
		ToolbarBuilder.AddToolBarButton(FMaterialEditorCommands::Get().SetPreviewMeshFromSelection);
	}
	ToolbarBuilder.EndSection();

	static const FName DefaultForegroundName("DefaultForeground");

	ChildSlot
	[
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("NoBorder"))
		.ForegroundColor(FAppStyle::GetSlateColor(DefaultForegroundName))
		.HAlign(HAlign_Right)
		[
			ToolbarBuilder.MakeWidget()
		]
	];

	SViewportToolBar::Construct(SViewportToolBar::FArguments());
}

///////////////////////////////////////////////////////////
// SMaterialEditorViewportToolBar

void SMaterialEditorViewportToolBar::Construct(const FArguments& InArgs, TSharedPtr<class SMaterialEditor3DPreviewViewport> InViewport)
{
	MaterialEditorViewportPtr = InViewport;
	SCommonEditorViewportToolbarBase::Construct(SCommonEditorViewportToolbarBase::FArguments().PreviewProfileController(MakeShared<FPreviewProfileController>()), InViewport);
}

TSharedRef<SWidget> SMaterialEditorViewportToolBar::GenerateShowMenu() const
{
	if (TSharedPtr<SMaterialEditor3DPreviewViewport> MaterialEditorViewport = MaterialEditorViewportPtr.Pin())
	{
		constexpr bool bShowViewportStats = false;
		return UE::MaterialEditor::CreateShowMenuWidget(MaterialEditorViewport.ToSharedRef(), bShowViewportStats);
	}

	return SNullWidget::NullWidget;
}

bool SMaterialEditorViewportToolBar::IsViewModeSupported(EViewModeIndex ViewModeIndex) const 
{
	switch (ViewModeIndex)
	{
	case VMI_PrimitiveDistanceAccuracy:
	case VMI_MeshUVDensityAccuracy:
	case VMI_RequiredTextureResolution:
		return false;
	default:
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
