// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "SAssetEditorViewport.h"



class UVEDITOR_API SUVEditor2DViewport : public SAssetEditorViewport
{
public:

	// These allow the toolkit to add an accept/cancel overlay when needed. PopulateViewportOverlays
	// is not helpful here because that gets called just once.
	virtual void AddOverlayWidget(TSharedRef<SWidget> OverlaidWidget, int32 ZOrder = INDEX_NONE);
	virtual void RemoveOverlayWidget(TSharedRef<SWidget> OverlaidWidget);

	// SEditorViewport
	virtual void BindCommands() override;
	virtual TSharedPtr<SWidget> MakeViewportToolbar() override;
	virtual TSharedPtr<SWidget> BuildViewportToolbar() override;
	bool IsWidgetModeActive(UE::Widget::EWidgetMode Mode) const override;
};
