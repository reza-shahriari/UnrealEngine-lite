// Copyright Epic Games, Inc. All Rights Reserved.

#include "Mask2D/AvaMask2DReadModifier.h"

#include "AvaMaskUtilities.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "GeometryMaskCanvas.h"
#include "GeometryMaskCanvasResource.h"
#include "GeometryMaskReadComponent.h"
#include "Handling/AvaHandleUtilities.h"
#include "Handling/AvaObjectHandleSubsystem.h"
#include "Handling/IAvaMaskMaterialCollectionHandle.h"
#include "Handling/IAvaMaskMaterialHandle.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Modifiers/ActorModifierCoreStack.h"

#define LOCTEXT_NAMESPACE "AvaMask2DReadModifier"

#if WITH_EDITOR
const TAvaPropertyChangeDispatcher<UAvaMask2DReadModifier> UAvaMask2DReadModifier::PropertyChangeDispatcher =
{
	{ GET_MEMBER_NAME_CHECKED(UAvaMask2DReadModifier, BaseOpacity), &UAvaMask2DReadModifier::OnBaseOpacityChanged }
};
#endif

void UAvaMask2DReadModifier::SetBaseOpacity(float InBaseOpacity)
{
	InBaseOpacity = FMath::Clamp(InBaseOpacity, 0.0f, 1.0f);
	if (!FMath::IsNearlyEqual(BaseOpacity, InBaseOpacity))
	{
		BaseOpacity = InBaseOpacity;
		OnBaseOpacityChanged();
	}
}

#if WITH_EDITOR
void UAvaMask2DReadModifier::PostEditChangeProperty(FPropertyChangedEvent& InPropertyChangedEvent)
{
	Super::PostEditChangeProperty(InPropertyChangedEvent);

	UAvaMask2DReadModifier::PropertyChangeDispatcher.OnPropertyChanged(this, InPropertyChangedEvent);
}

void UAvaMask2DReadModifier::OnMaterialCompiled(UMaterialInterface* InMaterial)
{
	if (InMaterial && InMaterial->GetTypedOuter<AActor>() == GetModifiedActor())
	{
		MarkModifierDirty();
	}
}
#endif

void UAvaMask2DReadModifier::Apply()
{
	Super::Apply();

	if (const UGeometryMaskCanvas* Canvas = GetCurrentCanvas())
	{
		for (TPair<TWeakObjectPtr<AActor>, FAvaMask2DActorData>& ActorDataPair : ActorData)
		{
			if (AActor* Actor = ActorDataPair.Key.Get())
			{
				if (!ApplyRead(Actor, ActorDataPair.Value))
				{
					return;
				}			
			}
		}
	}

	Next();
}

EBlendMode UAvaMask2DReadModifier::GetBlendMode() const
{
	return bUseBlur || bUseFeathering ? EBlendMode::BLEND_Translucent : EBlendMode::BLEND_Masked;
}

