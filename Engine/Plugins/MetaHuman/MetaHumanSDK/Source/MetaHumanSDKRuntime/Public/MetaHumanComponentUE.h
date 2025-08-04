// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MetaHumanComponentBase.h"
#include "MetaHumanComponentUE.generated.h"

#define UE_API METAHUMANSDKRUNTIME_API


UCLASS(MinimalAPI, Blueprintable, ClassGroup = "Animation", HideCategories = (Navigation, Variable, Sockets, Tags, Activation, Cooking, Events, ComponentTick, ComponentReplication, AssetUserData, Replication), meta = (BlueprintSpawnableComponent, DisplayName = "MetaHuman Component"))
class UMetaHumanComponentUE
	: public UMetaHumanComponentBase
{
	GENERATED_BODY()

public:
	// UActorComponent interface
	UE_API virtual void OnRegister() override;
	UE_API virtual void OnUnregister() override;
	UE_API virtual void BeginPlay() override;
	// End UActorComponent interface

private:
	void SetupCustomizableBodyPart(FMetaHumanCustomizableBodyPart& BodyPart);
	UE_API virtual void PostInitAnimBP(USkeletalMeshComponent* SkeletalMeshComponent, UAnimInstance* AnimInstance) const override final;

	/**
	 * The post-processing AnimBP to use for the body parts when either the physics asset or the control rig are set.
	 * Use the ABP_Clothing_PostProcess shipped along with MetaHumans. The MetaHuman component will control given variables
	 * to e.g. set the LOD thresholds.
	 */
	UPROPERTY(EditDefaultsOnly, Category = BodyParts)
	TSoftClassPtr<UAnimInstance> PostProcessAnimBP;
};

#undef UE_API
