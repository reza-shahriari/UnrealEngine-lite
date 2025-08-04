// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SkeletalMesh.cpp: Unreal skeletal mesh and animation implementation.
=============================================================================*/

#include "Engine/SkeletalMesh.h"

#include "Animation/MeshDeformerGeometry.h"
#include "DataDrivenShaderPlatformInfo.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMeshSampling.h"
#include "EngineStats.h"
#include "Engine/SkinnedAsset.h"
#include "Engine/SkinnedAssetAsyncCompileUtils.h"
#include "Engine/SkinnedAssetCommon.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "Materials/MaterialRenderProxy.h"
#include "MaterialDomain.h"
#include "Animation/Skeleton.h"
#include "Components/SkeletalMeshComponent.h"
#include "ComponentReregisterContext.h"
#include "Math/ScaleRotationTranslationMatrix.h"
#include "UObject/AssetRegistryTagsContext.h"
#include "UObject/FrameworkObjectVersion.h"
#include "PhysicsEngine/SphereElem.h"
#include "UObject/NiagaraObjectVersion.h"
#include "PhysicsEngine/SphylElem.h"
#include "UObject/ObjectSaveContext.h"
#include "PrimitiveViewRelevance.h"
#include "UObject/Package.h"
#include "RenderUtils.h"
#include "AssetCompilingManager.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "SceneInterface.h"
#include "EngineUtils.h"
#include "EditorSupportDelegates.h"
#include "SkeletalMeshLegacyCustomVersions.h"
#include "SkeletalMeshSceneProxy.h"
#include "SkeletalRenderPublic.h"
#include "PrimitiveDrawingUtils.h"
#include "PhysicsEngine/BodySetup.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "PhysicsEngine/SkeletalBodySetup.h"
#include "Engine/AssetUserData.h"
#include "Animation/NodeMappingContainer.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/NaniteResources.h"
#include "Rendering/RenderCommandPipes.h"
#include "AnimationRuntime.h"
#include "Animation/AnimSequence.h"
#include "Streaming/SkeletalMeshUpdate.h"
#include "Algo/MaxElement.h"
#include "UObject/FortniteMainBranchObjectVersion.h"
#include "UObject/ReleaseObjectVersion.h"
#include "GPUSkinCacheVisualizationData.h"
#include "GPUSkinCache.h"
#include "MeshPaintVisualize.h"

#if WITH_EDITOR
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "IMeshBuilderModule.h"
#include "IMeshReductionManagerModule.h"
#include "IMeshReductionInterfaces.h"
#include "SkinnedAssetCompiler.h"
#include "MeshUtilities.h"
#include "NaniteBuilder.h"
#include "Engine/SkeletalMeshEditorData.h"
#include "DerivedDataCacheInterface.h"
#include "DerivedDataCacheKey.h"
#include "Engine/RendererSettings.h"
#include "Misc/DataValidation.h"
#else
#include "Interfaces/ITargetPlatform.h"
#endif // #if WITH_EDITOR

#include "UObject/DevObjectVersion.h"
#include "UnrealEngine.h"

#include "EditorFramework/AssetImportData.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Components/BrushComponent.h"

#include "ClothingAssetBase.h"
#include "Async/Async.h"
#include "Misc/UObjectToken.h"

#if WITH_EDITOR
#include "ScopedTransaction.h"
#endif
#include "SkeletalDebugRendering.h"
#include "Misc/RuntimeErrors.h"
#include "PlatformInfo.h"

#if RHI_RAYTRACING
#include "RayTracingInstance.h"
#endif
#include "Animation/SkinWeightProfileManager.h"
#include "BoneWeights.h"
#include "SkeletalMeshAttributes.h"
#include "Algo/AnyOf.h"
#include "Logging/StructuredLog.h"
#include "Rendering/SkeletalMeshHalfEdgeBufferAccessor.h"
#include "Animation/MeshDeformerCollection.h"
#include "Animation/MeshDeformer.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(SkeletalMesh)

#define LOCTEXT_NAMESPACE "SkeltalMesh"

DEFINE_LOG_CATEGORY(LogSkeletalMesh);

const FGuid FSkeletalMeshCustomVersion::GUID(0xD78A4A00, 0xE8584697, 0xBAA819B5, 0x487D46B4);
FCustomVersionRegistration GRegisterSkeletalMeshCustomVersion(FSkeletalMeshCustomVersion::GUID, FSkeletalMeshCustomVersion::LatestVersion, TEXT("SkeletalMeshVer"));

static TAutoConsoleVariable<int32> CVarSkeletalMeshLODMaterialReference(
	TEXT("r.SkeletalMesh.LODMaterialReference"),
	1,
	TEXT("Whether a material needs to be referenced by at least one unstripped mesh LOD to be considered as used."));

static TAutoConsoleVariable<int32> CVarRayTracingSkeletalMeshLODBias(
	TEXT("r.RayTracing.Geometry.SkeletalMeshes.LODBias"),
	0,
	TEXT("Global LOD bias for skeletal meshes in ray tracing.\n")
	TEXT("When non-zero, a different LOD level other than the predicted LOD level will be used for ray tracing. Advanced features like morph targets and cloth simulation may not work properly.\n")
	TEXT("Final LOD level to use in ray tracing is the sum of this global bias and the bias set on each skeletal mesh asset."),
	ECVF_RenderThreadSafe);

const TCHAR* GSkeletalMeshMinLodQualityLevelCVarName = TEXT("r.SkeletalMesh.MinLodQualityLevel");
const TCHAR* GSkeletalMeshMinLodQualityLevelScalabilitySection = TEXT("ViewDistanceQuality");
int32 GSkeletalMeshMinLodQualityLevel = -1;
static FAutoConsoleVariableRef CVarSkeletalMeshMinLodQualityLevel(
	GSkeletalMeshMinLodQualityLevelCVarName,
	GSkeletalMeshMinLodQualityLevel,
	TEXT("The quality level for the Min stripping LOD. \n"),
	FConsoleVariableDelegate::CreateStatic(&USkeletalMesh::OnLodStrippingQualityLevelChanged),
	ECVF_Scalability);

#if WITH_EDITOR
const FName USkeletalMesh::MorphNamesTag("MorphTargetNames");
const FString USkeletalMesh::MorphNamesTagDelimiter(TEXT(";"));

const FName USkeletalMesh::MaterialParamNamesTag("MaterialParamNames");
const FString USkeletalMesh::MaterialParamNamesTagDelimiter(TEXT(";"));
#endif

/*-----------------------------------------------------------------------------
FGPUSkinVertexBase
-----------------------------------------------------------------------------*/

/**
* Serializer
*
* @param Ar - archive to serialize with
*/
void TGPUSkinVertexBase::Serialize(FArchive& Ar)
{
	Ar << TangentX;
	Ar << TangentZ;
}

const FGuid FRecomputeTangentCustomVersion::GUID(0x5579F886, 0x933A4C1F, 0x83BA087B, 0x6361B92F);
// Register the custom version with core
FCustomVersionRegistration GRegisterRecomputeTangentCustomVersion(FRecomputeTangentCustomVersion::GUID, FRecomputeTangentCustomVersion::LatestVersion, TEXT("RecomputeTangentCustomVer"));

const FGuid FOverlappingVerticesCustomVersion::GUID(0x612FBE52, 0xDA53400B, 0x910D4F91, 0x9FB1857C);
// Register the custom version with core
FCustomVersionRegistration GRegisterOverlappingVerticesCustomVersion(FOverlappingVerticesCustomVersion::GUID, FOverlappingVerticesCustomVersion::LatestVersion, TEXT("OverlappingVerticeDetectionVer"));


FArchive& operator<<(FArchive& Ar, FMeshToMeshVertData& V)
{
	Ar.UsingCustomVersion(FReleaseObjectVersion::GUID);

	Ar << V.PositionBaryCoordsAndDist
		<< V.NormalBaryCoordsAndDist
		<< V.TangentBaryCoordsAndDist
		<< V.SourceMeshVertIndices[0]
		<< V.SourceMeshVertIndices[1]
		<< V.SourceMeshVertIndices[2]
		<< V.SourceMeshVertIndices[3];

	if (Ar.IsLoading() && 
		Ar.CustomVer(FReleaseObjectVersion::GUID) < FReleaseObjectVersion::WeightFMeshToMeshVertData)
	{
		// Old version had "uint32 Padding[2]"
		uint32 Discard;
		Ar << Discard << V.Padding;
	}
	else
	{
		// New version has "float Weight and "uint32 Padding"
		Ar << V.Weight << V.Padding;
	}

	return Ar;
}

FArchive& operator<<(FArchive& Ar, FClothBufferIndexMapping& ClothBufferIndexMapping)
{
	return Ar
		<< ClothBufferIndexMapping.BaseVertexIndex
		<< ClothBufferIndexMapping.MappingOffset
		<< ClothBufferIndexMapping.LODBiasStride;
}

/*-----------------------------------------------------------------------------
	FClothingAssetData
-----------------------------------------------------------------------------*/

FArchive& operator<<(FArchive& Ar, FClothingAssetData_Legacy& A)
{
	// Serialization to load and skip legacy clothing assets
	if( Ar.IsLoading() )
	{
		uint32 AssetSize;
		Ar << AssetSize;

		if( AssetSize > 0 )
		{
			// Load the binary blob data
			TArray<uint8> Buffer;
			Buffer.AddUninitialized( AssetSize );
			Ar.Serialize( Buffer.GetData(), AssetSize );
		}
	}
	else
	if( Ar.IsSaving() )
	{
		{
			uint32 AssetSize = 0;
			Ar << AssetSize;
		}
	}

	return Ar;
}


FSkeletalMeshClothBuildParams::FSkeletalMeshClothBuildParams()
	: TargetAsset(nullptr)
	, TargetLod(INDEX_NONE)
	, bRemapParameters(false)
	, AssetName("Clothing")
	, LodIndex(0)
	, SourceSection(0)
	, bRemoveFromMesh(false)
	, PhysicsAsset(nullptr)
{

}

#if WITH_EDITOR
FScopedSkeletalMeshPostEditChange::FScopedSkeletalMeshPostEditChange(USkeletalMesh* InSkeletalMesh, bool InbCallPostEditChange /*= true*/, bool InbReregisterComponents /*= true*/)
{
	SkeletalMesh = nullptr;
	bReregisterComponents = InbReregisterComponents;
	bCallPostEditChange = InbCallPostEditChange;
	RecreateExistingRenderStateContext = nullptr;
	ComponentReregisterContexts.Empty();
	//Validation of the data
	if (bCallPostEditChange && !bReregisterComponents)
	{
		//We never want to call PostEditChange without re register components, since PostEditChange will recreate the skeletalmesh render resources
		ensure(bReregisterComponents);
		bReregisterComponents = true;
	}
	if (InSkeletalMesh != nullptr)
	{
		//Only set a valid skeletal mesh
		SetSkeletalMesh(InSkeletalMesh);
	}
}

FScopedSkeletalMeshPostEditChange::~FScopedSkeletalMeshPostEditChange()
{
	if (SkeletalMesh)
	{
		//If decrementing the post edit change stack counter return 0 it mean we are the first scope call instance, so we have to call posteditchange and re register component
		if (SkeletalMesh->UnStackPostEditChange() == 0)
		{
			if (bCallPostEditChange)
			{
				SkeletalMesh->PostEditChange();
			}
		}
		if (bReregisterComponents && SkeletalMesh->IsCompiling())
		{
			//wait until the compilation is done
			FSkinnedAssetCompilingManager::Get().FinishCompilation({ SkeletalMesh });
		}
	}
	//If there is some re register data it will be delete when the destructor go out of scope. This will re register
}

void FScopedSkeletalMeshPostEditChange::SetSkeletalMesh(USkeletalMesh* InSkeletalMesh)
{
	//Skip only if we are calling post edit change
	bool bSkipCompiling = InSkeletalMesh->IsCompiling() && bCallPostEditChange;
	//Some parallel task may try to call post edit change, we must prevent it
	if (!IsInGameThread() || bSkipCompiling)
	{
		return;
	}
	//We cannot set a different skeletal mesh, check that it was construct with null
	check(SkeletalMesh == nullptr);
	//We can only set a valid skeletal mesh
	check(InSkeletalMesh != nullptr);

	SkeletalMesh = InSkeletalMesh;
	//If we are the first to increment, unregister the data we need to
	if (SkeletalMesh->StackPostEditChange() == 1)
	{
		//Only allocate data if we re register
		if (bReregisterComponents)
		{
			//Make sure all components using this skeletalmesh have there render ressources free
			RecreateExistingRenderStateContext = new FSkinnedMeshComponentRecreateRenderStateContext(InSkeletalMesh, false);

			// Now iterate over all skeletal mesh components and unregister them from the world, we will reregister them in the destructor
			for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
			{
				USkeletalMeshComponent* SkelComp = *It;
				if (SkelComp->GetSkeletalMeshAsset() == SkeletalMesh)
				{
					ComponentReregisterContexts.Add(new FComponentReregisterContext(SkelComp));
				}
			}
		}

		if (bCallPostEditChange)
		{
			//Make sure the render ressource use by the skeletalMesh is free, we will reconstruct them when a PostEditChange will be call
			SkeletalMesh->FlushRenderState();
		}
	}
}

FScopedSkeletalMeshReregisterContexts::FScopedSkeletalMeshReregisterContexts(USkeletalMesh* InSkeletalMesh)
{
	check(IsInGameThread());
	SkeletalMesh = InSkeletalMesh;
	if (!ensure(SkeletalMesh))
	{
		return;
	}

	//Make sure all components using this skeletalmesh have there render ressources free
	RecreateExistingRenderStateContext = new FSkinnedMeshComponentRecreateRenderStateContext(SkeletalMesh, false);

	// Now iterate over all skeletal mesh components and unregister them from the world, we will reregister them in the destructor
	for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
	{
		USkeletalMeshComponent* SkelComp = *It;
		if (SkelComp->GetSkeletalMeshAsset() == SkeletalMesh)
		{
			ComponentReregisterContexts.Add(new FComponentReregisterContext(SkelComp));
		}
	}
}

FScopedSkeletalMeshReregisterContexts::~FScopedSkeletalMeshReregisterContexts()
{
	check(IsInGameThread());
	if (!ensure(SkeletalMesh))
	{
		return;
	}

	if (SkeletalMesh->IsCompiling())
	{
		//wait until the compilation is done before reregister the component
		FSkinnedAssetCompilingManager::Get().FinishCompilation({ SkeletalMesh });
	}

	//Recreate the render context by calling delete
	if (RecreateExistingRenderStateContext)
	{
		delete RecreateExistingRenderStateContext;
	}

	//Component will be reregister when going out of scope
}

const FString& GetSkeletalMeshDerivedDataVersion()
{
	static FString CachedVersionString = FDevSystemGuids::GetSystemGuid(FDevSystemGuids::Get().SkeletalMeshDerivedDataVersion).ToString();
	return CachedVersionString;
}

namespace SkeletalMeshImpl
{
	// Condition specifically designed to detect if we're going to enter the non-thread safe part of FLODUtilities::SimplifySkeletalMeshLOD while building.
	static bool HasInlineReductions(USkeletalMesh* SkeletalMesh)
	{
		for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum(); ++LODIndex)
		{
			if (SkeletalMesh->IsReductionActive(LODIndex))
			{
				if (FSkeletalMeshLODInfo* LODInfo = SkeletalMesh->GetLODInfo(LODIndex))
				{
					// If the BaseLOD has the same index as the LOD itself, it means we're going to have inline reduction where
					// some original data will need to be saved, which is not currently thread-safe.
					if (!LODInfo->bHasBeenSimplified || LODInfo->ReductionSettings.BaseLOD == LODIndex)
					{
						return true;
					}
				}
			}
		}

		return false;
	}
}

#endif // #if WITH_EDITOR

/*-----------------------------------------------------------------------------
	USkeletalMesh
-----------------------------------------------------------------------------*/

USkeletalMesh::USkeletalMesh(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITORONLY_DATA
	SetImportedModel(MakeShareable(new FSkeletalMeshModel()));
	SetVertexColorGuid(FGuid());
	SetSupportLODStreaming(FPerPlatformBool(false));
	SetMaxNumStreamedLODs(FPerPlatformInt(0));
	// TODO: support saving some but not all optional LODs
	SetMaxNumOptionalLODs(FPerPlatformInt(0));
#endif
	SetMinLod(FPerPlatformInt(0));
	SetQualityLevelMinLod(0);
	MinQualityLevelLOD.SetQualityLevelCVarForCooking(GSkeletalMeshMinLodQualityLevelCVarName, GSkeletalMeshMinLodQualityLevelScalabilitySection);
	SetDisableBelowMinLodStripping(FPerPlatformBool(false));
PRAGMA_DISABLE_DEPRECATION_WARNINGS
	bSupportRayTracing = true;
	RayTracingMinLOD = 0;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

USkeletalMesh::USkeletalMesh(FVTableHelper& Helper)
	: Super(Helper)
{
}

PRAGMA_DISABLE_DEPRECATION_WARNINGS
USkeletalMesh::~USkeletalMesh() = default;
PRAGMA_ENABLE_DEPRECATION_WARNINGS

FSkeletalMeshRenderData* USkeletalMesh::GetSkeletalMeshRenderData() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SkeletalMeshRenderData);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return SkeletalMeshRenderData.Get();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::SetSkeletalMeshRenderData(TUniquePtr<FSkeletalMeshRenderData>&& InSkeletalMeshRenderData)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SkeletalMeshRenderData);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	SkeletalMeshRenderData = MoveTemp(InSkeletalMeshRenderData);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

FSkeletalMeshRenderData* USkeletalMesh::GetResourceForRendering() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SkeletalMeshRenderData);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return SkeletalMeshRenderData.Get();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

bool USkeletalMesh::HasValidNaniteData() const
{
	if (const FSkeletalMeshRenderData* RenderData = GetResourceForRendering())
	{
		return RenderData->HasValidNaniteData();
	}

	return false;
}

void USkeletalMesh::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		SetAssetImportData(NewObject<UAssetImportData>(this, TEXT("AssetImportData")));
	}
#endif
	Super::PostInitProperties();
}

FBoxSphereBounds USkeletalMesh::GetBounds() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::ExtendedBounds, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return ExtendedBounds;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

FBoxSphereBounds USkeletalMesh::GetImportedBounds() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::ImportedBounds, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return ImportedBounds;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::SetImportedBounds(const FBoxSphereBounds& InBounds)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::ImportedBounds);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	ImportedBounds = InBounds;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	CalculateExtendedBounds();
}

void USkeletalMesh::SetPositiveBoundsExtension(const FVector& InExtension)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::PositiveBoundsExtension);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	PositiveBoundsExtension = InExtension;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	CalculateExtendedBounds();
}

void USkeletalMesh::SetNegativeBoundsExtension(const FVector& InExtension)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::NegativeBoundsExtension);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	NegativeBoundsExtension = InExtension;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	CalculateExtendedBounds();
}

void USkeletalMesh::CalculateExtendedBounds()
{
	FBoxSphereBounds CalculatedBounds = GetImportedBounds();

	// Convert to Min and Max
	FVector Min = CalculatedBounds.Origin - CalculatedBounds.BoxExtent;
	FVector Max = CalculatedBounds.Origin + CalculatedBounds.BoxExtent;
	// Apply bound extensions
	Min -= GetNegativeBoundsExtension();
	Max += GetPositiveBoundsExtension();
	// Convert back to Origin, Extent and update SphereRadius
	CalculatedBounds.Origin = (Min + Max) / 2;
	CalculatedBounds.BoxExtent = (Max - Min) / 2;
	CalculatedBounds.SphereRadius = CalculatedBounds.BoxExtent.Size();

	SetExtendedBounds(CalculatedBounds);
}

void USkeletalMesh::ValidateBoundsExtension()
{
	FVector HalfExtent = GetImportedBounds().BoxExtent;

	FVector::FReal MaxVal = MAX_flt;
	FVector Bounds = GetPositiveBoundsExtension();
	Bounds.X = FMath::Clamp(Bounds.X, -HalfExtent.X, MaxVal);
	Bounds.Y = FMath::Clamp(Bounds.Y, -HalfExtent.Y, MaxVal);
	Bounds.Z = FMath::Clamp(Bounds.Z, -HalfExtent.Z, MaxVal);
	SetPositiveBoundsExtension(Bounds);

	Bounds = GetNegativeBoundsExtension();
	Bounds.X = FMath::Clamp(Bounds.X, -HalfExtent.X, MaxVal);
	Bounds.Y = FMath::Clamp(Bounds.Y, -HalfExtent.Y, MaxVal);
	Bounds.Z = FMath::Clamp(Bounds.Z, -HalfExtent.Z, MaxVal);
	SetNegativeBoundsExtension(Bounds);
}

#if WITH_EDITOR

bool USkeletalMesh::IsReadyToRenderInThumbnail() const
{
	if (IsCompiling() || !GetResourceForRendering())
	{
		return false;
	}

	//Since skeletal mesh use material, we want to avoid drawing thumbnail when shader are compiling
	for (const FSkeletalMaterial& SkeletalMaterial : GetMaterials())
	{
		if (SkeletalMaterial.MaterialInterface && SkeletalMaterial.MaterialInterface->IsCompiling())
		{
			return false;
		}
	}

	return true;
}

bool USkeletalMesh::IsInitialBuildDone() const
{
	//We are consider built if we have a valid lod model and a valid inline reduction cache
	return GetImportedModel() != nullptr &&
		GetImportedModel()->LODModels.Num() > 0 &&
		GetImportedModel()->LODModels[0].Sections.Num() > 0 &&
		GetImportedModel()->InlineReductionCacheDatas.Num() > 0;
}

/* Return true if the reduction settings are setup to reduce a LOD*/
bool USkeletalMesh::IsReductionActive(int32 LODIndex) const
{
	//Invalid LOD are not reduced
	if(!IsValidLODIndex(LODIndex))
	{
		return false;
	}

	bool bReductionActive = false;
	if (IMeshReduction* ReductionModule = FModuleManager::Get().LoadModuleChecked<IMeshReductionManagerModule>("MeshReductionInterface").GetSkeletalMeshReductionInterface())
	{
		FSkeletalMeshOptimizationSettings ReductionSettings = GetReductionSettings(LODIndex);
		uint32 LODVertexNumber = MAX_uint32;
		uint32 LODTriNumber = MAX_uint32;
		const FSkeletalMeshLODInfo* LODInfoPtr = GetLODInfo(LODIndex);
		const bool bLODHasBeenSimplified = LODInfoPtr && LODInfoPtr->bHasBeenSimplified;
		//If we are not inline reduced, we wont set the LODVertexNumber and LODTriNumber from the LODModel or from the cache.
		const bool bInlineReduction = LODInfoPtr && (LODInfoPtr->ReductionSettings.BaseLOD == LODIndex);
		if (bInlineReduction && GetImportedModel() && GetImportedModel()->LODModels.IsValidIndex(LODIndex))
		{
			if (!bLODHasBeenSimplified)
			{
				LODVertexNumber = 0;
				LODTriNumber = 0;
				const FSkeletalMeshLODModel& LODModel = GetImportedModel()->LODModels[LODIndex];
				//We can take the vertices and triangles count from the source model
				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); ++SectionIndex)
				{
					const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];

					//Make sure the count fit in a uint32
					LODVertexNumber += Section.NumVertices < 0 ? 0 : Section.NumVertices;
					LODTriNumber += Section.NumTriangles;
				}
			}
			else if (GetImportedModel()->InlineReductionCacheDatas.IsValidIndex(LODIndex))
			{
				//In this case we have to use the inline cache reduction data to know how many vertices/triangles we have before the reduction
				GetImportedModel()->InlineReductionCacheDatas[LODIndex].GetCacheGeometryInfo(LODVertexNumber, LODTriNumber);
			}
		}
		bReductionActive = ReductionModule->IsReductionActive(ReductionSettings, LODVertexNumber, LODTriNumber);
	}
	return bReductionActive;
}

/* Get a copy of the reduction settings for a specified LOD index. */
FSkeletalMeshOptimizationSettings USkeletalMesh::GetReductionSettings(int32 LODIndex) const
{
	check(IsValidLODIndex(LODIndex));
	const FSkeletalMeshLODInfo& CurrentLODInfo = *(GetLODInfo(LODIndex));
	return CurrentLODInfo.ReductionSettings;
}

#endif

void USkeletalMesh::SetMaterials(const TArray<FSkeletalMaterial>& InMaterials)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::Materials);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	Materials = InMaterials;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#if WITH_EDITORONLY_DATA

bool USkeletalMesh::IsNaniteEnabled() const
{
	return NaniteSettings.bEnabled;
}

#endif

void USkeletalMesh::AddClothingAsset(UClothingAssetBase* InNewAsset)
{
	check(IsInGameThread());

	// Check the outer is us
	if (InNewAsset && InNewAsset->GetOuter() == this)
	{
		// Ok this should be a correctly created asset, we can add it
		GetMeshClothingAssets().AddUnique(InNewAsset);

		// Consolidate the shared cloth configs
		InNewAsset->PostUpdateAllAssets();

#if WITH_EDITOR
		OnClothingChange.Broadcast();
#endif
	}
}

#if WITH_EDITOR
void USkeletalMesh::RemoveClothingAsset(int32 InLodIndex, int32 InSectionIndex)
{
	check(IsInGameThread());
	if (UClothingAssetBase* Asset = GetSectionClothingAsset(InLodIndex, InSectionIndex))
	{
		Asset->UnbindFromSkeletalMesh(this, InLodIndex);
		GetMeshClothingAssets().Remove(Asset);
		OnClothingChange.Broadcast();
	}
}
#endif

UClothingAssetBase* USkeletalMesh::GetSectionClothingAsset(int32 InLodIndex, int32 InSectionIndex)
{
	if (FSkeletalMeshRenderData* SkelResource = GetResourceForRendering())
	{
		if (SkelResource->LODRenderData.IsValidIndex(InLodIndex))
		{
			FSkeletalMeshLODRenderData& LodData = SkelResource->LODRenderData[InLodIndex];
			if (LodData.RenderSections.IsValidIndex(InSectionIndex))
			{
				FSkelMeshRenderSection& Section = LodData.RenderSections[InSectionIndex];

				FGuid ClothingAssetGuid = Section.ClothingData.AssetGuid;

				if (ClothingAssetGuid.IsValid())
				{
					auto* FoundAsset = GetMeshClothingAssets().FindByPredicate([&](UClothingAssetBase* InAsset)
					{
						return InAsset && InAsset->GetAssetGuid() == ClothingAssetGuid;
					});

					return FoundAsset ? *FoundAsset : nullptr;
				}
			}
		}
	}

	return nullptr;
}

const UClothingAssetBase* USkeletalMesh::GetSectionClothingAsset(int32 InLodIndex, int32 InSectionIndex) const
{
	if (FSkeletalMeshRenderData* SkelResource = GetResourceForRendering())
	{
		if (SkelResource->LODRenderData.IsValidIndex(InLodIndex))
		{
			FSkeletalMeshLODRenderData& LodData = SkelResource->LODRenderData[InLodIndex];
			if (LodData.RenderSections.IsValidIndex(InSectionIndex))
			{
				FSkelMeshRenderSection& Section = LodData.RenderSections[InSectionIndex];

				FGuid ClothingAssetGuid = Section.ClothingData.AssetGuid;

				if (ClothingAssetGuid.IsValid())
				{
					UClothingAssetBase* const* FoundAsset = GetMeshClothingAssets().FindByPredicate([&](UClothingAssetBase* InAsset)
					{
						return InAsset && InAsset->GetAssetGuid() == ClothingAssetGuid;
					});

					return FoundAsset ? *FoundAsset : nullptr;
				}
			}
		}
	}

	return nullptr;
}

UClothingAssetBase* USkeletalMesh::GetClothingAsset(const FGuid& InAssetGuid) const
{
	if (!InAssetGuid.IsValid())
	{
		return nullptr;
	}

	UClothingAssetBase* const* FoundAsset = GetMeshClothingAssets().FindByPredicate([&](UClothingAssetBase* CurrAsset)
	{
		return CurrAsset && CurrAsset->GetAssetGuid() == InAssetGuid;
	});

	return FoundAsset ? *FoundAsset : nullptr;
}

int32 USkeletalMesh::GetClothingAssetIndex(UClothingAssetBase* InAsset) const
{
	return InAsset ? GetClothingAssetIndex(InAsset->GetAssetGuid()) : INDEX_NONE;
}

int32 USkeletalMesh::GetClothingAssetIndex(const FGuid& InAssetGuid) const
{
	const TArray<UClothingAssetBase*>& CachedMeshClothingAssets = GetMeshClothingAssets();
	const int32 NumAssets = CachedMeshClothingAssets.Num();
	for(int32 SearchIndex = 0; SearchIndex < NumAssets; ++SearchIndex)
	{
		if(CachedMeshClothingAssets[SearchIndex] &&
		   CachedMeshClothingAssets[SearchIndex]->GetAssetGuid() == InAssetGuid)
		{
			return SearchIndex;
		}
	}
	return INDEX_NONE;
}

bool USkeletalMesh::HasActiveClothingAssets() const
{
#if WITH_EDITOR
	return ComputeActiveClothingAssets();
#else
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return bHasActiveClothingAssets;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
#endif
}

bool USkeletalMesh::HasActiveClothingAssetsForLOD(int32 LODIndex) const
{
	if (FSkeletalMeshRenderData* Resource = GetResourceForRendering())
	{
		if (Resource->LODRenderData.IsValidIndex(LODIndex))
		{
			const FSkeletalMeshLODRenderData& LodData = Resource->LODRenderData[LODIndex];
			const int32 NumSections = LodData.RenderSections.Num();
			for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
			{
				const FSkelMeshRenderSection& Section = LodData.RenderSections[SectionIdx];

				if (Section.ClothingData.AssetGuid.IsValid())
				{
					return true;
				}
			}
		}
	}

	return false;
}

bool USkeletalMesh::ComputeActiveClothingAssets() const
{
	if (FSkeletalMeshRenderData* Resource = GetResourceForRendering())
	{
		for (const FSkeletalMeshLODRenderData& LodData : Resource->LODRenderData)
		{
			const int32 NumSections = LodData.RenderSections.Num();
			for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
			{
				const FSkelMeshRenderSection& Section = LodData.RenderSections[SectionIdx];

				if(Section.ClothingData.AssetGuid.IsValid())
				{
					return true;
				}
			}
		}
	}

	return false;
}

void USkeletalMesh::GetClothingAssetsInUse(TArray<UClothingAssetBase*>& OutClothingAssets) const
{
	OutClothingAssets.Reset();
	
	if (FSkeletalMeshRenderData* Resource = GetResourceForRendering())
	{
		for (FSkeletalMeshLODRenderData& LodData : Resource->LODRenderData)
		{
			const int32 NumSections = LodData.RenderSections.Num();
			for (int32 SectionIdx = 0; SectionIdx < NumSections; ++SectionIdx)
			{
				FSkelMeshRenderSection& Section = LodData.RenderSections[SectionIdx];
				if (Section.ClothingData.AssetGuid.IsValid())
				{
					if (UClothingAssetBase* Asset = GetClothingAsset(Section.ClothingData.AssetGuid))
					{
						OutClothingAssets.AddUnique(Asset);
					}
				}
			}
		}
	}
}

bool USkeletalMesh::NeedCPUData(int32 LODIndex) const
{
	return GetSamplingInfo().IsSamplingEnabled(this, LODIndex);
}

