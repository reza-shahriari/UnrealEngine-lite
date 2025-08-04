// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PropertyAnimatorPresetRotation.h"
#include "PropertyAnimatorPresetTextRotation.generated.h"

class AActor;
class UPropertyAnimatorCoreBase;

/**
 * Preset for text character position properties (Roll, Pitch, Yaw) on scene component
 */
UCLASS()
class UPropertyAnimatorPresetTextRotation : public UPropertyAnimatorPresetRotation
{
	GENERATED_BODY()

public:
	UPropertyAnimatorPresetTextRotation()
	{
		PresetName = TEXT("TextCharacterRotation");
	}

protected:
	//~ Begin UPropertyAnimatorCorePresetBase
	virtual void GetPresetProperties(const AActor* InActor, const UPropertyAnimatorCoreBase* InAnimator, TSet<FPropertyAnimatorCoreData>& OutProperties) const override;
	//~ End UPropertyAnimatorCorePresetBase
};