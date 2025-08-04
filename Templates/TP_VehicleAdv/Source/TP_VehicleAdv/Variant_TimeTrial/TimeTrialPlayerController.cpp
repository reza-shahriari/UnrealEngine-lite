// Copyright Epic Games, Inc. All Rights Reserved.


#include "TimeTrialPlayerController.h"
#include "TimeTrialUI.h"
#include "Engine/World.h"
#include "TimeTrialGameMode.h"
#include "TimeTrialTrackGate.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "TP_VehicleAdvUI.h"

void ATimeTrialPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// create the UI widget
	UIWidget = CreateWidget<UTimeTrialUI>(this, UIWidgetClass);
	UIWidget->AddToViewport(0);

	// spawn the UI widget and add it to the viewport
	VehicleUI = CreateWidget<UTP_VehicleAdvUI>(this, VehicleUIClass);
	VehicleUI->AddToViewport();

	// subscribe to the race start delegate
	UIWidget->OnRaceStart.AddDynamic(this, &ATimeTrialPlayerController::StartRace);
}

void ATimeTrialPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// get the enhanced input subsystem
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		// add the mapping context so we get controls
		Subsystem->AddMappingContext(InputMappingContext, 0);

		// optionally add the steering wheel context
		if (bUseSteeringWheelControls && SteeringWheelInputMappingContext)
		{
			Subsystem->AddMappingContext(SteeringWheelInputMappingContext, 1);
		}
	}
}

void ATimeTrialPlayerController::OnPossess(APawn* aPawn)
{
	Super::OnPossess(aPawn);

	// disable input on the pawn
	aPawn->DisableInput(this);
}

void ATimeTrialPlayerController::StartRace()
{
	// get the finish line from the game mode
	if (ATimeTrialGameMode* GM = Cast<ATimeTrialGameMode>(GetWorld()->GetAuthGameMode()))
	{
		SetTargetGate(GM->GetFinishLine()->GetNextMarker());
	}

	// start the first lap
	CurrentLap = 0;
	IncrementLapCount();

	// enable input on the pawn
	GetPawn()->EnableInput(this);
}

void ATimeTrialPlayerController::IncrementLapCount()
{
	// increment the lap counter
	++CurrentLap;

	// update the UI
	UIWidget->UpdateLapCount(CurrentLap, GetWorld()->GetTimeSeconds());
}

ATimeTrialTrackGate* ATimeTrialPlayerController::GetTargetGate()
{
	return TargetGate.Get();
}

void ATimeTrialPlayerController::SetTargetGate(ATimeTrialTrackGate* Gate)
{
	TargetGate = Gate;
}