void USkeletalMesh::InitResources()
{
	LLM_SCOPE_BYNAME(TEXT("SkeletalMesh/InitResources")); // This is an important test case for SCOPE_BYNAME without a matching LLM_DEFINE_TAG

	UpdateUVChannelData(false);
	CachedSRRState.Clear();

	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	if (SkelMeshRenderData)
	{
#if WITH_EDITOR
		//Editor sanity check, we must ensure all the data is in sync between LODModel, RenderData and UserSectionsData
		if (GetImportedModel())
		{
			for (int32 LODIndex = 0; LODIndex < GetLODNum(); ++LODIndex)
			{
				if (!GetImportedModel()->LODModels.IsValidIndex(LODIndex) || !GetSkeletalMeshRenderData()->LODRenderData.IsValidIndex(LODIndex))
				{
					continue;
				}
				const FSkeletalMeshLODModel& ImportLODModel = GetImportedModel()->LODModels[LODIndex];
				FSkeletalMeshLODRenderData& RenderLODModel = GetSkeletalMeshRenderData()->LODRenderData[LODIndex];
				check(ImportLODModel.Sections.Num() == RenderLODModel.RenderSections.Num());
				for (int32 SectionIndex = 0; SectionIndex < ImportLODModel.Sections.Num(); ++SectionIndex)
				{
					const FSkelMeshSection& ImportSection = ImportLODModel.Sections[SectionIndex];
					
					//In Editor we want to make sure the data is in sync between UserSectionsData and LODModel Sections
					const FSkelMeshSourceSectionUserData& SectionUserData = ImportLODModel.UserSectionsData.FindChecked(ImportSection.OriginalDataSectionIndex);
					bool bImportDataInSync = SectionUserData.bDisabled == ImportSection.bDisabled &&
						SectionUserData.bCastShadow == ImportSection.bCastShadow &&
						SectionUserData.bVisibleInRayTracing == ImportSection.bVisibleInRayTracing &&
						SectionUserData.bRecomputeTangent == ImportSection.bRecomputeTangent &&
						SectionUserData.RecomputeTangentsVertexMaskChannel == ImportSection.RecomputeTangentsVertexMaskChannel;
					//Check the cloth only for parent section, since chunked section should not have cloth
					if (bImportDataInSync && ImportSection.ChunkedParentSectionIndex == INDEX_NONE)
					{
						bImportDataInSync = SectionUserData.CorrespondClothAssetIndex == ImportSection.CorrespondClothAssetIndex &&
							SectionUserData.ClothingData.AssetGuid == ImportSection.ClothingData.AssetGuid &&
							SectionUserData.ClothingData.AssetLodIndex == ImportSection.ClothingData.AssetLodIndex;
					}
					
					//In Editor we want to make sure the data is in sync between UserSectionsData and RenderSections
					const FSkelMeshRenderSection& RenderSection = RenderLODModel.RenderSections[SectionIndex];
					bool bRenderDataInSync = SectionUserData.bDisabled == RenderSection.bDisabled &&
						SectionUserData.bCastShadow == RenderSection.bCastShadow &&
						SectionUserData.bVisibleInRayTracing == RenderSection.bVisibleInRayTracing &&
						SectionUserData.bRecomputeTangent == RenderSection.bRecomputeTangent &&
						SectionUserData.RecomputeTangentsVertexMaskChannel == RenderSection.RecomputeTangentsVertexMaskChannel &&
						SectionUserData.CorrespondClothAssetIndex == RenderSection.CorrespondClothAssetIndex &&
						SectionUserData.ClothingData.AssetGuid == RenderSection.ClothingData.AssetGuid &&
						SectionUserData.ClothingData.AssetLodIndex == RenderSection.ClothingData.AssetLodIndex;

					if (!bImportDataInSync || !bRenderDataInSync)
					{
						UE_ASSET_LOG(LogSkeletalMesh, Error, this, TEXT("Data out of sync in lod %d. bImportDataInSync=%d, bRenderDataInSync=%d. This happen when DDC cache has corrupted data (Key has change during the skeletalmesh build)"), LODIndex, bImportDataInSync, bRenderDataInSync);
					}
				}
			}
		}
#endif
		bool bAllLODsLookValid = true;	// TODO figure this out
		for (int32 LODIdx = 0; LODIdx < GetSkeletalMeshRenderData()->LODRenderData.Num(); ++LODIdx)
		{
			const FSkeletalMeshLODRenderData& LODRenderData = GetSkeletalMeshRenderData()->LODRenderData[LODIdx];
			if (!LODRenderData.GetNumVertices() && (!LODRenderData.bIsLODOptional || LODRenderData.BuffersSize > 0))
			{
				bAllLODsLookValid = false;
				break;
			}
		}

		{
			const int32 NumLODs = SkelMeshRenderData->LODRenderData.Num();
			const int32 MinFirstLOD = GetMinLodIdx(true);

			CachedSRRState.NumNonStreamingLODs = SkelMeshRenderData->NumInlinedLODs;
			CachedSRRState.NumNonOptionalLODs = SkelMeshRenderData->NumNonOptionalLODs;
			// Limit the number of LODs based on MinLOD value.
			CachedSRRState.MaxNumLODs = FMath::Clamp<int32>(NumLODs - MinFirstLOD, SkelMeshRenderData->NumInlinedLODs, NumLODs);
			CachedSRRState.AssetLODBias = MinFirstLOD;
			CachedSRRState.LODBiasModifier = SkelMeshRenderData->LODBiasModifier;
			// The optional LOD might be culled now.
			CachedSRRState.NumNonOptionalLODs = FMath::Min(CachedSRRState.NumNonOptionalLODs, CachedSRRState.MaxNumLODs);
			// Set LOD count to fit the current state.
			CachedSRRState.NumResidentLODs = NumLODs - SkelMeshRenderData->CurrentFirstLODIdx;
			CachedSRRState.NumRequestedLODs = CachedSRRState.NumResidentLODs;
			// Set whether the mips can be streamed.
			CachedSRRState.bSupportsStreaming = !NeverStream && bAllLODsLookValid && CachedSRRState.NumNonStreamingLODs != CachedSRRState.MaxNumLODs;
		}

		// TODO : Update RenderData->CurrentFirstLODIdx based on whether IStreamingManager::Get().IsRenderAssetStreamingEnabled(EStreamableRenderAssetType::SkeletalMesh).

		SkelMeshRenderData->InitResources(GetHasVertexColors(), MutableView(GetMorphTargets()), this);
		CachedSRRState.bHasPendingInitHint = true;

		// For now in the editor force all LODs to stream to make sure tools have all LODs available
		if (GIsEditor && CachedSRRState.bSupportsStreaming)
		{
			bForceMiplevelsToBeResident = true;
		}
	}

	LinkStreaming();
}


void USkeletalMesh::ReleaseResources()
{
	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	if (SkelMeshRenderData && SkelMeshRenderData->IsInitialized())
	{

		if(GIsEditor && !GIsPlayInEditorWorld)
		{
			//Flush the rendering command to be sure there is no command left that can create/modify a rendering ressource
			FlushRenderingCommands();
		}

		SkelMeshRenderData->ReleaseResources();

		// insert a fence to signal when these commands completed
		ReleaseResourcesFence.BeginFence();
	}
}

#if WITH_EDITORONLY_DATA
int32 USkeletalMesh::GetNumImportedVertices() const
{
	const FSkeletalMeshModel* SkeletalMeshModel = GetImportedModel();
	if (SkeletalMeshModel && !SkeletalMeshModel->LODModels.IsEmpty())
	{
		const int32 MaxIndex = SkeletalMeshModel->LODModels[0].MaxImportVertex;
		return (MaxIndex > 0) ? (MaxIndex + 1) : 0;
	}

	return 0;
}
#endif

const FMeshUVChannelInfo* USkeletalMesh::GetUVChannelData(int32 MaterialIndex) const
{
	if (GetMaterials().IsValidIndex(MaterialIndex))
	{
		ensure(GetMaterials()[MaterialIndex].UVChannelData.bInitialized);
		return &GetMaterials()[MaterialIndex].UVChannelData;
	}

	return nullptr;
}

void USkeletalMesh::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	Super::GetResourceSizeEx(CumulativeResourceSize);
	// Default implementation handles subobjects

	if (GetSkeletalMeshRenderData())
	{
		GetSkeletalMeshRenderData()->GetResourceSizeEx(CumulativeResourceSize);
	}

	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(GetRefBasesInvMatrix().GetAllocatedSize());
	CumulativeResourceSize.AddDedicatedSystemMemoryBytes(GetRefSkeleton().GetDataSize());
}

int32 USkeletalMesh::CalcCumulativeLODSize(int32 NumLODs) const
{
	uint32 Accum = 0;
	const int32 LODCount = GetLODNum();
	const int32 LastLODIdx = LODCount - NumLODs;
	for (int32 LODIdx = LODCount - 1; LODIdx >= LastLODIdx; --LODIdx)
	{
		Accum += GetSkeletalMeshRenderData()->LODRenderData[LODIdx].BuffersSize;
	}
	check(Accum >= 0u);
	return Accum;
}

FIoFilenameHash USkeletalMesh::GetMipIoFilenameHash(const int32 MipIndex) const
{
	if (GetSkeletalMeshRenderData() && GetSkeletalMeshRenderData()->LODRenderData.IsValidIndex(MipIndex))
	{
		return GetSkeletalMeshRenderData()->LODRenderData[MipIndex].StreamingBulkData.GetIoFilenameHash();
	}
	else
	{
		return INVALID_IO_FILENAME_HASH;
	}
}

bool USkeletalMesh::DoesMipDataExist(const int32 MipIndex) const
{
	return GetSkeletalMeshRenderData() && GetSkeletalMeshRenderData()->LODRenderData.IsValidIndex(MipIndex) && GetSkeletalMeshRenderData()->LODRenderData[MipIndex].StreamingBulkData.DoesExist();
}

bool USkeletalMesh::HasPendingRenderResourceInitialization() const
{
	// Verify we're not compiling before accessing the renderdata to avoid forcing the compilation
	// to finish during garbage collection. If we're still compiling, the render data has not
	// yet been created, hence it is not possible we're actively streaming anything from it...

	// Only check !bReadyForStreaming if the render data is initialized from FSkeletalMeshRenderData::InitResources(), 
	// otherwise no render commands are pending and the state will never resolve.
	// Note that bReadyForStreaming is set on the renderthread.
	return !IsCompiling() && GetSkeletalMeshRenderData() && GetSkeletalMeshRenderData()->IsInitialized() && !GetSkeletalMeshRenderData()->bReadyForStreaming;
}

bool USkeletalMesh::StreamOut(int32 NewMipCount)
{
	check(IsInGameThread());
	
	FSkeletalMeshRenderData* RenderData = GetResourceForRendering();
	if (!RenderData || !RenderData->IsInitialized())
	{
		return false;
	}
	
	if (!HasPendingInitOrStreaming() && CachedSRRState.StreamOut(NewMipCount))
	{
		PendingUpdate = new FSkeletalMeshStreamOut(this);
		return !PendingUpdate->IsCancelled();
	}
	return false;
}

bool USkeletalMesh::StreamIn(int32 NewMipCount, bool bHighPrio)
{
	check(IsInGameThread());
	
	FSkeletalMeshRenderData* RenderData = GetResourceForRendering();
	if (!RenderData || !RenderData->IsInitialized())
	{
		return false;
	}
	
	if (!HasPendingInitOrStreaming() && CachedSRRState.StreamIn(NewMipCount))
	{
		FRenderAssetUpdate::EThreadType CreateResourcesThread = GRHISupportsAsyncTextureCreation
			? FRenderAssetUpdate::TT_Async
			: FRenderAssetUpdate::TT_Render;

#if WITH_EDITOR
		// If editor data is available for the current platform, and the package isn't actually cooked.
		if (FPlatformProperties::HasEditorOnlyData() && !GetOutermost()->bIsCookedForEditor)
		{
			PendingUpdate = new FSkeletalMeshStreamIn_DDC(this, CreateResourcesThread);
		}
		else
#endif
		{
			PendingUpdate = new FSkeletalMeshStreamIn_IO(this, bHighPrio, CreateResourcesThread);
		}
		return !PendingUpdate->IsCancelled();
	}
	return false;
}

void USkeletalMesh::CancelAllPendingStreamingActions()
{
	FlushRenderingCommands();

	for (TObjectIterator<USkeletalMesh> It; It; ++It)
	{
		USkeletalMesh* StaticMesh = *It;
		StaticMesh->CancelPendingStreamingRequest();
	}

	FlushRenderingCommands();
}

/**
 * Operator for MemCount only, so it only serializes the arrays that needs to be counted.
*/
FArchive &operator<<( FArchive& Ar, FSkeletalMeshLODInfo& I )
{
	Ar << I.LODMaterialMap;

#if WITH_EDITORONLY_DATA
	if ( Ar.IsLoading() && Ar.UEVer() < VER_UE4_MOVE_SKELETALMESH_SHADOWCASTING )
	{
		Ar << I.bEnableShadowCasting_DEPRECATED;
	}
#endif

	// fortnite version
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FSkeletalMeshCustomVersion::GUID);
	if (Ar.CustomVer(FSkeletalMeshCustomVersion::GUID) < FSkeletalMeshCustomVersion::RemoveTriangleSorting)
	{
		uint8 DummyTriangleSorting;
		Ar << DummyTriangleSorting;

		uint8 DummyCustomLeftRightAxis;
		Ar << DummyCustomLeftRightAxis;

		FName DummyCustomLeftRightBoneName;
		Ar << DummyCustomLeftRightBoneName;
		}


	return Ar;
}

void RefreshSkelMeshOnPhysicsAssetChange(const USkeletalMesh* InSkeletalMesh)
{
	if (InSkeletalMesh)
	{
		for (FThreadSafeObjectIterator Iter(USkeletalMeshComponent::StaticClass()); Iter; ++Iter)
		{
			USkeletalMeshComponent* SkeletalMeshComponent = Cast<USkeletalMeshComponent>(*Iter);
			// if PhysicsAssetOverride is NULL, it uses SkeletalMesh Physics Asset, so I'll need to update here
			if  (SkeletalMeshComponent->GetSkeletalMeshAsset() == InSkeletalMesh &&
				 SkeletalMeshComponent->PhysicsAssetOverride == NULL)
			{
				// it needs to recreate IF it already has been created
				if (SkeletalMeshComponent->IsPhysicsStateCreated())
				{
					// do not call SetPhysAsset as it will setup physics asset override
					SkeletalMeshComponent->RecreatePhysicsState();
					SkeletalMeshComponent->UpdateHasValidBodies();
				}
			}
		}
#if WITH_EDITOR
		FEditorSupportDelegates::RedrawAllViewports.Broadcast();
#endif // WITH_EDITOR
	}
}

#if WITH_EDITOR

int32 USkeletalMesh::StackPostEditChange()
{
	check(PostEditChangeStackCounter >= 0);
	//Return true if this is the first stack ID
	PostEditChangeStackCounter++;
	return PostEditChangeStackCounter;
}

int32 USkeletalMesh::UnStackPostEditChange()
{
	check(PostEditChangeStackCounter > 0);
	PostEditChangeStackCounter--;
	return PostEditChangeStackCounter;
}

void USkeletalMesh::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	check(IsInGameThread());

	if (PostEditChangeStackCounter > 0)
	{
		//Ignore those call when we have an active delay stack
		return;
	}
	//Block any re-entrant call by incrementing PostEditChangeStackCounter. It will be decrement when we will go out of scope.
	const bool bCallPostEditChange = false;
	const bool bReRegisterComponents = false;
	FScopedSkeletalMeshPostEditChange BlockRecursiveCallScope(this, bCallPostEditChange, bReRegisterComponents);

	bool bFullPrecisionUVsReallyChanged = false;

	FProperty* PropertyThatChanged = PropertyChangedEvent.Property;
	
	if (GIsEditor &&
		PropertyThatChanged &&
		(PropertyThatChanged->GetFName() == FName(TEXT("bSupportRayTracing")) ||
		 PropertyThatChanged->GetFName() == FName(TEXT("RayTracingMinLOD")) ||
		 PropertyThatChanged->GetFName() == FName(TEXT("ClothLODBiasMode"))))
	{
		// Update the extra cloth deformer mapping LOD bias using this cloth entry
		for (UClothingAssetBase* const ClothingAsset : GetMeshClothingAssets())
		{
			if (ClothingAsset)
			{
				ClothingAsset->UpdateAllLODBiasMappings(this);
			}
		}

		// Invalidate the DDC, since the bias mappings are cached with the mesh sections, this needs to be done before the call to Build()
		InvalidateDeriveDataCacheGUID();
	}

	bool bWasBuilt = false;
	bool bHasToReregisterComponent = false;
	// Don't invalidate render data when dragging sliders, too slow
	if (PropertyChangedEvent.ChangeType != EPropertyChangeType::Interactive)
	{
		Build();
		bWasBuilt = true;
		bHasToReregisterComponent = true;
	}

	if( GIsEditor &&
		PropertyThatChanged &&
		PropertyThatChanged->GetFName() == FName(TEXT("PhysicsAsset")) )
	{
		RefreshSkelMeshOnPhysicsAssetChange(this);
	}

	if( GIsEditor &&
		CastField<FObjectProperty>(PropertyThatChanged) &&
		CastField<FObjectProperty>(PropertyThatChanged)->PropertyClass == UMorphTarget::StaticClass() )
	{
		// A morph target has changed, reinitialize morph target maps
		InitMorphTargets();
	}

	if ( GIsEditor &&
		 PropertyThatChanged &&
		 PropertyThatChanged->GetFName() == GetEnablePerPolyCollisionMemberName()
		)
	{
		BuildPhysicsData();
	}

	if (FProperty* MemberProperty = PropertyChangedEvent.MemberProperty)
	{
		if (MemberProperty->GetFName() == GetPositiveBoundsExtensionMemberName() ||
			MemberProperty->GetFName() == GetNegativeBoundsExtensionMemberName())
		{
			// If the bounds extensions change, recalculate extended bounds.
			ValidateBoundsExtension();
			CalculateExtendedBounds();
			bHasToReregisterComponent = true;
		}
	}
		
	if (PropertyThatChanged && PropertyThatChanged->GetFName() == USkeletalMesh::GetPostProcessAnimBlueprintMemberName())
	{
		bHasToReregisterComponent = true;
	}

	if (bHasToReregisterComponent)
	{
		TArray<UActorComponent*> ComponentsToReregister;
		for(TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			USkeletalMeshComponent* MeshComponent = *It;
			if(MeshComponent && !MeshComponent->IsTemplate() && MeshComponent->GetSkeletalMeshAsset() == this)
			{
				ComponentsToReregister.Add(*It);
			}
		}
		FMultiComponentReregisterContext ReregisterContext(ComponentsToReregister);
	}

	// Those are already handled by the Build method, no need to process those if Build() has been called.
	if (!bWasBuilt)
	{
		if (PropertyThatChanged && PropertyChangedEvent.MemberProperty)
		{
			if (PropertyChangedEvent.MemberProperty->GetFName() == GetSamplingInfoMemberName())
			{
				GetSamplingInfoInternal().BuildRegions(this);
			}
			else if (PropertyChangedEvent.MemberProperty->GetFName() == GetLODInfoMemberName())
			{
				GetSamplingInfoInternal().BuildWholeMesh(this);
			}
			else if (PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(FSkeletalMeshLODInfo, bSupportUniformlyDistributedSampling))
			{
				GetSamplingInfoInternal().BuildWholeMesh(this);
			}
		}
		else
		{
			//Rebuild the lot. No property could mean a reimport.
			GetSamplingInfoInternal().BuildRegions(this);
			GetSamplingInfoInternal().BuildWholeMesh(this);
		}

		UpdateUVChannelData(true);
		UpdateGenerateUpToData();
	}

	OnMeshChanged.Broadcast();

	for (UAssetUserData* Datum : AssetUserData)
	{
		if (Datum != nullptr)
		{
			Datum->PostEditChangeOwner(PropertyChangedEvent);
		}
	}
	for (UAssetUserData* Datum : AssetUserDataEditorOnly)
	{
		if (Datum != nullptr)
		{
			Datum->PostEditChangeOwner(PropertyChangedEvent);
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);

	//The stack counter here should be 1 since the BlockRecursiveCallScope protection has the lock and it will be decrement to 0 when we get out of the function scope
	check(PostEditChangeStackCounter == 1);
}

bool USkeletalMesh::IsTransacting() const
{
	return bTransacting;
}

void USkeletalMesh::PreEditChange(FProperty* PropertyAboutToChange)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::PreEditChange);

	//Don't call finish compile if this skeletal mesh is compiling and we are in a FSkinnedAssetAsyncBuildScope for this skeletal mesh.
	//If on the game thread we call LockPropertyUntil and we call PreEditChange after, in such a case a deadlock will happen if
	//we call finish compile on this skeletal mesh.
	if (FSkinnedAssetAsyncBuildScope::ShouldWaitOnLockedProperties(this))
	{
		// Tell the compiler to finish compiling us if we have a pending
		// compilation ongoing plus any dependency (i.e. UGroomBindings).
		FAssetCompilingManager::Get().FinishCompilationForObjects({ this });
	}

	Super::PreEditChange(PropertyAboutToChange);
}

void USkeletalMesh::PreEditUndo()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::PreEditUndo);

	// Tell the compiler to finish compiling us if we have a pending
	// compilation ongoing plus any dependency (i.e. UGroomBindings).
	FAssetCompilingManager::Get().FinishCompilationForObjects({ this });

	bTransacting = true;

	Super::PreEditUndo();
}

void USkeletalMesh::PostEditUndo()
{
	check(IsInGameThread());

	Super::PostEditUndo();
	for( TObjectIterator<USkeletalMeshComponent> It; It; ++It )
	{
		USkeletalMeshComponent* MeshComponent = *It;
		if( MeshComponent && 
			!MeshComponent->IsTemplate() &&
			MeshComponent->GetSkeletalMeshAsset() == this )
		{
			FComponentReregisterContext Context(MeshComponent);
		}
	}

	// ensure that morph targets belong to this skeletal mesh
	// note: removing a morph target can re-outer it to the transient package and mark it as garbage so it has to be reverted on post-undo if needed  
	for (UMorphTarget* MorphTarget: GetMorphTargets())
	{
		if (MorphTarget)
		{
			MorphTarget->ClearGarbage();
		}
	}
	
	if(GetMorphTargets().Num() > GetMorphTargetIndexMap().Num())
	{
		// A morph target remove has been undone, reinitialise
		InitMorphTargets();
	}
	
	bTransacting = false;
}

void USkeletalMesh::UpdateGenerateUpToData()
{
	for (int32 LodIndex = 0; LodIndex < GetImportedModel()->LODModels.Num(); ++LodIndex)
	{
		FSkeletalMeshLODModel& LodModel = GetImportedModel()->LODModels[LodIndex];
		for (int32 SectionIndex = 0; SectionIndex < LodModel.Sections.Num(); ++SectionIndex)
		{
			int32 SpecifiedLodIndex = LodModel.Sections[SectionIndex].GenerateUpToLodIndex;
			if (SpecifiedLodIndex != -1 && SpecifiedLodIndex < LodIndex)
			{
				LodModel.Sections[SectionIndex].GenerateUpToLodIndex = LodIndex;
			}
		}
	}
}

void USkeletalMesh::CheckForValidMinLODs(FPerQualityLevelInt& QualityLocalMinLOD, FPerPlatformInt& LocalMinLOD, int32& OutMinAvailableLOD, TArray<TPair<int32, FName>>& OutInvalidMinLODs) const
{
	const FSkeletalMeshRenderData* LocalRenderData = GetSkeletalMeshRenderData();
	if (!LocalRenderData)
	{
		return;
	}

	OutMinAvailableLOD = FMath::Max<int32>(LocalRenderData->LODRenderData.Num() - 1, 0);

	auto CheckValidMinLOD = [LocalRenderData, OutMinAvailableLOD, &OutInvalidMinLODs](int32& LODIdx, FName OverrideName)
	{
		if (!LocalRenderData->LODRenderData.IsValidIndex(LODIdx))
		{
			OutInvalidMinLODs.Emplace(LODIdx, OverrideName);
			LODIdx = OutMinAvailableLOD;
		}
	};

	if (IsMinLodQualityLevelEnable())
	{
		QualityLocalMinLOD = GetQualityLevelMinLod();
		CheckValidMinLOD(QualityLocalMinLOD.Default, NAME_None);

		for (TMap<int32, int32>::TIterator It(QualityLocalMinLOD.PerQuality); It; ++It)
		{
			CheckValidMinLOD(It.Value(), QualityLevelProperty::QualityLevelToFName(It.Key()));
		}
	}
	else
	{
		LocalMinLOD = GetMinLod();
		CheckValidMinLOD(LocalMinLOD.Default, NAME_None);

		for (TMap<FName, int32>::TIterator It(LocalMinLOD.PerPlatform); It; ++It)
		{
			CheckValidMinLOD(It.Value(), It.Key());
		}
	}
}

EDataValidationResult USkeletalMesh::IsDataValid(FDataValidationContext& Context) const
{
	EDataValidationResult ValidationResult = Super::IsDataValid(Context);
	// Do not validate a cooked skeletal mesh asset.
	if (GetPackage()->HasAnyPackageFlags(PKG_Cooked) == false)
	{
		if (!GetSkeleton())
		{
			//We must have a valid skeleton
			Context.AddError(LOCTEXT("SkeletalMeshValidation_NoSkeleton", "This skeletal mesh asset has no Skeleton. Skeletal mesh asset need a valid skeleton."));
			ValidationResult = EDataValidationResult::Invalid;
		}
		else
		{
			//Validate if the skeleton is compatible with this skeletal mesh
			if (!GetSkeleton()->IsCompatibleMesh(this))
			{
				Context.AddError(LOCTEXT("SkeletalMeshValidation_IncompatibleSkeleton", "This skeletal mesh asset has an incompatible Skeleton. Assign a compatible skeleton to this skeletal mesh asset."));
				ValidationResult = EDataValidationResult::Invalid;
			}
		}
		
		const int32 NumRealBones = GetRefSkeleton().GetRawBoneNum();
		const TArray<FTransform>& RawRefBonePose = GetRefSkeleton().GetRawRefBonePose();

		// Precompute the Mesh.RefBasesInverse.
		for (int32 BoneIndex = 0; BoneIndex < NumRealBones; BoneIndex++)
		{
			//Validate skeleton bone index
			if (!GetRefSkeleton().IsValidRawIndex(BoneIndex))
			{
				Context.AddError(LOCTEXT("SkeletalMeshValidation_InvalidBoneIndex", "This skeletal mesh asset has invalid bone index. Asset is corrupted and must be re-create"));
				ValidationResult = EDataValidationResult::Invalid;
			}

			//Validate Parent bone index
			if (BoneIndex > 0)
			{
				int32 Parent = GetRefSkeleton().GetRawParentIndex(BoneIndex);
				if (!GetRefSkeleton().IsValidRawIndex(Parent))
				{
					Context.AddError(LOCTEXT("SkeletalMeshValidation_InvalidParentBoneIndex", "This skeletal mesh asset has invalid parent bone index. Asset is corrupted and must be re-create"));
					ValidationResult = EDataValidationResult::Invalid;
				}
			}

			//Validate transform do not contains nan
			const FTransform& BoneTransform = RawRefBonePose[BoneIndex];
			if (BoneTransform.ContainsNaN())
			{
				Context.AddError(LOCTEXT("SkeletalMeshValidation_PoseMatrixContainNan", "This skeletal mesh asset has NAN (invalid float number) value in the pose matrix. Asset is corrupted and must be re-create"));
				ValidationResult = EDataValidationResult::Invalid;
			}
		}
	}

	{
		// check the MinLOD values are all within range
		FPerQualityLevelInt QualityLocalMinLOD;
		FPerPlatformInt LocalMinLOD;
		int32 MinAvailableLOD = INDEX_NONE;
		TArray<TPair<int32, FName>> InvalidMinLODs;
		CheckForValidMinLODs(QualityLocalMinLOD, LocalMinLOD, MinAvailableLOD, InvalidMinLODs);
		if (InvalidMinLODs.Num() > 0)
		{
			for (const TPair<int32, FName>& InvalidMinLOD : InvalidMinLODs)
			{
				const int32 LODIdx = InvalidMinLOD.Key;
				const FName OverrideName = InvalidMinLOD.Value;

				FFormatNamedArguments Arguments;
				Arguments.Add(TEXT("MinLOD"), FText::AsNumber(LODIdx));
				Arguments.Add(TEXT("MinAvailLOD"), FText::AsNumber(MinAvailableLOD));
				Arguments.Add(TEXT("OverrideName"), FText::FromName(OverrideName));

				if (OverrideName.IsNone())
				{
					Context.AddWarning(FText::Format(LOCTEXT("LoadError_BadMinLOD", "Min LOD value of {MinLOD} is out of range 0..{MinAvailLOD}."), Arguments));
				}
				else
				{
					Context.AddWarning(FText::Format(LOCTEXT("LoadError_BadMinLODWithOverride", "Min LOD override of {MinLOD} for {OverrideName} is out of range 0..{MinAvailLOD}."), Arguments));
				}
			}

			ValidationResult = EDataValidationResult::Invalid;
		}
	}

	return ValidationResult;
}

#endif // WITH_EDITOR

void USkeletalMesh::BeginDestroy()
{
	check(IsInGameThread());

	Super::BeginDestroy();

	if (FSkinWeightProfileManager * Manager = FSkinWeightProfileManager::Get(GetWorld()))
	{
		Manager->CancelSkinWeightProfileRequest(this);
	}

#if WITH_EDITOR
	// Before trying to touch GetSkeleton which might cause a wait on the async task,
	// tell the async task we don't need it anymore so it gets cancelled if possible.
	TryCancelAsyncTasks();
#endif

	// remove the cache of link up
	if (GetSkeleton())
	{
		GetSkeleton()->RemoveLinkup(this);
	}

	// Release the mesh's render resources now if no pending streaming op.
	if (!HasPendingInitOrStreaming())
	{
		ReleaseResources();
	}
}

bool USkeletalMesh::IsReadyForFinishDestroy()
{
#if WITH_EDITOR
	// We're being garbage collected and might still have async tasks pending
	if (!TryCancelAsyncTasks())
	{
		return false;
	}
#endif

	if (!Super::IsReadyForFinishDestroy())
	{
		return false;
	}

	// Match BeginDestroy() by checking for HasPendingInitOrStreaming().
	if (HasPendingInitOrStreaming())
	{
		return false;
	}

	ReleaseResources();

	// see if we have hit the resource flush fence
	return ReleaseResourcesFence.IsFenceComplete();
}

#if WITH_EDITOR
void CachePlatform(USkeletalMesh* Mesh, const ITargetPlatform* TargetPlatform, FSkeletalMeshRenderData* PlatformRenderData, const bool bIsSerializeSaving)
{
	//Cache the platform, dcc should be valid so it will be fast
	check(TargetPlatform);
	FSkinnedAssetBuildContext Context;
	Context.bIsSerializeSaving = bIsSerializeSaving;
	PlatformRenderData->Cache(TargetPlatform, Mesh, &Context);
	if (Context.FinishBuildMorphTargetData.IsValid())
	{
		// Morph target is only supported on USkeletalMesh
		Context.FinishBuildMorphTargetData->ApplyEditorData(Mesh, Context.bIsSerializeSaving);
	}
}

static FSkeletalMeshRenderData& GetPlatformSkeletalMeshRenderData(USkeletalMesh* Mesh, const ITargetPlatform* TargetPlatform, const bool bIsSerializeSaving)
{
	const FString PlatformDerivedDataKey = Mesh->BuildDerivedDataKey(TargetPlatform);
	FSkeletalMeshRenderData* PlatformRenderData = Mesh->GetResourceForRendering();
	if (Mesh->GetOutermost()->bIsCookedForEditor)
	{
		check(PlatformRenderData);
		return *PlatformRenderData;
	}

	while (PlatformRenderData && PlatformRenderData->DerivedDataKey != PlatformDerivedDataKey)
	{
		PlatformRenderData = PlatformRenderData->NextCachedRenderData.Get();
	}

	if (PlatformRenderData == nullptr)
	{
		// Cache render data for this platform and insert it in to the linked list.
		PlatformRenderData = new FSkeletalMeshRenderData();
		CachePlatform(Mesh, TargetPlatform, PlatformRenderData, bIsSerializeSaving);
		check(PlatformRenderData->DerivedDataKey == PlatformDerivedDataKey);
		Swap(PlatformRenderData->NextCachedRenderData, Mesh->GetResourceForRendering()->NextCachedRenderData);
		Mesh->GetResourceForRendering()->NextCachedRenderData = TUniquePtr<FSkeletalMeshRenderData>(PlatformRenderData);
	}
	check(PlatformRenderData->DerivedDataKey == PlatformDerivedDataKey);
	check(PlatformRenderData);
	return *PlatformRenderData;
}

FScopedSkeletalMeshRenderData::FScopedSkeletalMeshRenderData(USkeletalMesh* InMesh)
{
	Mesh = InMesh;
	if (Mesh)
	{
		// Lock the skeletal mesh properties since we call USkeletalMesh::Cache() function (through GetPlatformSkeletalMeshRenderData -> CachePlatform -> Cache) 
		// and which could be called by other threads at the same time
		Lock = Mesh->LockPropertiesUntil();
	}
}

FScopedSkeletalMeshRenderData::~FScopedSkeletalMeshRenderData()
{
	if (Lock)
	{
		check(Mesh);
		Lock->Trigger();
		//After we trigger the event we must tick the FSkinnedAssetCompilingManager so it clear the skeletal mesh AsyncTask and call
		//FinishAsyncTaskInternal to terminate the LockPropertiesUntil
		FSkinnedAssetCompilingManager::Get().FinishCompilation({ Mesh });
	}

	Data = nullptr;
	Mesh = nullptr;
	Lock = nullptr;
}

const FSkeletalMeshRenderData* FScopedSkeletalMeshRenderData::GetData() const
{
	return Data;
}

void USkeletalMesh::GetPlatformSkeletalMeshRenderData(const ITargetPlatform* TargetPlatform, FScopedSkeletalMeshRenderData& Out)
{
	if (Out.Mesh && Out.Lock)
	{
		constexpr bool bIsSerializeSaving = false;
		Out.Data = &::GetPlatformSkeletalMeshRenderData(Out.Mesh, TargetPlatform, bIsSerializeSaving);
	}
}
#endif

