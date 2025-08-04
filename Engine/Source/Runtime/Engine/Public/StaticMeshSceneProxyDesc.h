// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "PrimitiveSceneProxyDesc.h"

#include "StaticMeshSceneProxy.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/Material.h"
#include "Engine/Engine.h"
#include "StaticMeshComponentLODInfo.h"
#include "Rendering/NaniteResources.h"

class UBodySetup;

struct FStaticMeshSceneProxyDesc : public FPrimitiveSceneProxyDesc
{
	FStaticMeshSceneProxyDesc()
	{
		CastShadow = true;
		bUseAsOccluder = true;

		// bitfields init (until we switch to c++20)
		bReverseCulling = false;
#if STATICMESH_ENABLE_DEBUG_RENDERING
		bDrawMeshCollisionIfComplex = false;
		bDrawMeshCollisionIfSimple = false;
#endif
		bEvaluateWorldPositionOffset = true;
		bOverrideMinLOD = false;

		bCastDistanceFieldIndirectShadow = false;
		bOverrideDistanceFieldSelfShadowBias = false;
		bEvaluateWorldPositionOffsetInRayTracing = false;
		bSortTriangles = false;

		bDisplayNaniteFallbackMesh = false;
		bDisallowNanite = false;
		bForceDisableNanite = false;
		bForceNaniteForMasked = false;

		bUseProvidedMaterialRelevance = false;
	}

	ENGINE_API FStaticMeshSceneProxyDesc(const UStaticMeshComponent*);

	ENGINE_API void InitializeFromStaticMeshComponent(const UStaticMeshComponent*);

	UE_DEPRECATED(5.5, "Use InitializeFromStaticMeshComponent instead.")
	void InitializeFrom(const UStaticMeshComponent* InComponent) { InitializeFromStaticMeshComponent(InComponent); }

	FORCEINLINE bool IsReverseCulling() const { return bReverseCulling; }
	FORCEINLINE bool IsDisallowNanite() const { return bDisallowNanite; }
	FORCEINLINE bool IsForceDisableNanite() const { return bForceDisableNanite; }
	FORCEINLINE bool IsForceNaniteForMasked() const { return bForceNaniteForMasked; }
	FORCEINLINE int32 GetForcedLodModel() const { return ForcedLodModel; }
	FORCEINLINE bool IsDisplayNaniteFallbackMesh() const { return bDisplayNaniteFallbackMesh; }

	UStaticMesh* StaticMesh = nullptr;
	TArrayView<TObjectPtr<UMaterialInterface>>	OverrideMaterials;
	TObjectPtr<class UMaterialInterface> OverlayMaterial;
	float OverlayMaterialMaxDrawDistance = 0.0f;
	TArray<TObjectPtr<UMaterialInterface>> MaterialSlotsOverlayMaterial;

	int32 ForcedLodModel = 0;
	int32 MinLOD = 0;
	int32 WorldPositionOffsetDisableDistance = 0;
	float NanitePixelProgrammableDistance = 0.0f;
	
	uint32 bReverseCulling : 1 = false;
#if STATICMESH_ENABLE_DEBUG_RENDERING
	uint32 bDrawMeshCollisionIfComplex : 1;
	uint32 bDrawMeshCollisionIfSimple : 1;
#endif
	uint32 bEvaluateWorldPositionOffset : 1;
	uint32 bOverrideMinLOD : 1;

	uint32 bCastDistanceFieldIndirectShadow : 1;
	uint32 bOverrideDistanceFieldSelfShadowBias : 1;
	uint32 bEvaluateWorldPositionOffsetInRayTracing :1;
	uint32 bSortTriangles :1;

	uint32 bDisplayNaniteFallbackMesh : 1;
	uint32 bDisallowNanite : 1;
	uint32 bForceDisableNanite : 1;
	uint32 bForceNaniteForMasked : 1;

	uint32 bUseProvidedMaterialRelevance:1;
	uint32 bUseProvidedCollisionResponseContainer:1;

	float DistanceFieldSelfShadowBias = 0;
	float DistanceFieldIndirectShadowMinVisibility = 0.1f;
	int32 StaticLightMapResolution = 0;

