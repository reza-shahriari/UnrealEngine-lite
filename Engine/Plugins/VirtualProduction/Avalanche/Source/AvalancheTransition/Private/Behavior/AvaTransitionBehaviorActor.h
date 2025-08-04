// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AvaTransitionContext.h"
#include "Behavior/IAvaTransitionBehavior.h"
#include "GameFramework/Actor.h"
#include "StateTreeExecutionTypes.h"
#include "StateTreeInstanceData.h"
#include "StateTreeReference.h"
#include "AvaTransitionBehaviorActor.generated.h"

class UAvaTransitionBehaviorInstance;
class UAvaTransitionSubsystem;
struct FStateTreeExecutionContext;

UCLASS(NotPlaceable, Hidden, DisplayName = "Motion Design Transition Behavior Actor")
class AAvaTransitionBehaviorActor : public AActor, public IAvaTransitionBehavior
{
	GENERATED_BODY()

public:
	AAvaTransitionBehaviorActor();

protected:
	//~ Begin IAvaTransitionBehavior
	virtual UObject& AsUObject() override { return *this; }
	virtual UAvaTransitionTree* GetTransitionTree() const override { return TransitionTree; }
	virtual const FStateTreeReference& GetStateTreeReference() const override { return StateTreeReference; }
	//~ End IAvaTransitionBehavior

	//~ Begin AActor
	virtual void PostActorCreated() override;
#if WITH_EDITOR
	virtual bool IsSelectable() const override { return false; }
	virtual bool SupportsExternalPackaging() const override { return false; }
#endif
	//~ End AActor

	//~ Begin UObject
	virtual void PostLoad() override;
#if WITH_EDITOR
	virtual bool CanEditChange(const FProperty* InProperty) const override;
	virtual bool CanEditChange(const FEditPropertyChain& InPropertyChain) const;
#endif
	//~ End UObject

private:
	UAvaTransitionSubsystem* GetTransitionSubsystem() const;

	void ValidateTransitionTree();

	UPROPERTY()
	TObjectPtr<UAvaTransitionTree> TransitionTree;

	UPROPERTY(EditAnywhere, Category="Transition Logic")
	FStateTreeReference StateTreeReference;
};
