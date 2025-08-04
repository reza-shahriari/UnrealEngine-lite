// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "TimeTrialPlayerController.generated.h"

class ATimeTrialTrackGate;
class UTimeTrialUI;
class UInputMappingContext;
class UTP_VehicleAdvUI;

/**
 *  A simple PlayerController for a Time Trial racing game
 */
UCLASS(abstract)
class ATimeTrialPlayerController : public APlayerController
{
	GENERATED_BODY()
	
protected:

	/** Input Mapping Context to be used for player input */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Input")
	UInputMappingContext* InputMappingContext;

	/** If true, the optional steering wheel input mapping context will be registered */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input")
	bool bUseSteeringWheelControls = false;

	/** Optional Input Mapping Context to be used for steering wheel input.
	 *  This is added alongside the default Input Mapping Context and does not block other forms of input.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Input", meta=(EditCondition = "bUseSteeringWheelControls"))
	UInputMappingContext* SteeringWheelInputMappingContext;

	/** Type of UI widget to spawn*/
	UPROPERTY(EditAnywhere, Category="Time Trial")
	TSubclassOf<UTimeTrialUI> UIWidgetClass;

	/** Pointer to the UI Widget */
	TObjectPtr<UTimeTrialUI> UIWidget;

	/** Type of the UI to spawn */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="UI")
	TSubclassOf<UTP_VehicleAdvUI> VehicleUIClass;

	/** Pointer to the UI widget */
	TObjectPtr<UTP_VehicleAdvUI> VehicleUI;

	/** Next track gate the car should pass */
	TObjectPtr<ATimeTrialTrackGate> TargetGate;

	/** Lap counter */
	int32 CurrentLap = 0;

protected:

	/** Gameplay initialization */
	virtual void BeginPlay() override;

	/** Input initialization */
	virtual void SetupInputComponent() override;

	/** Pawn initialization */
	virtual void OnPossess(APawn* aPawn) override;

public:

	/** Sets up the race start */
	UFUNCTION()
	void StartRace();

	/** Moves on to the next lap */
	void IncrementLapCount();

	/** Returns the current target track gate */
	ATimeTrialTrackGate* GetTargetGate();

	/** Sets the target track gate for this player */
	void SetTargetGate(ATimeTrialTrackGate* Gate);
};
