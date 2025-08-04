// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuCOE/Nodes/CustomizableObjectNodeModifierClipMorphDetails.h"

#include "DetailLayoutBuilder.h"
#include "Engine/SkeletalMesh.h"
#include "IDetailsView.h"
#include "MuCOE/CustomizableObjectEditorUtilities.h"
#include "MuCOE/CustomizableObjectGraph.h"
#include "MuCOE/GraphTraversal.h"
#include "MuCOE/Nodes/CustomizableObjectNodeModifierClipMorph.h"
#include "MuCOE/UnrealEditorPortabilityHelpers.h"
#include "PropertyCustomizationHelpers.h"
#include "MuCO/CustomizableObjectPrivate.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/STextComboBox.h"

class UCustomizableObjectNodeObject;


#define LOCTEXT_NAMESPACE "MeshClipMorphDetails"


TSharedRef<IDetailCustomization> FCustomizableObjectNodeModifierClipMorphDetails::MakeInstance()
{
	return MakeShareable(new FCustomizableObjectNodeModifierClipMorphDetails);
}


void FCustomizableObjectNodeModifierClipMorphDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	FCustomizableObjectNodeModifierBaseDetails::CustomizeDetails(DetailBuilder);

	Node = nullptr;
	DetailBuilderPtr = &DetailBuilder;

	TSharedPtr<const IDetailsView> DetailsView = DetailBuilder.GetDetailsViewSharedPtr();
	if (DetailsView && DetailsView->GetSelectedObjects().Num())
	{
		Node = Cast<UCustomizableObjectNodeModifierClipMorph>(DetailsView->GetSelectedObjects()[0].Get());
	}

	IDetailCategoryBuilder& BlocksCategory = DetailBuilder.EditCategory("MeshToClipAndMorph");
	DetailBuilder.HideProperty("BoneName");

	IDetailCategoryBuilder& MeshClipParametersCategory = DetailBuilder.EditCategory("MeshClipParameters");
	DetailBuilder.HideProperty("bInvertNormal");

	TSharedPtr<IPropertyHandle> ReferenceSkeletonIndexPropertyHandle = DetailBuilder.GetProperty("ReferenceSkeletonComponent");
	const FSimpleDelegate OnReferenceSkeletonComponentChangedDelegate = 
			FSimpleDelegate::CreateSP(this, &FCustomizableObjectNodeModifierClipMorphDetails::OnReferenceSkeletonComponentChanged);
	ReferenceSkeletonIndexPropertyHandle->SetOnPropertyValueChanged(OnReferenceSkeletonComponentChangedDelegate);

	if (Node)
	{
		SkeletalMesh = nullptr;
		TSharedPtr<FString> BoneToSelect;

		UCustomizableObject* CustomizableObject = Cast<UCustomizableObject>(Node->GetCustomizableObjectGraph()->GetOuter());

		if (CustomizableObject)
		{
			const FName ReferenceComponentName = Node->ReferenceSkeletonComponent;
			SkeletalMesh = CustomizableObject->GetComponentMeshReferenceSkeletalMesh(ReferenceComponentName);
		}

		if (SkeletalMesh)
		{
			BoneComboOptions.Empty();
				
			for (int32 i = 0; i < SkeletalMesh->GetRefSkeleton().GetRawBoneNum(); ++i)
			{
				FName BoneName = SkeletalMesh->GetRefSkeleton().GetBoneName(i);
				BoneComboOptions.Add(MakeShareable(new FString(BoneName.ToString())));

				if (BoneName == Node->BoneName)
				{
					BoneToSelect = BoneComboOptions.Last();
				}
			}

            BoneComboOptions.Sort(CompareNames);

			// Add them to the parent combo box
			TSharedRef<IPropertyHandle> BoneProperty = DetailBuilder.GetProperty("BoneName");

			BlocksCategory.AddCustomRow(LOCTEXT("ClipMorphDetails_BoneName", "Bone Name"))
			[
				SNew(SProperty, BoneProperty)
				.ShouldDisplayName(false)
				.CustomWidget()
				[
					SNew(SBorder)
					.BorderImage(UE_MUTABLE_GET_BRUSH("NoBorder"))
					.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
					.HAlign(EHorizontalAlignment::HAlign_Fill)
					[
						SNew(SHorizontalBox)
						+SHorizontalBox::Slot().HAlign(HAlign_Left).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text( LOCTEXT("ClipMorphDetails_BoneName", "Bone Name"))
							.Font(IDetailLayoutBuilder::GetDetailFont())
						]
						+SHorizontalBox::Slot().HAlign(HAlign_Fill)
						[
							SNew(STextComboBox)
							.OptionsSource(&BoneComboOptions)
							.InitiallySelectedItem(BoneToSelect)
							.OnSelectionChanged(this, &FCustomizableObjectNodeModifierClipMorphDetails::OnBoneComboBoxSelectionChanged, BoneProperty)
							.Font(IDetailLayoutBuilder::GetDetailFont())
						]
					]
				]
			];

			TSharedRef<IPropertyHandle> InvertPlaneProperty = DetailBuilder.GetProperty("bInvertNormal");
			MeshClipParametersCategory.AddCustomRow(LOCTEXT("ClipMorphDetails_PlaneNormal", "Invert plane normal"))
			[
				SNew(SProperty, BoneProperty)
				.ShouldDisplayName(false)
				.CustomWidget()
				[
					SNew(SBorder)
					.BorderImage(UE_MUTABLE_GET_BRUSH("NoBorder"))
					.Padding(FMargin(0.0f, 0.0f, 10.0f, 0.0f))
					.HAlign(EHorizontalAlignment::HAlign_Fill)
					[
						SNew(SHorizontalBox)
						+SHorizontalBox::Slot().HAlign(HAlign_Left).VAlign(VAlign_Center)
						[
							SNew(STextBlock)
							.Text( LOCTEXT("ClipMorphDetails_PlaneNormal", "Invert plane normal"))
						]
						+SHorizontalBox::Slot().HAlign(HAlign_Left)
						[
							SNew(SCheckBox)
							.OnCheckStateChanged(this, &FCustomizableObjectNodeModifierClipMorphDetails::OnInvertNormalCheckboxChanged, InvertPlaneProperty)
							.IsChecked(this, &FCustomizableObjectNodeModifierClipMorphDetails::GetInvertNormalCheckBoxState)
							.ToolTipText(LOCTEXT("ClipMorphDetails_InvertNormal_Tooltip", "Invert normal direction of the clip plane"))
						]
					]
				]
			];
		}
	}
	else
	{
		BlocksCategory.AddCustomRow(LOCTEXT("Node", "Node"))
		[
			SNew(STextBlock)
			.Text(LOCTEXT("ClipMorphDetails_InvertNormal_NodeNotFound", "Node not found"))
		];
	}
}


