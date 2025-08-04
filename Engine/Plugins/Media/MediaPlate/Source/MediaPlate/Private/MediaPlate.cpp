// Copyright Epic Games, Inc. All Rights Reserved.

#include "MediaPlate.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HoldoutCompositeComponent.h"
#include "CompositeCoreSubsystem.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MediaPlateComponent.h"
#include "MediaPlateModule.h"
#include "MediaTexture.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/FortniteMainBranchObjectVersion.h"

#if WITH_EDITOR
#include "Editor.h"
#include "MediaPlateAssetUserData.h"
#include "Misc/MessageDialog.h"
#include "UObject/ObjectSaveContext.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(MediaPlate)

#define LOCTEXT_NAMESPACE "MediaPlate"

FLazyName AMediaPlate::MediaPlateComponentName(TEXT("MediaPlateComponent0"));
FLazyName AMediaPlate::MediaTextureName("MediaTexture");

namespace UE::MediaPlate::Private
{
	// TODO: Could be removed (with associated materials) now that we've deprecated overlay compositing.
	void ApplyTranslucencyScreenPercentageCVar(int32 InBasis)
	{
		static IConsoleVariable* TranslucencySPBasisCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Translucency.ScreenPercentage.Basis"));
		if (TranslucencySPBasisCVar->GetInt() != InBasis)
		{
			if (InBasis)
			{
				UE_LOG(LogMediaPlate, Warning, TEXT("Setting 'r.Translucency.ScreenPercentage.Basis' to 1. For media plates with overlay materials, please apply this console variable permanently to your project."));
			}
			else
			{
				UE_LOG(LogMediaPlate, Warning, TEXT("Setting 'r.Translucency.ScreenPercentage.Basis' to 0."));
			}

			TranslucencySPBasisCVar->Set(InBasis);
		}
	}

	/** Check for the presence of the (now deprecated) overlay composite material. */
	bool HasDeprecatedOverlayCompositeMaterial(AMediaPlate& InMediaPlate)
	{
		if (UMaterialInterface* OverlayMaterial = InMediaPlate.GetCurrentOverlayMaterial())
		{
			if (UMaterial* ParentMaterial = OverlayMaterial->GetMaterial())
			{
				return ParentMaterial->GetPathName() == TEXT("/MediaPlate/M_MediaPlate_OverlayComp.M_MediaPlate_OverlayComp");
			}
		}

		return false;
	}
}

AMediaPlate::AMediaPlate(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	// Set up media component.
	MediaPlateComponent = CreateDefaultSubobject<UMediaPlateComponent>(MediaPlateComponentName);

	// Set up static mesh component.
	StaticMeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StaticMeshComponent"));
	StaticMeshComponent->SetupAttachment(RootComponent);
	StaticMeshComponent->bCastStaticShadow = false;
	StaticMeshComponent->bCastDynamicShadow = false;
	MediaPlateComponent->StaticMeshComponent = StaticMeshComponent;

	// 16:9 by default since most videos are that format.
	MediaPlateComponent->SetAspectRatio(16.0f/9.0f);

	// Hook up mesh.
	struct FConstructorStatics
	{
		ConstructorHelpers::FObjectFinder<UStaticMesh> Plane;
		FConstructorStatics()
			: Plane(TEXT("/MediaPlate/SM_MediaPlateScreen"))
		{}
	};

	static FConstructorStatics ConstructorStatics;
	if (ConstructorStatics.Plane.Object != nullptr)
	{
		StaticMeshComponent->SetStaticMesh(ConstructorStatics.Plane.Object);
	}

#if WITH_EDITOR
	// If we are not the class default object...
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		// Hook into pre/post save.
		FEditorDelegates::PreSaveWorldWithContext.AddUObject(this, &AMediaPlate::OnPreSaveWorld);
		FEditorDelegates::PostSaveWorldWithContext.AddUObject(this, &AMediaPlate::OnPostSaveWorld);
	}
#endif // WITH_EDITOR
}