LLM_DEFINE_TAG(SkeletalMesh_Serialize); // This is an important test case for LLM_DEFINE_TAG

void USkeletalMesh::Serialize( FArchive& Ar )
{
	LLM_SCOPE_BYNAME(TEXT("SkeletalMesh/Serialize")); // This is an important test case for SCOPE_BYNAME with a matching LLM_DEFINE_TAG
	DECLARE_SCOPE_CYCLE_COUNTER( TEXT("USkeletalMesh::Serialize"), STAT_SkeletalMesh_Serialize, STATGROUP_LoadTime );
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::Serialize);

#if WITH_EDITOR
	if (IsCompiling())
	{
		// Skip serialization during compilation if told to do so.
		if (Ar.ShouldSkipCompilingAssets())
		{
			return;
		}

		// Since UPROPERTY are accessed directly by offset during serialization instead of using accessors, 
		// the protection put in place to automatically finish compilation if a locked property is accessed will not work. 
		// We have no choice but to force finish the compilation here to avoid potential race conditions between 
		// async compilation and the serialization.
		FSkinnedAssetCompilingManager::Get().FinishCompilation({ this });
	}

	if (Ar.IsSaving() && !Ar.IsCooking())
	{
		// If saving out to disk, ensure that all source models have had their raw mesh bulk data converted to mesh description,
		// since the bulk data won't be reloaded.
		for (int32 LODIndex = 0; LODIndex < GetNumSourceModels(); LODIndex++)
		{
			GetSourceModel(LODIndex).EnsureRawMeshBulkDataIsConvertedToNew();
		}
		
		// Ensure source models and LODs match.
		SetNumSourceModels(GetLODNum());
	}
	
#endif

	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FFrameworkObjectVersion::GUID);
	Ar.UsingCustomVersion(FEditorObjectVersion::GUID);
	Ar.UsingCustomVersion(FSkeletalMeshCustomVersion::GUID);
	Ar.UsingCustomVersion(FRenderingObjectVersion::GUID);
	Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
	Ar.UsingCustomVersion(FNiagaraObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);

	FStripDataFlags StripFlags( Ar );

	FBoxSphereBounds LocalImportedBounds = GetImportedBounds();
	Ar << LocalImportedBounds;
	SetImportedBounds(LocalImportedBounds);

	Ar << GetMaterials();

	Ar << GetRefSkeleton();

	if (Ar.IsLoading())
	{
		const bool bRebuildNameMap = false;
		GetRefSkeleton().RebuildRefSkeleton(GetSkeleton(), bRebuildNameMap);
	}

#if WITH_EDITORONLY_DATA
	// Serialize the source model (if we want editor data)
	if (!StripFlags.IsEditorDataStripped())
	{
		GetImportedModel()->Serialize(Ar, this);
	}
#endif // WITH_EDITORONLY_DATA

	if (Ar.CustomVer(FSkeletalMeshCustomVersion::GUID) >= FSkeletalMeshCustomVersion::SplitModelAndRenderData)
	{
		bool bCooked = Ar.IsCooking();
		Ar << bCooked;

		const bool bIsDuplicating = Ar.HasAnyPortFlags(PPF_Duplicate);

		// Inline the derived data for cooked builds. Never include render data when
		// counting memory as it is included by GetResourceSize.
		if ((bIsDuplicating || bCooked) && !IsTemplate() && !Ar.IsCountingMemory())
		{
			if (Ar.IsLoading())
			{
				SetSkeletalMeshRenderData(MakeUnique<FSkeletalMeshRenderData>());
				GetSkeletalMeshRenderData()->Serialize(Ar, this);
			}
			else if (Ar.IsSaving())
			{

				FSkeletalMeshRenderData* LocalSkeletalMeshRenderData = GetSkeletalMeshRenderData();
				if (bCooked)
				{
#if WITH_EDITORONLY_DATA
					ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
					const ITargetPlatform* ArchiveCookingTarget = Ar.CookingTarget();
					constexpr bool bIsSerializeSaving = true;
					if (ArchiveCookingTarget)
					{
						LocalSkeletalMeshRenderData = &::GetPlatformSkeletalMeshRenderData(this, ArchiveCookingTarget, bIsSerializeSaving);
					}
					else
					{
						//Fall back in case we use an archive that the cooking target has not been set (i.e. Duplicate archive)
						check(RunningPlatform != NULL);
						LocalSkeletalMeshRenderData = &::GetPlatformSkeletalMeshRenderData(this, RunningPlatform, bIsSerializeSaving);
					}
#endif
					int32 MaxBonesPerChunk = LocalSkeletalMeshRenderData->GetMaxBonesPerSection();

					TArray<FName> DesiredShaderFormats;
					Ar.CookingTarget()->GetAllTargetedShaderFormats(DesiredShaderFormats);

					for (int32 FormatIndex = 0; FormatIndex < DesiredShaderFormats.Num(); ++FormatIndex)
					{
						const EShaderPlatform LegacyShaderPlatform = ShaderFormatToLegacyShaderPlatform(DesiredShaderFormats[FormatIndex]);
						const ERHIFeatureLevel::Type FeatureLevelType = GetMaxSupportedFeatureLevel(LegacyShaderPlatform);

						int32 MaxNrBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones(Ar.CookingTarget());
						if (MaxBonesPerChunk > MaxNrBones)
						{
							FString FeatureLevelName;
							GetFeatureLevelName(FeatureLevelType, FeatureLevelName);
							UE_LOG(LogSkeletalMesh, Warning, TEXT("Skeletal mesh %s has a LOD section with %d bones and the maximum supported number for feature level %s is %d.\n!This mesh will not be rendered on the specified platform!"),
								*GetFullName(), MaxBonesPerChunk, *FeatureLevelName, MaxNrBones);
						}
					}
				}
				LocalSkeletalMeshRenderData->Serialize(Ar, this);
			}
		}
	}

	// make sure we're counting properly
	if ((!Ar.IsLoading() && !Ar.IsSaving()) || Ar.IsTransacting())
	{
		Ar << GetRefBasesInvMatrix();
	}

	if( Ar.UEVer() < VER_UE4_REFERENCE_SKELETON_REFACTOR )
	{
		TMap<FName, int32> DummyNameIndexMap;
		Ar << DummyNameIndexMap;
	}

	//@todo legacy
	TArray<UObject*> DummyObjs;
	Ar << DummyObjs;

	if (Ar.IsLoading() && Ar.CustomVer(FRenderingObjectVersion::GUID) < FRenderingObjectVersion::TextureStreamingMeshUVChannelData)
	{
		TArray<float> CachedStreamingTextureFactors;
		Ar << CachedStreamingTextureFactors;
	}

#if WITH_EDITORONLY_DATA
	if ( !StripFlags.IsEditorDataStripped() )
	{
		// Backwards compat for old SourceData member
		// Doing a <= check here as no asset from UE streams could ever have been saved at exactly 11, but a stray no-op vesion increment was added
		// in Fortnite/Main meaning some assets there were at exactly version 11. Doing a <= allows us to properly apply this version even to those assets
		if (Ar.IsLoading() && Ar.CustomVer(FSkeletalMeshCustomVersion::GUID) <= FSkeletalMeshCustomVersion::RemoveSourceData)
		{
			bool bHaveSourceData = false;
			Ar << bHaveSourceData;
			if (bHaveSourceData)
			{
				FSkeletalMeshLODModel DummyLODModel;
				DummyLODModel.Serialize(Ar, this, INDEX_NONE);
			}
		}
	}

	if ( Ar.IsLoading() && Ar.UEVer() < VER_UE4_ASSET_IMPORT_DATA_AS_JSON && !GetAssetImportData())
	{
		// AssetImportData should always be valid
		SetAssetImportData(NewObject<UAssetImportData>(this, TEXT("AssetImportData")));
	}
	
	// SourceFilePath and SourceFileTimestamp were moved into a subobject
	if ( Ar.IsLoading() && Ar.UEVer() < VER_UE4_ADDED_FBX_ASSET_IMPORT_DATA && GetAssetImportData())
	{
		// AssetImportData should always have been set up in the constructor where this is relevant
		FAssetImportInfo Info;
		Info.Insert(FAssetImportInfo::FSourceFile(SourceFilePath_DEPRECATED));
		GetAssetImportData()->SourceData = MoveTemp(Info);
		
		SourceFilePath_DEPRECATED = TEXT("");
		SourceFileTimestamp_DEPRECATED = TEXT("");
	}

	if (Ar.UEVer() >= VER_UE4_APEX_CLOTH)
	{
		if(Ar.CustomVer(FSkeletalMeshCustomVersion::GUID) < FSkeletalMeshCustomVersion::NewClothingSystemAdded)
		{
		// Serialize non-UPROPERTY ApexClothingAsset data.
			for(int32 Idx = 0; Idx < ClothingAssets_DEPRECATED.Num(); Idx++)
		{
				Ar << ClothingAssets_DEPRECATED[Idx];
			}
		}

		if (Ar.UEVer() < VER_UE4_REFERENCE_SKELETON_REFACTOR)
		{
			RebuildRefSkeletonNameToIndexMap();
		}
	}

	if ( Ar.IsLoading() && Ar.UEVer() < VER_UE4_MOVE_SKELETALMESH_SHADOWCASTING )
	{
		// Previous to this version, shadowcasting flags were stored in the LODInfo array
		// now they're in the Materials array so we need to move them over
		MoveDeprecatedShadowFlagToMaterials();
	}

	if (Ar.UEVer() < VER_UE4_SKELETON_ASSET_PROPERTY_TYPE_CHANGE)
	{
		GetPreviewAttachedAssetContainer().SaveAttachedObjectsFromDeprecatedProperties();
	}
#endif

	if (GetEnablePerPolyCollision())
	{
		const USkeletalMesh* ConstThis = this;
		UBodySetup* LocalBodySetup = ConstThis->GetBodySetup();
		Ar << LocalBodySetup;
		SetBodySetup(LocalBodySetup);
	}

#if WITH_EDITORONLY_DATA
	if (Ar.CustomVer(FEditorObjectVersion::GUID) < FEditorObjectVersion::RefactorMeshEditorMaterials)
	{
		MoveMaterialFlagsToSections();
	}
#endif

#if WITH_EDITORONLY_DATA
	SetRequiresLODScreenSizeConversion(Ar.CustomVer(FFrameworkObjectVersion::GUID) < FFrameworkObjectVersion::LODsUseResolutionIndependentScreenSize);
	SetRequiresLODHysteresisConversion(Ar.CustomVer(FFrameworkObjectVersion::GUID) < FFrameworkObjectVersion::LODHysteresisUseResolutionIndependentScreenSize);
#endif

	if (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::ConvertReductionSettingOptions)
	{
		for (int32 LodIndex = 1, LODCount = GetLODNum(); LodIndex < LODCount; LodIndex++)
		{
			FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(LodIndex);
			// prior to this version, both of them were used
			ThisLODInfo.ReductionSettings.ReductionMethod = SMOT_TriangleOrDeviation;
			if (ThisLODInfo.ReductionSettings.MaxDeviationPercentage == 0.f)
			{
				// 0.f and 1.f should produce same result. However, it is bad to display 0.f in the slider
				// as 0.01 and 0.f causes extreme confusion. 
				ThisLODInfo.ReductionSettings.MaxDeviationPercentage = 1.f;
			}
		}
	}

	if (Ar.IsLoading() && Ar.CustomVer(FEditorObjectVersion::GUID) < FEditorObjectVersion::SkeletalMeshBuildRefactor)
	{
		for (int32 LodIndex = 0, LODCount = GetLODNum(); LodIndex < LODCount; LodIndex++)
		{
			FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(LodIndex);
			// Restore the deprecated settings
			ThisLODInfo.BuildSettings.bUseFullPrecisionUVs = bUseFullPrecisionUVs_DEPRECATED;
			ThisLODInfo.BuildSettings.bUseHighPrecisionTangentBasis = bUseHighPrecisionTangentBasis_DEPRECATED;
			ThisLODInfo.BuildSettings.bRemoveDegenerates = true;
			//We cannot get back the imported build option here since those option are store in the UAssetImportData which FBX has derive in the UnrealEd module
			//We are in engine module so there is no way to recover this data.
			//Anyway because the asset was not re-import yet the build settings will not be shown in the UI and the asset will not be build
			//With the new build until it get re-import (geo and skinning)
			//So we will leave the default value for the rest of the new build settings
		}
	}
	
	// Automatically detect assets saved before CL 16135278 which changed F16 to RTNE
	//	set them to bUseBackwardsCompatibleF16TruncUVs
	if (Ar.IsLoading() && Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::DirLightsAreAtmosphereLightsByDefault)
	{
		for (int32 LodIndex = 0, LODCount = GetLODNum(); LodIndex < LODCount; LodIndex++)
		{
			FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(LodIndex);

			ThisLODInfo.BuildSettings.bUseBackwardsCompatibleF16TruncUVs = true;
		}
	}

#if WITH_EDITOR
	// Preload MeshClothingAssets because we call ConditionalPostLoad on them in our PostLoad. The PostLoad of these assets requires the data to actually have been loaded already
	if (Ar.IsLoading())
	{
		for (UClothingAssetBase* MeshClothingAsset : GetMeshClothingAssets())
		{
			if (MeshClothingAsset)
			{
				Ar.Preload(MeshClothingAsset);
			}
		}
	}
#endif //WITH_EDITOR
}

#if WITH_EDITORONLY_DATA
void USkeletalMesh::DeclareCustomVersions(FArchive& Ar, const UClass* SpecificSubclass)
{
	Super::DeclareCustomVersions(Ar, SpecificSubclass);
	FSkeletalMaterial::DeclareCustomVersions(Ar);
	FSkeletalMeshLODModel::DeclareCustomVersions(Ar);
}
#endif

void USkeletalMesh::GetPreloadDependencies(TArray<UObject*>& OutDeps)
{
	Super::GetPreloadDependencies(OutDeps);
	OutDeps.Add(GetSkeleton());
}

void USkeletalMesh::FlushRenderState()
{
	//TComponentReregisterContext<USkeletalMeshComponent> ReregisterContext;

	// Release the mesh's render resources.
	ReleaseResources();

	// Flush the resource release commands to the rendering thread to ensure that the edit change doesn't occur while a resource is still
	// allocated, and potentially accessing the mesh data.
	ReleaseResourcesFence.Wait();
}

#if WITH_EDITOR

void USkeletalMesh::PrepareForAsyncCompilation()
{
	// Make sure statics are initialized before calling from multiple threads
	GetSkeletalMeshDerivedDataVersion();

	// Make sure the target platform is properly initialized before accessing it from multiple threads
	ITargetPlatformManagerModule& TargetPlatformManager = GetTargetPlatformManagerRef();
	ITargetPlatform* RunningPlatform = TargetPlatformManager.GetRunningTargetPlatform();
	check(RunningPlatform);

	// Ensure those modules are loaded on the main thread - we'll need them in async tasks
	FModuleManager::Get().LoadModuleChecked<IMeshUtilities>(TEXT("MeshUtilities"));
	FModuleManager::Get().LoadModuleChecked<IMeshReductionManagerModule>(TEXT("MeshReductionInterface"));
	IMeshBuilderModule::GetForRunningPlatform();
	for (const ITargetPlatform* TargetPlatform : TargetPlatformManager.GetActiveTargetPlatforms())
	{
		IMeshBuilderModule::GetForPlatform(TargetPlatform);
	}

	// Release any property that are not touched by async build/postload here

	// The properties are still protected so if an async step tries to 
	// use them without protection, it will assert and will mean we have
	// to either avoid touching them asynchronously or we need to remove
	// the release property here at the cost of maybe causing more stalls
	// from the game thread.

	// Not touched during async build and can cause stalls when the content browser refresh its tiles.
	ReleaseAsyncProperty((uint64)ESkeletalMeshAsyncProperties::ThumbnailInfo);
}

void USkeletalMesh::Build()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::Build);

	// Tell the compiler to finish compiling us if we have a pending
	// compilation ongoing plus any dependency (i.e. UGroomBindings).
	FAssetCompilingManager::Get().FinishCompilationForObjects({this});

	FSkinnedAssetAsyncBuildScope AsyncBuildScope(this);

	FSkinnedAssetBuildContext Context;
	BeginBuildInternal(Context);
	
	if (FSkinnedAssetCompilingManager::Get().IsAsyncCompilationAllowed(this))
	{
		PrepareForAsyncCompilation();

		FQueuedThreadPool* SkeletalMeshThreadPool = FSkinnedAssetCompilingManager::Get().GetThreadPool();
		EQueuedWorkPriority BasePriority = FSkinnedAssetCompilingManager::Get().GetBasePriority(this);
		check(AsyncTask == nullptr);
		AsyncTask = MakeUnique<FSkinnedAssetAsyncBuildTask>(this, MoveTemp(Context));
		AsyncTask->StartBackgroundTask(SkeletalMeshThreadPool, BasePriority, EQueuedWorkFlags::DoNotRunInsideBusyWait);
		FSkinnedAssetCompilingManager::Get().AddSkinnedAssets({ this });
	}
	else
	{
		ExecuteBuildInternal(Context);
		FinishBuildInternal(Context);
	}
}

void USkeletalMesh::BeginBuildInternal(FSkinnedAssetBuildContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::BeginBuildInternal);

	// Unregister all instances of this component
	Context.RecreateRenderStateContext = MakeUnique<FSkinnedMeshComponentRecreateRenderStateContext>(this, false);

	// Release the static mesh's resources.
	ReleaseResources();

	//Make sure InlineReduction structure are created
	const int32 MaxLodIndex = GetLODNum() - 1;
	if (GetImportedModel()->InlineReductionCacheDatas.IsValidIndex(MaxLodIndex))
	{
		//We should not do that in main thread, this is why there is an ensure
		GetImportedModel()->InlineReductionCacheDatas.AddDefaulted((MaxLodIndex + 1) - GetImportedModel()->InlineReductionCacheDatas.Num());
	}

	// Flush the resource release commands to the rendering thread to ensure that the build doesn't occur while a resource is still
	// allocated, and potentially accessing the USkeletalMesh.
	ReleaseResourcesFence.Wait();

	// Lock all properties that should not be modified/accessed during async post-load
	AcquireAsyncProperty();
}

void USkeletalMesh::ExecuteBuildInternal(FSkinnedAssetBuildContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::ExecuteBuildInternal);

	// This scope allows us to use any locked properties without causing stalls
	FSkinnedAssetAsyncBuildScope AsyncBuildScope(this);

	// rebuild render data from imported model
	CacheDerivedData(&Context);

	GetSamplingInfoInternal().BuildRegions(this);
	GetSamplingInfoInternal().BuildWholeMesh(this);

	UpdateUVChannelData(true);
	UpdateGenerateUpToData();
}

void USkeletalMesh::ApplyFinishBuildInternalData(FSkinnedAssetCompilationContext* ContextPtr)
{
	//We cannot execute this code outside of the game thread
	checkf(IsInGameThread(), TEXT("Cannot execute function USkeletalMesh::ApplyFinishBuildInternalData asynchronously. Asset: %s"), *this->GetFullName());
	check(ContextPtr);

	//Apply the morphtargets change if any
	if (ContextPtr->FinishBuildMorphTargetData.IsValid())
	{
		// Morph target is only supported on USkeletalMesh
		ContextPtr->FinishBuildMorphTargetData->ApplyEditorData(this, ContextPtr->bIsSerializeSaving);
	}
}

void USkeletalMesh::FinishBuildInternal(FSkinnedAssetBuildContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::FinishBuildInternal);

	ReleaseAsyncProperty();

	ApplyFinishBuildInternalData(&Context);
	
	// Note: meshes can be built during automated importing.  We should not create resources in that case
	// as they will never be released when this object is deleted
	if (FApp::CanEverRender())
	{
		// Reinitialize the static mesh's resources.
		InitResources();
	}

	PostMeshCached.Broadcast(this);
}

FEvent* USkeletalMesh::LockPropertiesUntil()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::Import);
	
	check(IsInGameThread());
	
	FEvent* Event = FPlatformProcess::GetSynchEventFromPool();
	check(Event);

	// Tell the compiler to finish compiling us if we have a pending
	// compilation ongoing plus any dependency (i.e. UGroomBindings).
	FAssetCompilingManager::Get().FinishCompilationForObjects({ this });

	//Use the async task compile to lock the properties
	FSkinnedAsyncTaskContext Context(Event);
	BeginAsyncTaskInternal(Context);
	PrepareForAsyncCompilation();
	FQueuedThreadPool* SkeletalMeshThreadPool = FSkinnedAssetCompilingManager::Get().GetThreadPool();
	EQueuedWorkPriority BasePriority = FSkinnedAssetCompilingManager::Get().GetBasePriority(this);
	check(AsyncTask == nullptr);
	AsyncTask = MakeUnique<FSkinnedAssetAsyncBuildTask>(this, MoveTemp(Context));
	AsyncTask->StartBackgroundTask(SkeletalMeshThreadPool, BasePriority, EQueuedWorkFlags::DoNotRunInsideBusyWait);
	FSkinnedAssetCompilingManager::Get().AddSkinnedAssets({ this });
	return Event;
}

void USkeletalMesh::BeginAsyncTaskInternal(FSkinnedAsyncTaskContext& Context)
{
	check(IsInGameThread());

	AcquireAsyncProperty();
	//Allow thumbnail data so content browser get refresh properly
	ReleaseAsyncProperty((uint64)ESkeletalMeshAsyncProperties::ThumbnailInfo);
}

void USkeletalMesh::ExecuteAsyncTaskInternal(FSkinnedAsyncTaskContext& Context)
{
	if (ensure(Context.Event))
	{
		Context.Event->Wait();
		FPlatformProcess::ReturnSynchEventToPool(Context.Event);
	}
}

void USkeletalMesh::FinishAsyncTaskInternal(FSkinnedAsyncTaskContext& Context)
{
	check(IsInGameThread());
	ReleaseAsyncProperty();
}

#endif // #if WITH_EDITOR

void USkeletalMesh::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	// check the parent index of the root bone is invalid
	check((GetRefSkeleton().GetNum() == 0) || (GetRefSkeleton().GetRefBoneInfo()[0].ParentIndex == INDEX_NONE));

	Super::PreSave(ObjectSaveContext);
}

// Pre-calculate refpose-to-local transforms
void USkeletalMesh::CalculateInvRefMatrices()
{
	const int32 NumRealBones = GetRefSkeleton().GetRawBoneNum();
	
	TArray<FMatrix>& ComposedRefPoseMatrices = GetCachedComposedRefPoseMatrices();

	if(GetRefBasesInvMatrix().Num() != NumRealBones)
	{
		GetRefBasesInvMatrix().Empty(NumRealBones);
		GetRefBasesInvMatrix().AddUninitialized(NumRealBones);

		// Reset cached mesh-space ref pose
		ComposedRefPoseMatrices.Empty(NumRealBones);
		ComposedRefPoseMatrices.AddUninitialized(NumRealBones);

		// Precompute the Mesh.RefBasesInverse.
		for( int32 b=0; b<NumRealBones; b++)
		{
			// Render the default pose.
			ComposedRefPoseMatrices[b] = GetRefPoseMatrix(b);

			// Construct mesh-space skeletal hierarchy.
			if( b>0 )
			{
				int32 Parent = GetRefSkeleton().GetRawParentIndex(b);
				ComposedRefPoseMatrices[b] = ComposedRefPoseMatrices[b] * ComposedRefPoseMatrices[Parent];
			}

			FVector XAxis, YAxis, ZAxis;

			ComposedRefPoseMatrices[b].GetScaledAxes(XAxis, YAxis, ZAxis);
			if(	XAxis.IsNearlyZero(UE_SMALL_NUMBER) &&
				YAxis.IsNearlyZero(UE_SMALL_NUMBER) &&
				ZAxis.IsNearlyZero(UE_SMALL_NUMBER))
			{
				// this is not allowed, warn them 
				UE_LOG(LogSkeletalMesh, Warning, TEXT("Reference Pose for asset %s for joint (%s) includes NIL matrix. Zero scale isn't allowed on ref pose. "), *GetPathName(), *GetRefSkeleton().GetBoneName(b).ToString());
			}

			// Precompute inverse so we can use from-refpose-skin vertices.
			GetRefBasesInvMatrix()[b] = FMatrix44f(ComposedRefPoseMatrices[b].Inverse());
		}
	}
}

#if WITH_EDITOR

void USkeletalMesh::CalculateRequiredBones(FSkeletalMeshLODModel& LODModel, const struct FReferenceSkeleton& InRefSkeleton, const TMap<FBoneIndexType, FBoneIndexType> * BonesToRemove)
{
	// RequiredBones for base model includes all raw bones.
	int32 RequiredBoneCount = InRefSkeleton.GetRawBoneNum();
	LODModel.RequiredBones.Empty(RequiredBoneCount);
	for(int32 i=0; i<RequiredBoneCount; i++)
	{
		// Make sure it's not in BonesToRemove
		// @Todo change this to one TArray
		if (!BonesToRemove || BonesToRemove->Find(i) == NULL)
		{
			LODModel.RequiredBones.Add(i);
		}
	}

	LODModel.RequiredBones.Shrink();	
}

void USkeletalMesh::RemoveLegacyClothingSections()
{
	// Remove duplicate skeletal mesh sections previously used for clothing simulation
	if(GetLinkerCustomVersion(FSkeletalMeshCustomVersion::GUID) < FSkeletalMeshCustomVersion::RemoveDuplicatedClothingSections)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::RemoveLegacyClothingSections);

		if(FSkeletalMeshModel* Model = GetImportedModel())
		{
			for(FSkeletalMeshLODModel& LodModel : Model->LODModels)
			{
				int32 ClothingSectionCount = 0;
				uint32 BaseVertex = MAX_uint32;
				int32 VertexCount = 0;
				uint32 BaseIndex = MAX_uint32;
				int32 IndexCount = 0;

				for(int32 SectionIndex = 0; SectionIndex < LodModel.Sections.Num(); ++SectionIndex)
				{
					FSkelMeshSection& Section = LodModel.Sections[SectionIndex];

					// If the section is disabled, it could be a clothing section
					if(Section.bLegacyClothingSection_DEPRECATED && Section.CorrespondClothSectionIndex_DEPRECATED != INDEX_NONE)
					{
						FSkelMeshSection& DuplicatedSection = LodModel.Sections[Section.CorrespondClothSectionIndex_DEPRECATED];

						// Cache the base index for the first clothing section (will be in correct order)
						BaseVertex = FMath::Min(DuplicatedSection.BaseVertexIndex, BaseVertex);
						BaseIndex = FMath::Min(DuplicatedSection.BaseIndex, BaseIndex);

						VertexCount += DuplicatedSection.SoftVertices.Num();
						IndexCount += DuplicatedSection.NumTriangles * 3;

						// Mapping data for clothing could be built either on the source or the
						// duplicated section and has changed a few times, so check here for
						// where to get our data from
						constexpr int32 ClothLODBias = 0;  // There isn't any cloth LOD bias on legacy sections
						if (DuplicatedSection.ClothMappingDataLODs.Num() && DuplicatedSection.ClothMappingDataLODs[ClothLODBias].Num())
						{
							Section.ClothingData = DuplicatedSection.ClothingData;
							Section.ClothMappingDataLODs = DuplicatedSection.ClothMappingDataLODs;
						}

						Section.CorrespondClothAssetIndex = GetMeshClothingAssets().IndexOfByPredicate([&Section](const UClothingAssetBase* CurrAsset)
						{
							return CurrAsset && CurrAsset->GetAssetGuid() == Section.ClothingData.AssetGuid;
						});

						Section.BoneMap = DuplicatedSection.BoneMap;
						Section.bLegacyClothingSection_DEPRECATED = false;

						// Remove the reference index
						Section.CorrespondClothSectionIndex_DEPRECATED = INDEX_NONE;

						//Make sure the UserSectionsData is up to date
						if (FSkelMeshSourceSectionUserData* SectionUserData = LodModel.UserSectionsData.Find(Section.OriginalDataSectionIndex))
						{
							SectionUserData->CorrespondClothAssetIndex = Section.CorrespondClothAssetIndex;
							SectionUserData->ClothingData = Section.ClothingData;
						}

						ClothingSectionCount++;
					}
					else
					{
						Section.CorrespondClothAssetIndex = INDEX_NONE;
						Section.ClothingData.AssetGuid = FGuid();
						Section.ClothingData.AssetLodIndex = INDEX_NONE;
						Section.ClothMappingDataLODs.Empty();
					}
				}

				if(BaseVertex != MAX_uint32 && BaseIndex != MAX_uint32)
				{
					// Remove from section list
					LodModel.Sections.RemoveAt(LodModel.Sections.Num() - ClothingSectionCount, ClothingSectionCount);

					// Clean up actual geometry
					LodModel.IndexBuffer.RemoveAt(BaseIndex, IndexCount);
					LodModel.NumVertices -= VertexCount;

					// Clean up index entries above the base we removed.
					// Ideally this shouldn't be unnecessary as clothing was at the end of the buffer
					// but this will always be safe to run to make sure adjacency generates correctly.
					for(uint32& Index : LodModel.IndexBuffer)
					{
						if(Index >= BaseVertex)
						{
							Index -= VertexCount;
						}
					}
				}
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////
//// Source Model API

int32 USkeletalMesh::GetNumSourceModels() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return SourceModels.Num();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::SetNumSourceModels(const int32 InNumSourceModels)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	SetNumSourceModelsInternal(InNumSourceModels);
}

void USkeletalMesh::SetNumSourceModelsInternal(const int32 InNumSourceModels)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	const int32 OldNumSourceModels = SourceModels.Num();

	// If we're lowering the LOD count, clear out any mesh data stored immediately rather
	// than wait for GC to do the work.
	if (OldNumSourceModels > InNumSourceModels)
	{
		for (int32 Index = InNumSourceModels; Index < OldNumSourceModels; Index++)
		{
			FSkeletalMeshSourceModel& SourceModel = SourceModels[Index];
			SourceModel.ClearAllMeshData();
		}
	}

	SourceModels.SetNum(InNumSourceModels);

	for (int32 Index = OldNumSourceModels; Index < InNumSourceModels; Index++)
	{
		SourceModels[Index].Initialize(this);
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::RemoveSourceModel(const int32 InLODIndex)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	RemoveSourceModelInternal(InLODIndex);	
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::RemoveSourceModelInternal(const int32 InLODIndex)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (ensure(SourceModels.IsValidIndex(InLODIndex)))
	{
		FSkeletalMeshSourceModel& SourceModel = SourceModels[InLODIndex];

		SourceModel.ClearAllMeshData();
		SourceModels.RemoveAt(InLODIndex);
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}


TConstArrayView<FSkeletalMeshSourceModel> USkeletalMesh::GetAllSourceModels() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return SourceModels;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

TArrayView<FSkeletalMeshSourceModel> USkeletalMesh::GetAllSourceModels()
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return SourceModels;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

const FSkeletalMeshSourceModel& USkeletalMesh::GetSourceModel(const int32 InLODIndex) const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	check(SourceModels.IsValidIndex(InLODIndex));
	return SourceModels[InLODIndex];
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

FSkeletalMeshSourceModel& USkeletalMesh::GetSourceModel(const int32 InLODIndex)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	check(SourceModels.IsValidIndex(InLODIndex));
	return SourceModels[InLODIndex];
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}


///////////////////////////////////////////////////////////////////////
//// Mesh Description API

FMeshDescription* USkeletalMesh::GetMeshDescription(const int32 InLODIndex) const
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return nullptr;
	}

	return GetSourceModel(InLODIndex).GetMeshDescription();
}


bool USkeletalMesh::CloneMeshDescription(const int32 InLODIndex, FMeshDescription& OutMeshDescription) const
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return false;
	}

	return GetSourceModel(InLODIndex).CloneMeshDescription(OutMeshDescription);
}


bool USkeletalMesh::HasMeshDescription(const int32 InLODIndex) const
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return false;
	}

	return GetSourceModel(InLODIndex).HasMeshDescription();
}


FMeshDescription* USkeletalMesh::CreateMeshDescription(const int32 InLODIndex)
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return nullptr;
	}

	return GetSourceModel(InLODIndex).CreateMeshDescription();
}


FMeshDescription* USkeletalMesh::CreateMeshDescription(const int32 InLODIndex, FMeshDescription&& InMeshDescription)
{
	FMeshDescription* MeshDescription = CreateMeshDescription(InLODIndex);
	if (MeshDescription)
	{
		*MeshDescription = MoveTemp(InMeshDescription);
	}

	return MeshDescription;
}