FVector FindBoneLocation(int32 BoneIndex, USkeletalMesh* SkeletalMesh)
{
	const TArray<FTransform>& BoneArray = SkeletalMesh->GetRefSkeleton().GetRefBonePose();

	int32 ParentIndex = BoneIndex;
	FVector Location = FVector::ZeroVector;

	while (ParentIndex >= 0)
	{
		Location = BoneArray[ParentIndex].TransformPosition(Location);
		ParentIndex = SkeletalMesh->GetRefSkeleton().GetParentIndex(ParentIndex);
	}

	return Location;
}


void FCustomizableObjectNodeModifierClipMorphDetails::OnBoneComboBoxSelectionChanged(TSharedPtr<FString> Selection, ESelectInfo::Type SelectInfo, TSharedRef<IPropertyHandle> BoneProperty)
{
	for (int OptionIndex = 0; OptionIndex < BoneComboOptions.Num(); ++OptionIndex)
	{
		if (BoneComboOptions[OptionIndex] == Selection)
		{
			//TArray<FVector> AffectedBoneLocations;

			//if (SkeletalMesh)
			//{
			//	const TArray<FTransform>& BoneArray = SkeletalMesh->RefSkeleton.GetRefBonePose();
			//	int32 BoneIndex = SkeletalMesh->RefSkeleton.FindBoneIndex(FName(**Selection));
			//	
			//	if (BoneIndex >= 0)
			//	{
			//		// Get tranform from skeleton root to selected bone
			//		int32 ParentIndex = BoneIndex;
			//		FTransform RootToBoneTransform = FTransform::Identity;

			//		while (ParentIndex >= 0)
			//		{
			//			RootToBoneTransform = RootToBoneTransform * BoneArray[ParentIndex];
			//			ParentIndex = SkeletalMesh->RefSkeleton.GetParentIndex(ParentIndex);
			//		}

			//		// Now that we have the transform from root to the selected bone, go down from this bone to the end of the bone chain hanging from it building a bounding box
			//		int32 CurrentBone = BoneIndex;

			//		FVector SelectedBoneLocationWithTransform = RootToBoneTransform.TransformPosition(FVector::ZeroVector);
			//		FVector SelectedBoneLocationFromSelection = BoneLocations[OptionIndex];
			//		check(FVector::DistSquared(SelectedBoneLocationWithTransform, SelectedBoneLocationFromSelection) < 0.01f);

			//		while (CurrentBone >= 0)
			//		{
			//			FVector CurrentBoneLocation = RootToBoneTransform.TransformPosition(FVector::ZeroVector);
			//			AffectedBoneLocations.Add(CurrentBoneLocation);

			//			check(FVector::DistSquared(CurrentBoneLocation, FindBoneLocation(CurrentBone, SkeletalMesh)) < 0.01f);

			//			int32 NextBone = -1;

			//			for (int32 i = 0; i < SkeletalMesh->RefSkeleton.GetRawBoneNum(); ++i)
			//			{
			//				if (SkeletalMesh->RefSkeleton.GetParentIndex(i) == CurrentBone)
			//				{
			//					NextBone = i;
			//					break;
			//				}
			//			}

			//			CurrentBone = NextBone;

			//			if (CurrentBone >= 0)
			//			{
			//				RootToBoneTransform = BoneArray[CurrentBone] * RootToBoneTransform;
			//			}
			//		}
			//	}
			//}

			FVector Location = FVector::ZeroVector;
			FVector Direction = FVector::ForwardVector;

			if (SkeletalMesh)
			{
				const TArray<FTransform>& BoneArray = SkeletalMesh->GetRefSkeleton().GetRefBonePose();
				int32 ParentIndex = SkeletalMesh->GetRefSkeleton().FindBoneIndex(FName(**Selection));

				FVector ChildLocation = FVector::ForwardVector;

				for (int32 i = 0; i < SkeletalMesh->GetRefSkeleton().GetRawBoneNum(); ++i)
				{
					if (SkeletalMesh->GetRefSkeleton().GetParentIndex(i) == ParentIndex)
					{
						ChildLocation = BoneArray[i].TransformPosition(FVector::ZeroVector);
						break;
					}
				}

				//Direction = ChildLocation;

				while (ParentIndex >= 0)
				{
					Location = BoneArray[ParentIndex].TransformPosition(Location);
					ChildLocation = BoneArray[ParentIndex].TransformPosition(ChildLocation);
					//Direction = BoneArray[ParentIndex].TransformVector(Direction);
					ParentIndex = SkeletalMesh->GetRefSkeleton().GetParentIndex(ParentIndex);
				}

				Direction = (ChildLocation - Location).GetSafeNormal();
			}
			 
			if (Node)
			{
				Node->Origin = Location;
				Node->Normal = Direction;
			}

			// Set the bone property after Node Origin and Normal update, otherwise the 
			// viewport gizmo will be constructed with the old values.
			BoneProperty->SetValue(*BoneComboOptions[OptionIndex].Get());
			
			return;
		}
	}

	BoneProperty->SetValue(FString());
}


void FCustomizableObjectNodeModifierClipMorphDetails::OnInvertNormalCheckboxChanged(ECheckBoxState CheckBoxState, TSharedRef<IPropertyHandle> InvertPlaneProperty)
{
	if (Node == nullptr)
	{
		return;
	}

	if (Node->bLocalStartOffset)
	{
		Node->StartOffset.Z *= -1.0f;
		Node->StartOffset.X *= -1.0f;
	}

	Node->Normal *= -1.0f;

	InvertPlaneProperty->SetValue(CheckBoxState == ECheckBoxState::Checked);
}


ECheckBoxState FCustomizableObjectNodeModifierClipMorphDetails::GetInvertNormalCheckBoxState() const
{
	if (Node == nullptr)
	{
		return ECheckBoxState::Unchecked;
	}

	return Node->bInvertNormal ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
}


void FCustomizableObjectNodeModifierClipMorphDetails::OnReferenceSkeletonComponentChanged()
{
	if (Node)
	{
		Node->BoneName = FName();
	}
	DetailBuilderPtr->ForceRefreshDetails();
}


#undef LOCTEXT_NAMESPACE