	ELightmapType LightmapType = ELightmapType::Default;

#if WITH_EDITORONLY_DATA
	float StreamingDistanceMultiplier = 1.0f;
	TArrayView<uint32> MaterialStreamingRelativeBoxes;
	int32 SectionIndexPreview = INDEX_NONE;
	int32 MaterialIndexPreview = INDEX_NONE;
	int32 SelectedEditorMaterial = INDEX_NONE;
	int32 SelectedEditorSection = INDEX_NONE;

	float TextureStreamingTransformScale = 1.0f;
#endif

	const Nanite::FResources*	NaniteResources = nullptr;

	TArrayView<struct FStaticMeshComponentLODInfo> LODData;
	FMaterialRelevance MaterialRelevance;

	UTexture* MeshPaintTexture = nullptr;
	int32 MeshPaintTextureCoordinateIndex = 0;

	UTexture* MaterialCacheTexture = nullptr;

	UStaticMesh* GetStaticMesh() const { return StaticMesh; }

	UBodySetup* BodySetup = nullptr;
	UBodySetup* GetBodySetup() const
	{
		if (BodySetup)
		{
			return BodySetup;
		}

		if (GetStaticMesh())
		{
			return GetStaticMesh()->GetBodySetup();
		}

		return nullptr;
	}

	TOptional<FCollisionResponseContainer> CollisionResponseContainer;
	
	UObject* LODParentPrimitive = nullptr;
	UObject* GetLODParentPrimitive() const { return LODParentPrimitive; }

	const Nanite::FResources* GetNaniteResources() const { return NaniteResources; }
	
	ENGINE_API bool HasValidNaniteData() const;

	int32 GetNumMaterials() const { return GetStaticMesh() ? GetStaticMesh()->GetStaticMaterials().Num() : 0; }

	UMaterialInterface* GetOverlayMaterial() const { return OverlayMaterial; }
	float GetOverlayMaterialMaxDrawDistance() const { return OverlayMaterialMaxDrawDistance; }
	void GetMaterialSlotsOverlayMaterial(TArray<TObjectPtr<UMaterialInterface>>& OutMaterialSlotsOverlayMaterial) const { OutMaterialSlotsOverlayMaterial = MaterialSlotsOverlayMaterial; }

	ENGINE_API UMaterialInterface* GetMaterial(int32 MaterialIndex, bool bDoingNaniteMaterialAudit = false, bool bIgnoreNaniteOverrideMaterials = false) const;
	
	ENGINE_API bool ShouldCreateNaniteProxy(Nanite::FMaterialAudit* OutNaniteMaterials = nullptr) const;
	ENGINE_API bool UseNaniteOverrideMaterials(bool bDoingMaterialAudit) const;
	
	UMaterialInterface* GetNaniteAuditMaterial(int32 MaterialIndex) const
	{
		return GetMaterial(MaterialIndex, true);
	}

	void SetMaterialRelevance(const FMaterialRelevance& InRelevance) { MaterialRelevance = InRelevance; bUseProvidedMaterialRelevance = true; }
	ENGINE_API FMaterialRelevance GetMaterialRelevance(ERHIFeatureLevel::Type InFeatureLevel) const;
	int32 GetStaticLightMapResolution() const { return StaticLightMapResolution; }
	
	FTextureResource* GetMeshPaintTextureResource() const;
	
	FTextureResource* GetMaterialCacheTextureResource() const;

	void SetCollisionResponseToChannels(const FCollisionResponseContainer& InContainer) 
	{ 
		if (&InContainer != &FCollisionResponseContainer::GetDefaultResponseContainer())
		{
			CollisionResponseContainer = InContainer;
		}
	}

	const FCollisionResponseContainer& GetCollisionResponseToChannels() const
	{
		if (CollisionResponseContainer.IsSet())
		{
			return *CollisionResponseContainer;
		}

		return FCollisionResponseContainer::GetDefaultResponseContainer();
	}

	UObject* GetObjectForPropertyColoration() const { return Component; }

	//@todo: share color selection logic according to mobility with USMC?
	FColor	WireframeColor = FColor(0, 255, 255, 255);
	FColor GetWireframeColor() const { return WireframeColor; }

	ENGINE_API virtual void GetUsedMaterials(TArray<UMaterialInterface*>& OutMaterials, bool bGetDebugMaterials = false) const override;

	const UStaticMeshComponent* GetUStaticMeshComponent() const { return Cast<UStaticMeshComponent>(Component); }
};