bool USkeletalMesh::CommitMeshDescription(
	const int32 InLODIndex,
	const FCommitMeshDescriptionParams& InParams
	)
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return false;
	}

	FSkeletalMeshSourceModel& SourceModel = GetSourceModel(InLODIndex);
	const bool bUseHashAsGuid = !InParams.bForceUpdate;
	SourceModel.CommitMeshDescription(bUseHashAsGuid);

	if (InLODIndex == 0)
	{
		SetImportedBounds(SourceModel.GetBoundsFast());
	}
	
	if (SourceModel.HasMeshDescription())
	{
		if (InParams.bUpdateMorphTargets)
		{
			static FCriticalSection MorphTargetUpdateMutex;

			// Since MorphTargets/MorphTargetIndexMap are USkeletalMesh members, we want to 
			// avoid multiple threads all mutating them at the same time, in case we have a 
			// geometry processor that is committing multiple meshes across differing LODs 
			// simultaneously.
			FScopeLock ScopeLock(&MorphTargetUpdateMutex);

			TArray<TObjectPtr<UMorphTarget>>& ExistingMorphTargets = GetMorphTargets();
			TSet<FName> ExistingMorphTargetNames;
			for (TObjectPtr<UMorphTarget> MorphTarget: ExistingMorphTargets)
			{
				ExistingMorphTargetNames.Add(MorphTarget->GetFName());
			}
			
			TSet<FName> ValidMorphTargetNames;
			for (const FSkeletalMeshSourceModel& OtherSourceModels: GetAllSourceModels())
			{
				ValidMorphTargetNames.Append(OtherSourceModels.GetMorphTargetNames());
			}

			// Add in a dummy UMorphTarget placeholder for any morph target that is being added.
			bool bMorphTargetsChanged = false;
			for (const FName& MorphTargetName: ValidMorphTargetNames)
			{
				if (!ExistingMorphTargetNames.Contains(MorphTargetName))
				{
					UMorphTarget* MorphTarget = nullptr;
					// See if object already exists.
					UObject* Obj = StaticFindObjectFastInternal(nullptr, this, MorphTargetName, true);
					if (Obj)
					{
						MorphTarget = Cast<UMorphTarget>(Obj);
						if (MorphTarget)
						{
							//Make sure the UObject is not garbage anymore and there is no data
							MorphTarget->ClearGarbage();
							MorphTarget->EmptyMorphLODModels();
							MorphTarget->SetFlags(RF_Standalone);
						}
						else
						{
							UE_LOG(LogSkeletalMesh, Error, TEXT("Skeletal Mesh (%s) : Commit mesh description, cannot create a morph target name %s because a subobject of class %s already exist"), *GetPathName(), *(MorphTargetName.ToString()), *(Obj->GetClass()->GetName()));
						}
					}
					else
					{
						MorphTarget = NewObject<UMorphTarget>(this, MorphTargetName);
					}

					if (MorphTarget)
					{
						MorphTarget->BaseSkelMesh = this;
						MorphTarget->ClearInternalFlags(EInternalObjectFlags::Async);
						ExistingMorphTargets.Add(MorphTarget);
					}
					bMorphTargetsChanged = true;
				}
			}

			// Remove any existing morph targets that don't have a corresponding representation on
			// any of the source models.
			if (ExistingMorphTargets.RemoveAll([&ValidMorphTargetNames](const TObjectPtr<UMorphTarget>& InMorphTarget)
				{ return !ValidMorphTargetNames.Contains(InMorphTarget->GetFName()); }) != 0)
			{
				bMorphTargetsChanged = true;
			}

			if (bMorphTargetsChanged)
			{
				constexpr bool bKeepEmptyMorphTargets = true;
				InitMorphTargets(bKeepEmptyMorphTargets);

				// Ensure all components are working from the latest morph target data.
				if (IsInGameThread())
				{
					for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
					{
						if (It->GetSkeletalMeshAsset() == this)
						{
							It->RefreshMorphTargets();
						}
					}
				}
			}
		}
	
		if (InParams.bUpdateSkinWeightProfiles)
		{
			static FCriticalSection ProfileUpdateMutex;

			// Since SkinWeightProfiles is a USkeletalMesh member, we want to avoid multiple
			// threads all mutating it at the same time, in case we have a geometry processor
			// that is committing multiple meshes across differing LODs simultaneously.
			FScopeLock ScopeLock(&ProfileUpdateMutex);
			
			TArray<FSkinWeightProfileInfo>& ExistingProfiles = GetSkinWeightProfiles();
			TSet<FName> ExistingProfileNames;

			for (const FSkinWeightProfileInfo& ProfileInfo: ExistingProfiles)
			{
				ExistingProfileNames.Add(ProfileInfo.Name);
			}

			// Get all profiles from the models on all LODs, since we may have some that aren't
			// defined on the skeletal mesh's list of profiles.
			TSet<FName> ValidProfileNames;
			for (const FSkeletalMeshSourceModel& OtherSourceModel: GetAllSourceModels())
			{
				ValidProfileNames.Append(OtherSourceModel.GetSkinWeightProfileNames());
			}
			for (const FName& ProfileName: ValidProfileNames)
			{
				if (!ExistingProfileNames.Contains(ProfileName))
				{
					FSkinWeightProfileInfo& NewProfile = ExistingProfiles.AddDefaulted_GetRef();
					NewProfile.Name = ProfileName;
				}
			}

			// Remove all profiles listed on the skeletal mesh that no longer have a correspondence 
			// on the source models. 
			ExistingProfiles.RemoveAll([&ValidProfileNames](const FSkinWeightProfileInfo& InProfileInfo)
			{
				return !ValidProfileNames.Contains(InProfileInfo.Name);
			});
		}
		
		if (InParams.bUpdateVertexAttributes)
		{
			TSet<FName> MeshVertexAttributes;
			
			// NOTE: We're currently limited to just single-channel attributes for rendering.
			SourceModel.GetMeshDescription()->VertexAttributes().ForEachByType<float>([&MeshVertexAttributes](const FName InAttributeName, TVertexAttributesConstRef<float> InAttributeRef)
			{
				if (!FSkeletalMeshAttributes::IsReservedAttributeName(InAttributeName))
				{
					MeshVertexAttributes.Add(InAttributeName);
				}
			});

			bool bVertexAttributesChanged = false;
			TArray<FSkeletalMeshVertexAttributeInfo>& ExistingVertexAttributes = GetLODInfo(InLODIndex)->VertexAttributes;
			TSet<FName> ExistingVertexAttributeNames;
			for (const FSkeletalMeshVertexAttributeInfo& AttributeInfo: ExistingVertexAttributes)
			{
				ExistingVertexAttributeNames.Add(AttributeInfo.Name);
			}
			
			for (FName AttributeName: MeshVertexAttributes)
			{
				if (!ExistingVertexAttributeNames.Contains(AttributeName))
				{
					FSkeletalMeshVertexAttributeInfo& NewAttribute = ExistingVertexAttributes.AddDefaulted_GetRef();
					NewAttribute.Name = AttributeName;

					bVertexAttributesChanged = true;
				}
			}

			// Remove all attributes from the LOD that no longer exist on the mesh.
			if (ExistingVertexAttributes.RemoveAll([&MeshVertexAttributes](const FSkeletalMeshVertexAttributeInfo& InInfo)
				{
					return !MeshVertexAttributes.Contains(InInfo.Name);
				}))
			{
				bVertexAttributesChanged = true;
			}

			if (bVertexAttributesChanged)
			{
#if WITH_EDITOR
				// Notify UI and other systems of the change
				// Dispatch it on the game thread for thread-safety as this can be called on a worker thread
				FFunctionGraphTask::CreateAndDispatchWhenReady(
					[WeakSkelMesh = TWeakObjectPtr<USkeletalMesh>(this)]()
					{
						if (USkeletalMesh* SkeletalMesh = WeakSkelMesh.Get())
						{
							SkeletalMesh->GetOnVertexAttributesArrayChanged().Broadcast();
						}
					}, TStatId(), nullptr, ENamedThreads::GameThread);	
#endif			
			}
			
			if (InParams.bUpdateVertexColors)
			{
				FGuid SourceVertexColorGuid{};	// Zero-initialized
				bool bSourceHasVertexColors = false;
				for (const FSkeletalMeshSourceModel& OtherSourceModel: GetAllSourceModels())
				{
					TOptional<FGuid> SourceModelVertexColorGuid = OtherSourceModel.GetVertexColorGuid();
					if (SourceModelVertexColorGuid.IsSet())
					{
						bSourceHasVertexColors = true;
						SourceVertexColorGuid = FGuid::Combine(SourceVertexColorGuid, *SourceModelVertexColorGuid);
					}
				}
				
				SetHasVertexColors(bSourceHasVertexColors);
				if (bSourceHasVertexColors)
				{
					SetVertexColorGuid(SourceVertexColorGuid);
				}
			}
		}
	}

	if (ensure(GetImportedModel()->LODModels.IsValidIndex(InLODIndex)))
	{
		const FMeshDescriptionBulkData& BulkData = SourceModel.MeshDescriptionBulkData->GetBulkData();
		FSkeletalMeshLODModel& LODModel = GetImportedModel()->LODModels[InLODIndex];
		
		LODModel.RawSkeletalMeshBulkDataID = BulkData.GetIdString();
		LODModel.bIsBuildDataAvailable = !BulkData.IsEmpty();
		LODModel.bIsRawSkeletalMeshBulkDataEmpty = BulkData.IsEmpty();
	}
	

	if (IsInGameThread() && InParams.bMarkPackageDirty)
	{
		(void)MarkPackageDirty();
	}
	
	return true;
}


bool USkeletalMesh::ModifyMeshDescription(const int32 InLODIndex, const bool bInAlwaysMarkPackageDirty)
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return false;
	}

	return GetSourceModel(InLODIndex).MeshDescriptionBulkData->Modify(bInAlwaysMarkPackageDirty);
}


void USkeletalMesh::ClearMeshDescription(const int32 InLODIndex)
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return;
	}

	return GetSourceModel(InLODIndex).ClearMeshDescription();
}


void USkeletalMesh::ClearAllMeshDescriptions()
{
	for (int32 LODIndex = 0, LODCount = GetLODNum(); LODIndex < LODCount; LODIndex++)
	{
		ClearMeshDescription(LODIndex);
	}
}


void USkeletalMesh::ClearMeshDescriptionAndBulkData(const int32 InLODIndex)
{
	if (!ensure(IsValidLODIndex(InLODIndex)))
	{
		return;
	}

	return GetSourceModel(InLODIndex).ClearAllMeshData();
}


void USkeletalMesh::LoadLODImportedData(const int32 LODIndex, FSkeletalMeshImportData& OutMesh) const
{
	if (!ensure(IsValidLODIndex(LODIndex)))
	{
		return;
	}

	if (const FMeshDescription* MeshDescription = GetMeshDescription(LODIndex))
	{
		if (!MeshDescription->IsEmpty())
		{
			OutMesh = FSkeletalMeshImportData::CreateFromMeshDescription(*MeshDescription);
		}
	}
}

void USkeletalMesh::SaveLODImportedData(const int32 LODIndex, const FSkeletalMeshImportData& InMesh)
{
	if (!ensure(IsValidLODIndex(LODIndex)))
	{
		return;
	}
	
	FMeshDescription MeshDescription;

	if (InMesh.GetMeshDescription(nullptr, &GetLODInfo(LODIndex)->BuildSettings, MeshDescription))
	{
		CreateMeshDescription(LODIndex, MoveTemp(MeshDescription));
		CommitMeshDescription(LODIndex);
	}
}

bool USkeletalMesh::IsLODImportedDataBuildAvailable(const int32 LODIndex) const
{
	return HasMeshDescription(LODIndex);
}

bool USkeletalMesh::IsLODImportedDataEmpty(const int32 LODIndex) const
{
	return !HasMeshDescription(LODIndex);
}

void USkeletalMesh::GetLODImportedDataVersions(const int32 LODIndex, ESkeletalMeshGeoImportVersions& OutGeoImportVersion, ESkeletalMeshSkinningImportVersions& OutSkinningImportVersion) const
{
	OutGeoImportVersion = ESkeletalMeshGeoImportVersions::SkeletalMeshBuildRefactor;
	OutSkinningImportVersion = ESkeletalMeshSkinningImportVersions::SkeletalMeshBuildRefactor;
}

void USkeletalMesh::SetLODImportedDataVersions(const int32 LODIndex, const ESkeletalMeshGeoImportVersions& InGeoImportVersion, const ESkeletalMeshSkinningImportVersions& InSkinningImportVersion)
{
}

void USkeletalMesh::CopyImportedData(int32 SrcLODIndex, USkeletalMesh* SrcSkeletalMesh, int32 DestLODIndex, USkeletalMesh* DestSkeletalMesh)
{
	FMeshDescription MeshDescription;
	if (SrcSkeletalMesh->CloneMeshDescription(SrcLODIndex, MeshDescription))
	{
		DestSkeletalMesh->CreateMeshDescription(DestLODIndex, MoveTemp(MeshDescription));
		DestSkeletalMesh->CommitMeshDescription(DestLODIndex);
	}
}

void USkeletalMesh::ReserveLODImportData(int32 MaxLODIndex)
{
}

void USkeletalMesh::ForceBulkDataResident(const int32 LODIndex)
{	
}

void USkeletalMesh::EmptyLODImportData(const int32 LODIndex)
{
	ClearMeshDescriptionAndBulkData(LODIndex);
}

void USkeletalMesh::EmptyAllImportData()
{
	for(int32 LODIndex = 0, LODCount = GetLODNum(); LODIndex < LODCount; ++LODIndex)
	{
		ClearMeshDescriptionAndBulkData(LODIndex);
	}
}


void USkeletalMesh::CreateUserSectionsDataForLegacyAssets()
{
	//Fill up the Section ChunkedParentSectionIndex and OriginalDataSectionIndex
	//We also want to create the UserSectionsData structure so the user can change the section data
	for (int32 LodIndex = 0, LODCount = GetLODNum(); LodIndex < LODCount; LodIndex++)
	{
		FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(LodIndex);
		FSkeletalMeshLODModel& ThisLODModel = GetImportedModel()->LODModels[LodIndex];

		//Reset the reduction setting to a non active state if the asset has active reduction but have no RawSkeletalMeshBulkData (we cannot reduce it)
		const bool bIsLODReductionActive = IsReductionActive(LodIndex);


		bool bMustUseReductionSourceData = bIsLODReductionActive
			&& ThisLODInfo.bHasBeenSimplified
			&& GetImportedModel()->OriginalReductionSourceMeshData_DEPRECATED.IsValidIndex(LodIndex)
			&& !(GetImportedModel()->OriginalReductionSourceMeshData_DEPRECATED[LodIndex]->IsEmpty());

		if (bIsLODReductionActive && !ThisLODInfo.bHasBeenSimplified && !HasMeshDescription(LodIndex))
		{
			if (LodIndex > ThisLODInfo.ReductionSettings.BaseLOD)
			{
				ThisLODInfo.bHasBeenSimplified = true;
			}
			else if (LodIndex == ThisLODInfo.ReductionSettings.BaseLOD)
			{
				if (ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_AbsNumOfTriangles
					|| ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_AbsNumOfVerts
					|| ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_AbsTriangleOrVert)
				{
					//MaxNum.... cannot be inactive, switch to NumOfTriangle
					ThisLODInfo.ReductionSettings.TerminationCriterion = SMTC_NumOfTriangles;
				}

				//Now that we use triangle or vert num, set an inactive value
				if (ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_NumOfTriangles
					|| ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_TriangleOrVert)
				{
					ThisLODInfo.ReductionSettings.NumOfTrianglesPercentage = 1.0f;
				}
				if (ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_NumOfVerts
					|| ThisLODInfo.ReductionSettings.TerminationCriterion == SMTC_TriangleOrVert)
				{
					ThisLODInfo.ReductionSettings.NumOfVertPercentage = 1.0f;
				}
			}
			bMustUseReductionSourceData = false;
		}

		ThisLODModel.UpdateChunkedSectionInfo(GetName());

		if (bMustUseReductionSourceData)
		{
			//We must load the reduction source model, since reduction can remove section
			FSkeletalMeshLODModel ReductionSrcLODModel;
			TMap<FString, TArray<FMorphTargetDelta>> TmpMorphTargetData;
			GetImportedModel()->OriginalReductionSourceMeshData_DEPRECATED[LodIndex]->LoadReductionData(ReductionSrcLODModel, TmpMorphTargetData, this);
			
			//Fill the user data with the original value
			TMap<int32, FSkelMeshSourceSectionUserData> BackupUserSectionsData = ThisLODModel.UserSectionsData;
			ThisLODModel.UserSectionsData.Empty();

			ThisLODModel.UserSectionsData = ReductionSrcLODModel.UserSectionsData;

			//Now restore the reduce section user change and adjust the originalDataSectionIndex to point on the correct UserSectionData
			TBitArray<> SourceSectionMatched;
			SourceSectionMatched.Init(false, ReductionSrcLODModel.Sections.Num());
			for (int32 SectionIndex = 0; SectionIndex < ThisLODModel.Sections.Num(); ++SectionIndex)
			{
				FSkelMeshSection& Section = ThisLODModel.Sections[SectionIndex];
				FSkelMeshSourceSectionUserData& BackupUserData = FSkelMeshSourceSectionUserData::GetSourceSectionUserData(BackupUserSectionsData, Section);
				for (int32 SourceSectionIndex = 0; SourceSectionIndex < ReductionSrcLODModel.Sections.Num(); ++SourceSectionIndex)
				{
					if (SourceSectionMatched[SourceSectionIndex])
					{
						continue;
					}
					FSkelMeshSection& SourceSection = ReductionSrcLODModel.Sections[SourceSectionIndex];
					FSkelMeshSourceSectionUserData& UserData = FSkelMeshSourceSectionUserData::GetSourceSectionUserData(ThisLODModel.UserSectionsData, SourceSection);
					if (Section.MaterialIndex == SourceSection.MaterialIndex)
					{
						Section.OriginalDataSectionIndex = SourceSection.OriginalDataSectionIndex;
						UserData = BackupUserData;
						SourceSectionMatched[SourceSectionIndex] = true;
						break;
					}
				}
			}
			ThisLODModel.SyncronizeUserSectionsDataArray();
		}
	}
}

void USkeletalMesh::ValidateAllLodMaterialIndexes()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::ValidateAllLodMaterialIndexes);

	for (int32 LodIndex = 0; LodIndex < GetLODNum(); LodIndex++)
	{
		FSkeletalMeshLODInfo* LODInfoPtr = GetLODInfo(LodIndex);
		if (!LODInfoPtr)
		{
			continue;
		}
		FSkeletalMeshLODModel& LODModel = GetImportedModel()->LODModels[LodIndex];
		const int32 SectionNum = LODModel.Sections.Num();
		const TArray<FSkeletalMaterial>& ListOfMaterials = GetMaterials();
		//See if more then one section use the same UserSectionData
		for (int32 SectionIndex = 0; SectionIndex < SectionNum; ++SectionIndex)
		{
			int32 LodMaterialMapOverride = INDEX_NONE;
			FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
			//Validate and fix the LODMaterialMap override
			if (LODInfoPtr->LODMaterialMap.IsValidIndex(SectionIndex) && LODInfoPtr->LODMaterialMap[SectionIndex] != INDEX_NONE)
			{
				LodMaterialMapOverride = LODInfoPtr->LODMaterialMap[SectionIndex];
				if (!ListOfMaterials.IsValidIndex(LodMaterialMapOverride))
				{
					UE_ASSET_LOG(LogSkeletalMesh
						, Display
						, this
						, TEXT("Fix LOD %d Section %d LODMaterialMap override material index from %d to INDEX_NONE. The value is not pointing on a valid Material slot index.")
						, LodIndex
						, SectionIndex
						, LodMaterialMapOverride);
					LODInfoPtr->LODMaterialMap[SectionIndex] = INDEX_NONE;
				}
			}
			//Validate and fix the section material index
			{
				if (!ListOfMaterials.IsValidIndex(Section.MaterialIndex))
				{
					if (LodMaterialMapOverride != INDEX_NONE)
					{
						UE_ASSET_LOG(LogSkeletalMesh
							, Display
							, this
							, TEXT("Fix LOD %d Section %d Material index from %d to %d. The fallback value is from the LODMaterialMap Override. The value is not pointing on a valid Material slot index.")
							, LodIndex
							, SectionIndex
							, Section.MaterialIndex
							, LodMaterialMapOverride);
						Section.MaterialIndex = LodMaterialMapOverride;
					}
					else
					{
						//Fall back on the original section index
						if (ListOfMaterials.IsValidIndex(Section.OriginalDataSectionIndex))
						{
							UE_ASSET_LOG(LogSkeletalMesh
								, Display
								, this
								, TEXT("Fix LOD %d Section %d Material index from %d to %d. The fallback value is from the OriginalDataSectionIndex. The value is not pointing on a valid Material slot index.")
								, LodIndex
								, SectionIndex
								, Section.MaterialIndex
								, Section.OriginalDataSectionIndex);
							Section.MaterialIndex = Section.OriginalDataSectionIndex;
						}
						else
						{
							UE_ASSET_LOG(LogSkeletalMesh
								, Display
								, this
								, TEXT("Fix LOD %d Section %d Material index from %d to 0. The fallback value is 0. The value is not pointing on a valid Material slot index.")
								, LodIndex
								, SectionIndex
								, Section.MaterialIndex);
							//Fallback on material index 0
							Section.MaterialIndex = 0;
						}
					}
				}
			}
		}
	}
}

void USkeletalMesh::PostLoadValidateUserSectionData()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::PostLoadValidateUserSectionData);

	for (int32 LodIndex = 0; LodIndex < GetLODNum(); LodIndex++)
	{
		const FSkeletalMeshLODInfo* LODInfoPtr = GetLODInfo(LodIndex);
		if (!LODInfoPtr || !LODInfoPtr->bHasBeenSimplified)
		{
			//We validate only generated LOD from a base LOD
			continue;
		}

		const int32 ReductionBaseLOD = LODInfoPtr->ReductionSettings.BaseLOD;
		if (!GetImportedModel()->LODModels.IsValidIndex(ReductionBaseLOD))
		{
			//The base LOD should always be valid for generated LOD
			UE_ASSET_LOG(LogSkeletalMesh, Display, this, TEXT("This asset generated lod %d, is base on an invalid LOD index %d."), LodIndex, ReductionBaseLOD);
			continue;
		}
		FSkeletalMeshLODModel& BaseReductionLODModel = GetImportedModel()->LODModels[ReductionBaseLOD];
		FSkeletalMeshLODModel& ThisLODModel = GetImportedModel()->LODModels[LodIndex];
		const int32 SectionNum = ThisLODModel.Sections.Num();
		const int32 UserSectionsDataNum = ThisLODModel.UserSectionsData.Num();
		const int32 BaseUserSectionsDataNum = BaseReductionLODModel.UserSectionsData.Num();
		//We must make sure the result is similar to what the reduction will give. So we will not have more user section data then the number we have for the base LOD.
		//Because reduction reset the UserSectionData to the number of parent section after the reduction.
		const bool bIsInlineReduction = LodIndex == ReductionBaseLOD;
		bool bLODHaveSectionIssue = !bIsInlineReduction && ( (UserSectionsDataNum > SectionNum) || (UserSectionsDataNum > BaseUserSectionsDataNum) );
		if (!bLODHaveSectionIssue)
		{
			//See if more then one section use the same UserSectionData
			TBitArray<> AvailableUserSectionData;
			AvailableUserSectionData.Init(true, ThisLODModel.UserSectionsData.Num());
			for (int32 SectionIndex = 0; SectionIndex < SectionNum; ++SectionIndex)
			{
				FSkelMeshSection& Section = ThisLODModel.Sections[SectionIndex];
				if (Section.ChunkedParentSectionIndex != INDEX_NONE)
				{
					continue;
				}
				if (!AvailableUserSectionData.IsValidIndex(Section.OriginalDataSectionIndex) || !AvailableUserSectionData[Section.OriginalDataSectionIndex])
				{
					bLODHaveSectionIssue = true;
					break;
				}
				AvailableUserSectionData[Section.OriginalDataSectionIndex] = false;
			}
			if (!bLODHaveSectionIssue)
			{
				//Everything is good nothing to fix
				continue;
			}
		}

		//Force the source UserSectionData, then restore the UserSectionData value each section was using
		//We use the source section user data entry in case we do not have any override
		TMap<int32, FSkelMeshSourceSectionUserData> NewUserSectionsData;

		int32 CurrentOriginalSectionIndex = -1;
		for (int32 SectionIndex = 0; SectionIndex < SectionNum; ++SectionIndex)
		{
			FSkelMeshSection& Section = ThisLODModel.Sections[SectionIndex];
			if (Section.ChunkedParentSectionIndex != INDEX_NONE)
			{
				//The section zero must never be a chunked children
				if (!ensure(CurrentOriginalSectionIndex >= 0))
				{
					CurrentOriginalSectionIndex = 0;
				}
				//We do not restore user section data for chunked section, the parent has already fix it
				Section.OriginalDataSectionIndex = CurrentOriginalSectionIndex;
				continue;
			}

			//Parent (non chunked) section must increment the index
			CurrentOriginalSectionIndex++;

			FSkelMeshSourceSectionUserData& SectionUserData = NewUserSectionsData.FindOrAdd(CurrentOriginalSectionIndex);
			if(const FSkelMeshSourceSectionUserData* BackupSectionUserData = ThisLODModel.UserSectionsData.Find(Section.OriginalDataSectionIndex))
			{
				SectionUserData = *BackupSectionUserData;
			}
			else if(const FSkelMeshSourceSectionUserData* BaseSectionUserData = BaseReductionLODModel.UserSectionsData.Find(CurrentOriginalSectionIndex))
			{
				SectionUserData = *BaseSectionUserData;
			}

			Section.OriginalDataSectionIndex = CurrentOriginalSectionIndex;
		}
		ThisLODModel.UserSectionsData = NewUserSectionsData;

		UE_ASSET_LOG(LogSkeletalMesh, Display, this, TEXT("Fix some section data of this asset for lod %d. Verify all sections of this mesh are ok and save the asset to fix this issue."), LodIndex);
	}
}

bool USkeletalMesh::IsAsyncTaskComplete() const
{
	return AsyncTask == nullptr || AsyncTask->IsWorkDone();
}


void USkeletalMesh::PostLoadVerifyAndFixBadTangent()
{
	IMeshUtilities& MeshUtilities = FModuleManager::Get().LoadModuleChecked<IMeshUtilities>("MeshUtilities");
	bool bFoundBadTangents = false;
	for (int32 LodIndex = 0; LodIndex < GetLODNum(); LodIndex++)
	{
		if (HasMeshDescription(LodIndex))
		{
			//No need to verify skeletalmesh that have valid imported data, the tangents will always exist in this case
			continue;
		}
		const FSkeletalMeshLODInfo* LODInfoPtr = GetLODInfo(LodIndex);
		if (!LODInfoPtr || LODInfoPtr->bHasBeenSimplified)
		{
			//No need to validate simplified LOD
			continue;
		}

		auto ComputeTriangleTangent = [&MeshUtilities](const FSoftSkinVertex& VertexA, const FSoftSkinVertex& VertexB, const FSoftSkinVertex& VertexC, TArray<FVector3f>& OutTangents)
		{
			MeshUtilities.CalculateTriangleTangent(VertexA, VertexB, VertexC, OutTangents, FLT_MIN);
		};

		FSkeletalMeshLODModel& ThisLODModel = GetImportedModel()->LODModels[LodIndex];
		const int32 SectionNum = ThisLODModel.Sections.Num();
		TArray<FSoftSkinVertex> Vertices;
		TMap<int32, TArray<FVector3f>> TriangleTangents;

		for (int32 SectionIndex = 0; SectionIndex < SectionNum; ++SectionIndex)
		{
			FSkelMeshSection& Section = ThisLODModel.Sections[SectionIndex];
			const int32 NumVertices = Section.GetNumVertices();
			const int32 SectionBaseIndex = Section.BaseIndex;
			const int32 SectionNumTriangles = Section.NumTriangles;
			TArray<uint32>& IndexBuffer = ThisLODModel.IndexBuffer;
			//We inspect triangle per section so we need to reset the array when we start a new section.
			TriangleTangents.Empty(SectionNumTriangles);
			for (int32 FaceIndex = 0; FaceIndex < SectionNumTriangles; ++FaceIndex)
			{
				int32 BaseFaceIndexBufferIndex = SectionBaseIndex + (FaceIndex * 3);
				if (!ensure(IndexBuffer.IsValidIndex(BaseFaceIndexBufferIndex)) || !ensure(IndexBuffer.IsValidIndex(BaseFaceIndexBufferIndex + 2)))
				{
					break;
				}
				for (int32 Corner = 0; Corner < 3; ++Corner)
				{
					const int32 CornerIndexBufferIndex = BaseFaceIndexBufferIndex + Corner;
					ensure(IndexBuffer.IsValidIndex(CornerIndexBufferIndex));
					int32 VertexIndex = IndexBuffer[CornerIndexBufferIndex] - Section.BaseVertexIndex;
					ensure(Section.SoftVertices.IsValidIndex(VertexIndex));
					FSoftSkinVertex& SoftSkinVertex = Section.SoftVertices[VertexIndex];

					bool bNeedToOrthonormalize = false;

					//Make sure we have normalized tangents
					auto NormalizedTangent = [&bNeedToOrthonormalize, &bFoundBadTangents](FVector3f& Tangent)
					{
						if (Tangent.ContainsNaN() || Tangent.SizeSquared() < UE_THRESH_VECTOR_NORMALIZED)
						{
							//This is a degenerated tangent, we will set it to zero. It will be fix by the
							//FixTangent lambda function.
							Tangent = FVector3f::ZeroVector;
							//If we can fix this tangents, we have to orthonormalize the result
							bNeedToOrthonormalize = true;
							bFoundBadTangents = true;
							return false;
						}
						else if (!Tangent.IsNormalized())
						{
							//This is not consider has a bad normal since the tangent vector is not near zero.
							//We are just making sure the tangent is normalize.
							Tangent.Normalize();
						}
						return true;
					};

					/** Call this lambda only if you need to fix the tangent */
					auto FixTangent = [&IndexBuffer, &Section, &TriangleTangents, &ComputeTriangleTangent, &BaseFaceIndexBufferIndex](FVector3f& TangentA, const FVector3f& TangentB, const FVector3f& TangentC, const int32 Offset)
					{
						//If the two other axis are valid, fix the tangent with a cross product and normalize the answer.
						if (TangentB.IsNormalized() && TangentC.IsNormalized())
						{
							TangentA = FVector3f::CrossProduct(TangentB, TangentC);
							TangentA.Normalize();
							return true;
						}

						//We do not have any valid data to help us for fixing this normal so apply the triangle normals, this will create a faceted mesh but this is better then a black not shade mesh.
						TArray<FVector3f>& Tangents = TriangleTangents.FindOrAdd(BaseFaceIndexBufferIndex);
						if (Tangents.Num() == 0)
						{
							const int32 VertexIndex0 = IndexBuffer[BaseFaceIndexBufferIndex] - Section.BaseVertexIndex;
							const int32 VertexIndex1 = IndexBuffer[BaseFaceIndexBufferIndex + 1] - Section.BaseVertexIndex;
							const int32 VertexIndex2 = IndexBuffer[BaseFaceIndexBufferIndex + 2] - Section.BaseVertexIndex;
							if (!ensure(
								Section.SoftVertices.IsValidIndex(VertexIndex0) &&
								Section.SoftVertices.IsValidIndex(VertexIndex1) &&
								Section.SoftVertices.IsValidIndex(VertexIndex2) ) )
							{
								//We found bad vertex indices, we cannot compute this face tangents.
								return false;
							}
							ComputeTriangleTangent(Section.SoftVertices[VertexIndex0], Section.SoftVertices[VertexIndex1], Section.SoftVertices[VertexIndex2], Tangents);
							const FVector3f Axis[3] = { {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f} };
							if (!ensure(Tangents.Num() == 3))
							{
								Tangents.Empty(3);
								Tangents.AddZeroed(3);
							}
							for (int32 TangentIndex = 0; TangentIndex < Tangents.Num(); ++TangentIndex)
							{
								if (Tangents[TangentIndex].IsNearlyZero())
								{
									Tangents[TangentIndex] = Axis[TangentIndex];
								}
							}
							if (!ensure(Tangents.Num() == 3))
							{
								//We are not able to compute the triangle tangent, this is probably a degenerated triangle
								Tangents.Empty(3);

								Tangents.Add(Axis[0]);
								Tangents.Add(Axis[1]);
								Tangents.Add(Axis[2]);
							}
						}
						//Use the offset to know which tangent type we are setting (0: Tangent X, 1: bi-normal Y, 2: Normal Z)
						TangentA = Tangents[(Offset) % 3];
						return TangentA.IsNormalized();
					};

					//The SoftSkinVertex TangentZ is a FVector4 so we must use a temporary FVector to be able to pass reference
					FVector3f TangentZ = SoftSkinVertex.TangentZ;
					//Make sure the tangent space is normalize before fixing bad tangent, because we want to do a cross product
					//of 2 valid axis if possible. If not possible we will use the triangle normal which give a faceted triangle.
					bool ValidTangentX = NormalizedTangent(SoftSkinVertex.TangentX);
					bool ValidTangentY = NormalizedTangent(SoftSkinVertex.TangentY);
					bool ValidTangentZ = NormalizedTangent(TangentZ);

					if (!ValidTangentX)
					{
						ValidTangentX = FixTangent(SoftSkinVertex.TangentX, SoftSkinVertex.TangentY, TangentZ, 0);
					}
					if (!ValidTangentY)
					{
						ValidTangentY = FixTangent(SoftSkinVertex.TangentY, TangentZ, SoftSkinVertex.TangentX, 1);
					}
					if (!ValidTangentZ)
					{
						ValidTangentZ = FixTangent(TangentZ, SoftSkinVertex.TangentX, SoftSkinVertex.TangentY, 2);
					}

					//Make sure the result tangent space is orthonormal, only if we succeed to fix all tangents
					if (bNeedToOrthonormalize && ValidTangentX && ValidTangentY && ValidTangentZ)
					{
						FVector3f::CreateOrthonormalBasis(
							SoftSkinVertex.TangentX,
							SoftSkinVertex.TangentY,
							TangentZ
						);
					}
					SoftSkinVertex.TangentZ = TangentZ;
				}
			}
		}
	}
	if (bFoundBadTangents)
	{
		//Notify the user that we have to fix the normals on this model.
		UE_ASSET_LOG(LogSkeletalMesh, Display, this, TEXT("Find and fix some bad tangent! please re-import this skeletal mesh asset to fix the issue. The shading of the skeletal mesh will be bad and faceted."));
	}
}