bool UAvaMask2DReadModifier::ApplyRead(AActor* InActor, FAvaMask2DActorData& InActorData)
{
	// Only add read/write component to actors with primitives
	if (!ActorSupportsMaskReadWrite(InActor))
	{
		// Ok to continue, just nothing done for this actor
		return true;
	}

	if (const UGeometryMaskCanvas* Canvas = GetCurrentCanvas())
	{
		// Get MaterialCollectionHandle
		const TSharedPtr<IAvaMaskMaterialCollectionHandle> MaterialCollectionHandle =
			UE::Ava::Internal::FindOrAddHandleByLambda(
				MaterialCollectionHandles
				, InActor
				, [this, InActor]()
				{
					return GetObjectHandleSubsystem()->MakeHandle<IAvaMaskMaterialCollectionHandle>(InActor, UE::AvaMask::Internal::HandleTag);
				});

		const UTexture* CanvasTexture = TryResolveCanvasTexture(InActor, InActorData);
		if (!CanvasTexture)
		{
			return true;
		}

		{
			FAvaMask2DSubjectParameters ApplyParameters;
			ApplyParameters.MaterialParameters.BlendMode = GetBlendMode();		
			ApplyParameters.MaterialParameters.CanvasName = Canvas->GetCanvasName();
			ApplyParameters.MaterialParameters.Texture = Canvas->GetTexture();
			ApplyParameters.MaterialParameters.BaseOpacity = BaseOpacity;
			ApplyParameters.MaterialParameters.Channel = Canvas->GetColorChannel();
			ApplyParameters.MaterialParameters.ChannelAsVector = UE::GeometryMask::MaskChannelEnumToVector[ApplyParameters.MaterialParameters.Channel];
			ApplyParameters.MaterialParameters.bInvert = bInverted;

			FGeometryMaskDrawingContext DrawingContext(Canvas->GetCanvasId().Level);

			const FIntPoint ViewportSize = Canvas->GetResource()->GetMaxViewportSize();
			const FVector2f ViewportPadding = FVector2f::One() - FVector2f(ViewportSize + Canvas->GetResource()->GetViewportPadding(DrawingContext)) / ViewportSize;
			ApplyParameters.MaterialParameters.Padding = ViewportPadding;

			ApplyParameters.MaterialParameters.bApplyFeathering = bUseFeathering;
			ApplyParameters.MaterialParameters.OuterFeatherRadius = OuterFeatherRadius;
			ApplyParameters.MaterialParameters.InnerFeatherRadius = InnerFeatherRadius;

			FInstancedStruct& MaterialCollectionData = MaterialCollectionHandleData.FindOrAdd(InActor, MaterialCollectionHandle->MakeDataStruct());
			MaterialCollectionHandle->ApplyModifiedState(ApplyParameters, MaterialCollectionData);
		}

		if (UGeometryMaskReadComponent* ReadComponent = FindOrAddMaskComponent<UGeometryMaskReadComponent>(InActor))
		{
			FGeometryMaskReadParameters Parameters = ReadComponent->GetParameters();
			Parameters.CanvasName = Channel;
			Parameters.ColorChannel = Canvas->GetColorChannel();
			Parameters.bInvert = bInverted;

			ReadComponent->SetParameters(Parameters);
		}

		// Only check for valid materials here, after ApplyModifiedState has occured once
		FText FailReason;
		if (!MaterialCollectionHandle->ValidateMaterials(FailReason))
		{
			Fail(FailReason);
			return false;
		}
	}

	return true;
}

void UAvaMask2DReadModifier::OnModifierCDOSetup(FActorModifierCoreMetadata& InMetadata)
{
	Super::OnModifierCDOSetup(InMetadata);

	InMetadata.SetName(TEXT("MaskRead"));
	InMetadata.SetCategory(TEXT("Rendering"));
	InMetadata.DisallowAfter(TEXT("MaskWrite"));
	InMetadata.DisallowBefore(TEXT("MaskWrite"));
#if WITH_EDITOR
	InMetadata.SetDisplayName(FText::FromString(TEXT("Masked Layer (Output)")));
	InMetadata.SetDescription(LOCTEXT("ModifierDescription", "Reads from a canvas texture and uses it on materials"));
#endif
}

void UAvaMask2DReadModifier::OnModifierEnabled(EActorModifierCoreEnableReason InReason)
{
	Super::OnModifierEnabled(InReason);

#if WITH_EDITOR
	UMaterial::OnMaterialCompilationFinished().AddUObject(this, &UAvaMask2DReadModifier::OnMaterialCompiled);
#endif
}

void UAvaMask2DReadModifier::OnModifierDisabled(EActorModifierCoreDisableReason InReason)
{
	Super::OnModifierDisabled(InReason);

#if WITH_EDITOR
	UMaterial::OnMaterialCompilationFinished().RemoveAll(this);
#endif
}

void UAvaMask2DReadModifier::SetupMaskComponent(UActorComponent* InComponent)
{
	if (!InComponent)
	{
		return;
	}

	if (IGeometryMaskReadInterface* MaskReader = Cast<IGeometryMaskReadInterface>(InComponent))
	{
		SetupMaskReadComponent(MaskReader);
	}
	
	UAvaMask2DBaseModifier::SetupMaskComponent(InComponent);
}

void UAvaMask2DReadModifier::SetupMaskReadComponent(IGeometryMaskReadInterface* InMaskReader)
{
	if (const UGeometryMaskCanvas* Canvas = GetCurrentCanvas())
	{
		// Set canvas name to read from
		FGeometryMaskReadParameters ReadParameters = InMaskReader->GetParameters();
		ReadParameters.CanvasName = Channel;
		ReadParameters.ColorChannel = Canvas->GetColorChannel();

		InMaskReader->SetParameters(ReadParameters);
	}
}

void UAvaMask2DReadModifier::OnBaseOpacityChanged()
{
	MarkModifierDirty();
}

#undef LOCTEXT_NAMESPACE