void AMediaPlate::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		// The holdout composite component was replaced with a checkbox.
		if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::MediaPlateHoldoutComponentRemoval)
		{
			if (UHoldoutCompositeComponent* HoldoutCompositeComponent = FindComponentByClass<UHoldoutCompositeComponent>())
			{
				bEnableHoldoutComposite = HoldoutCompositeComponent->IsEnabled();

				HoldoutCompositeComponent->DestroyComponent();
			}

			//TODO (5.7): Remove deprecated overlay composite material & enable holdout composite.
		}
	}
#endif
}

void AMediaPlate::PostActorCreated()
{
	Super::PostActorCreated();

#if WITH_EDITOR
	if (GIsEditor)
	{
		// Set which material to use.
		UseDefaultMaterial();
	}
#endif
}

void AMediaPlate::PostRegisterAllComponents()
{
	Super::PostRegisterAllComponents();

#if WITH_EDITOR
	// If this media plate is in a Sequencer media track,
	// and we go though a convert to spawnable/possessible,
	// then a multi user client will not receive the correct material
	// as multi user cannot send materials that are not assets.
	// So if we have an override material but its nullptr, then just use the default material.
	if ((StaticMeshComponent != nullptr) &&
		(StaticMeshComponent->GetNumOverrideMaterials() > 0) &&
		(StaticMeshComponent->OverrideMaterials[0] == nullptr))
	{
		UseDefaultMaterial();		
	}
	
	AddAssetUserData();
#endif // WITH_EDITOR

	if (IsValid(StaticMeshComponent))
	{
		if (IsValid(StaticMeshComponent->OverlayMaterial))
		{
			UE::MediaPlate::Private::ApplyTranslucencyScreenPercentageCVar(1);
		}

		ConditionallyEnableHoldoutComposite();
	}
}

void AMediaPlate::BeginDestroy()
{
	Super::BeginDestroy();
}

void AMediaPlate::SetHoldoutCompositeEnabled(bool bInEnabled)
{
	bEnableHoldoutComposite = bInEnabled;

	UCompositeCoreSubsystem* CompositeSubsystem = UWorld::GetSubsystem<UCompositeCoreSubsystem>(GetWorld());
	if (IsValid(CompositeSubsystem))
	{
		if (bEnableHoldoutComposite)
		{
			// Note: re-registering the same component is safe.
			CompositeSubsystem->RegisterPrimitive(StaticMeshComponent);
		}
		else
		{
			// Note: destroyed components are also automatically removed by the system.
			CompositeSubsystem->UnregisterPrimitive(StaticMeshComponent);
		}
	}
}

bool AMediaPlate::IsHoldoutCompositeEnabled() const
{
	return bEnableHoldoutComposite;
}

UMaterialInterface* AMediaPlate::GetCurrentMaterial() const
{
	if (IsValid(StaticMeshComponent))
	{
		return StaticMeshComponent->GetMaterial(0);
	}

	return nullptr;
}

UMaterialInterface* AMediaPlate::GetCurrentOverlayMaterial() const
{
	if (IsValid(StaticMeshComponent))
	{
		return StaticMeshComponent->GetOverlayMaterial();
	}

	return nullptr;
}

void AMediaPlate::ConditionallyEnableHoldoutComposite()
{
	if (bEnableHoldoutComposite)
	{
		SetHoldoutCompositeEnabled(true);
	}
}

#if WITH_EDITOR

void AMediaPlate::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(ThisClass, bEnableHoldoutComposite))
	{
		SetHoldoutCompositeEnabled(bEnableHoldoutComposite);
		
		// TODO (5.7): Remove once we fully migrate to holdout composite.
		if (bEnableHoldoutComposite && UE::MediaPlate::Private::HasDeprecatedOverlayCompositeMaterial(*this))
		{
			static TOptional<EAppReturnType::Type> bYesAllNoAll;
			EAppReturnType::Type Response = EAppReturnType::No;

			if (bYesAllNoAll.IsSet())
			{
				Response = bYesAllNoAll.GetValue();
			}
			else
			{
				Response = FMessageDialog::Open(EAppMsgType::YesNoYesAllNoAll, LOCTEXT("RemoveOverlayMaterial", "The deprecated overlay composite material conflicts with holdout composite.\nProceed to delete it?"));

				if (Response == EAppReturnType::YesAll || Response == EAppReturnType::NoAll)
				{
					bYesAllNoAll = Response;
				}
			}

			if (Response == EAppReturnType::Yes || Response == EAppReturnType::YesAll)
			{
				StaticMeshComponent->SetOverlayMaterial(nullptr);
			}
		}
	}
}