void USkeletalMesh::PostLoadRecoverConvertLODModelsToMeshDescription()
{
	// Make sure we have enough space in the FMeshDescription storage for all the recovered LOD models.
	if (GetNumSourceModels() < GetImportedModel()->LODModels.Num())
	{
		SetNumSourceModels(GetImportedModel()->LODModels.Num());
	}

	// If we didn't get any meshes from the bulk data, then try to recover them from the LODModel listings.
	for (int32 LODIndex = 0; LODIndex < GetImportedModel()->LODModels.Num(); ++LODIndex)
	{
		if (HasMeshDescription(LODIndex))
		{
			continue;
		}

		// If the mesh was not pulled out of the reduction data, we need to reset the LOD settings
		// so that the mesh doesn't get reduced again if it gets regenerated.
		FSkeletalMeshLODInfo* MeshLODInfo = GetLODInfo(LODIndex); 
		const bool bReductionActive = IsReductionActive(LODIndex);
		const bool bInlineReduction = (MeshLODInfo->ReductionSettings.BaseLOD == LODIndex);
		if (!bReductionActive || bInlineReduction)
		{
			const FSkeletalMeshLODModel& LODModel = GetImportedModel()->LODModels[LODIndex];
			FMeshDescription MeshDescription;
			LODModel.GetMeshDescription(this, LODIndex, MeshDescription);
			CreateMeshDescription(LODIndex, MoveTemp(MeshDescription));
			CommitMeshDescription(LODIndex);
			
			// Ensure normals aren't automatically computed when we rebuild.
			FSkeletalMeshBuildSettings& BuildSettings = MeshLODInfo->BuildSettings;
			BuildSettings.bRecomputeNormals = false;

			// Reset the reduction settings so that we don't re-reduce the mesh and possibly lose morph targets
			// in the process.
			FSkeletalMeshOptimizationSettings& ReductionSettings = MeshLODInfo->ReductionSettings;
		
			//Remove the reduction settings
			ReductionSettings.NumOfTrianglesPercentage = 1.0f;
			ReductionSettings.NumOfVertPercentage = 1.0f;
			ReductionSettings.MaxNumOfTrianglesPercentage = MAX_uint32;
			ReductionSettings.MaxNumOfVertsPercentage = MAX_uint32;
			ReductionSettings.TerminationCriterion = SMTC_NumOfTriangles;
			MeshLODInfo->bHasBeenSimplified = false;
		}
		else if (MeshLODInfo->LODMaterialMap.IsEmpty())
		{
			// Generated LODs (not inline) do not need imported data. We do need a material map though,
			// because in many cases the map was not created when a section material got overridden, so reconstruct one if it isn't available.
			const FSkeletalMeshLODModel& BaseLODModel = GetImportedModel()->LODModels[MeshLODInfo->ReductionSettings.BaseLOD];
			const FSkeletalMeshLODModel& LODModel = GetImportedModel()->LODModels[LODIndex];
			TArray<int32> MaterialMap;
			MaterialMap.Init(INDEX_NONE, LODModel.Sections.Num());

			if (BaseLODModel.Sections.Num() == LODModel.Sections.Num())
			{
				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
				{
					const int32 MaterialIndex = LODModel.Sections[SectionIndex].MaterialIndex;
					if (BaseLODModel.Sections[SectionIndex].MaterialIndex != MaterialIndex)
					{
						MaterialMap[SectionIndex] = MaterialIndex;
					}
				}
			}
			else
			{
				for (int32 SectionIndex = 0; SectionIndex < LODModel.Sections.Num(); SectionIndex++)
				{
					const int32 BaseSectionIndex = LODModel.Sections[SectionIndex].OriginalDataSectionIndex;
					
					if (BaseLODModel.Sections.IsValidIndex(BaseSectionIndex))
					{
						const int32 MaterialIndex = LODModel.Sections[SectionIndex].MaterialIndex;

						if (BaseLODModel.Sections[BaseSectionIndex].MaterialIndex != MaterialIndex)
						{
							MaterialMap[SectionIndex] = MaterialIndex;
						}
					}
				}
			}
			if (Algo::AnyOf(MaterialMap, [](int32 Item) { return Item != INDEX_NONE; }))
			{
				MeshLODInfo->LODMaterialMap = MoveTemp(MaterialMap);
			}
		}
	}
}


#endif // WITH_EDITOR

bool USkeletalMesh::IsPostLoadThreadSafe() const
{
	return false;	// PostLoad is not thread safe
}

bool USkeletalMesh::HasHalfEdgeBuffer(int32 LODIndex) const
{
	const FSkeletalMeshLODInfo* Info = GetLODInfo(LODIndex);
	if (!Info)
	{
		return false;
	}
	
	if (!Info->bAllowMeshDeformer)
	{
		return false;
	}
	
	if (Info->bBuildHalfEdgeBuffers)
	{
		return true;
	}
	
	if (SkeletalMeshHalfEdgeBufferAccessor::IsHalfEdgeRequired(GetDefaultMeshDeformer()))
	{
		return true;
	}

	if (TargetMeshDeformers)
	{
		for (const TSoftObjectPtr<UMeshDeformer>& MeshDeformer : TargetMeshDeformers->GetMeshDeformers())
		{
			if (SkeletalMeshHalfEdgeBufferAccessor::IsHalfEdgeRequired(MeshDeformer))
			{
				return true;
			}
		}	
	}
	
	return false;
}

void USkeletalMesh::BeginPostLoadInternal(FSkinnedAssetPostLoadContext& Context)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::BeginPostLoadInternal);

	// Lock all properties that should not be modified/accessed during async post-load
	AcquireAsyncProperty();

	// This scope allows us to use any locked properties without causing stalls
	FSkinnedAssetAsyncBuildScope AsyncBuildScope(this);

	// Make sure the cloth assets have finished loading
	// TODO: Remove all UObject PostLoad dependencies.
	//       Even with these ConditionalPostLoad calls, the UObject PostLoads' order of execution cannot be guaranted.
	//       E.g. in some instance it has been found that the SkeletalMesh EndLoad can trigger a ConditionalPostLoad
	//       on the cloth assets even before reaching this point.
	//       In these occurences, the cloth asset's RF_NeedsPostLoad flag is already cleared despite its PostLoad still
	//       being un-executed, making the following block code ineffective.
	for (UClothingAssetBase* MeshClothingAsset : GetMeshClothingAssets())
	{
		MeshClothingAsset->ConditionalPostLoad();
	}

	if (!GetOutermost()->bIsCookedForEditor)
	{
		// If LODInfo is missing - create array of correct size.
		while (GetLODNum() < GetImportedModel()->LODModels.Num())
		{
			FSkeletalMeshLODInfo NewLODInfo;
			NewLODInfo.LODHysteresis = 0.02f;
			AddLODInfo(NewLODInfo);
		}
			
		const int32 TotalLODNum = GetLODNum();
		for (int32 LodIndex = 0; LodIndex < TotalLODNum; LodIndex++)
		{
			FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(LodIndex);

			if (ThisLODInfo.ReductionSettings.BonesToRemove_DEPRECATED.Num() > 0)
			{
				for (auto& BoneToRemove : ThisLODInfo.ReductionSettings.BonesToRemove_DEPRECATED)
				{
					AddBoneToReductionSetting(LodIndex, BoneToRemove.BoneName);
				}

				// since in previous system, we always removed from previous LOD, I'm adding this 
				// here for previous LODs
				for (int32 CurLodIndx = LodIndex + 1; CurLodIndx < TotalLODNum; ++CurLodIndx)
				{
					AddBoneToReductionSetting(CurLodIndx, ThisLODInfo.RemovedBones_DEPRECATED);
				}

				// we don't apply this change here, but this will be applied when you re-gen simplygon
				ThisLODInfo.ReductionSettings.BonesToRemove_DEPRECATED.Empty();
			}

			if (ThisLODInfo.ReductionSettings.BakePose_DEPRECATED != nullptr)
			{
				ThisLODInfo.BakePose = ThisLODInfo.ReductionSettings.BakePose_DEPRECATED;
				ThisLODInfo.ReductionSettings.BakePose_DEPRECATED = nullptr;
			}
		}

		// load LODinfo if using shared asset, it can override existing bone remove settings
		if (GetLODSettings() != nullptr)
		{
			//before we copy
			if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::AddBakePoseOverrideForSkeletalMeshReductionSetting)
			{
				// if LODsetting doesn't have BakePose, but this does, we'll have to copy that to BakePoseOverride
				const int32 NumSettings = FMath::Min(GetLODSettings()->GetNumberOfSettings(), GetLODNum());
				for (int32 Index = 0; Index < NumSettings; ++Index)
				{
					const FSkeletalMeshLODGroupSettings& GroupSetting = GetLODSettings()->GetSettingsForLODLevel(Index);
					// if lod setting doesn't have bake pose, but this lod does, that means this bakepose has to move to BakePoseOverride
					// since we want to match what GroupSetting has
					FSkeletalMeshLODInfo& ThisLODInfo = *GetLODInfo(Index);
					if (GroupSetting.BakePose == nullptr && ThisLODInfo.BakePose)
					{
						// in this case,
						ThisLODInfo.BakePoseOverride = ThisLODInfo.BakePose;
						ThisLODInfo.BakePose = nullptr;
					}
				}
			}
			GetLODSettings()->SetLODSettingsToMesh(this);
		}

		if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::MeshDescriptionForSkeletalMesh)
		{
			// Ensure we have source model storage that matches the number of LODs defined on this mesh.
			SetNumSourceModels(GetLODNum());
		}
		

		if (GetLinkerUEVersion() < VER_UE4_SORT_ACTIVE_BONE_INDICES)
		{
			for (FSkeletalMeshLODModel& LODModel: GetImportedModel()->LODModels)
			{
				LODModel.ActiveBoneIndices.Sort();
			}
		}

		// make sure older versions contain active bone indices with parents present
		// even if they're not skinned, missing matrix calculation will mess up skinned children
		if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::EnsureActiveBoneIndicesToContainParents)
		{
			for (FSkeletalMeshLODModel& LODModel: GetImportedModel()->LODModels)
			{
				GetRefSkeleton().EnsureParentsExistAndSort(LODModel.ActiveBoneIndices);
			}
		}

		if (GetLinkerCustomVersion(FEditorObjectVersion::GUID) < FEditorObjectVersion::SkeletalMeshMoveEditorSourceDataToPrivateAsset)
		{
			for (int32 LODIndex = 0; LODIndex < GetImportedModel()->LODModels.Num(); ++LODIndex)
			{
				FSkeletalMeshLODModel& ThisLODModel = GetImportedModel()->LODModels[LODIndex];
				
				//We can have partial data if the asset was save after the split workflow implementation
				//Use the deprecated member to retrieve this data
				if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::NewSkeletalMeshImporterWorkflow)
				{
					//Get the deprecated data 
					FRawSkeletalMeshBulkData* RawSkeletalMeshBulkData_DEPRECATED;
					{
						PRAGMA_DISABLE_DEPRECATION_WARNINGS
						RawSkeletalMeshBulkData_DEPRECATED = &ThisLODModel.GetRawSkeletalMeshBulkData_DEPRECATED();
						PRAGMA_ENABLE_DEPRECATION_WARNINGS
					}
					check(RawSkeletalMeshBulkData_DEPRECATED);
					if (!RawSkeletalMeshBulkData_DEPRECATED->IsEmpty())
					{
						FSkeletalMeshImportData SkeletalMeshImportData;
						RawSkeletalMeshBulkData_DEPRECATED->LoadRawMesh(SkeletalMeshImportData);

						// Some older versions of the bulk data did not store the morph targets or alternate skin profiles, but they're 
						// available on the skeletal mesh's LOD model. Try to back-fill from the LOD model.
						if (SkeletalMeshImportData.MorphTargets.IsEmpty() && !GetMorphTargets().IsEmpty())
						{
							for (UMorphTarget* MorphTarget: GetMorphTargets())
							{
								if (!MorphTarget->HasDataForLOD(LODIndex))
								{
									continue;
								}

								const FMorphTargetLODModel& MorphTargetModel = MorphTarget->GetMorphLODModels()[LODIndex];

								// Confusingly, morph targets need FSkeletalMeshLODModel::RawPointIndices2 to map back to the import model, whereas
								// skin weight profiles need MeshToImportVertexMap.
								SkeletalMeshImportData.AddMorphTarget(MorphTarget->GetFName(), MorphTargetModel, ThisLODModel.GetRawPointIndices());
							}
						}

						if (SkeletalMeshImportData.AlternateInfluences.IsEmpty() && !GetSkinWeightProfiles().IsEmpty() && !ThisLODModel.SkinWeightProfiles.IsEmpty())
						{
							for (const TPair<FName, FImportedSkinWeightProfileData>& SkinWeightProfileInfo: ThisLODModel.SkinWeightProfiles)
							{
								SkeletalMeshImportData.AddSkinWeightProfile(SkinWeightProfileInfo.Key, SkinWeightProfileInfo.Value,
									ThisLODModel.MeshToImportVertexMap, ThisLODModel.ActiveBoneIndices);
							}
						}

						FMeshDescription MeshDescription;
						if (SkeletalMeshImportData.GetMeshDescription(this, &GetLODInfo(LODIndex)->BuildSettings, MeshDescription))
						{
							CreateMeshDescription(LODIndex, MoveTemp(MeshDescription));
							CommitMeshDescription(LODIndex);
						}
					}
					//Empty the DEPRECATED member
					FSkeletalMeshImportData EmptyMeshData;
					RawSkeletalMeshBulkData_DEPRECATED->SaveRawMesh(EmptyMeshData);
					RawSkeletalMeshBulkData_DEPRECATED->EmptyBulkData();
				}
			}
		}

		if (GetLinkerCustomVersion(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::MeshDescriptionForSkeletalMesh)
		{
			// Transfer all bulk data from the deprecated private bulk storage objects to the new source model structure.
			// When the user asks for mesh description from the bulk data, the raw mesh bulk data will be unpacked, converted and
			// discarded.
			PRAGMA_DISABLE_DEPRECATION_WARNINGS
			USkeletalMeshEditorData* ImportData = MeshEditorDataObject_DEPRECATED;
			PRAGMA_ENABLE_DEPRECATION_WARNINGS

			if (ImportData)
			{
				for (int32 LODIndex = 0, LODNum = GetLODNum(); LODIndex < LODNum; LODIndex++)
				{
					if (LODIndex < ImportData->RawSkeletalMeshBulkDatas.Num() &&
						!ImportData->RawSkeletalMeshBulkDatas[LODIndex]->IsEmpty() &&
						ImportData->RawSkeletalMeshBulkDatas[LODIndex]->IsBuildDataAvailable())
					{
						FSkeletalMeshSourceModel& SourceModel = GetSourceModel(LODIndex);
						SourceModel.RawMeshBulkData = ImportData->RawSkeletalMeshBulkDatas[LODIndex];
						// When we do on-demand conversion of the raw mesh bulk data, we need to know which
						// LOD we came from so that we can grab the appropriate reconstruction data from the 
						// correct FSkeletalMeshModel::LODModels variant.
						SourceModel.RawMeshBulkDataLODIndex = LODIndex; 
					}
				}
			}
		}
		
		if (GetLinkerCustomVersion(FEditorObjectVersion::GUID) < FEditorObjectVersion::SkeletalMeshBuildRefactor)
		{
			CreateUserSectionsDataForLegacyAssets();
		}

		ValidateAllLodMaterialIndexes();
	}

#endif // #if WITH_EDITOR
}

#if WITH_EDITORONLY_DATA
void USkeletalMesh::DeclareConstructClasses(TArray<FTopLevelAssetPath>& OutConstructClasses, const UClass* SpecificSubclass)
{
	Super::DeclareConstructClasses(OutConstructClasses, SpecificSubclass);

	OutConstructClasses.Add(FTopLevelAssetPath(USkeletalMeshEditorData::StaticClass()));
}
#endif

void USkeletalMesh::PostLoad()
{
	LLM_SCOPE(ELLMTag::SkeletalMesh);
	Super::PostLoad();

}

void USkeletalMesh::ExecutePostLoadInternal(FSkinnedAssetPostLoadContext& Context)
{
#if WITH_EDITOR
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::ExecutePostLoadInternal);

	// This scope allows us to use any locked properties without causing stalls
	FSkinnedAssetAsyncBuildScope AsyncBuildScope(this);

	if (!GetOutermost()->bIsCookedForEditor)
	{
		RemoveLegacyClothingSections();

		UpdateGenerateUpToData();

		PostLoadValidateUserSectionData();

		PostLoadVerifyAndFixBadTangent();

		// Fixup missing material slot names and import slot names, so that mesh editing 
		// preserves material assignments.
		IMeshUtilities* MeshUtilities = FModuleManager::Get().LoadModulePtr<IMeshUtilities>("MeshUtilities");
		if (MeshUtilities)
		{
			MeshUtilities->FixupMaterialSlotNames(this);
		}

		PostLoadRecoverConvertLODModelsToMeshDescription();		

		if (GetResourceForRendering() == nullptr)
		{
			CacheDerivedData(&Context);
			Context.bHasCachedDerivedData = true;
		}
	}
	else
	{
		// Ensure we have source model storage that matches the number of LODs defined on this mesh since we're not doing any translation from
		// old models. 
		SetNumSourceModels(GetLODNum());
	}

	// check the MinLOD values are all within range
	FPerQualityLevelInt QualityLocalMinLOD;
	FPerPlatformInt LocalMinLOD;
	int32 MinAvailableLOD = INDEX_NONE;
	TArray<TPair<int32, FName>> InvalidMinLODs;
	CheckForValidMinLODs(QualityLocalMinLOD, LocalMinLOD, MinAvailableLOD, InvalidMinLODs);
	if (InvalidMinLODs.Num())
	{
		if (IsMinLodQualityLevelEnable())
		{
			SetQualityLevelMinLod(QualityLocalMinLOD);
		}
		else
		{
			SetMinLod(LocalMinLOD);
		}

		TArray<FText> MinLODErrors;
		for (const TPair<int32, FName>& InvalidMinLOD : InvalidMinLODs)
		{
			const int32 LODIdx = InvalidMinLOD.Key;
			const FName OverrideName = InvalidMinLOD.Value;

			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("MinLOD"), FText::AsNumber(LODIdx));
			Arguments.Add(TEXT("MinAvailLOD"), FText::AsNumber(MinAvailableLOD));
			Arguments.Add(TEXT("OverrideName"), FText::FromName(OverrideName));
			if (OverrideName.IsNone())
			{
				MinLODErrors.Add(FText::Format(LOCTEXT("LoadError_BadMinLOD_Fixed", "Min LOD value of {MinLOD} is out of range 0..{MinAvailLOD} and has been adjusted to {MinAvailLOD}. Please verify and resave the asset."), Arguments));
			}
			else
			{
				MinLODErrors.Add(FText::Format(LOCTEXT("LoadError_BadMinLODWithOverride_Fixed", "Min LOD override of {MinLOD} for {OverrideName} is out of range 0..{MinAvailLOD} and has been adjusted to {MinAvailLOD}. Please verify and resave the asset."), Arguments));
			}
		}

		if (IsRunningCommandlet())
		{
			for (const FText& MinLODError : MinLODErrors)
			{
				UE_ASSET_LOG(LogSkeletalMesh, Warning, this, TEXT("%s"), *MinLODError.ToString());
			}
		}
		else
		{
			TSharedRef<FUObjectToken> TokenRef = FUObjectToken::Create(this);
			Async(EAsyncExecution::TaskGraphMainThread,
				// No choice to MoveTemp here, the SharedRef is not thread safe so it cannot
				// be copied to another thread, only moved.
				[Token = MoveTemp(TokenRef), MinAvailableLOD, MinLODErrors]()
				{
					for (const FText& MinLODError : MinLODErrors)
					{
						FMessageLog("LoadErrors").Warning()
							->AddToken(Token)
							->AddToken(FTextToken::Create(MinLODError));
					}

					FMessageLog("LoadErrors").Open();
				}
			);
		}
	}


#endif // WITH_EDITOR
}

void USkeletalMesh::FinishPostLoadInternal(FSkinnedAssetPostLoadContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::FinishPostLoadInternal);
	
#if WITH_EDITOR

	// This scope allows us to use any locked properties without causing stalls
	FSkinnedAssetAsyncBuildScope AsyncBuildScope(this);

	//Make sure unused cloth are unbind
	if (GetMeshClothingAssets().Num() > 0)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UnbindUnusedCloths);

		TArray<UClothingAssetBase*> InUsedClothingAssets;
		GetClothingAssetsInUse(InUsedClothingAssets);
		//Look if we have some cloth binding to unbind
		for (UClothingAssetBase* MeshClothingAsset : GetMeshClothingAssets())
		{
			if (MeshClothingAsset == nullptr)
			{
				continue;
			}
			bool bFound = false;
			for (UClothingAssetBase* UsedMeshClothingAsset : InUsedClothingAssets)
			{
				if (UsedMeshClothingAsset->GetAssetGuid() == MeshClothingAsset->GetAssetGuid())
				{
					bFound = true;
					break;
				}
			}
			if (!bFound)
			{
				//No post edit change and no reregister, we just prevent the inner scope to call postedit change and reregister
				FScopedSkeletalMeshPostEditChange ScopedPostEditChange(this, false, false);
				//Make sure the asset is unbind, some old code path was allowing to have bind cloth asset not present in the imported model.
				//The old inline reduction code was not rebinding the cloth asset nor unbind it.
				MeshClothingAsset->UnbindFromSkeletalMesh(this);
			}
		}
	}
#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA
	if (GetLinkerCustomVersion(FRenderingObjectVersion::GUID) < FRenderingObjectVersion::FixedMeshUVDensity)
	{
		UpdateUVChannelData(true);
	}
#endif // WITH_EDITOR

#if WITH_EDITOR
	ApplyFinishBuildInternalData(&Context);
#endif
	// should do this before InitResources.
	InitMorphTargets();

	// initialize rendering resources
	if (FApp::CanEverRender())
	{
		InitResources();
	}
	else
	{
		// Update any missing data when cooking.
		UpdateUVChannelData(false);
	}

	CalculateInvRefMatrices();

	// Bounds have been loaded - apply extensions.
	CalculateExtendedBounds();

#if WITH_EDITORONLY_DATA
	if (GetRequiresLODScreenSizeConversion() || GetRequiresLODHysteresisConversion())
	{
		// Convert screen area to screen size
		ConvertLegacyLODScreenSize();
	}

	// If inverse masses have never been cached, invalidate data so it will be recalculated
	if (GetLinkerCustomVersion(FSkeletalMeshCustomVersion::GUID) < FSkeletalMeshCustomVersion::CachedClothInverseMasses)
	{
		for (UClothingAssetBase* ClothingAsset : GetMeshClothingAssets())
		{
			if (ClothingAsset) 
			{
				ClothingAsset->InvalidateAllCachedData();
			}
		}
	}
#endif

	SetHasActiveClothingAssets(ComputeActiveClothingAssets());

#if WITH_EDITOR
	if (GetLinkerCustomVersion(FNiagaraObjectVersion::GUID) < FNiagaraObjectVersion::SkeletalMeshVertexSampling)
	{
		GetSamplingInfoInternal().BuildRegions(this);
		GetSamplingInfoInternal().BuildWholeMesh(this);
	}
#endif

#if !WITH_EDITOR
	RebuildSocketMap();
#endif // !WITH_EDITOR

#if WITH_EDITORONLY_DATA
	FPerPlatformInt PerPlatformData = GetMinLod();
	FPerQualityLevelInt PerQualityLevelData = GetQualityLevelMinLod();

	// Convert PerPlatForm data to PerQuality if perQuality data have not been serialized.
	// Also test default value, since PerPLatformData can have Default !=0 and no PerPlaform data overrides.
	bool bConvertMinLODData = (PerQualityLevelData.PerQuality.Num() == 0 && PerQualityLevelData.Default == 0) && (PerPlatformData.PerPlatform.Num() != 0 || PerPlatformData.Default != 0);

	if (GEngine && GEngine->UseSkeletalMeshMinLODPerQualityLevels && bConvertMinLODData)
	{
		// get the platform groups
		const TArray<FName>& PlatformGroupNameArray = PlatformInfo::GetAllPlatformGroupNames();

		// Make sure all platforms and groups are known before updating any of them. Missing platforms would not properly be converted to PerQuality if some of them were known and others were not.
		bool bAllPlatformsKnown = true;
		for (const TPair<FName, int32>& Pair : PerPlatformData.PerPlatform)
		{
			const bool bIsPlatformGroup = PlatformGroupNameArray.Contains(Pair.Key);
			const bool bIsKnownPlatform = (FDataDrivenPlatformInfoRegistry::GetPlatformInfo(Pair.Key).IniPlatformName.IsNone() == false);
			if (!bIsPlatformGroup && !bIsKnownPlatform)
			{
				bAllPlatformsKnown = false;
				break;
			}
		}

		if (bAllPlatformsKnown)
		{
			//assign the default value
			PerQualityLevelData.Default = PerPlatformData.Default;

			// iterate over all platform and platform group entry: ex: XBOXONE = 2, CONSOLE=1, MOBILE = 3
			if (PerQualityLevelData.PerQuality.Num() == 0) //-V547
			{
				TMap<FName, int32> SortedPerPlatforms = PerPlatformData.PerPlatform;
				SortedPerPlatforms.KeySort([&](const FName& A, const FName& B) { return (PlatformGroupNameArray.Contains(A) > PlatformGroupNameArray.Contains(B)); });

				for (const TPair<FName, int32>& Pair : SortedPerPlatforms)
				{
					FSupportedQualityLevelArray QualityLevels;
					FString PlatformEntry = Pair.Key.ToString();

					QualityLevels = QualityLevelProperty::PerPlatformOverrideMapping(PlatformEntry, this);

					// we now have a range of quality levels supported on that platform or from that group
					// note: 
					// -platform group overrides will be applied first
					// -platform override sharing the same quality level will take the smallest MinLOD value between them
					// -ex: if XboxOne and PS4 maps to high and XboxOne MinLOD = 2 and PS4 MINLOD = 1, MINLOD 1 will be selected
					for (int32& QLKey : QualityLevels)
					{
						int32* Value = PerQualityLevelData.PerQuality.Find(QLKey);
						if (Value != nullptr)
						{
							*Value = FMath::Min(Pair.Value, *Value);
						}
						else
						{
							PerQualityLevelData.PerQuality.Add(QLKey, Pair.Value);
						}
					}
				}
			}
			SetQualityLevelMinLod(PerQualityLevelData);
		}
	}
#endif

	ReleaseAsyncProperty();
#if WITH_EDITOR
	if (Context.bHasCachedDerivedData)
	{
		//We must call PostMeshCached after:
		// - The async properties are release
		// - The init resource is done
		PostMeshCached.Broadcast(this);
	}
#endif //WITH_EDITOR
}

#if WITH_EDITORONLY_DATA

void USkeletalMesh::RebuildRefSkeletonNameToIndexMap()
{
	TArray<FBoneIndexType> DuplicateBones;
	// Make sure we have no duplicate bones. Some content got corrupted somehow. :(
	GetRefSkeleton().RemoveDuplicateBones(this, DuplicateBones);

	// If we have removed any duplicate bones, we need to fix up any broken LODs as well.
	// Duplicate bones are given from the highest index to lowest.
	// so it's safe to decrease indices for children, we're not going to lose the index of the remaining duplicate bones.
	for (int32 Index = 0; Index < DuplicateBones.Num(); Index++)
	{
		const FBoneIndexType& DuplicateBoneIndex = DuplicateBones[Index];
		for (FSkeletalMeshLODModel& LODModel: GetImportedModel()->LODModels)
		{
			int32 FoundIndex;
			if (LODModel.RequiredBones.Find(DuplicateBoneIndex, FoundIndex))
			{
				LODModel.RequiredBones.RemoveAt(FoundIndex, 1);
				// we need to shift indices of the remaining bones.
				for (int32 BoneIndex = FoundIndex; BoneIndex < LODModel.RequiredBones.Num(); BoneIndex++)
				{
					LODModel.RequiredBones[BoneIndex] = LODModel.RequiredBones[BoneIndex] - 1;
				}
			}
			
			if (LODModel.ActiveBoneIndices.Find(DuplicateBoneIndex, FoundIndex))
			{
				LODModel.ActiveBoneIndices.RemoveAt(FoundIndex, 1);
				// we need to shift indices of the remaining bones.
				for (int32 BoneIndex = FoundIndex; BoneIndex < LODModel.ActiveBoneIndices.Num(); BoneIndex++)
				{
					LODModel.ActiveBoneIndices[BoneIndex] = LODModel.ActiveBoneIndices[BoneIndex] - 1;
				}
			}
		}
	}

	// Rebuild name table.
	GetRefSkeleton().RebuildNameToIndexMap();
}

#endif


