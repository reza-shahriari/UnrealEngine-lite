// Copyright Epic Games, Inc. All Rights Reserved.

#include "Nodes/Attach/AttachToPlayerPawnCameraNode.h"

#include "Core/CameraEvaluationContext.h"
#include "Core/CameraParameterReader.h"
#include "Core/CameraSystemEvaluator.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayCameras.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(AttachToPlayerPawnCameraNode)

namespace UE::Cameras
{

class FAttachToPlayerPawnCameraNodeEvaluator : public FCameraNodeEvaluator
{
	UE_DECLARE_CAMERA_NODE_EVALUATOR(GAMEPLAYCAMERAS_API, FAttachToPlayerPawnCameraNodeEvaluator)

protected:

	virtual void OnInitialize(const FCameraNodeEvaluatorInitializeParams& Params, FCameraNodeEvaluationResult& OutResult) override;
	virtual void OnRun(const FCameraNodeEvaluationParams& Params, FCameraNodeEvaluationResult& OutResult) override;

private:

	TCameraParameterReader<bool> AttachToLocationReader;
	TCameraParameterReader<bool> AttachToRotationReader;

	bool bHasValidPlayerController = true;
};

UE_DEFINE_CAMERA_NODE_EVALUATOR(FAttachToPlayerPawnCameraNodeEvaluator)

void FAttachToPlayerPawnCameraNodeEvaluator::OnInitialize(const FCameraNodeEvaluatorInitializeParams& Params, FCameraNodeEvaluationResult& OutResult)
{
	SetNodeEvaluatorFlags(ECameraNodeEvaluatorFlags::None);

	const UAttachToPlayerPawnCameraNode* AttachNode = GetCameraNodeAs<UAttachToPlayerPawnCameraNode>();
	AttachToLocationReader.Initialize(AttachNode->AttachToLocation);
	AttachToRotationReader.Initialize(AttachNode->AttachToRotation);

	APlayerController* PlayerController = Params.EvaluationContext->GetPlayerController();
	if (!PlayerController)
	{
		UE_LOG(LogCameraSystem, Error, 
				TEXT("Can't run AttatchToPlayerPawn camera node because no player controller was found on the context."));
		bHasValidPlayerController = false;
	}
}

void FAttachToPlayerPawnCameraNodeEvaluator::OnRun(const FCameraNodeEvaluationParams& Params, FCameraNodeEvaluationResult& OutResult)
{
#if WITH_EDITOR
	if (Params.Evaluator && Params.Evaluator->GetRole() == ECameraSystemEvaluatorRole::EditorPreview)
	{
		// No player pawn in editor preview.
		return;
	}
#endif

	if (!bHasValidPlayerController)
	{
		return;
	}

	APlayerController* PlayerController = Params.EvaluationContext->GetPlayerController();
	if (!ensure(PlayerController))
	{
		bHasValidPlayerController = false;
		return;
	}

	const APawn* Pawn = PlayerController->GetPawnOrSpectator();
	if (!ensure(Pawn))
	{
		return;
	}

	const bool bAttachToLocation = AttachToLocationReader.Get(OutResult.VariableTable);
	const bool bAttachToRotation = AttachToRotationReader.Get(OutResult.VariableTable);
	
	if (bAttachToLocation)
	{
		const FVector3d PawnLocation = Pawn->GetActorLocation();
		OutResult.CameraPose.SetLocation(PawnLocation);
	}

	if (bAttachToRotation)
	{
		const FRotator3d PawnRotation = Pawn->GetActorRotation();
		OutResult.CameraPose.SetRotation(PawnRotation);
	}
}

}  // namespace UE::Cameras

FCameraNodeEvaluatorPtr UAttachToPlayerPawnCameraNode::OnBuildEvaluator(FCameraNodeEvaluatorBuilder& Builder) const
{
	using namespace UE::Cameras;
	return Builder.BuildEvaluator<FAttachToPlayerPawnCameraNodeEvaluator>();
}

