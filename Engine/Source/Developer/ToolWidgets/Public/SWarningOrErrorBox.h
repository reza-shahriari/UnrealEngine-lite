// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/Layout/SBorder.h"

#define UE_API TOOLWIDGETS_API

enum class EMessageStyle
{
	Warning,
	Error
};

class SWarningOrErrorBox : public SBorder
{
public:
	SLATE_BEGIN_ARGS(SWarningOrErrorBox)
		: _MessageStyle(EMessageStyle::Warning)
		, _Padding(16.0f)
		, _IconSize(24,24)
		, _AutoWrapText(true)
		, _Content()
	{}
		SLATE_ATTRIBUTE(FText, Message)
		SLATE_ATTRIBUTE(EMessageStyle, MessageStyle)
		SLATE_ARGUMENT(FMargin, Padding)
		SLATE_ARGUMENT(FVector2D, IconSize)
		SLATE_ARGUMENT(bool, AutoWrapText)

		SLATE_DEFAULT_SLOT( FArguments, Content )

	SLATE_END_ARGS()

	UE_API void Construct(const FArguments& InArgs);

private:
	TAttribute<EMessageStyle> MessageStyle;
};

#undef UE_API