void USkeletalMesh::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::GetAssetRegistryTags(OutTags);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void USkeletalMesh::GetAssetRegistryTags(FAssetRegistryTagsContext Context) const
{
#if WITH_EDITOR
	// Avoid accessing properties being compiled, this function will get called again after compilation is finished.
	if (IsCompiling())
	{
		IAssetRegistry* AssetRegistry = IAssetRegistry::Get();
		if (AssetRegistry)
		{
			FAssetData AssetData = AssetRegistry->GetAssetByObjectPath(FSoftObjectPath(this), true /* bIncludeOnlyOnDiskAssets */);
			AssetData.EnumerateTags([&Context](const TPair<FName, FAssetTagValueRef>& Pair)
				{
					Context.AddTag(FAssetRegistryTag(Pair.Key, Pair.Value.GetStorageString(), FAssetRegistryTag::TT_Alphabetical));
				});
		}
		return;
	}
#endif

	int32 NumTriangles = 0;
	int32 NumVertices = 0;
	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	if (SkelMeshRenderData && SkelMeshRenderData->LODRenderData.Num() > 0)
	{
		const FSkeletalMeshLODRenderData& LODData = SkelMeshRenderData->LODRenderData[0];
		NumTriangles = LODData.GetTotalFaces();
		NumVertices = LODData.GetNumVertices();
	}

#if WITH_EDITORONLY_DATA
	uint64 PhysicsSize = 0;

	if (UBodySetup* MeshBodySetup = GetBodySetup())
	{
		FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
		MeshBodySetup->GetResourceSizeEx(EstimatedSize);
		PhysicsSize = EstimatedSize.GetTotalMemoryBytes();
	}

	if (UPhysicsAsset* PhysAssetSetup = GetPhysicsAsset())
	{
		FResourceSizeEx EstimatedSize(EResourceSizeMode::EstimatedTotal);
		PhysAssetSetup->GetResourceSizeEx(EstimatedSize);
		PhysicsSize += EstimatedSize.GetTotalMemoryBytes();
	}
	Context.AddTag(FAssetRegistryTag("PhysicsSize", FString::Printf(TEXT("%llu"), PhysicsSize), FAssetRegistryTag::TT_Numerical, FAssetRegistryTag::TD_Memory));
#endif
	
	const int32 NumLODs = GetLODNum();

	int32 NumNaniteTriangles = GetNumNaniteTriangles();
	int32 NumNaniteVertices = GetNumNaniteVertices();

	uint64 EstimatedCompressedSize = 0;
	uint64 EstimatedNaniteCompressedSize = 0;
#if WITH_EDITORONLY_DATA && 0 // TODO: Nanite-Skinning
	if (GetResourceForRendering())
	{
		EstimatedCompressedSize = (int32)GetResourceForRendering()->EstimatedCompressedSize;
		EstimatedNaniteCompressedSize = (int32)GetResourceForRendering()->EstimatedNaniteTotalCompressedSize;
	}
#endif

#if WITH_EDITORONLY_DATA
	Context.AddTag(FAssetRegistryTag("NaniteEnabled", IsNaniteEnabled() ? TEXT("True") : TEXT("False"), FAssetRegistryTag::TT_Alphabetical));
#endif

	Context.AddTag(FAssetRegistryTag("NaniteTriangles", FString::FromInt(NumNaniteTriangles), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("NaniteVertices", FString::FromInt(NumNaniteVertices), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("Vertices", FString::FromInt(NumVertices), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("Triangles", FString::FromInt(NumTriangles), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("LODs", FString::FromInt(NumLODs), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("Bones", FString::FromInt(GetRefSkeleton().GetRawBoneNum()), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("MorphTargets", FString::FromInt(GetMorphTargets().Num()), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("SkinWeightProfiles", FString::FromInt(GetSkinWeightProfiles().Num()), FAssetRegistryTag::TT_Numerical));
	Context.AddTag(FAssetRegistryTag("EstTotalCompressedSize", FString::Printf(TEXT("%llu"), EstimatedCompressedSize), FAssetRegistryTag::TT_Numerical, FAssetRegistryTag::TD_Memory));
	Context.AddTag(FAssetRegistryTag("EstNaniteCompressedSize", FString::Printf(TEXT("%llu"), EstimatedNaniteCompressedSize), FAssetRegistryTag::TT_Numerical, FAssetRegistryTag::TD_Memory));

#if WITH_EDITORONLY_DATA
	if (GetAssetImportData())
	{
		Context.AddTag( FAssetRegistryTag(SourceFileTagName(), GetAssetImportData()->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden) );
#if WITH_EDITOR
		PRAGMA_DISABLE_DEPRECATION_WARNINGS;
		TArray<UObject::FAssetRegistryTag> DeprecatedFunctionTags;
		GetAssetImportData()->AppendAssetRegistryTags(DeprecatedFunctionTags);
		for (UObject::FAssetRegistryTag& Tag : DeprecatedFunctionTags)
		{
			Context.AddTag(MoveTemp(Tag));
		}
		PRAGMA_ENABLE_DEPRECATION_WARNINGS;
		GetAssetImportData()->AppendAssetRegistryTags(Context);
#endif
	}

	FString MaxBoneInfluencesString;
	if (GetImportedModel())
	{
		// Find the LOD with the highest maximum bone influences
		//
		// This will be nullptr if LODModels is empty
		const FSkeletalMeshLODModel* MaxBoneInfluencesLODModel = Algo::MaxElementBy(GetImportedModel()->LODModels, &FSkeletalMeshLODModel::GetMaxBoneInfluences);

		if (MaxBoneInfluencesLODModel)
		{
			// Note that this value is clamped to FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones, so it's affected
			// by project settings such as r.GPUSkin.UnlimitedBoneInfluences.
			MaxBoneInfluencesString = FString::FromInt(MaxBoneInfluencesLODModel->GetMaxBoneInfluences());
		}
	}

	// The tag must be added unconditionally, because some code calls this function on the CDO to find out what
	// tags are available.
	Context.AddTag(FAssetRegistryTag("MaxBoneInfluences", MaxBoneInfluencesString, FAssetRegistryTag::TT_Numerical));

	// Expose morph target names to the asset registry
	{
		TStringBuilder<256> MorphNamesBuilder;
		MorphNamesBuilder.Append(MorphNamesTagDelimiter);

		for(UMorphTarget* MorphTarget : GetMorphTargets())
		{
			if (!MorphTarget)
			{
				continue;
			}
			MorphTarget->GetFName().AppendString(MorphNamesBuilder);
			MorphNamesBuilder.Append(MorphNamesTagDelimiter);
		}

		Context.AddTag(FAssetRegistryTag(MorphNamesTag, MorphNamesBuilder.ToString(), FAssetRegistryTag::TT_Hidden));
	}

	// Expose material scalar params (these can be driven by curves)
	{
		TStringBuilder<256> MaterialParamNamesBuilder;
		MaterialParamNamesBuilder.Append(MaterialParamNamesTagDelimiter);

		for (const FSkeletalMaterial& SkeletalMaterial : GetMaterials())
		{
			UMaterial* Material = (SkeletalMaterial.MaterialInterface != nullptr) ? SkeletalMaterial.MaterialInterface->GetMaterial() : nullptr;
			if (Material)
			{
				TArray<FMaterialParameterInfo> OutParameterInfo;
				TArray<FGuid> OutParameterIds;
				SkeletalMaterial.MaterialInterface->GetAllScalarParameterInfo(OutParameterInfo, OutParameterIds);

				for (const FMaterialParameterInfo& MaterialParameterInfo : OutParameterInfo)
				{
					MaterialParameterInfo.Name.AppendString(MaterialParamNamesBuilder);
					MaterialParamNamesBuilder.Append(MorphNamesTagDelimiter);
				}
			}
		}

		Context.AddTag(FAssetRegistryTag(MaterialParamNamesTag, MaterialParamNamesBuilder.ToString(), FAssetRegistryTag::TT_Hidden));
	}

	// Allow asset user data to output tags
	for(UAssetUserData* AssetUserDataItem : *GetAssetUserDataArray())
	{
		if(AssetUserDataItem)
		{
			AssetUserDataItem->GetAssetRegistryTags(Context);
		}
	}
#endif // WITH_EDITORONLY_DATA
	
	Super::GetAssetRegistryTags(Context);
}

#if WITH_EDITOR
void USkeletalMesh::GetAssetRegistryTagMetadata(TMap<FName, FAssetRegistryTagMetadata>& OutMetadata) const
{
	Super::GetAssetRegistryTagMetadata(OutMetadata);
	OutMetadata.Add("PhysicsAsset", FAssetRegistryTagMetadata().SetImportantValue(TEXT("None")));
}
#endif

void USkeletalMesh::DebugVerifySkeletalMeshLOD()
{
	// if LOD do not have displayfactor set up correctly
	const int32 NumLODs = GetLODNum();
	if (NumLODs > 1)
	{
		for(int32 LODIndex = 1; LODIndex < NumLODs; LODIndex++)
		{
			const float DefaultScreenSize = GetLODInfo(LODIndex)->ScreenSize.Default;
			if (DefaultScreenSize <= 0.1f)
			{
				// too small
				UE_LOG(LogSkeletalMesh, Warning, TEXT("SkelMeshLOD (%s) : ScreenSize for LOD %d may be too small (%0.5f)"), *GetPathName(), LODIndex, DefaultScreenSize);
			}
		}
	}
	else
	{
		// no LODInfo
		UE_LOG(LogSkeletalMesh, Warning, TEXT("SkelMeshLOD (%s) : LOD does not exist"), *GetPathName());
	}
}

void USkeletalMesh::InitMorphTargetsAndRebuildRenderData()
{
#if WITH_EDITOR
	FScopedSkeletalMeshPostEditChange ScopedPostEditChange(this);
#endif
	
	// need to refresh the map
	InitMorphTargets();

	if (IsInGameThread())
	{
		MarkPackageDirty();
		// reset all morphtarget for all components
		for (TObjectIterator<USkeletalMeshComponent> It; It; ++It)
		{
			if (It->GetSkeletalMeshAsset() == this)
			{
				It->RefreshMorphTargets();
			}
		}
	}
}

bool USkeletalMesh::RegisterMorphTarget(UMorphTarget* MorphTarget, bool bInvalidateRenderData)
{
	if ( MorphTarget )
	{
		// if MorphTarget has SkelMesh, make sure you unregister before registering yourself
		if ( MorphTarget->BaseSkelMesh && MorphTarget->BaseSkelMesh!=this )
		{
			MorphTarget->BaseSkelMesh->UnregisterMorphTarget(MorphTarget);
		}

		// if the input morphtarget doesn't have valid data, do not add to the base morphtarget
		ensureMsgf(MorphTarget->HasValidData(), TEXT("RegisterMorphTarget: %s has empty data."), *MorphTarget->GetName());

		MorphTarget->BaseSkelMesh = this;

		bool bRegistered = false;
		const FName MorphTargetName = MorphTarget->GetFName();
		TArray<TObjectPtr<UMorphTarget>>& RegisteredMorphTargets = GetMorphTargets();
		for ( int32 Index = 0; Index < RegisteredMorphTargets.Num(); ++Index )
		{
			if (RegisteredMorphTargets[Index]->GetFName() == MorphTargetName )
			{
				UE_LOG( LogSkeletalMesh, Verbose, TEXT("RegisterMorphTarget: %s already exists, replacing"), *MorphTarget->GetName() );
				RegisteredMorphTargets[Index] = MorphTarget;
				bRegistered = true;
				break;
			}
		}

		if (!bRegistered)
		{
			RegisteredMorphTargets.Add( MorphTarget );
			bRegistered = true;
		}

		if (bRegistered && bInvalidateRenderData)
		{
			InitMorphTargetsAndRebuildRenderData();
		}
		return bRegistered;
	}
	return false;
}


void USkeletalMesh::UnregisterAllMorphTarget()
{
	GetMorphTargets().Empty();
	InitMorphTargetsAndRebuildRenderData();
}

void USkeletalMesh::UnregisterMorphTarget(UMorphTarget* MorphTarget, bool bInvalidateRenderData)
{
	if ( MorphTarget )
	{
		// Do not remove with MorphTarget->GetFName(). The name might have changed
		// Search the value, and delete	
		for ( int32 I=0; I< GetMorphTargets().Num(); ++I)
		{
			if (GetMorphTargets()[I] == MorphTarget )
			{
				GetMorphTargets().RemoveAt(I);
				--I;
				if(bInvalidateRenderData)
				{
					InitMorphTargetsAndRebuildRenderData();
				}
				return;
			}
		}
		UE_LOG( LogSkeletalMesh, Log, TEXT("UnregisterMorphTarget: %s not found."), *MorphTarget->GetName() );
	}
}

void USkeletalMesh::InitMorphTargets(bool bInKeepEmptyMorphTargets)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::InitMorphTargets);
	GetMorphTargetIndexMap().Empty();

	TArray<TObjectPtr<UMorphTarget>>& MorphTargetsLocal = GetMorphTargets();
	for (int32 Index = 0; Index < MorphTargetsLocal.Num(); ++Index)
	{
		const UMorphTarget* MorphTarget = MorphTargetsLocal[Index];
		
		// If asked to remove empty morph targets and the morph target doesn't have any data, just remove it.
		if (!bInKeepEmptyMorphTargets && !MorphTarget->HasValidData())
		{
			MorphTargetsLocal.RemoveAt(Index);
			--Index;
			continue;
		}

		const FName ShapeName = MorphTarget->GetFName();
		if (GetMorphTargetIndexMap().Find(ShapeName) == nullptr)
		{
			GetMorphTargetIndexMap().Add(ShapeName, Index);

			// Note: we don't register as morph target curves here as curves metadata can now be
			// specified on this mesh, which can now opt out of the morph flag being set
		}
	}
}

UMorphTarget* USkeletalMesh::FindMorphTarget(FName MorphTargetName) const
{
	int32 Index;
	return FindMorphTargetAndIndex(MorphTargetName, Index);
}

UMorphTarget* USkeletalMesh::FindMorphTargetAndIndex(FName MorphTargetName, int32& OutIndex) const
{
	OutIndex = INDEX_NONE;
	if( MorphTargetName != NAME_None )
	{
		const int32* Found = GetMorphTargetIndexMap().Find(MorphTargetName);
		if (Found)
		{
			OutIndex = *Found;
			return GetMorphTargets()[*Found];
		}
	}

	return nullptr;
}

#if WITH_EDITOR
bool USkeletalMesh::RemoveMorphTargets(TConstArrayView<FName> InMorphTargetNames)
{
	if(InMorphTargetNames.Num() == 0)
	{
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("DeleteMorphTargets", "Delete Morph Targets"));

	bool bRemoved = false;
	for(FName MorphTargetName : InMorphTargetNames)
	{
		UMorphTarget* MorphTarget = FindMorphTarget(MorphTargetName);
		if(MorphTarget)
		{
			MorphTarget->RemoveFromRoot();
			MorphTarget->ClearFlags(RF_Standalone);

			Modify();
			MorphTarget->Modify();

			constexpr int32 LODIndex = 0;

			if (HasMeshDescription(LODIndex))
			{
				//Remove the morph target from the raw import data
				FMeshDescription* MeshDescription = GetMeshDescription(LODIndex);
				FSkeletalMeshAttributes MeshAttributes(*MeshDescription);

				if (MeshAttributes.GetMorphTargetNames().Contains(MorphTargetName))
				{
					ModifyMeshDescription(LODIndex);
					MeshAttributes.UnregisterMorphTargetAttribute(MorphTargetName);
					CommitMeshDescription(LODIndex);
				}
				else
				{
					// this means that MorphTargets and MeshDescription are not synchronized (which should not happen)
					// if the DDC is not invalidated, the MorphTargets array will be reset to its previous value in the next build. 
					InvalidateDeriveDataCacheGUID();
				}
			}

			UnregisterMorphTarget(MorphTarget);

			MorphTarget->Rename(nullptr, GetTransientPackage(), REN_DoNotDirty | REN_DontCreateRedirectors);
			MorphTarget->MarkAsGarbage();
			
			bRemoved = true;
		}

		//Clean up the LodInfo Imported morph target source filename
		for (int32 LODIndex = 0; LODIndex < GetLODNum(); ++LODIndex)
		{
			FSkeletalMeshLODInfo& LODInfoEntry = *GetLODInfo(LODIndex);
			LODInfoEntry.ImportedMorphTargetSourceFilename.Remove(MorphTargetName.ToString());
		}
	}

	return bRemoved;
}

bool USkeletalMesh::RenameMorphTarget(FName InOldName, FName InNewName)
{
	FText Reason;
	if(!InOldName.IsValidObjectName(Reason) || !InNewName.IsValidObjectName(Reason))
	{
		UE_LOGFMT(LogAnimation, Warning, "Could not rename morph target from {0} to {1}. {2}", InOldName, InNewName, Reason.ToString());
		return false;
	}

	if(FindObject<UObject>(this, *InNewName.ToString()))
	{
		UE_LOGFMT(LogAnimation, Warning, "Could not rename morph target from {0} to {1}. Destination object already exists.", InOldName, InNewName);
		return false;
	}
	
	UMorphTarget* MorphTarget = FindMorphTarget(InOldName);
	if(MorphTarget == nullptr)
	{
		UE_LOGFMT(LogAnimation, Warning, "Could not rename morph target from {0} to {1}. Could not find morph target.", InOldName, InNewName);
		return false;
	}

	FScopedTransaction Transaction(LOCTEXT("RenameMorphTarget", "Rename Morph Target"));

	// Unregister the morph target (but dont invalidate render data yet, we will recreate it below in RegisterMorphTarget)
	UnregisterMorphTarget(MorphTarget, false);

	Modify();
	MorphTarget->Modify();

	constexpr int32 LODIndex = 0;
	if (HasMeshDescription(LODIndex))
	{
		FMeshDescription* MeshDescription = GetMeshDescription(LODIndex);
		FSkeletalMeshAttributes MeshAttributes(*MeshDescription);

		if (MeshAttributes.GetMorphTargetNames().Contains(InOldName))
		{
			ModifyMeshDescription(LODIndex);

			const bool bNeedNormals = MeshAttributes.GetVertexInstanceMorphNormalDelta(InOldName).IsValid();
			
			if (MeshAttributes.RegisterMorphTargetAttribute(InNewName, bNeedNormals))
			{
				const TVertexAttributesConstRef<FVector3f> SourcePositionDelta{ MeshAttributes.GetVertexMorphPositionDelta(InOldName) };
				TVertexAttributesRef<FVector3f> TargetPositionDelta{ MeshAttributes.GetVertexMorphPositionDelta(InNewName) };

				TargetPositionDelta.Copy(SourcePositionDelta);

				if(bNeedNormals)
				{
					const TVertexInstanceAttributesConstRef<FVector3f> SourceNormalDelta{ MeshAttributes.GetVertexInstanceMorphNormalDelta(InOldName) };
					TVertexInstanceAttributesRef<FVector3f> TargetNormalDelta{ MeshAttributes.GetVertexInstanceMorphNormalDelta(InNewName) };
					TargetNormalDelta.Copy(SourceNormalDelta);
				}
				
				MeshAttributes.UnregisterMorphTargetAttribute(InOldName);
			}
		}
	}

	// Rename the morph target itself
	MorphTarget->Rename(*InNewName.ToString(), nullptr, REN_DontCreateRedirectors);

	//Clean up the LodInfo Imported morph target source filename we must also rename the entry
	for (int32 InternalLodIndex = 0; InternalLodIndex < GetLODNum(); ++InternalLodIndex)
	{
		FSkeletalMeshLODInfo& LODInfoEntry = *GetLODInfo(InternalLodIndex);
		if (const FMorphTargetImportedSourceFileInfo* MorphTargetImportedSourceFileInfo = LODInfoEntry.ImportedMorphTargetSourceFilename.Find(InOldName.ToString()))
		{
			const FString OldFilename = MorphTargetImportedSourceFileInfo->GetSourceFilename();
			const bool bOldIsGeneratedByEngine = MorphTargetImportedSourceFileInfo->IsGeneratedByEngine();
			FMorphTargetImportedSourceFileInfo& NewData = LODInfoEntry.ImportedMorphTargetSourceFilename.FindOrAdd(InNewName.ToString());
			NewData.SetSourceFilename(OldFilename);
			NewData.SetGeneratedByEngine(bOldIsGeneratedByEngine);
			LODInfoEntry.ImportedMorphTargetSourceFilename.Remove(InOldName.ToString());
		}
	}

	// Re-register the morph target
	RegisterMorphTarget(MorphTarget);

	return true;
}
#endif

USkeletalMeshSocket* USkeletalMesh::FindSocket(FName InSocketName) const
{
	int32 DummyIdx;
	return FindSocketAndIndex(InSocketName, DummyIdx);
}

#if WITH_EDITOR
void USkeletalMesh::AddSocket(USkeletalMeshSocket* InSocket, bool bAddToSkeleton /*= false*/)
{
	if (!InSocket)
	{
		return;
	}
	
	// The socket needs to be owned already by this skeletal mesh.
	if (InSocket->GetOuter() != this)
	{
		UE_LOG(LogSkeletalMesh, Error, TEXT("Failed to add socket as the socket its outer should be %s but is %s."), *GetFullName(), *InSocket->GetOuter()->GetFullName());
		return;
	}

	// If the socket was freshly created, which by default doesn't have a name, assign a default name to it now.
	FName WantedSocketName;
	if (InSocket->SocketName.IsNone())
	{
		static const FName BaseSocketName("Socket");
		
		int32 TestNumber = 0;
		do
		{
			WantedSocketName = FName(BaseSocketName, TestNumber++);
		} while (FindSocket(WantedSocketName) != nullptr);
	}
	else
	{
		// Make sure it's unique across all known sockets.
		const FString SocketNameString = InSocket->SocketName.ToString();
		const FString TrimmedNameString = SocketNameString.TrimStartAndEnd();
		WantedSocketName = FName(*TrimmedNameString);
		if (Sockets.ContainsByPredicate([WantedSocketName](const TObjectPtr<USkeletalMeshSocket>& Socket)
		{
			return Socket->SocketName == WantedSocketName;
		}))
		{
			// Socket already exists
			UE_LOG(LogSkeletalMesh, Error, TEXT("Failed to add socket as a socket with name %s already exist."), *InSocket->BoneName.ToString());
			return;
		}
	}

	// Check if the bone exists. If set to None, as would happen with a default created socket, then assign the socket to the root bone.
	// If wanting to add to the skeleton, then the additional restriction of the bone name needing to exist on the skeleton is also required.
	const FReferenceSkeleton& ReferenceSkeleton = bAddToSkeleton ? GetSkeleton()->GetReferenceSkeleton() : GetRefSkeleton();
	FName WantedBoneName = InSocket->BoneName;
	if (InSocket->BoneName.IsNone())
	{
		WantedBoneName = ReferenceSkeleton.GetBoneName(0);
	}
	else if (ReferenceSkeleton.FindBoneIndex(InSocket->BoneName) == INDEX_NONE)
	{
		UE_LOG(LogSkeletalMesh, Error, TEXT("Failed to add socket as the provided bone name %s does not exist."), *InSocket->BoneName.ToString());
		return;
	}

	const FScopedTransaction Transaction(LOCTEXT("AddSocket", "Add Socket"));
	
	Modify();
	InSocket->Modify();

	InSocket->SocketName = WantedSocketName;
	InSocket->BoneName = WantedBoneName;
	
	Sockets.Add(InSocket);

	if (bAddToSkeleton)
	{
		USkeleton* CurrentSkeleton = GetSkeleton();				
		if (!CurrentSkeleton->Sockets.ContainsByPredicate([Name=InSocket->SocketName](const TObjectPtr<USkeletalMeshSocket>& Socket)
		{
			return Socket->SocketName == Name;
		}))
		{
			CurrentSkeleton->Modify();

			USkeletalMeshSocket* NewSocket = DuplicateObject<USkeletalMeshSocket>(InSocket, CurrentSkeleton);
			check(NewSocket);
			CurrentSkeleton->Sockets.Add(NewSocket);
		}
	}
}
#endif

#if !WITH_EDITOR

USkeletalMesh::FSocketInfo::FSocketInfo(const USkeletalMesh* InSkeletalMesh, USkeletalMeshSocket* InSocket, int32 InSocketIndex)
	: SocketLocalTransform(InSocket->GetSocketLocalTransform())
	, Socket(InSocket)
	, SocketIndex(InSocketIndex)
	, SocketBoneIndex(InSkeletalMesh->GetRefSkeleton().FindBoneIndex(InSocket->BoneName))
{}

#endif

USkeletalMeshSocket* USkeletalMesh::FindSocketAndIndex(FName InSocketName, int32& OutIndex) const
{
	OutIndex = INDEX_NONE;
	if (InSocketName == NAME_None)
	{
		return nullptr;
	}

#if WITH_EDITOR
	for (int32 i = 0; i < Sockets.Num(); i++)
	{
		USkeletalMeshSocket* Socket = Sockets[i];
		if (Socket && Socket->SocketName == InSocketName)
		{
			OutIndex = i;
			return Socket;
		}
	}

	// If the socket isn't on the mesh, try to find it on the skeleton
	if (GetSkeleton())
	{
		USkeletalMeshSocket* SkeletonSocket = GetSkeleton()->FindSocketAndIndex(InSocketName, OutIndex);
		if (SkeletonSocket != nullptr)
		{
			OutIndex += Sockets.Num();
		}
		return SkeletonSocket;
	}
#else
	check(!HasAnyFlags(RF_NeedPostLoad));

	const FSocketInfo* FoundSocketInfo = SocketMap.Find(InSocketName);
	if (FoundSocketInfo)
	{
		OutIndex = FoundSocketInfo->SocketIndex;
		return FoundSocketInfo->Socket;
	}
#endif

	return nullptr;
}

USkeletalMeshSocket* USkeletalMesh::FindSocketInfo(FName InSocketName, FTransform& OutTransform, int32& OutBoneIndex, int32& OutIndex) const
{
	OutIndex = INDEX_NONE;
	OutTransform = FTransform::Identity;
	OutBoneIndex = INDEX_NONE;

	if (InSocketName == NAME_None)
	{
		return nullptr;
	}

#if WITH_EDITOR
	for (int32 i = 0; i < Sockets.Num(); i++)
	{
		USkeletalMeshSocket* Socket = Sockets[i];
		if (Socket && Socket->SocketName == InSocketName)
		{
			OutIndex = i;
			OutTransform = Socket->GetSocketLocalTransform();
			OutBoneIndex = GetRefSkeleton().FindBoneIndex(Socket->BoneName);
			return Socket;
		}
	}

	// If the socket isn't on the mesh, try to find it on the skeleton
	if (GetSkeleton())
	{
		USkeletalMeshSocket* SkeletonSocket = GetSkeleton()->FindSocketAndIndex(InSocketName, OutIndex);
		if (SkeletonSocket != nullptr)
		{
			OutIndex += Sockets.Num();
			OutTransform = SkeletonSocket->GetSocketLocalTransform();
			OutBoneIndex = GetRefSkeleton().FindBoneIndex(SkeletonSocket->BoneName);
		}
		return SkeletonSocket;
	}
#else
	check(!HasAnyFlags(RF_NeedPostLoad));

	const FSocketInfo* FoundSocketInfo = SocketMap.Find(InSocketName);
	if (FoundSocketInfo)
	{
		OutTransform = FoundSocketInfo->SocketLocalTransform;
		OutIndex = FoundSocketInfo->SocketIndex;
		OutBoneIndex = FoundSocketInfo->SocketBoneIndex;
		return FoundSocketInfo->Socket;
	}
#endif

	return nullptr;
}

int32 USkeletalMesh::NumSockets() const
{
	return Sockets.Num() + (GetSkeleton() ? GetSkeleton()->Sockets.Num() : 0);
}

USkeletalMeshSocket* USkeletalMesh::GetSocketByIndex(int32 Index) const
{
	const int32 NumMeshSockets = Sockets.Num();
	if (Index < NumMeshSockets)
	{
		return Sockets[Index];
	}

	if (GetSkeleton() && (Index - NumMeshSockets) < GetSkeleton()->Sockets.Num())
	{
		return GetSkeleton()->Sockets[Index - NumMeshSockets];
	}

	return nullptr;
}

TMap<FVector3f, FColor> USkeletalMesh::GetVertexColorData(const uint32 PaintingMeshLODIndex) const
{
	TMap<FVector3f, FColor> VertexColorData;
#if WITH_EDITOR
	const FSkeletalMeshModel* SkeletalMeshModel = GetImportedModel();
	if (GetHasVertexColors() && SkeletalMeshModel && SkeletalMeshModel->LODModels.IsValidIndex(PaintingMeshLODIndex))
	{
		const TArray<FSkelMeshSection>& Sections = SkeletalMeshModel->LODModels[PaintingMeshLODIndex].Sections;

		for (int32 SectionIndex = 0; SectionIndex < Sections.Num(); ++SectionIndex)
		{
			const TArray<FSoftSkinVertex>& SoftVertices = Sections[SectionIndex].SoftVertices;
			
			for (int32 VertexIndex = 0; VertexIndex < SoftVertices.Num(); ++VertexIndex)
			{
				const FVector3f& Position(SoftVertices[VertexIndex].Position);
				FColor& Color = VertexColorData.FindOrAdd(Position);
				Color = SoftVertices[VertexIndex].Color;
			}
		}
	}
#endif // #if WITH_EDITOR

	return VertexColorData;
}


void USkeletalMesh::RebuildSocketMap()
{
#if !WITH_EDITOR

	check(IsInGameThread());

	SocketMap.Reset();
	SocketMap.Reserve(Sockets.Num() + (GetSkeleton() ? GetSkeleton()->Sockets.Num() : 0));

	int32 SocketIndex;
	for (SocketIndex = 0; SocketIndex < Sockets.Num(); ++SocketIndex)
	{
		USkeletalMeshSocket* Socket = Sockets[SocketIndex];
		SocketMap.Add(Socket->SocketName, FSocketInfo(this, Socket, SocketIndex));
	}

	// If the socket isn't on the mesh, try to find it on the skeleton
	if (GetSkeleton())
	{
		for (SocketIndex = 0; SocketIndex < GetSkeleton()->Sockets.Num(); ++SocketIndex)
		{
			USkeletalMeshSocket* Socket = GetSkeleton()->Sockets[SocketIndex];
			if (!SocketMap.Contains(Socket->SocketName))
			{
				SocketMap.Add(Socket->SocketName, FSocketInfo(this, Socket, Sockets.Num() + SocketIndex));
			}
		}
	}

#endif // !WITH_EDITOR
}

FMatrix USkeletalMesh::GetRefPoseMatrix( int32 BoneIndex ) const
{
 	check( BoneIndex >= 0 && BoneIndex < GetRefSkeleton().GetRawBoneNum() );
	FTransform BoneTransform = GetRefSkeleton().GetRawRefBonePose()[BoneIndex];
	// Make sure quaternion is normalized!
	BoneTransform.NormalizeRotation();
	return BoneTransform.ToMatrixWithScale();
}

FMatrix USkeletalMesh::GetComposedRefPoseMatrix( FName InBoneName ) const
{
	FMatrix LocalPose( FMatrix::Identity );

	if ( InBoneName != NAME_None )
	{
		int32 BoneIndex = GetRefSkeleton().FindBoneIndex(InBoneName);
		if (BoneIndex != INDEX_NONE)
		{
			return GetComposedRefPoseMatrix(BoneIndex);
		}
		else
		{
			USkeletalMeshSocket const* Socket = FindSocket(InBoneName);

			if(Socket != NULL)
			{
				BoneIndex = GetRefSkeleton().FindBoneIndex(Socket->BoneName);

				if(BoneIndex != INDEX_NONE)
				{
					const FRotationTranslationMatrix SocketMatrix(Socket->RelativeRotation, Socket->RelativeLocation);
					LocalPose = SocketMatrix * GetComposedRefPoseMatrix(BoneIndex);
				}
			}
		}
	}

	return LocalPose;
}

FMatrix USkeletalMesh::GetComposedRefPoseMatrix(int32 InBoneIndex) const
{
	return GetCachedComposedRefPoseMatrices()[InBoneIndex];
}

TArray<TObjectPtr<USkeletalMeshSocket>>& USkeletalMesh::GetMeshOnlySocketList()
{
	return Sockets;
}

const TArray<USkeletalMeshSocket*>& USkeletalMesh::GetMeshOnlySocketList() const
{
	return Sockets;
}

#if WITH_EDITORONLY_DATA
void USkeletalMesh::MoveDeprecatedShadowFlagToMaterials()
{
	// First, the easy case where there's no LOD info (in which case, default to true!)
	const int32 NumLODs = GetLODNum();
	if (NumLODs == 0)
	{
		for (FSkeletalMaterial& Material: GetMaterials())
		{
			Material.bEnableShadowCasting_DEPRECATED = true;
		}
		return;
	}
	
	TArray<bool> PerLodShadowFlags;
	bool bDifferenceFound = false;

	// Second, detect whether the shadow casting flag is the same for all sections of all lods
	for (int32 LODIndex = 0; LODIndex < NumLODs; ++LODIndex)
	{
		const FSkeletalMeshLODInfo& MeshLODInfo = *GetLODInfo(LODIndex);
		if ( MeshLODInfo.bEnableShadowCasting_DEPRECATED.Num() )
		{
			PerLodShadowFlags.Add( MeshLODInfo.bEnableShadowCasting_DEPRECATED[0] );
		}

		if ( !AreAllFlagsIdentical( MeshLODInfo.bEnableShadowCasting_DEPRECATED ) )
		{
			// We found a difference in the sections of this LOD!
			bDifferenceFound = true;
			break;
		}
	}

	if ( !bDifferenceFound && !AreAllFlagsIdentical( PerLodShadowFlags ) )
	{
		// Difference between LODs
		bDifferenceFound = true;
	}

	if ( !bDifferenceFound )
	{
		// All the same, so just copy the shadow casting flag to all materials
		for (FSkeletalMaterial& Material: GetMaterials())
		{
			Material.bEnableShadowCasting_DEPRECATED = PerLodShadowFlags.Num() ? PerLodShadowFlags[0] : true;
		}
	}
	else
	{
		FSkeletalMeshModel* Resource = GetImportedModel();
		check( Resource->LODModels.Num() == NumLODs );

		TArray<FSkeletalMaterial> NewMaterialArray;
		TArray<FSkeletalMaterial>& CurrentMaterials = GetMaterials();

		// There was a difference, so we need to build a new material list which has all the combinations of UMaterialInterface and shadow casting flag required
		for ( int32 LODIndex = 0; LODIndex < Resource->LODModels.Num(); ++LODIndex )
		{
			const TArray<bool>& EnableShadowCasting = GetLODInfo(LODIndex)->bEnableShadowCasting_DEPRECATED;
			check(Resource->LODModels[LODIndex].Sections.Num() == EnableShadowCasting.Num());

			for ( int32 SectionIndex = 0; SectionIndex < Resource->LODModels[LODIndex].Sections.Num(); ++SectionIndex )
			{
				NewMaterialArray.Add( FSkeletalMaterial(CurrentMaterials[ Resource->LODModels[LODIndex].Sections[SectionIndex].MaterialIndex ].MaterialInterface, EnableShadowCasting[SectionIndex], false, NAME_None, NAME_None ) );
			}
		}

		// Reassign the materials array to the new one
		SetMaterials(NewMaterialArray);
		int32 NewIndex = 0;

		// Remap the existing LODModels to point at the correct new material index
		for ( int32 LODIndex = 0; LODIndex < Resource->LODModels.Num(); ++LODIndex )
		{
			check( Resource->LODModels[LODIndex].Sections.Num() == GetLODInfo(LODIndex)->bEnableShadowCasting_DEPRECATED.Num() );

			for ( int32 SectionIndex = 0; SectionIndex < Resource->LODModels[LODIndex].Sections.Num(); ++SectionIndex )
			{
				Resource->LODModels[LODIndex].Sections[SectionIndex].MaterialIndex = NewIndex;
				++NewIndex;
			}
		}
	}
}

void USkeletalMesh::MoveMaterialFlagsToSections()
{
	//No LOD we cant set the value
	if (GetLODNum() == 0)
	{
		return;
	}

	TArray<FSkeletalMaterial>& CurrentMaterials = GetMaterials();
	for (FSkeletalMeshLODModel &StaticLODModel : GetImportedModel()->LODModels)
	{
		for (int32 SectionIndex = 0; SectionIndex < StaticLODModel.Sections.Num(); ++SectionIndex)
		{
			FSkelMeshSection &Section = StaticLODModel.Sections[SectionIndex];
			//Prior to FEditorObjectVersion::RefactorMeshEditorMaterials Material index match section index
			if (CurrentMaterials.IsValidIndex(SectionIndex))
			{
				Section.bCastShadow = CurrentMaterials[SectionIndex].bEnableShadowCasting_DEPRECATED;

				Section.bRecomputeTangent = CurrentMaterials[SectionIndex].bRecomputeTangent_DEPRECATED;
			}
			else
			{
				//Default cast shadow to true this is a fail safe code path it should not go here if the data
				//is valid
				Section.bCastShadow = true;
				//Recompute tangent is serialize prior to FEditorObjectVersion::RefactorMeshEditorMaterials
				// We just keep the serialize value
			}
		}
	}
}
#endif // WITH_EDITORONLY_DATA

#if WITH_EDITOR

FDelegateHandle USkeletalMesh::RegisterOnClothingChange(const FSimpleMulticastDelegate::FDelegate& InDelegate)
{
	return OnClothingChange.Add(InDelegate);
}

void USkeletalMesh::UnregisterOnClothingChange(const FDelegateHandle& InHandle)
{
	OnClothingChange.Remove(InHandle);
}

#endif

bool USkeletalMesh::AreAllFlagsIdentical( const TArray<bool>& BoolArray ) const
{
	if ( BoolArray.Num() == 0 )
	{
		return true;
	}

	for ( int32 i = 0; i < BoolArray.Num() - 1; ++i )
	{
		if ( BoolArray[i] != BoolArray[i + 1] )
		{
			return false;
		}
	}

	return true;
}

TArray<USkeletalMeshSocket*> USkeletalMesh::GetActiveSocketList() const
{
	TArray<USkeletalMeshSocket*> ActiveSockets = Sockets;

	// Then the skeleton sockets that aren't in the mesh
	if (GetSkeleton())
	{
		for (auto SkeletonSocketIt = GetSkeleton()->Sockets.CreateConstIterator(); SkeletonSocketIt; ++SkeletonSocketIt)
		{
			USkeletalMeshSocket* Socket = *(SkeletonSocketIt);

			if (!IsSocketOnMesh(Socket->SocketName))
			{
				ActiveSockets.Add(Socket);
			}
		}
	}
	return ActiveSockets;
}

bool USkeletalMesh::IsSocketOnMesh(const FName& InSocketName) const
{
	for(int32 SocketIdx=0; SocketIdx < Sockets.Num(); SocketIdx++)
	{
		USkeletalMeshSocket* Socket = Sockets[SocketIdx];

		if(Socket != NULL && Socket->SocketName == InSocketName)
		{
			return true;
		}
	}

	return false;
}

void USkeletalMesh::AllocateResourceForRendering()
{
	SetSkeletalMeshRenderData(MakeUnique<FSkeletalMeshRenderData>());
}

#if WITH_EDITOR
void USkeletalMesh::InvalidateDeriveDataCacheGUID()
{
	// Create new DDC guid
	GetImportedModel()->GenerateNewGUID();
}

namespace InternalSkeletalMeshHelper
{
	/**
	 * We want to recreate the LODMaterialMap correctly. The hypothesis is the original section will always be the same when we build the skeletalmesh
	 * Max GPU bone per section which drive the chunking which can generate different number of section but the number of original section will always be the same.
	 * So we simply reset the LODMaterialMap and rebuild it with the backup we took before building the skeletalmesh.
	 */
	void CreateLodMaterialMapBackup(const USkeletalMesh* SkeletalMesh, TMap<int32, TArray<int16>>& BackupSectionsPerLOD)
	{
		if (!ensure(SkeletalMesh != nullptr))
		{
			return;
		}
		BackupSectionsPerLOD.Reset();
		const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
		if (!ImportedModel)
		{
			return;
		}
		//Create the backup
		for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum(); ++LODIndex)
		{
			const FSkeletalMeshLODInfo* LODInfoEntry = SkeletalMesh->GetLODInfo(LODIndex);
			//Do not backup/restore LODMaterialMap if...
			if (!ImportedModel->LODModels.IsValidIndex(LODIndex)
				|| LODInfoEntry == nullptr
				|| LODInfoEntry->LODMaterialMap.Num() == 0 //If there is no LODMaterialMap we have nothing to backup
				|| SkeletalMesh->IsReductionActive(LODIndex) //Reduction will manage the LODMaterialMap, avoid backup restore
				|| !SkeletalMesh->HasMeshDescription(LODIndex)) //Legacy asset are not build, avoid backup restore
			{
				continue;
			}
			const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];
			TArray<int16>& BackupSections = BackupSectionsPerLOD.FindOrAdd(LODIndex);
			int32 SectionCount = LODModel.Sections.Num();
			BackupSections.Reserve(SectionCount);
			for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
			{
				if (LODModel.Sections[SectionIndex].ChunkedParentSectionIndex == INDEX_NONE)
				{
					BackupSections.Add(LODInfoEntry->LODMaterialMap.IsValidIndex(SectionIndex) ? LODInfoEntry->LODMaterialMap[SectionIndex] : INDEX_NONE);
				}
			}
		}
	}

	void RestoreLodMaterialMapBackup(USkeletalMesh* SkeletalMesh, const TMap<int32, TArray<int16>>& BackupSectionsPerLOD)
	{
		if (!ensure(SkeletalMesh != nullptr))
		{
			return;
		}
		const FSkeletalMeshModel* ImportedModel = SkeletalMesh->GetImportedModel();
		if (!ImportedModel)
		{
			return;
		}

		for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum(); ++LODIndex)
		{
			FSkeletalMeshLODInfo* LODInfoEntry = SkeletalMesh->GetLODInfo(LODIndex);
			if (!ImportedModel->LODModels.IsValidIndex(LODIndex) || LODInfoEntry == nullptr)
			{
				continue;
			}
			const TArray<int16>* BackupSectionsPtr = BackupSectionsPerLOD.Find(LODIndex);
			if (!BackupSectionsPtr || BackupSectionsPtr->IsEmpty())
			{
				continue;
			}

			const TArray<int16>& BackupSections = *BackupSectionsPtr;
			const FSkeletalMeshLODModel& LODModel = ImportedModel->LODModels[LODIndex];
			LODInfoEntry->LODMaterialMap.Reset();
			const int32 SectionCount = LODModel.Sections.Num();
			for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
			{
				const FSkelMeshSection& Section = LODModel.Sections[SectionIndex];
				int16 NewLODMaterialMapValue = INDEX_NONE;
				if (BackupSections.IsValidIndex(Section.OriginalDataSectionIndex))
				{
					NewLODMaterialMapValue = BackupSections[Section.OriginalDataSectionIndex];
				}
				LODInfoEntry->LODMaterialMap.Add(NewLODMaterialMapValue);
			}
		}
	}
} //namespace InternalSkeletalMeshHelper

void USkeletalMesh::CacheDerivedData(FSkinnedAssetCompilationContext* ContextPtr)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(USkeletalMesh::CacheDerivedData);
	check(ContextPtr);

	// Cache derived data for the running platform.
	ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	check(RunningPlatform);

	AllocateResourceForRendering();

	// Warn if the platform support minimal number of per vertex bone influences 
	ValidateBoneWeights(RunningPlatform);

	//LODMaterialMap from LODInfo is store in the uasset and not in the DDC, so we want to fix it here
	//to cover the post load and the post edit change. The build can change the number of section and LODMaterialMap is index per section
	//TODO, move LODMaterialmap functionality into the LODModel UserSectionsData which are index per original section (imported section).
	TMap<int32, TArray<int16>> BackupSectionsPerLOD;
	InternalSkeletalMeshHelper::CreateLodMaterialMapBackup(this, BackupSectionsPerLOD);

	GetSkeletalMeshRenderData()->Cache(RunningPlatform, this, ContextPtr);

	InternalSkeletalMeshHelper::RestoreLodMaterialMapBackup(this, BackupSectionsPerLOD);
}


void USkeletalMesh::ValidateBoneWeights(const ITargetPlatform* TargetPlatform)
{
	if (TargetPlatform->SupportsFeature(ETargetPlatformFeatures::MobileRendering))
	{
		if (!GetImportedModel())
		{
			return;
		}
		for (int32 LODIndex = 0; LODIndex < GetLODNum(); ++LODIndex)
		{
			if (!GetImportedModel()->LODModels.IsValidIndex(LODIndex))
			{
				continue;
			}
			const FSkeletalMeshLODModel& ImportLODModel = GetImportedModel()->LODModels[LODIndex];

			int32 MaxBoneInfluences = ImportLODModel.GetMaxBoneInfluences();
			if (MaxBoneInfluences > 12 )
			{
				UE_LOG(LogSkeletalMesh, Warning, TEXT("Mesh: %s has more than 12 max bone influences, it has: %d"), *GetFullName(), MaxBoneInfluences);
			}
		}
	}
}

void USkeletalMesh::BeginCacheForCookedPlatformData(const ITargetPlatform* TargetPlatform)
{
	LLM_SCOPE(ELLMTag::SkeletalMesh);
	// Make sure to cache platform data so it doesn't happen lazily during serialization of the skeletal mesh
	constexpr bool bIsSerializeSaving = false;
	::GetPlatformSkeletalMeshRenderData(this, TargetPlatform, bIsSerializeSaving);
	ValidateBoneWeights(TargetPlatform);
}

void USkeletalMesh::ClearAllCachedCookedPlatformData()
{
	LLM_SCOPE(ELLMTag::SkeletalMesh);
	GetResourceForRendering()->NextCachedRenderData.Reset();
	GetResourceForRendering()->NaniteResourcesPtr->DropBulkData();
	
	if (FApp::CanEverRender())
	{
		// We need to keep the ddc editor data LODModel for rendering; it can be different (The number of sections, the number of vertices, the number of morphtargets) because of chunking, build or reduction setting that are or will be per platform.
		//Normally this call should be able to read values out of ddc rather than rebuilding, because the ddc for the running platform was cached when we loaded the asset.
		ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
		check(RunningPlatform != NULL);
		const FString RunningPlatformDerivedDataKey = BuildDerivedDataKey(RunningPlatform);
		FSkeletalMeshRenderData RunningPlatformSkeletalMeshRenderData;
		constexpr bool bIsSerializeSaving = false;
		CachePlatform(this, RunningPlatform, &RunningPlatformSkeletalMeshRenderData, bIsSerializeSaving);
	}
}

//Serialize the LODInfo and append the result to the KeySuffix to build the LODInfo part of the DDC KEY
//Note: this serializer is only used to build the mesh DDC key, no versioning is required
static void SerializeLODInfoForDDC(USkeletalMesh* SkeletalMesh, FString& KeySuffix)
{
	for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum(); ++LODIndex)
	{
		FSkeletalMeshLODInfo& LODInfo = *SkeletalMesh->GetLODInfo(LODIndex);
		bool bValidLODSettings = false;
		if (SkeletalMesh->GetLODSettings() != nullptr)
		{
			const int32 NumSettings = FMath::Min(SkeletalMesh->GetLODSettings()->GetNumberOfSettings(), SkeletalMesh->GetLODNum());
			if (LODIndex < NumSettings)
			{
				bValidLODSettings = true;
			}
		}
		const FSkeletalMeshLODGroupSettings* SkeletalMeshLODGroupSettings = bValidLODSettings ? &SkeletalMesh->GetLODSettings()->GetSettingsForLODLevel(LODIndex) : nullptr;
		LODInfo.BuildGUID = LODInfo.ComputeDeriveDataCacheKey(SkeletalMeshLODGroupSettings);
		KeySuffix += LODInfo.BuildGUID.ToString(EGuidFormats::Digits);

		KeySuffix += SkeletalMesh->HasHalfEdgeBuffer(LODIndex) ? "1" : "0";
	}
}

