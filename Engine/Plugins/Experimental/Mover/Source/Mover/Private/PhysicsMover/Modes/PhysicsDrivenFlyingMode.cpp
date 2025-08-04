// Copyright Epic Games, Inc. All Rights Reserved.

#include "PhysicsMover/Modes/PhysicsDrivenFlyingMode.h"

#include "Chaos/Character/CharacterGroundConstraint.h"
#include "GameFramework/PhysicsVolume.h"
#include "MoverComponent.h"
#include "DefaultMovementSet/Settings/CommonLegacyMovementSettings.h"
#include "Math/UnitConversion.h"
#include "MoveLibrary/MovementUtils.h"
#include "PhysicsMover/PhysicsMovementUtils.h"
#if WITH_EDITOR
#include "Backends/MoverNetworkPhysicsLiaison.h"
#include "Internationalization/Text.h"
#include "Misc/DataValidation.h"
#endif // WITH_EDITOR

#include UE_INLINE_GENERATED_CPP_BY_NAME(PhysicsDrivenFlyingMode)

#define LOCTEXT_NAMESPACE "PhysicsDrivenFlyingMode"

UPhysicsDrivenFlyingMode::UPhysicsDrivenFlyingMode(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void UPhysicsDrivenFlyingMode::UpdateConstraintSettings(Chaos::FCharacterGroundConstraint& Constraint) const
{
	Constraint.SetRadialForceLimit(300000.0); // TEMP - Move radial force limit to shared mode data
	Constraint.SetTwistTorqueLimit(FUnitConversion::Convert(TwistTorqueLimit, EUnit::NewtonMeters, EUnit::KilogramCentimetersSquaredPerSecondSquared));
	Constraint.SetSwingTorqueLimit(FUnitConversion::Convert(SwingTorqueLimit, EUnit::NewtonMeters, EUnit::KilogramCentimetersSquaredPerSecondSquared));
	Constraint.SetTargetHeight(0.0f);
}

#if WITH_EDITOR
EDataValidationResult UPhysicsDrivenFlyingMode::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult Result = Super::IsDataValid(Context);
	PhysicsMovementModeUtils::ValidateBackendClass(GetMoverComponent(), Context, Result);
	return Result;
}
#endif // WITH_EDITOR

void UPhysicsDrivenFlyingMode::SimulationTick_Implementation(const FSimulationTickParams& Params, FMoverTickEndData& OutputState)
{
	const UMoverComponent* MoverComp = GetMoverComponent();
	const FMoverTickStartData& StartState = Params.StartState;
	USceneComponent* UpdatedComponent = Params.MovingComps.UpdatedComponent.Get();
	UPrimitiveComponent* UpdatedPrimitive = Params.MovingComps.UpdatedPrimitive.Get();
	FProposedMove ProposedMove = Params.ProposedMove;

	const FMoverDefaultSyncState* StartingSyncState = StartState.SyncState.SyncStateCollection.FindDataByType<FMoverDefaultSyncState>();
	check(StartingSyncState);

	FMoverDefaultSyncState& OutputSyncState = OutputState.SyncState.SyncStateCollection.FindOrAddMutableDataByType<FMoverDefaultSyncState>();

	const float DeltaSeconds = Params.TimeStep.StepMs * 0.001f;
	const FVector UpDir = MoverComp->GetUpDirection();

	// Don't need a floor query - just invalidate the blackboard to ensure we don't use an old result elsewhere

	if (UMoverBlackboard* SimBlackboard = MoverComp->GetSimBlackboard_Mutable())
	{
		SimBlackboard->Invalidate(CommonBlackboard::LastFloorResult);
		SimBlackboard->Invalidate(CommonBlackboard::LastWaterResult);
	}

	// In air steering

	FRotator TargetOrient = StartingSyncState->GetOrientation_WorldSpace();
	if (!UMovementUtils::IsAngularVelocityZero(ProposedMove.AngularVelocity))
	{
		TargetOrient += (ProposedMove.AngularVelocity * DeltaSeconds);
	}

	FVector TargetVel = ProposedMove.LinearVelocity;
	if (const APhysicsVolume* CurPhysVolume = UpdatedComponent->GetPhysicsVolume())
	{
		// The physics simulation applies Z-only gravity acceleration via physics volumes, so we need to account for it here 
		TargetVel -= (CurPhysVolume->GetGravityZ() * FVector::UpVector * DeltaSeconds);
	}

	FVector TargetPos = StartingSyncState->GetLocation_WorldSpace() + TargetVel * DeltaSeconds;

	OutputState.MovementEndState.NextModeName = DefaultModeNames::Flying;

	OutputState.MovementEndState.RemainingMs = 0.0f;
	OutputSyncState.MoveDirectionIntent = ProposedMove.bHasDirIntent ? ProposedMove.DirectionIntent : FVector::ZeroVector;
	OutputSyncState.SetTransforms_WorldSpace(
		TargetPos,
		TargetOrient,
		TargetVel);
}

#undef LOCTEXT_NAMESPACE