// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ImportTestFunctionsBase.h"
#include "SkeletalMeshImportTestFunctions.generated.h"

struct FInterchangeTestFunctionResult;
class USkeletalMesh;


UCLASS()
class INTERCHANGETESTS_API USkeletalMeshImportTestFunctions : public UImportTestFunctionsBase
{
	GENERATED_BODY()

public:

	// UImportTestFunctionsBase interface
	virtual UClass* GetAssociatedAssetType() const override;

	/** Check whether the expected number of skeletal meshes are imported */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckImportedSkeletalMeshCount(const TArray<USkeletalMesh*>& Meshes, int32 ExpectedNumberOfImportedSkeletalMeshes);

	/** Check whether the vertex count in the built render data for the given LOD is as expected */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckRenderVertexCount(USkeletalMesh* Mesh, int32 LodIndex, int32 ExpectedNumberOfRenderVertices);

	/** Check whether the triangle count in the built render data for the given LOD is as expected */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckRenderTriangleCount(USkeletalMesh* Mesh, int32 LodIndex, int32 ExpectedNumberOfRenderTriangles);

	/** Check whether the mesh has the expected number of LODs */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckLodCount(USkeletalMesh* Mesh, int32 ExpectedNumberOfLods);

	/** Check whether the mesh has the expected number of material slots */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckMaterialSlotCount(USkeletalMesh* Mesh, int32 ExpectedNumberOfMaterialSlots);

	/** Check whether the built render data for the given mesh LOD has the expected number of sections */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSectionCount(USkeletalMesh* Mesh, int32 LodIndex, int32 ExpectedNumberOfSections);

	/** Check whether the given section in the built render data for the given LOD has the expected number of triangles */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckTriangleCountInSection(USkeletalMesh* Mesh, int32 LodIndex, int32 SectionIndex, int32 ExpectedNumberOfTriangles);

	/** Check whether the mesh has the expected number of UV channels */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckUVChannelCount(USkeletalMesh* Mesh, int32 LodIndex, int32 ExpectedNumberOfUVChannels);

	/** Check whether the material name for the given section in the render data for the given LOD is as expected */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSectionMaterialName(USkeletalMesh* Mesh, int32 LodIndex, int32 SectionIndex, const FString& ExpectedMaterialName);

	/** Check whether the imported material slot name for the given section in the render data for the given LOD is as expected */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSectionImportedMaterialSlotName(USkeletalMesh* Mesh, int32 LodIndex, int32 SectionIndex, const FString& ExpectedImportedMaterialSlotName);

	/** Check whether the vertex of the given index is at the expected position */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckVertexIndexPosition(USkeletalMesh* Mesh, int32 LodIndex, int32 VertexIndex, const FVector& ExpectedVertexPosition);

	/** Check whether the vertex of the given index is at the expected normal */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckVertexIndexNormal(USkeletalMesh* Mesh, int32 LodIndex, int32 VertexIndex, const FVector& ExpectedVertexNormal);

	/** Check whether the vertex of the given index is at the expected normal */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckVertexIndexColor(USkeletalMesh* Mesh, int32 LodIndex, int32 VertexIndex, const FColor& ExpectedVertexColor);

	/** Check whether the mesh has the expected number of morph targets */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckMorphTargetCount(USkeletalMesh* Mesh, int32 ExpectedNumberOfMorphTargets);

	/** Check whether the imported morph target name is as expected */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckMorphTargetName(USkeletalMesh* Mesh, int32 MorphTargetIndex, const FString& ExpectedMorphTargetName);

	/** Check whether the mesh has the expected number of bones */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckBoneCount(USkeletalMesh* Mesh, int32 ExpectedNumberOfBones);

	/** Check that the bone of the specified index has the expected position */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckBonePosition(USkeletalMesh* Mesh, int32 BoneIndex, const FVector& ExpectedBonePosition);

	/** Check whether the static mesh expected number of sockets were imported */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSocketCount(USkeletalMesh* Mesh, int32 ExpectedSocketCount);

	/** Check whether the static mesh given socket index has the expected name */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSocketName(USkeletalMesh* Mesh, int32 SocketIndex, const FString& ExpectedSocketName);

	/** Check whether the static mesh given socket index has the expected location */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSocketLocation(USkeletalMesh* Mesh, int32 SocketIndex, const FVector& ExpectedSocketLocation);

	/** Check that the specified bone name is skinned with the expected number of vertices */
	UFUNCTION(Exec)
	static FInterchangeTestFunctionResult CheckSkinnedVertexCountForBone(USkeletalMesh* Mesh, const FString& BoneName, bool bTestFirstAlternateProfile, int32 ExpectedSkinnedVertexCount);
};