extern int32 GStripSkeletalMeshLodsDuringCooking;
extern int32 GSkeletalMeshKeepMobileMinLODSettingOnDesktop;

FString USkeletalMesh::BuildDerivedDataKey(const ITargetPlatform* TargetPlatform)
{
	TArray<uint8> TempBytes;
	TempBytes.Reserve(64);

	FString KeySuffix(TEXT(""));

	FString TmpPartialKeySuffix;
	//Synchronize the user data that are part of the key
	GetImportedModel()->SyncronizeLODUserSectionsData();
	TmpPartialKeySuffix = GetImportedModel()->GetIdString();
	KeySuffix += TmpPartialKeySuffix;
	TmpPartialKeySuffix = GetImportedModel()->GetLODModelIdString();
	KeySuffix += TmpPartialKeySuffix;

	//Add the max gpu bone per section
	const int32 MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones(TargetPlatform);
	KeySuffix += FString::FromInt(MaxGPUSkinBones);

	TmpPartialKeySuffix = TEXT("");
	SerializeLODInfoForDDC(this, TmpPartialKeySuffix);
	KeySuffix += TmpPartialKeySuffix;
	KeySuffix += GetHasVertexColors() ? "1" : "0";
	KeySuffix += GetVertexColorGuid().ToString(EGuidFormats::Digits);

	if (GetEnableLODStreaming(TargetPlatform))
	{
		const int32 MaxStreamedLODs = GetMaxNumStreamedLODs(TargetPlatform);
		const int32 MaxOptionalLODs = GetMaxNumOptionalLODs(TargetPlatform);
		KeySuffix += *FString::Printf(TEXT("1%08x%08x"), MaxStreamedLODs, MaxOptionalLODs);
	}
	else
	{
		KeySuffix += TEXT("0zzzzzzzzzzzzzzzz");
	}

	if (TargetPlatform->GetPlatformInfo().PlatformGroupName == TEXT("Desktop")
		&& GStripSkeletalMeshLodsDuringCooking != 0
		&& GSkeletalMeshKeepMobileMinLODSettingOnDesktop != 0)
	{
		KeySuffix += TEXT("_MinMLOD");
	}

	IMeshBuilderModule::GetForPlatform(TargetPlatform).AppendToDDCKey(KeySuffix, true);
	const bool bUnlimitedBoneInfluences = FGPUBaseSkinVertexFactory::GetUnlimitedBoneInfluences(TargetPlatform);
	KeySuffix += bUnlimitedBoneInfluences ? "1" : "0";

	// Include the global default bone influences limit in case any LODs don't set an explicit limit (highly likely)
	KeySuffix += FString::FromInt(GetDefault<URendererSettings>()->DefaultBoneInfluenceLimit.GetValueForPlatform(*TargetPlatform->IniPlatformName()));

	if (IsNaniteEnabled())
	{
		TempBytes.Reset();
		FMemoryWriter Ar(TempBytes, /*bIsPersistent=*/ true);
		SerializeNaniteSettingsForDDC(Ar, NaniteSettings, false /* Is force enabled */);

		const uint8* SettingsAsBytes = TempBytes.GetData();
		KeySuffix.Reserve(KeySuffix.Len() + TempBytes.Num() + 1);
		for (int32 ByteIndex = 0; ByteIndex < TempBytes.Num(); ++ByteIndex)
		{
			ByteToHex(SettingsAsBytes[ByteIndex], KeySuffix);
		}

		// Nanite skeletal mesh version
		KeySuffix += TEXT("_NSK_WIP_1");

		static FString CachedNaniteVersion = FDevSystemGuids::GetSystemGuid(FDevSystemGuids::Get().NANITE_DERIVEDDATA_VER).ToString();
		KeySuffix += *CachedNaniteVersion;
	}
	
	const bool bStoreDuplicatedVertices = GPUSkinCacheStoreDuplicatedVertices();
	KeySuffix += bStoreDuplicatedVertices ? TEXT("_SDV_1") : TEXT("_SDV_0");

#if PLATFORM_CPU_ARM_FAMILY
	// Separate out arm keys as x64 and arm64 clang do not generate the same data for a given
	// input. Add the arm specifically so that a) we avoid rebuilding the current DDC and
	// b) we can remove it once we get arm64 to be consistent.
	KeySuffix.Append(TEXT("_arm64"));
#endif

	static UE::DerivedData::FCacheBucket LegacyBucket(TEXTVIEW("LegacySKELETALMESH"), TEXTVIEW("SkeletalMesh"));
	return FDerivedDataCacheInterface::BuildCacheKey(
		TEXT("SKELETALMESH"),
		*GetSkeletalMeshDerivedDataVersion(),
		*KeySuffix
	);
}

FString USkeletalMesh::GetDerivedDataKey()
{
	// Cache derived data for the running platform.
	ITargetPlatform* RunningPlatform = GetTargetPlatformManagerRef().GetRunningTargetPlatform();
	check(RunningPlatform);

	return BuildDerivedDataKey(RunningPlatform);
}

int32 USkeletalMesh::ValidatePreviewAttachedObjects()
{
	int32 NumBrokenAssets = GetPreviewAttachedAssetContainer().ValidatePreviewAttachedObjects();

	if (NumBrokenAssets > 0)
	{
		MarkPackageDirty();
	}
	return NumBrokenAssets;
}

void USkeletalMesh::RemoveMeshSection(int32 InLodIndex, int32 InSectionIndex)
{
	// Need a mesh resource
	if (GetImportedModel() == nullptr)
	{
		UE_LOG(LogSkeletalMesh, Warning, TEXT("Failed to remove skeletal mesh section, ImportedResource is invalid."));
		return;
	}

	// Need a valid LOD
	if (!GetImportedModel()->LODModels.IsValidIndex(InLodIndex))
	{
		UE_LOG(LogSkeletalMesh, Warning, TEXT("Failed to remove skeletal mesh section, LOD%d does not exist in the mesh"), InLodIndex);
		return;
	}

	FSkeletalMeshLODModel& LodModel = GetImportedModel()->LODModels[InLodIndex];

	// Need a valid section
	if (!LodModel.Sections.IsValidIndex(InSectionIndex))
	{
		UE_LOG(LogSkeletalMesh, Warning, TEXT("Failed to remove skeletal mesh section, Section %d does not exist in LOD%d."), InSectionIndex, InLodIndex);
		return;
	}

	FSkelMeshSection& SectionToDisable = LodModel.Sections[InSectionIndex];
	
	//Get the UserSectionData
	FSkelMeshSourceSectionUserData& UserSectionToDisableData = LodModel.UserSectionsData.FindChecked(SectionToDisable.OriginalDataSectionIndex);

	if(UserSectionToDisableData.HasClothingData())
	{
		// Can't remove this, clothing currently relies on it
		UE_LOG(LogSkeletalMesh, Warning, TEXT("Failed to remove skeletal mesh section, clothing is currently bound to Lod%d Section %d, unbind clothing before removal."), InLodIndex, InSectionIndex);
		return;
	}

	if (!UserSectionToDisableData.bDisabled || !SectionToDisable.bDisabled)
	{
		//Scope a post edit change
		FScopedSkeletalMeshPostEditChange ScopedPostEditChange(this);
		// Valid to disable, dirty the mesh
		Modify();
		PreEditChange(nullptr);
		//Disable the section
		UserSectionToDisableData.bDisabled = true;
		SectionToDisable.bDisabled = true;
	}
}

#endif // #if WITH_EDITOR

void USkeletalMesh::ReleaseCPUResources()
{
	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	if (SkelMeshRenderData)
	{
		for(int32 Index = 0; Index < SkelMeshRenderData->LODRenderData.Num(); ++Index)
		{
			if (!NeedCPUData(Index))
			{
				SkelMeshRenderData->LODRenderData[Index].ReleaseCPUResources();
			}
		}
	}
}

void USkeletalMesh::CreateBodySetup()
{
	const USkeletalMesh* ConstThis = this;
	if (ConstThis->GetBodySetup() == nullptr)
	{
		UBodySetup* NewBodySetup = NewObject<UBodySetup>(this);
		NewBodySetup->bSharedCookedData = true;
		NewBodySetup->AddToCluster(this);
		SetBodySetup(NewBodySetup);
	}
}

#if WITH_EDITOR
void USkeletalMesh::BuildPhysicsData()
{
	CreateBodySetup();
	const USkeletalMesh* ConstThis = this;
	UBodySetup* LocalBodySetup = ConstThis->GetBodySetup();
	LocalBodySetup->CookedFormatData.FlushData();	//we need to force a re-cook because we're essentially re-creating the bodysetup so that it swaps whether or not it has a trimesh
	LocalBodySetup->InvalidatePhysicsData();
	LocalBodySetup->CreatePhysicsMeshes();
}
#endif

bool USkeletalMesh::ContainsPhysicsTriMeshData(bool InUseAllTriData) const
{
	return GetEnablePerPolyCollision();
}

bool USkeletalMesh::GetTriMeshSizeEstimates(struct FTriMeshCollisionDataEstimates& OutTriMeshEstimates, bool bInUseAllTriData) const
{
#if WITH_EDITORONLY_DATA
	if (GetResourceForRendering() && GetEnablePerPolyCollision())
	{
		OutTriMeshEstimates.VerticeCount = GetResourceForRendering()->LODRenderData[0].GetNumVertices();
	}
#endif // #if WITH_EDITORONLY_DATA

	return true;
}

bool USkeletalMesh::GetPhysicsTriMeshData(FTriMeshCollisionData* CollisionData, bool bInUseAllTriData)
{
#if WITH_EDITORONLY_DATA

	// Fail if no mesh or not per poly collision
	if (!GetResourceForRendering() || !GetEnablePerPolyCollision())
	{
		return false;
	}

	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	const FSkeletalMeshLODRenderData& LODData = SkelMeshRenderData->LODRenderData[0];

	const TArray<int32>* MaterialMapPtr = nullptr;
	if (const FSkeletalMeshLODInfo* LODZeroInfo = GetLODInfo(0))
	{
		MaterialMapPtr = &LODZeroInfo->LODMaterialMap;
	}
	// Copy all verts into collision vertex buffer.
	CollisionData->Vertices.Empty();
	CollisionData->Vertices.AddUninitialized(LODData.GetNumVertices());

	for (uint32 VertIdx = 0; VertIdx < LODData.GetNumVertices(); ++VertIdx)
	{
		CollisionData->Vertices[VertIdx] = LODData.StaticVertexBuffers.PositionVertexBuffer.VertexPosition(VertIdx);
	}

	{
		// Copy indices into collision index buffer
		const FMultiSizeIndexContainer& IndexBufferContainer = LODData.MultiSizeIndexContainer;

		TArray<uint32> Indices;
		IndexBufferContainer.GetIndexBuffer(Indices);

		const uint32 NumTris = Indices.Num() / 3;
		CollisionData->Indices.Empty();
		CollisionData->Indices.Reserve(NumTris);

		FTriIndices TriIndex;
		for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); ++SectionIndex)
		{
			const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
			const uint32 OnePastLastIndex = Section.BaseIndex + Section.NumTriangles * 3;
			uint16 MaterialIndex = Section.MaterialIndex;
			if (MaterialMapPtr)
			{
				if (MaterialMapPtr->IsValidIndex(SectionIndex))
				{
					const uint16 RemapMaterialIndex = static_cast<uint16>((*MaterialMapPtr)[SectionIndex]);
					if (GetMaterials().IsValidIndex(RemapMaterialIndex))
					{
						MaterialIndex = RemapMaterialIndex;
					}
				}
			}

			for (uint32 i = Section.BaseIndex; i < OnePastLastIndex; i += 3)
			{
				TriIndex.v0 = Indices[i];
				TriIndex.v1 = Indices[i + 1];
				TriIndex.v2 = Indices[i + 2];

				CollisionData->Indices.Add(TriIndex);
				CollisionData->MaterialIndices.Add(MaterialIndex);
			}
		}
	}

	CollisionData->bFlipNormals = true;
	CollisionData->bDeformableMesh = true;

	// We only have a valid TriMesh if the CollisionData has vertices AND indices. For meshes with disabled section collision, it
	// can happen that the indices will be empty, in which case we do not want to consider that as valid trimesh data
	return CollisionData->Vertices.Num() > 0 && CollisionData->Indices.Num() > 0;
#else // #if WITH_EDITORONLY_DATA
	return false;
#endif // #if WITH_EDITORONLY_DATA
}

void USkeletalMesh::AddAssetUserData( UAssetUserData* InUserData)
{
	if (InUserData != NULL)
	{
		RemoveUserDataOfClass(InUserData->GetClass());
		AssetUserData.Add(InUserData);
	}
}

UAssetUserData* USkeletalMesh::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	const TArray<UAssetUserData*>* ArrayPtr = GetAssetUserDataArray();
	for (int32 DataIdx = 0; DataIdx < ArrayPtr->Num(); DataIdx++)
	{
		UAssetUserData* Datum = (*ArrayPtr)[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return NULL;
}

void USkeletalMesh::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIdx);
			return;
		}
	}
#if WITH_EDITOR
	for (int32 DataIdx = 0; DataIdx < AssetUserDataEditorOnly.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserDataEditorOnly[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			AssetUserDataEditorOnly.RemoveAt(DataIdx);
			return;
		}
	}
#endif
}

const TArray<UAssetUserData*>* USkeletalMesh::GetAssetUserDataArray() const
{
#if WITH_EDITOR
	if (IsRunningCookCommandlet())
	{
		return &ToRawPtrTArrayUnsafe(AssetUserData);
	}
	else
	{
		static thread_local TArray<TObjectPtr<UAssetUserData>> CachedAssetUserData;
		CachedAssetUserData.Reset();
		CachedAssetUserData.Append(AssetUserData);
		CachedAssetUserData.Append(AssetUserDataEditorOnly);
		return &ToRawPtrTArrayUnsafe(CachedAssetUserData);
	}
#else
	return &ToRawPtrTArrayUnsafe(AssetUserData);
#endif
}

////// SKELETAL MESH THUMBNAIL SUPPORT ////////

/** 
 * Returns a one line description of an object for viewing in the thumbnail view of the generic browser
 */
FString USkeletalMesh::GetDesc()
{
	FString DescString;

	FSkeletalMeshRenderData* Resource = GetResourceForRendering();
	if (Resource)
	{
		check(Resource->LODRenderData.Num() > 0);
		DescString = FString::Printf(TEXT("%d Triangles, %d Bones"), Resource->LODRenderData[0].GetTotalFaces(), GetRefSkeleton().GetRawBoneNum());
	}
	return DescString;
}

bool USkeletalMesh::IsSectionUsingCloth(int32 InSectionIndex, bool bCheckCorrespondingSections) const
{
	FSkeletalMeshRenderData* SkelMeshRenderData = GetResourceForRendering();
	if(SkelMeshRenderData)
	{
		for (FSkeletalMeshLODRenderData& LodData : SkelMeshRenderData->LODRenderData)
		{
			if(LodData.RenderSections.IsValidIndex(InSectionIndex))
			{
				FSkelMeshRenderSection* SectionToCheck = &LodData.RenderSections[InSectionIndex];
				return SectionToCheck->HasClothingData();
			}
		}
	}

	return false;
}

#if WITH_EDITOR
void USkeletalMesh::AddBoneToReductionSetting(int32 LODIndex, const TArray<FName>& BoneNames)
{
	if (FSkeletalMeshLODInfo* MeshLODInfo = GetLODInfo(LODIndex))
	{
		for (const FName& BoneName : BoneNames)
		{
			MeshLODInfo->BonesToRemove.AddUnique(BoneName);
		}
	}
}
void USkeletalMesh::AddBoneToReductionSetting(int32 LODIndex, FName BoneName)
{
	if (FSkeletalMeshLODInfo* MeshLODInfo = GetLODInfo(LODIndex))
	{
		MeshLODInfo->BonesToRemove.AddUnique(BoneName);
	}
}
#endif // WITH_EDITOR

#if WITH_EDITORONLY_DATA
void USkeletalMesh::ConvertLegacyLODScreenSize()
{
	if (GetLODNum() == 1)
	{
		// Only one LOD
		GetLODInfo(0)->ScreenSize = 1.0f;
	}
	else
	{
		// Use 1080p, 90 degree FOV as a default, as this should not cause runtime regressions in the common case.
		// LODs will appear different in Persona, however.
		const float HalfFOV = UE_PI * 0.25f;
		const float ScreenWidth = 1920.0f;
		const float ScreenHeight = 1080.0f;
		const FPerspectiveMatrix ProjMatrix(HalfFOV, ScreenWidth, ScreenHeight, 1.0f);
		FBoxSphereBounds Bounds = GetBounds();

		// Multiple models, we should have LOD screen area data.
		for (int32 LODIndex = 0; LODIndex < GetLODNum(); ++LODIndex)
		{
			FSkeletalMeshLODInfo& LODInfoEntry = *GetLODInfo(LODIndex);

			if (GetRequiresLODScreenSizeConversion())
			{
				if (LODInfoEntry.ScreenSize.Default == 0.0f)
				{
					LODInfoEntry.ScreenSize.Default = 1.0f;
				}
				else
				{
					// legacy screen size was scaled by a fixed constant of 320.0f, so its kinda arbitrary. Convert back to distance based metric first.
					const float ScreenDepth = FMath::Max(ScreenWidth / 2.0f * ProjMatrix.M[0][0], ScreenHeight / 2.0f * ProjMatrix.M[1][1]) * Bounds.SphereRadius / (LODInfoEntry.ScreenSize.Default * 320.0f);

					// Now convert using the query function
					LODInfoEntry.ScreenSize.Default = ComputeBoundsScreenSize(FVector::ZeroVector, Bounds.SphereRadius, FVector(0.0f, 0.0f, ScreenDepth), ProjMatrix);
				}
			}

			if (GetRequiresLODHysteresisConversion())
			{
				if (LODInfoEntry.LODHysteresis != 0.0f)
				{
					// Also convert the hysteresis as if it was a screen size topo
					const float ScreenHysteresisDepth = FMath::Max(ScreenWidth / 2.0f * ProjMatrix.M[0][0], ScreenHeight / 2.0f * ProjMatrix.M[1][1]) * Bounds.SphereRadius / (LODInfoEntry.LODHysteresis * 320.0f);
					LODInfoEntry.LODHysteresis = ComputeBoundsScreenSize(FVector::ZeroVector, Bounds.SphereRadius, FVector(0.0f, 0.0f, ScreenHysteresisDepth), ProjMatrix);
				}
			}
		}
	}
}
#endif

class UNodeMappingContainer* USkeletalMesh::GetNodeMappingContainer(class UBlueprint* SourceAsset) const
{
	const TArray<UNodeMappingContainer*>& LocalNodeMappingData = GetNodeMappingData();
	for (int32 Index = 0; Index < LocalNodeMappingData.Num(); ++Index)
	{
		UNodeMappingContainer* Iter = LocalNodeMappingData[Index];
		if (Iter && Iter->GetSourceAssetSoftObjectPtr() == TSoftObjectPtr<UObject>(SourceAsset))
		{
			return Iter;
		}
	}

	return nullptr;
}

FSkeletalMeshLODInfo* USkeletalMesh::GetLODInfo(int32 Index)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return LODInfo.IsValidIndex(Index) ? &LODInfo[Index] : nullptr;  
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}
	
const FSkeletalMeshLODInfo* USkeletalMesh::GetLODInfo(int32 Index) const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return LODInfo.IsValidIndex(Index) ? &LODInfo[Index] : nullptr;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#if WITH_EDITOR
FSimpleMulticastDelegate& USkeletalMesh::GetOnVertexAttributesArrayChanged()
{
	return OnVertexAttributesArrayChanged;
}
#endif

const UAnimSequence* USkeletalMesh::GetBakePose(int32 LODIndex) const
{
	const FSkeletalMeshLODInfo* LOD = GetLODInfo(LODIndex);
	if (LOD)
	{
		if (LOD->BakePoseOverride && GetSkeleton() && GetSkeleton() == LOD->BakePoseOverride->GetSkeleton())
		{
			return LOD->BakePoseOverride;
		}

		// we make sure bake pose uses same skeleton
		if (LOD->BakePose && GetSkeleton() && GetSkeleton() == LOD->BakePose->GetSkeleton())
		{
			return LOD->BakePose;
		}
	}

	return nullptr;
}