void AMediaPlate::OnStaticMeshChange()
{
	ApplyCurrentMaterial();

	ConditionallyEnableHoldoutComposite();
}

void AMediaPlate::UseDefaultMaterial()
{
	UMaterial* DefaultMaterial = LoadObject<UMaterial>(NULL, TEXT("/MediaPlate/M_MediaPlate_Opaque"), NULL, LOAD_None, NULL);
	
	ApplyMaterial(DefaultMaterial);

	if (IsValid(StaticMeshComponent))
	{
		StaticMeshComponent->SetOverlayMaterial(nullptr);
		
		LastOverlayMaterial = nullptr;
	}
}

void AMediaPlate::ApplyCurrentMaterial()
{
	UMaterialInterface* MaterialInterface = GetCurrentMaterial();
	
	if ((MaterialInterface != nullptr) && (LastMaterial != MaterialInterface))
	{
		ApplyMaterial(MaterialInterface);
	}

	UMaterialInterface* OverlayMaterialInterface = GetCurrentOverlayMaterial();

	if ((OverlayMaterialInterface != nullptr) && (LastOverlayMaterial != OverlayMaterialInterface))
	{
		ApplyOverlayMaterial(OverlayMaterialInterface);
	}
}

UMaterialInterface* AMediaPlate::CreateMaterialInstanceConstant(UMaterialInterface* InMaterial)
{
	// Change M_ to MI_ in material name and then generate a unique one.
	FString MaterialName = InMaterial->GetName();
	if (MaterialName.StartsWith(TEXT("M_")))
	{
		MaterialName.InsertAt(1, TEXT("I"));
	}
	FName MaterialUniqueName = MakeUniqueObjectName(StaticMeshComponent, UMaterialInstanceConstant::StaticClass(),
		FName(*MaterialName));

	// Create instance.
	UMaterialInstanceConstant* MaterialInstance =
		NewObject<UMaterialInstanceConstant>(StaticMeshComponent, MaterialUniqueName, RF_Transactional);
	MaterialInstance->SetParentEditorOnly(InMaterial);
	MaterialInstance->CopyMaterialUniformParametersEditorOnly(InMaterial);
	MaterialInstance->SetTextureParameterValueEditorOnly(
		FMaterialParameterInfo(MediaTextureName),
		MediaPlateComponent->GetMediaTexture());
	MaterialInstance->PostEditChange();

	// We force call post-load to indirectly call UpdateParameters() (for integration with VPUtilities plugin).
	MaterialInstance->PostLoad();

	return MaterialInstance;
}

void AMediaPlate::ApplyMaterial(UMaterialInterface* Material)
{
	if (Material != nullptr && StaticMeshComponent != nullptr)
	{
		if (GEditor == nullptr)
		{
			UMaterialInstanceDynamic* MaterialDynamic = StaticMeshComponent->CreateAndSetMaterialInstanceDynamicFromMaterial(0, Material);
			SetMIDParameters(MaterialDynamic);
			LastMaterial = MaterialDynamic;
		}
		else
		{
			UMaterialInterface* Result = nullptr;

			// See if we can modify this material.
			bool bCanModify = true;
			FMediaPlateModule* MediaPlateModule = FModuleManager::GetModulePtr<FMediaPlateModule>("MediaPlate");
			if (MediaPlateModule != nullptr)
			{
				MediaPlateModule->OnMediaPlateApplyMaterial.Broadcast(Material, this, bCanModify);
			}

			if (bCanModify == false)
			{
				MediaPlateComponent->SetNumberOfTextures(1);
				LastMaterial = Material;
			}
			else if (UMaterialInstanceDynamic* InMID = Cast<UMaterialInstanceDynamic>(Material))
			{
				SetMIDParameters(InMID);
				Result = InMID;
			}
			else if (Material->IsA(UMaterialInstance::StaticClass()))
			{
				UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Material, StaticMeshComponent);
				SetMIDParameters(MID);
				Result = MID;
			}
			else
			{
				MediaPlateComponent->SetNumberOfTextures(1);
				Result = CreateMaterialInstanceConstant(Material);
			}

			// Update static mesh.
			if (Result != nullptr)
			{
				StaticMeshComponent->Modify();
				StaticMeshComponent->SetMaterial(0, Result);

				LastMaterial = Result;
			}
		}
	}
}

