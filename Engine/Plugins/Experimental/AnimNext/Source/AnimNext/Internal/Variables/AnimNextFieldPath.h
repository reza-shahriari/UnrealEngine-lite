// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AnimNextFieldPath.generated.h"

/** Struct wrapper around TFieldPath, used to allow RigVM support */
USTRUCT()
struct FAnimNextFieldPath
{
	GENERATED_BODY()

	UPROPERTY()
	TFieldPath<FProperty> FieldPath;
};
