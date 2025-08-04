// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Attribute.h"
#include "Input/Reply.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SLeafWidget.h"
#include "Framework/SlateDelegates.h"

class FPaintArgs;
class FSlateWindowElementList;
struct FSlateBrush;

/**
 * Implements the color wheel widget.
 */
class SColorWheel
	: public SLeafWidget
{
public:

	SLATE_BEGIN_ARGS(SColorWheel)
		: _SelectedColor()
		, _OnMouseCaptureBegin()
		, _OnMouseCaptureEnd()
		, _OnValueChanged()
		, _CtrlMultiplier(0.1f)
	{ }
	
		/** The current color selected by the user. */
		SLATE_ATTRIBUTE(FLinearColor, SelectedColor)
		
		/** Invoked when the mouse is pressed and a capture begins. */
		SLATE_EVENT(FSimpleDelegate, OnMouseCaptureBegin)

		/** Invoked when the mouse is released and a capture ends. */
		SLATE_EVENT(FSimpleDelegate, OnMouseCaptureEnd)

		/** Invoked when a new value is selected on the color wheel. */
		SLATE_EVENT(FOnLinearColorValueChanged, OnValueChanged)

		/** Multiplier to use when ctrl is held down */
		SLATE_ATTRIBUTE(float, CtrlMultiplier)

	SLATE_END_ARGS()
	
public:
	SLATE_API SColorWheel();
	SLATE_API virtual ~SColorWheel();

	/**
	 * Construct this widget.
	 *
	 * @param InArgs The declaration data for this widget.
	 */
	SLATE_API void Construct(const FArguments& InArgs);

public:

	//~ SWidget overrides

	SLATE_API virtual FVector2D ComputeDesiredSize(float) const override;
	SLATE_API virtual FReply OnMouseButtonDoubleClick(const FGeometry& InMyGeometry, const FPointerEvent& InMouseEvent) override;
	SLATE_API virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	SLATE_API virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	SLATE_API virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	SLATE_API virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect, FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	SLATE_API virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

protected:

	/**
	 * Calculates the position of the color selection indicator.
	 *
	 * @return The position relative to the widget.
	 */
	SLATE_API UE::Slate::FDeprecateVector2DResult CalcRelativePositionFromCenter() const;

	/**
	 * Performs actions according to mouse click / move
	 *
	 * @return	True if the mouse action occurred within the color wheel radius
	 */
	SLATE_API bool ProcessMouseAction(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent, bool bProcessWhenOutsideColorWheel);

private:

	/** The color wheel image to show. */
	const FSlateBrush* Image;
	
	/** The current color selected by the user. */
	TSlateAttribute<FLinearColor, EInvalidateWidgetReason::Paint> SelectedColor;

	/** Mouse sensitivity multiplier to use when dragging the selector on the color wheel, applied when the ctrl modifier key is pressed */
	TAttribute<float> CtrlMultiplier;

	/** The color selector image to show. */
	const FSlateBrush* SelectorImage;

	/** Whether the user is dragging the slider */
	bool bDragging = false;

	/** Cached mouse position to restore after dragging. */
	FVector2f LastWheelPosition;

private:

	/** Invoked when the mouse is pressed and a capture begins. */
	FSimpleDelegate OnMouseCaptureBegin;

	/** Invoked when the mouse is let up and a capture ends. */
	FSimpleDelegate OnMouseCaptureEnd;

	/** Invoked when a new value is selected on the color wheel. */
	FOnLinearColorValueChanged OnValueChanged;
};