const USkeletalMeshLODSettings* USkeletalMesh::GetDefaultLODSetting() const
{ 
#if WITH_EDITORONLY_DATA
	if (GetLODSettings())
	{
		return GetLODSettings();
	}
#endif // WITH_EDITORONLY_DATA

	return GetDefault<USkeletalMeshLODSettings>();
}

bool USkeletalMesh::IsValidLODIndex(int32 Index) const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return LODInfo.IsValidIndex(Index);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}
/* 
	* Returns total number of LOD. USkinnedAsset interface.
	*/
int32 USkeletalMesh::GetLODNum() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo, ESkinnedAssetAsyncPropertyLockType::ReadOnly);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	return LODInfo.Num();
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

int32 USkeletalMesh::GetNumNaniteVertices() const
{
	int32 NumVertices = 0;
	if (HasValidNaniteData())
	{
		const Nanite::FResources& Resources = *GetResourceForRendering()->NaniteResourcesPtr.Get();
		if (Resources.RootData.Num() > 0)
		{
			NumVertices = Resources.NumInputVertices;
		}
	}
	return NumVertices;
}

int32 USkeletalMesh::GetNumNaniteTriangles() const
{
	int32 NumTriangles = 0;
	if (HasValidNaniteData())
	{
		const Nanite::FResources& Resources = *GetResourceForRendering()->NaniteResourcesPtr.Get();
		if (Resources.RootData.Num() > 0)
		{
			NumTriangles = Resources.NumInputTriangles;
		}
	}
	return NumTriangles;
}

bool USkeletalMesh::IsMaterialUsed(int32 MaterialIndex) const
{
	if (GIsEditor || !CVarSkeletalMeshLODMaterialReference.GetValueOnAnyThread())
	{
		return true;
	}

	const FSkeletalMeshRenderData* RenderData = GetSkeletalMeshRenderData();

	if (RenderData)
	{
		for (int32 LODIndex = 0; LODIndex < RenderData->LODRenderData.Num(); ++LODIndex)
		{
			const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[LODIndex];

			if (LODData.BuffersSize > 0)
			{
				const TArray<int32>& RemappedMaterialIndices = GetLODInfo(LODIndex)->LODMaterialMap;

				for (int32 SectionIndex = 0; SectionIndex < LODData.RenderSections.Num(); ++SectionIndex)
				{
					const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
					const int32 UsedMaterialIndex =
						SectionIndex < RemappedMaterialIndices.Num() && GetMaterials().IsValidIndex(RemappedMaterialIndices[SectionIndex]) ?
						RemappedMaterialIndices[SectionIndex] :
						Section.MaterialIndex;
					
					if (UsedMaterialIndex == MaterialIndex)
					{
						return true;
					}
				}
			}
		}
	}

	return false;
}


void USkeletalMesh::AddSkinWeightProfile(const FSkinWeightProfileInfo& Profile) 
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::SkinWeightProfiles); 
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	SkinWeightProfiles.Add(Profile); 
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}


void USkeletalMesh::ReleaseSkinWeightProfileResources()
{
	// This assumes that skin weights buffers are not used anywhere
	if (FSkeletalMeshRenderData* RenderData = GetResourceForRendering())
	{
		for (FSkeletalMeshLODRenderData& LODData : RenderData->LODRenderData)
		{
			LODData.SkinWeightProfilesData.ReleaseResources();
		}
	}
}

FSkeletalMeshLODInfo& USkeletalMesh::AddLODInfo()
{
	const int32 NewIndex = AddLODInfo({});

	const USkeletalMeshLODSettings* DefaultSetting = GetDefaultLODSetting();
	// if failed to get setting, that means, we don't have proper setting 
	// in that case, use last index setting
	FSkeletalMeshLODInfo& NewLODInfo = *GetLODInfo(NewIndex);
	if (!DefaultSetting->SetLODSettingsToMesh(this, NewIndex))
	{
		if (NewIndex > 0)
		{
			// copy previous copy
			const int32 LastIndex = NewIndex - 1;
			const FSkeletalMeshLODInfo& LastLODInfo = *GetLODInfo(LastIndex);
			NewLODInfo.ScreenSize.Default = LastLODInfo.ScreenSize.Default * 0.5f;
			NewLODInfo.LODHysteresis = LastLODInfo.LODHysteresis;
			NewLODInfo.BakePose = LastLODInfo.BakePose;
			NewLODInfo.BakePoseOverride = LastLODInfo.BakePoseOverride;
			NewLODInfo.BonesToRemove = LastLODInfo.BonesToRemove;
			NewLODInfo.BonesToPrioritize = LastLODInfo.BonesToPrioritize;
			NewLODInfo.SectionsToPrioritize = LastLODInfo.SectionsToPrioritize;
			// now find reduction setting
			for (int32 SubLOD = LastIndex; SubLOD >= 0; --SubLOD)
			{
				const FSkeletalMeshLODInfo& SubLODInfo = *GetLODInfo(SubLOD);
				if (SubLODInfo.bHasBeenSimplified)
				{
					// copy from previous index of LOD info reduction setting
					// this may not match with previous copy - as we're only looking for simplified version
					NewLODInfo.ReductionSettings = SubLODInfo.ReductionSettings;
					// and make it 50 % of that
					NewLODInfo.ReductionSettings.NumOfTrianglesPercentage = FMath::Clamp(NewLODInfo.ReductionSettings.NumOfTrianglesPercentage * 0.5f, 0.f, 1.f);
					// increase maxdeviation, 1.5 is random number
					NewLODInfo.ReductionSettings.MaxDeviationPercentage = FMath::Clamp(NewLODInfo.ReductionSettings.MaxDeviationPercentage * 1.5f, 0.f, 1.f);
					break;
				}
			}

		}
		// if this is the first LOD, then just use default setting of the struct
	}

	return NewLODInfo;
}

int32 USkeletalMesh::AddLODInfo(const FSkeletalMeshLODInfo& NewLODInfo) 
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo|ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	int32 NewIndex = LODInfo.Add(NewLODInfo);
#if WITH_EDITOR
	SetNumSourceModelsInternal(LODInfo.Num());
#endif
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	
	return NewIndex;
}

void USkeletalMesh::RemoveLODInfo(int32 Index)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo|ESkeletalMeshAsyncProperties::SourceModels|ESkeletalMeshAsyncProperties::ImportedModel);
	
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (LODInfo.IsValidIndex(Index))
	{
#if WITH_EDITOR
		RemoveSourceModelInternal(Index);
		
		if (ImportedModel->InlineReductionCacheDatas.IsValidIndex(Index))
		{
			ImportedModel->InlineReductionCacheDatas.RemoveAt(Index);
		}
#endif // WITH_EDITOR
		LODInfo.RemoveAt(Index);
	}
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::ResetLODInfo()
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODInfo|ESkeletalMeshAsyncProperties::SourceModels);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	
	LODInfo.Reset();
#if WITH_EDITOR
	SetNumSourceModelsInternal(0);
#endif
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

#if WITH_EDITOR
bool USkeletalMesh::GetEnableLODStreaming(const ITargetPlatform* TargetPlatform) const
{
	if (NeverStream)
	{
		return false;
	}

	static auto* VarMeshStreaming = IConsoleManager::Get().FindConsoleVariable(TEXT("r.MeshStreaming"));
	if (VarMeshStreaming && VarMeshStreaming->GetInt() == 0)
	{
		return false;

	}

	check(TargetPlatform);
	// Check whether the target platforms support LOD streaming. 
	// Even if it does, disable streaming if it has editor only data since most tools don't support mesh streaming.
	if (!TargetPlatform->SupportsFeature(ETargetPlatformFeatures::MeshLODStreaming) || TargetPlatform->HasEditorOnlyData())
	{
		return false;
	}

	if (GetOverrideLODStreamingSettings())
	{
		return GetSupportLODStreaming().GetValueForPlatform(*TargetPlatform->IniPlatformName());
	}
	else
	{
		return GetDefault<URendererSettings>()->bStreamSkeletalMeshLODs.GetValueForPlatform(*TargetPlatform->IniPlatformName());
	}
}

int32 USkeletalMesh::GetMaxNumStreamedLODs(const ITargetPlatform* TargetPlatform) const
{
	check(TargetPlatform);
	if (GetOverrideLODStreamingSettings())
	{
		return GetMaxNumStreamedLODs().GetValueForPlatform(*TargetPlatform->IniPlatformName());
	}
	else
	{
		return MAX_MESH_LOD_COUNT;
	}
}

int32 USkeletalMesh::GetMaxNumOptionalLODs(const ITargetPlatform* TargetPlatform) const
{
	check(TargetPlatform);
	if (GetOverrideLODStreamingSettings())
	{
		return GetMaxNumOptionalLODs().GetValueForPlatform(*TargetPlatform->IniPlatformName()) <= 0 ? 0 : MAX_MESH_LOD_COUNT;
	}
	else
	{
		return GetDefault<URendererSettings>()->bDiscardSkeletalMeshOptionalLODs.GetValueForPlatform(*TargetPlatform->IniPlatformName()) ? 0 : MAX_MESH_LOD_COUNT;
	}
}

void USkeletalMesh::BuildLODModel(FSkeletalMeshRenderData& RenderData, const ITargetPlatform* TargetPlatform, int32 LODIndex)
{
	FSkeletalMeshModel* SkelMeshModel = GetImportedModel();
	check(SkelMeshModel);

	FSkeletalMeshLODInfo* LODInfoPtr = GetLODInfo(LODIndex);
	check(LODInfoPtr);

	//We want to avoid building a LOD if the LOD was generated from a previous LODIndex.
	const bool bIsGeneratedLodNotInline = (LODInfoPtr->bHasBeenSimplified && IsReductionActive(LODIndex) && GetReductionSettings(LODIndex).BaseLOD < LODIndex);

	//Build the source model before the render data, if we are a purely generated LOD we do not need to be build
	IMeshBuilderModule& MeshBuilderModule = IMeshBuilderModule::GetForPlatform(TargetPlatform);
	if (!bIsGeneratedLodNotInline && HasMeshDescription(LODIndex))
	{
		LODInfoPtr->bHasBeenSimplified = false;
		constexpr bool bRegenDepLODs = true;
		const FSkeletalMeshBuildParameters BuildParameters(this, TargetPlatform, LODIndex, bRegenDepLODs);
		MeshBuilderModule.BuildSkeletalMesh(RenderData, BuildParameters);
	}
	else
	{
		//We need to synchronize when we are generated mesh or if we have load an old asset that was not re-imported
		SkelMeshModel->LODModels[LODIndex].SyncronizeUserSectionsDataArray();
	}
}
#endif

void USkeletalMesh::SetLODSettings(USkeletalMeshLODSettings* InLODSettings)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::LODSettings);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
#if WITH_EDITORONLY_DATA
	LODSettings = InLODSettings;
	if (LODSettings)
	{
		LODSettings->SetLODSettingsToMesh(this);
	}
#endif // WITH_EDITORONLY_DATA
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void USkeletalMesh::SetDefaultAnimatingRig(TSoftObjectPtr<UObject> InAnimatingRig)
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::DefaultAnimationRig);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
#if WITH_EDITORONLY_DATA
	DefaultAnimatingRig = InAnimatingRig;
#endif // WITH_EDITORONLY_DATA
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

TSoftObjectPtr<UObject> USkeletalMesh::GetDefaultAnimatingRig() const
{
	WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties::DefaultAnimationRig);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
#if WITH_EDITORONLY_DATA
	return DefaultAnimatingRig;
#else // WITH_EDITORONLY_DATA
	return nullptr;
#endif // WITH_EDITORONLY_DATA
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

bool USkeletalMesh::GetHasBeenSimplified() const
{
	for (int32 LODIndex = 0, LODCount = GetLODNum(); LODIndex < LODCount; LODIndex++)
	{
		if (GetLODInfo(LODIndex)->bHasBeenSimplified)
		{
			return true;
		}
	}
	return false;
}

void USkeletalMesh::GetMappableNodeData(TArray<FName>& OutNames, TArray<FNodeItem>& OutNodeItems) const
{
	

	const int32 NumJoint = GetRefSkeleton().GetNum();
	// allocate buffer
	OutNames.Reset(NumJoint);
	OutNodeItems.Reset(NumJoint);

	TArray<FTransform> ComponentSpaceRefPose;
	FAnimationRuntime::FillUpComponentSpaceTransforms(GetRefSkeleton(), GetRefSkeleton().GetRefBonePose(), ComponentSpaceRefPose);

	if (NumJoint > 0)
	{
		OutNames.AddDefaulted(NumJoint);
		OutNodeItems.AddDefaulted(NumJoint);

		const TArray<FMeshBoneInfo> MeshBoneInfo = GetRefSkeleton().GetRefBoneInfo();
		for (int32 NodeIndex = 0; NodeIndex < NumJoint; ++NodeIndex)
		{
			OutNames[NodeIndex] = MeshBoneInfo[NodeIndex].Name;
			if (MeshBoneInfo[NodeIndex].ParentIndex != INDEX_NONE)
			{
				OutNodeItems[NodeIndex] = FNodeItem(MeshBoneInfo[MeshBoneInfo[NodeIndex].ParentIndex].Name, ComponentSpaceRefPose[NodeIndex]);
			}
			else
			{
				OutNodeItems[NodeIndex] = FNodeItem(NAME_None, ComponentSpaceRefPose[NodeIndex]);
			}
		}
	}
}

#if WITH_EDITORONLY_DATA
FText USkeletalMesh::GetSourceFileLabelFromIndex(int32 SourceFileIndex)
{
	int32 RealSourceFileIndex = SourceFileIndex == INDEX_NONE ? 0 : SourceFileIndex;
	return RealSourceFileIndex == 0 ? NSSkeletalMeshSourceFileLabels::GeoAndSkinningText() : RealSourceFileIndex == 1 ? NSSkeletalMeshSourceFileLabels::GeometryText() : NSSkeletalMeshSourceFileLabels::SkinningText();
}

#endif //WITH_EDITORONLY_DATA


TArray<FString> USkeletalMesh::K2_GetAllMorphTargetNames() const
{
	TArray<FString> Names;
	for (UMorphTarget* MorphTarget : GetMorphTargets())
	{
		Names.Add(MorphTarget->GetFName().ToString());
	}
	return Names;
}

int32 USkeletalMesh::GetMinLodIdx(bool bForceLowestLODIdx) const
{
	if (IsMinLodQualityLevelEnable())
	{
		return bForceLowestLODIdx ? GetQualityLevelMinLod().GetLowestValue() : GetQualityLevelMinLod().GetValue(GSkeletalMeshMinLodQualityLevel);
	}
	else
	{
		return GetMinLod().GetValue();
	}
}

int32 USkeletalMesh::GetDefaultMinLod() const
{
	if (IsMinLodQualityLevelEnable())
	{
		return GetQualityLevelMinLod().Default;
	}
	else
	{
		return GetMinLod().Default;
	}
}

void USkeletalMesh::SetMinLodIdx(int32 InMinLOD)
{
	if (IsMinLodQualityLevelEnable())
	{
		SetQualityLevelMinLod(InMinLOD);
	}
	else
	{
		SetMinLod(InMinLOD);
	}
}

bool USkeletalMesh::IsMinLodQualityLevelEnable() const
{
	return (GEngine && GEngine->UseSkeletalMeshMinLODPerQualityLevels);
}

int32 USkeletalMesh::GetPlatformMinLODIdx(const ITargetPlatform* TargetPlatform) const
{
#if WITH_EDITOR
	check(TargetPlatform);

	if (IsMinLodQualityLevelEnable())
	{
		// get all supported quality level from scalability + engine ini files
		return GetQualityLevelMinLod().GetValueForPlatform(TargetPlatform);
	}
	else
	{
		return GetMinLod().GetValueForPlatform(*TargetPlatform->IniPlatformName());
	}
#else
	return 0;
#endif
}

void USkeletalMesh::SetSkinWeightProfilesData(int32 LODIndex, FSkinWeightProfilesData& SkinWeightProfilesData)
{
#if !WITH_EDITOR
	if (GSkinWeightProfilesLoadByDefaultMode == 1)
	{
		// Only allow overriding the base buffer in non-editor builds as it could otherwise be serialized into the asset
		SkinWeightProfilesData.OverrideBaseBufferSkinWeightData(this, LODIndex);
	}
	else
#endif
	if (GSkinWeightProfilesLoadByDefaultMode == 3)
	{
		SkinWeightProfilesData.SetDynamicDefaultSkinWeightProfile(this, LODIndex, true);
	}
}

FSkinWeightProfilesData* USkeletalMesh::GetSkinWeightProfilesData(int32 LODIndex)
{
	FSkeletalMeshRenderData* RenderData = GetSkeletalMeshRenderData();
	if (RenderData && RenderData->LODRenderData.IsValidIndex(LODIndex))
	{
		FSkeletalMeshLODRenderData& LODRenderData = RenderData->LODRenderData[LODIndex];
		return &LODRenderData.SkinWeightProfilesData;
	}
	
	return nullptr;
}

void USkeletalMesh::OnLodStrippingQualityLevelChanged(IConsoleVariable* Variable) {
#if WITH_EDITOR || PLATFORM_DESKTOP
	if (GEngine && GEngine->UseSkeletalMeshMinLODPerQualityLevels)
	{
		for (TObjectIterator<USkeletalMesh> It; It; ++It)
		{
			USkeletalMesh* SkeletalMesh = *It;
			if (SkeletalMesh && SkeletalMesh->GetQualityLevelMinLod().PerQuality.Num() > 0)
			{
				FSkinnedMeshComponentRecreateRenderStateContext Context(SkeletalMesh, false);
			}
		}
	}
#endif
}

void USkeletalMesh::WaitUntilAsyncPropertyReleased(ESkeletalMeshAsyncProperties AsyncProperties, ESkinnedAssetAsyncPropertyLockType LockType) const
{
	// Cast strongly-typed enum to uint64
	WaitUntilAsyncPropertyReleasedInternal((uint64)AsyncProperties, LockType);
}

FString USkeletalMesh::GetAsyncPropertyName(uint64 Property) const
{
	return StaticEnum<ESkeletalMeshAsyncProperties>()->GetValueOrBitfieldAsString(Property);
}

int32 USkeletalMesh::GetPostProcessAnimGraphLODThreshold() const
{
	return PostProcessAnimBPLODThreshold;
}

void USkeletalMesh::SetPostProcessAnimGraphLODThreshold(int32 LODThreshold)
{
	PostProcessAnimBPLODThreshold = LODThreshold;
}

bool USkeletalMesh::ShouldEvaluatePostProcessAnimGraph(int32 LODLevel) const
{
	return ((PostProcessAnimBPLODThreshold == INDEX_NONE) || (LODLevel <= PostProcessAnimBPLODThreshold));
}

/*-----------------------------------------------------------------------------
USkeletalMeshSocket
-----------------------------------------------------------------------------*/

void USkeletalMeshSocket::InitializeSocketFromLocation(const class USkeletalMeshComponent* SkelComp, FVector WorldLocation, FVector WorldNormal)
{
	if (ensureAsRuntimeWarning(SkelComp))
	{
		BoneName = SkelComp->FindClosestBone(WorldLocation);
		if (BoneName != NAME_None)
		{
			SkelComp->TransformToBoneSpace(BoneName, WorldLocation, WorldNormal.Rotation(), RelativeLocation, RelativeRotation);
		}
	}
}


#if WITH_EDITOR
void USkeletalMeshSocket::SetSocketParent(USkeletalMesh* InSkeletalMesh, FName InBoneName)
{
	if (!InSkeletalMesh)
	{
		UE_LOG(LogSkeletalMesh, Error, TEXT("SetSocketParent: No skeletal mesh asset given."));
		return;
	}

	if (InBoneName == BoneName)
	{
		// Nothing more to do.
		return;
	}

	// The socket can be owned by either the skeletal mesh or its associated skeleton. We need to ensure that the bone in
	// question exists on the correct owner so we don't end up with a situation where a skeleton socket refers to a bone that
	// only exists on the skeletal mesh.
	if (GetOuter() == InSkeletalMesh)
	{
		if (InSkeletalMesh->GetRefSkeleton().FindBoneIndex(InBoneName) == INDEX_NONE)
		{
			UE_LOG(LogSkeletalMesh, Error, TEXT("SetSocketParent: The owning skeletal asset (%s) does not contain any bone named '%s'."),
				*InSkeletalMesh->GetName(), *BoneName.ToString());
			return;
		}
	}
	else if (GetOuter() == InSkeletalMesh->GetSkeleton())
	{
		const USkeleton* Skeleton = InSkeletalMesh->GetSkeleton();
		if (Skeleton->GetReferenceSkeleton().FindBoneIndex(InBoneName) == INDEX_NONE)
		{
			UE_LOG(LogSkeletalMesh, Error, TEXT("SetSocketParent: The owning skeleton (%s) does not contain any bone named '%s'."),
				*InSkeletalMesh->GetName(), *BoneName.ToString());
			return;
		}
	}
	else
	{
		UE_LOG(LogSkeletalMesh, Error, TEXT("SetSocketParent: Neither the skeletal asset (%s) nor its skeleton are the owners of this socket (%s)."),
			*InSkeletalMesh->GetName(), *SocketName.ToString());
	}
	
	// Make sure we can undo this change.
	SetFlags(RF_Transactional);
	Modify();

	BoneName = InBoneName;

	// Let the world know.
	ChangedEvent.Broadcast(this, GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(USkeletalMeshSocket, BoneName)));
}
#endif


FVector USkeletalMeshSocket::GetSocketLocation(const class USkeletalMeshComponent* SkelComp) const
{
	if (ensureAsRuntimeWarning(SkelComp))
	{
		FMatrix SocketMatrix;
		if (GetSocketMatrix(SocketMatrix, SkelComp))
		{
			return SocketMatrix.GetOrigin();
		}

		// Fall back to MeshComp origin, so it's visible in case of failure.
		return SkelComp->GetComponentLocation();
	}
	return FVector(0.f);
}

bool USkeletalMeshSocket::GetSocketMatrix(FMatrix& OutMatrix, const class USkeletalMeshComponent* SkelComp) const
{
	const int32 BoneIndex = SkelComp ? SkelComp->GetBoneIndex(BoneName) : INDEX_NONE;
	if(BoneIndex != INDEX_NONE)
	{
		FMatrix BoneMatrix = SkelComp->GetBoneMatrix(BoneIndex);
		FScaleRotationTranslationMatrix RelSocketMatrix( RelativeScale, RelativeRotation, RelativeLocation );
		OutMatrix = RelSocketMatrix * BoneMatrix;
		return true;
	}

	return false;
}

FTransform USkeletalMeshSocket::GetSocketLocalTransform() const
{
	return FTransform(RelativeRotation, RelativeLocation, RelativeScale);
}

#if WITH_EDITOR
void USkeletalMeshSocket::SetSocketLocalTransform(FTransform InTransform)
{
	// Make sure we can undo this change.
	SetFlags(RF_Transactional);
	Modify();
	
	RelativeLocation = InTransform.GetLocation();
	RelativeRotation = InTransform.GetRotation().Rotator();
	RelativeScale = InTransform.GetScale3D();
}
#endif


FTransform USkeletalMeshSocket::GetSocketTransform(const class USkeletalMeshComponent* SkelComp) const
{
	FTransform OutTM;

	const int32 BoneIndex = SkelComp ? SkelComp->GetBoneIndex(BoneName) : INDEX_NONE;
	if(BoneIndex != INDEX_NONE)
	{
		FTransform BoneTM = SkelComp->GetBoneTransform(BoneIndex);
		FTransform RelSocketTM( RelativeRotation, RelativeLocation, RelativeScale );
		OutTM = RelSocketTM * BoneTM;
	}

	return OutTM;
}

bool USkeletalMeshSocket::GetSocketMatrixWithOffset(FMatrix& OutMatrix, class USkeletalMeshComponent* SkelComp, const FVector& InOffset, const FRotator& InRotation) const
{
	const int32 BoneIndex = SkelComp ? SkelComp->GetBoneIndex(BoneName) : INDEX_NONE;
	if(BoneIndex != INDEX_NONE)
	{
		FMatrix BoneMatrix = SkelComp->GetBoneMatrix(BoneIndex);
		FScaleRotationTranslationMatrix RelSocketMatrix(RelativeScale, RelativeRotation, RelativeLocation);
		FRotationTranslationMatrix RelOffsetMatrix(InRotation, InOffset);
		OutMatrix = RelOffsetMatrix * RelSocketMatrix * BoneMatrix;
		return true;
	}

	return false;
}


bool USkeletalMeshSocket::GetSocketPositionWithOffset(FVector& OutPosition, class USkeletalMeshComponent* SkelComp, const FVector& InOffset, const FRotator& InRotation) const
{
	const int32 BoneIndex = SkelComp ? SkelComp->GetBoneIndex(BoneName) : INDEX_NONE;
	if(BoneIndex != INDEX_NONE)
	{
		FMatrix BoneMatrix = SkelComp->GetBoneMatrix(BoneIndex);
		FScaleRotationTranslationMatrix RelSocketMatrix(RelativeScale, RelativeRotation, RelativeLocation);
		FRotationTranslationMatrix RelOffsetMatrix(InRotation, InOffset);
		FMatrix SocketMatrix = RelOffsetMatrix * RelSocketMatrix * BoneMatrix;
		OutPosition = SocketMatrix.GetOrigin();
		return true;
	}

	return false;
}

/** 
 *	Utility to associate an actor with a socket
 *	
 *	@param	Actor			The actor to attach to the socket
 *	@param	SkelComp		The skeletal mesh component that the socket comes from
 *
 *	@return	bool			true if successful, false if not
 */
bool USkeletalMeshSocket::AttachActor(AActor* Actor, class USkeletalMeshComponent* SkelComp) const
{
	bool bAttached = false;
	if (ensureAlways(SkelComp))
	{
		// Don't support attaching to own socket
		if ((Actor != SkelComp->GetOwner()) && Actor->GetRootComponent())
		{
			FMatrix SocketTM;
			if (GetSocketMatrix(SocketTM, SkelComp))
			{
				Actor->Modify();

				Actor->SetActorLocation(SocketTM.GetOrigin(), false);
				Actor->SetActorRotation(SocketTM.Rotator());
				Actor->GetRootComponent()->AttachToComponent(SkelComp, FAttachmentTransformRules::SnapToTargetNotIncludingScale, SocketName);

	#if WITH_EDITOR
				if (GIsEditor)
				{
					Actor->PreEditChange(NULL);
					Actor->PostEditChange();
				}
	#endif // WITH_EDITOR

				bAttached = true;
			}
		}
	}
	return bAttached;
}

#if WITH_EDITOR
void USkeletalMeshSocket::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property)
	{
		ChangedEvent.Broadcast(this, PropertyChangedEvent.MemberProperty);
	}
}

void USkeletalMeshSocket::CopyFrom(const class USkeletalMeshSocket* OtherSocket)
{
	if (OtherSocket)
	{
		SocketName = OtherSocket->SocketName;
		BoneName = OtherSocket->BoneName;
		RelativeLocation = OtherSocket->RelativeLocation;
		RelativeRotation = OtherSocket->RelativeRotation;
		RelativeScale = OtherSocket->RelativeScale;
		bForceAlwaysAnimated = OtherSocket->bForceAlwaysAnimated;
	}
}

#endif

void USkeletalMeshSocket::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FFrameworkObjectVersion::GUID);

	if(Ar.CustomVer(FFrameworkObjectVersion::GUID) < FFrameworkObjectVersion::MeshSocketScaleUtilization)
	{
		// Set the relative scale to 1.0. As it was not used before this should allow existing data
		// to work as expected.
		RelativeScale = FVector(1.0f, 1.0f, 1.0f);
	}
}

//////////////////////////////////////////////////////////////////////////

FVector GetRefVertexLocationTyped(
	const USkeletalMesh* Mesh,
	const FSkelMeshRenderSection& Section,
	const FPositionVertexBuffer& PositionBuffer,
	const FSkinWeightVertexBuffer& SkinWeightVertexBuffer,
	const int32 VertIndex
)
{
	FVector SkinnedPos(0, 0, 0);

	// Do soft skinning for this vertex.
	int32 BufferVertIndex = Section.GetVertexBufferIndex() + VertIndex;
	int32 MaxBoneInfluences = SkinWeightVertexBuffer.GetMaxBoneInfluences();

#if !PLATFORM_LITTLE_ENDIAN
	// uint8[] elements in LOD.VertexBufferGPUSkin have been swapped for VET_UBYTE4 vertex stream use
	for (int32 InfluenceIndex = MAX_INFLUENCES - 1; InfluenceIndex >= MAX_INFLUENCES - MaxBoneInfluences; InfluenceIndex--)
#else
	for (int32 InfluenceIndex = 0; InfluenceIndex < MaxBoneInfluences; InfluenceIndex++)
#endif
	{
		const float	Weight = (float)SkinWeightVertexBuffer.GetBoneWeight(BufferVertIndex, InfluenceIndex) * UE::AnimationCore::InvMaxRawBoneWeightFloat;
		{
			const FMatrix BoneTransformMatrix = FMatrix::Identity;
			SkinnedPos += BoneTransformMatrix.TransformPosition((FVector)PositionBuffer.VertexPosition(BufferVertIndex)) * Weight;
		}
	}

	return SkinnedPos;
}

FVector GetSkeletalMeshRefVertLocation(const USkeletalMesh* Mesh, const FSkeletalMeshLODRenderData& LODData, const FSkinWeightVertexBuffer& SkinWeightVertexBuffer, const int32 VertIndex)
{
	int32 SectionIndex;
	int32 VertIndexInChunk;
	LODData.GetSectionFromVertexIndex(VertIndex, SectionIndex, VertIndexInChunk);
	const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
	return GetRefVertexLocationTyped(Mesh, Section, LODData.StaticVertexBuffers.PositionVertexBuffer, SkinWeightVertexBuffer, VertIndexInChunk);
}

//GetRefTangentBasisTyped
void GetRefTangentBasisTyped(const USkeletalMesh* Mesh, const FSkelMeshRenderSection& Section, const FStaticMeshVertexBuffer& StaticVertexBuffer, const FSkinWeightVertexBuffer& SkinWeightVertexBuffer, const int32 VertIndex, FVector3f& OutTangentX, FVector3f& OutTangentY, FVector3f& OutTangentZ)
{
	OutTangentX = FVector3f::ZeroVector;
	OutTangentY = FVector3f::ZeroVector;
	OutTangentZ = FVector3f::ZeroVector;

	// Do soft skinning for this vertex.
	const int32 BufferVertIndex = Section.GetVertexBufferIndex() + VertIndex;
	const int32 MaxBoneInfluences = SkinWeightVertexBuffer.GetMaxBoneInfluences();

	const FVector3f VertexTangentX = StaticVertexBuffer.VertexTangentX(BufferVertIndex);
	const FVector3f VertexTangentY = StaticVertexBuffer.VertexTangentY(BufferVertIndex);
	const FVector3f VertexTangentZ = StaticVertexBuffer.VertexTangentZ(BufferVertIndex);

#if !PLATFORM_LITTLE_ENDIAN
	// uint8[] elements in LOD.VertexBufferGPUSkin have been swapped for VET_UBYTE4 vertex stream use
	for (int32 InfluenceIndex = MAX_INFLUENCES - 1; InfluenceIndex >= MAX_INFLUENCES - MaxBoneInfluences; InfluenceIndex--)
#else
	for (int32 InfluenceIndex = 0; InfluenceIndex < MaxBoneInfluences; InfluenceIndex++)
#endif
	{
		const float	Weight = (float)SkinWeightVertexBuffer.GetBoneWeight(BufferVertIndex, InfluenceIndex) * UE::AnimationCore::InvMaxRawBoneWeightFloat;
		const FMatrix44f BoneTransformMatrix = FMatrix44f::Identity;
		OutTangentX += BoneTransformMatrix.TransformVector(VertexTangentX) * Weight;
		OutTangentY += BoneTransformMatrix.TransformVector(VertexTangentY) * Weight;
		OutTangentZ += BoneTransformMatrix.TransformVector(VertexTangentZ) * Weight;
	}
}

void GetSkeletalMeshRefTangentBasis(const USkeletalMesh* Mesh, const FSkeletalMeshLODRenderData& LODData, const FSkinWeightVertexBuffer& SkinWeightVertexBuffer, const int32 VertIndex, FVector3f& OutTangentX, FVector3f& OutTangentY, FVector3f& OutTangentZ)
{
	int32 SectionIndex;
	int32 VertIndexInChunk;
	LODData.GetSectionFromVertexIndex(VertIndex, SectionIndex, VertIndexInChunk);
	const FSkelMeshRenderSection& Section = LODData.RenderSections[SectionIndex];
	GetRefTangentBasisTyped(Mesh, Section, LODData.StaticVertexBuffers.StaticMeshVertexBuffer, SkinWeightVertexBuffer, VertIndexInChunk, OutTangentX, OutTangentY, OutTangentZ);
}

#undef LOCTEXT_NAMESPACE
