// Copyright Epic Games, Inc. All Rights Reserved.


#include "TP_FirstPersonPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "InputMappingContext.h"
#include "TP_FirstPersonCameraManager.h"

ATP_FirstPersonPlayerController::ATP_FirstPersonPlayerController()
{
	// set the player camera manager class
	PlayerCameraManagerClass = ATP_FirstPersonCameraManager::StaticClass();
}

void ATP_FirstPersonPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// Add Input Mapping Context
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			Subsystem->AddMappingContext(CurrentContext, 0);
		}
	}
}
