// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuCOE/Nodes/CustomizableObjectNode.h"

#include "CustomizableObjectNodeProjectorConstant.generated.h"

namespace ENodeTitleType { enum Type : int; }

class UCustomizableObjectNodeRemapPins;
class UEdGraphPin;
class UObject;
struct FPropertyChangedEvent;


UCLASS()
class CUSTOMIZABLEOBJECTEDITOR_API UCustomizableObjectNodeProjectorConstant : public UCustomizableObjectNode
{
public:
	GENERATED_BODY()

	UCustomizableObjectNodeProjectorConstant();

	/**  */
	UPROPERTY(EditAnywhere, Category = CustomizableObject, meta = (ShowOnlyInnerProperties))
	FCustomizableObjectProjector Value;

	UPROPERTY(EditAnywhere, Category = UI, meta = (DisplayName = "Parameter UI Metadata"))
	FMutableParamUIMetadata ParamUIMetadata;

	UPROPERTY(EditAnywhere, Category = CustomizableObject, Meta = (DisplayName = "Projection Angle (degrees)"))
	float ProjectionAngle;

	UPROPERTY()
	uint32 ReferenceSkeletonIndex_DEPRECATED = 0;

	UPROPERTY(EditAnywhere, Category = ProjectorSnapToBone)
	FName ReferenceSkeletonComponent;

	UPROPERTY(EditAnywhere, Category = ProjectorSnapToBone)
	FName ProjectorBone;

	/** Temporary variable where to put the location information for bone combo box selection changes (in FCustomizableObjectNodeProjectorParameterDetails)*/
	FVector BoneComboBoxLocation;

	/** Temporary variable where to put the direction information for bone combo box selection changes (in FCustomizableObjectNodeProjectorParameterDetails)*/
	FVector BoneComboBoxForwardDirection;

	/** Temporary variable where to put the direction information for bone combo box selection changes (in FCustomizableObjectNodeProjectorParameterDetails)*/
	FVector BoneComboBoxUpDirection;

	// UObject interface.
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	// Begin EdGraphNode interface
	virtual FText GetNodeTitle(ENodeTitleType::Type TitleType) const override;
	virtual FLinearColor GetNodeTitleColor() const override;
	virtual FText GetTooltipText() const override;

	// UCustomizableObjectNode interface
	virtual void AllocateDefaultPins(UCustomizableObjectNodeRemapPins* RemapPins) override;
	virtual void BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion) override;

	// Own interface

	ECustomizableObjectProjectorType GetProjectorType() const;

	FVector GetProjectorPosition() const;

	void SetProjectorPosition(const FVector& Position);

	FVector GetProjectorDirection() const;

	void SetProjectorDirection(const FVector& Direction);

	FVector GetProjectorUp() const;

	void SetProjectorUp(const FVector& Up);

	FVector GetProjectorScale() const;

	void SetProjectorScale(const FVector& Scale);

	float GetProjectorAngle() const;

	void SetProjectorAngle(float Angle);

private:

	UPROPERTY()
	ECustomizableObjectProjectorType ProjectionType_DEPRECATED;
};

