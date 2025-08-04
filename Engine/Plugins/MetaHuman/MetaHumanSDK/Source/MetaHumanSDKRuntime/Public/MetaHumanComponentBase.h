// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "UObject/ObjectMacros.h"
#include "GameFramework/Actor.h"
#include "MetaHumanBodyType.h"
#include "Animation/AnimInstance.h"

#include "MetaHumanComponentBase.generated.h"

#define UE_API METAHUMANSDKRUNTIME_API

class USkeletalMeshComponent;
class UControlRig;
class UPhysicsAsset;

namespace MetaHumanComponentHelpers
{
	template <typename PropertyBPType, typename PropertyVarType>
	void ConnectVariable(UAnimInstance* AnimInstance, const FName& InIdentifier, const PropertyVarType& InVar)
	{
		FProperty* BlueprintProperty = AnimInstance->GetClass()->FindPropertyByName(InIdentifier);
		if (BlueprintProperty)
		{
			if (PropertyBPType* BlueprintObjectProperty = CastFieldChecked<PropertyBPType>(BlueprintProperty))
			{
				BlueprintObjectProperty->SetPropertyValue_InContainer(AnimInstance, InVar);
			}
		}
	}

	template<typename T>
	static bool GetPropertyValue(UObject* InObject, FStringView InPropertyName, T& OutPropertyValue)
	{
		if (FProperty* Property = InObject->GetClass()->FindPropertyByName(FName{ InPropertyName }))
		{
			Property->GetValue_InContainer(InObject, &OutPropertyValue);
			return true;
		}
		return false;
	}
}

USTRUCT()
struct FMetaHumanCustomizableBodyPart
{
	GENERATED_BODY()

	/** Control rig to run on the body part. Evaluation happens after the base skeleton. */
	UPROPERTY(EditDefaultsOnly, Category = BodyParts)
	TSubclassOf<UControlRig> ControlRigClass;

	/*
	 * Max LOD level to evaluate the assigned control rig for the body part.
	 * For example if you have the threshold set to 2, the control rig will be evaluated for LOD 0, 1, and 2. Setting it to -1 will always evaluate it and disable LODing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = BodyParts, meta = (EditCondition = "ControlRigClass != nullptr", EditConditionHides))
	int32 ControlRigLODThreshold = INDEX_NONE;

	/** Physics asset used for rigid body simulation on the body part. Evaluation happens after the base skeleton. */
	UPROPERTY(EditDefaultsOnly, Category = BodyParts)
	TObjectPtr<UPhysicsAsset> PhysicsAsset;

	/*
	 * Max LOD level to simulate the rigid bodies of the assigned physics asset.
	 * For example if you have the threshold set to 2, simulation will be enabled for LOD 0, 1, and 2. Setting it to -1 will make it simulate always and disable LODing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = BodyParts, meta = (EditCondition = "PhysicsAsset != nullptr", EditConditionHides))
	int32 RigidBodyLODThreshold = INDEX_NONE;

	UPROPERTY(EditDefaultsOnly, Category = BodyParts, AdvancedDisplay)
	FString ComponentName;
};

UCLASS(MinimalAPI)
class UMetaHumanComponentBase
	: public UActorComponent
{
	GENERATED_BODY()

protected:
	UE_API UMetaHumanComponentBase();

	/** Get the first skeletal mesh component with the given name from the owning actor. */
	UE_API USkeletalMeshComponent* GetSkelMeshComponentByName(const FString& ComponentName) const;

	/** Get skeletal mesh component for the Body (based on the specified name) of the owning actor. */
	UE_API USkeletalMeshComponent* GetBodySkelMeshComponent() const;

	/** Run the given AnimBP either on the skeletal mesh asset or on the instance, the component and initialize it afterwards. */
	UE_API void RunAndInitPostAnimBP(USkeletalMeshComponent* SkelMeshComponent, TSubclassOf<UAnimInstance> AnimInstance, bool bRunAsOverridePostAnimBP, bool bReinitAnimInstances = true) const;

	/** Load and run AnimBP on the given skeletal mesh component. */
	UE_API void LoadAndRunAnimBP(TSoftClassPtr<UAnimInstance> AnimBlueprint, TWeakObjectPtr<USkeletalMeshComponent> SkelMeshComponent, bool bIsPostProcessingAnimBP, bool bRunAsOverridePostAnimBP = false);

	/** Post-loading callback to be used to connect AnimBP variables. */
	UE_API virtual void PostInitAnimBP(USkeletalMeshComponent* SkeletalMeshComponent, UAnimInstance* AnimInstance) const;

