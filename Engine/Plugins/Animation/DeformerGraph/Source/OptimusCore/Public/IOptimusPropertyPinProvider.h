// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/Interface.h"

#include "IOptimusPropertyPinProvider.generated.h"


class UOptimusNodePin;


UINTERFACE(MinimalAPI)
class UOptimusPropertyPinProvider :
	public UInterface
{
	GENERATED_BODY()
};


class IOptimusPropertyPinProvider
{
	GENERATED_BODY()

public:
	virtual TArray<UOptimusNodePin*> GetPropertyPins() const = 0;
};
