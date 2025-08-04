// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Factories/Factory.h"

#include "NamingTokensFactory.generated.h"

UCLASS(MinimalAPI)
class UNamingTokensFactory : public UFactory
{
	GENERATED_BODY()

public:
	UNamingTokensFactory();
	
	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn) override;
};