	UE_API void PostConnectAnimBPVariables(const FMetaHumanCustomizableBodyPart& BodyPart, USkeletalMeshComponent* SkeletalMeshComponent, UAnimInstance* AnimInstance) const;

	//////////////////////////////////////////////////////////////////////////////
	// Body
	//////////////////////////////////////////////////////////////////////////////
	UE_API void SetFollowBody(USkeletalMeshComponent* SkelMeshComponent) const;

	UPROPERTY(EditDefaultsOnly, Category = Body, AdvancedDisplay)
	FString BodyComponentName = "Body";

	UPROPERTY()
	EMetaHumanBodyType BodyType = EMetaHumanBodyType::BlendableBody;

	/*
	 * Enable evaluation of the body procedural control rig, the head movement IK control rig and the arm and finger pose drivers.
	 * When enabled, evaluation for LODs can still be controlled via the Body LOD threshold.
	 * When disabled, the body procedural control rig, the head movement IK control rig and the arm and finger pose drivers will not be evaluated which will result in higher performance but decreases mesh deformation quality.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Body)
	bool bEnableBodyCorrectives = true;

	//////////////////////////////////////////////////////////////////////////////
	// Face
	//////////////////////////////////////////////////////////////////////////////
	UPROPERTY(EditDefaultsOnly, Category = Face, AdvancedDisplay)
	FString FaceComponentName = "Face";

	/*
	 * Max LOD level where Rig Logic is evaluated.
	 * For example if you have the threshold set to 2, it will evaluate until including LOD 2 (based on 0 index). In case the LOD level gets set to 3, it will stop evaluating Rig Logic.
	 * Setting it to -1 will always evaluate it and disable LODing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Face, meta = (DisplayName = "Facial Animation LOD Threshold"))
	int32 RigLogicLODThreshold = INDEX_NONE;

	/*
	 * Enable evaluation of neck correctives.
	 * When enabled, evaluation for LODs can still be controlled via the LOD threshold.
	 * When disabled, neck correctives will not be evaluated which will result in higher performance but decreases mesh deformation quality.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Face, meta = (EditCondition = "BodyType!=EMetaHumanBodyType::BlendableBody", EditConditionHides))
	bool bEnableNeckCorrectives = true;

	/*
	 * Max LOD level where neck correctives (pose drivers) are evaluated.
	 * For example if you have the threshold set to 2, it will evaluate until including LOD 2 (based on 0 index). In case the LOD level gets set to 3, it will stop evaluating neck correctives.
	 * Setting it to -1 will always evaluate it and disable LODing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Face, meta = (EditCondition = "bEnableNeckCorrectives==true && BodyType!=EMetaHumanBodyType::BlendableBody", EditConditionHides))
	int32 NeckCorrectivesLODThreshold = INDEX_NONE;

	/*
	 * Enable evaluation of the neck procedural control rig.
	 * When enabled, evaluation for LODs can still be controlled via the LOD threshold.
	 * When disabled, the neck procedural control rig will not be evaluated which will result in higher performance but decreases mesh deformation quality.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Face, meta = (EditCondition = "BodyType!=EMetaHumanBodyType::BlendableBody", EditConditionHides))
	bool bEnableNeckProcControlRig = true;

	/*
	 * Max LOD level where the neck procedural control rig is evaluated.
	 * For example if you have the threshold set to 2, it will evaluate until including LOD 2 (based on 0 index). In case the LOD level gets set to 3, it will stop evaluating the neck procedural control rig.
	 * Setting it to -1 will always evaluate it and disable LODing.
	 */
	UPROPERTY(EditDefaultsOnly, Category = Face, meta = (EditCondition = "bEnableNeckProcControlRig==true && BodyType!=EMetaHumanBodyType::BlendableBody", EditConditionHides))
	int32 NeckProcControlRigLODThreshold = INDEX_NONE;

	//////////////////////////////////////////////////////////////////////////////
	// Body Parts
	//////////////////////////////////////////////////////////////////////////////
	UPROPERTY(EditAnywhere, Category = BodyParts)
	FMetaHumanCustomizableBodyPart Torso;

	UPROPERTY(EditAnywhere, Category = BodyParts)
	FMetaHumanCustomizableBodyPart Legs;

	UPROPERTY(EditAnywhere, Category = BodyParts)
	FMetaHumanCustomizableBodyPart Feet;
};

#undef UE_API