void AMediaPlate::ApplyOverlayMaterial(UMaterialInterface* InOverlayMaterial)
{
	if (InOverlayMaterial != nullptr && StaticMeshComponent != nullptr)
	{
		UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(InOverlayMaterial);

		if (GEditor == nullptr)
		{
			if (MID == nullptr)
			{
				// Create and set the dynamic material instance.
				MID = UMaterialInstanceDynamic::Create(InOverlayMaterial, StaticMeshComponent);
			}
			StaticMeshComponent->SetOverlayMaterial(MID);
			SetMIDParameters(MID);
			LastOverlayMaterial = MID;
		}
		else
		{
			UMaterialInterface* Result = nullptr;

			if (MID != nullptr)
			{
				SetMIDParameters(MID);
				Result = MID;
			}
			else if (InOverlayMaterial->IsA(UMaterialInstance::StaticClass()))
			{
				UMaterialInstanceDynamic* OverlayMID = UMaterialInstanceDynamic::Create(InOverlayMaterial, StaticMeshComponent);
				SetMIDParameters(OverlayMID);
				Result = OverlayMID;
			}
			else
			{
				MediaPlateComponent->SetNumberOfTextures(1);
				Result = CreateMaterialInstanceConstant(InOverlayMaterial);
			}

			// Update static mesh.
			if (Result != nullptr)
			{
				StaticMeshComponent->Modify();
				StaticMeshComponent->SetOverlayMaterial(Result);

				LastOverlayMaterial = Result;
			}
		}

		UE::MediaPlate::Private::ApplyTranslucencyScreenPercentageCVar(1);
	}
}

void AMediaPlate::SetMIDParameters(UMaterialInstanceDynamic* InMaterial)
{
	InMaterial->SetTextureParameterValue(MediaTextureName, MediaPlateComponent->GetMediaTexture());
	
	int32 NumTextures = 0;
	FString MediaTextureString = MediaTextureName.Resolve().ToString();
	for (const struct FTextureParameterValue& Param : InMaterial->TextureParameterValues)
	{
		FString Name = Param.ParameterInfo.Name.ToString();
		if (Name.StartsWith(MediaTextureString))
		{
			NumTextures++;
		}
	}
	MediaPlateComponent->SetNumberOfTextures(NumTextures);

	for (int32 Index = 0; Index < NumTextures; Index++)
	{
		FString NameString = MediaTextureString;
		if (Index != 0)
		{
			NameString.AppendInt(Index);
		}
		FName TextureParameterName = FName(*NameString);
		InMaterial->SetTextureParameterValue(TextureParameterName,
			MediaPlateComponent->GetMediaTexture(Index));
	}
}

void AMediaPlate::OnPreSaveWorld(UWorld* InWorld, FObjectPreSaveContext ObjectSaveContext)
{
	// We need to remove our asset user data before saving, as we do not need to save it out
	// and only use it to know when the static mesh component changes.
	RemoveAssetUserData();
}

void AMediaPlate::OnPostSaveWorld(UWorld* InWorld, FObjectPostSaveContext ObjectSaveContext)
{
	AddAssetUserData();
}

void AMediaPlate::AddAssetUserData()
{
	if (StaticMeshComponent != nullptr && !StaticMeshComponent->HasAssetUserDataOfClass(UMediaPlateAssetUserData::StaticClass()))
	{
		UMediaPlateAssetUserData* AssetUserData = NewObject<UMediaPlateAssetUserData>(GetTransientPackage());
		AssetUserData->OnPostEditChangeOwner.BindUObject(this, &AMediaPlate::OnStaticMeshChange);
		StaticMeshComponent->AddAssetUserData(AssetUserData);
	}
}

void AMediaPlate::RemoveAssetUserData()
{
	if (StaticMeshComponent != nullptr)
	{
		StaticMeshComponent->RemoveUserDataOfClass(UMediaPlateAssetUserData::StaticClass());
	}
}

#endif // WITH_EDITOR

#undef LOCTEXT_NAMESPACE

