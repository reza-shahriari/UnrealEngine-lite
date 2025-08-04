// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once 

#include "ChaosClothAsset/ImportFilePath.h"
#include "Dataflow/DataflowNode.h"
#include "Dataflow/DataflowFunctionProperty.h"
#include "GeometryCollection/ManagedArrayCollection.h"

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5
#include "Misc/SecureHash.h"
#endif
#include "USDImportNode_v2.generated.h"

#if UE_ENABLE_INCLUDE_ORDER_DEPRECATED_IN_5_5
namespace Dataflow = UE::Dataflow;
#else
namespace UE_DEPRECATED(5.5, "Use UE::Dataflow instead.") Dataflow {}
#endif

class UStaticMesh;

/** Support struct for storage of the extra data that doesn't fit inside a static mesh. */
USTRUCT()
struct FChaosClothAssetUsdClothData final
{
	GENERATED_USTRUCT_BODY()

	TMap<FName, TSet<int32>> SimPatterns;
	TMap<FName, TSet<FIntVector2>> Sewings;
	TMap<FName, TSet<int32>> RenderPatterns;
	TMap<FName, TSet<FName>> RenderToSimPatterns;

	bool Serialize(FArchive& Ar)
	{
		Ar << SimPatterns;
		Ar << Sewings;
		Ar << RenderPatterns;
		Ar << RenderToSimPatterns;
		return true;
	}
};

template<> struct TStructOpsTypeTraits<FChaosClothAssetUsdClothData> : public TStructOpsTypeTraitsBase2<FChaosClothAssetUsdClothData>
{
	enum
	{
		WithSerializer = true,
	};
};

/** Import a USD file from a third party garment construction software. */
USTRUCT(meta = (DataflowCloth))
struct FChaosClothAssetUSDImportNode_v2 : public FDataflowNode
{
	GENERATED_USTRUCT_BODY()
	DATAFLOW_NODE_DEFINE_INTERNAL(FChaosClothAssetUSDImportNode_v2, "USDImport", "Cloth", "Cloth USD Import")
	DATAFLOW_NODE_RENDER_TYPE("SurfaceRender", FName("FClothCollection"), "Collection")

public:
	UPROPERTY(meta = (DataflowOutput))
	FManagedArrayCollection Collection;

	/** Only import the simulation mesh data from the USD file. */
	UPROPERTY(EditAnywhere, Category = "USD Import")
	bool bImportSimMesh = true;

	/** Only import the render mesh data from the USD file. */
	UPROPERTY(EditAnywhere, Category = "USD Import")
	bool bImportRenderMesh = true;

	/** Importing the render mesh with opacity requires transluscency to be enable in the project settings. */
	UPROPERTY(EditAnywhere, Category = "USD Import", Meta = (EditCondition = "bImportRenderMesh", EditConditionHides))
	bool bImportWithOpacity = false;

	/** Path of the USD file to import. */
	UPROPERTY(EditAnywhere, Category = "USD Import", Meta = (DisplayName = "USD File"))
	FChaosClothAssetImportFilePath UsdFile;

	/** Click on this button to reimport the specified USD file and regenerate the intermediary assets. */
	UPROPERTY(EditAnywhere, Category = "USD Import", Meta = (ButtonImage = "Persona.ReimportAsset", DisplayName = "Reimport USD File"))
	FDataflowFunctionProperty ReimportUsdFile;

	/** The USD import process generates an intermediary simulation static mesh. Click on this button to reimport it without reimporting the USD file. */
	UPROPERTY(EditAnywhere, Category = "USD Import", Meta = (ButtonImage = "Icons.Refresh", EditCondition = "ImportedSimStaticMesh != nullptr && bImportSimMesh", EditConditionHides))
	FDataflowFunctionProperty ReloadSimStaticMesh;

	/** The USD import process generates an intermediary render static mesh. Click on this button to reimport it without reimporting the USD file. */
	UPROPERTY(EditAnywhere, Category = "USD Import", Meta = (ButtonImage = "Icons.Refresh", EditCondition = "ImportedRenderStaticMesh != nullptr && bImportRenderMesh", EditConditionHides))
	FDataflowFunctionProperty ReloadRenderStaticMesh;

	FChaosClothAssetUSDImportNode_v2(const UE::Dataflow::FNodeParameters& InParam, FGuid InGuid = FGuid::NewGuid());

private:
	//~ Begin FDataflowNode interface
	virtual void Evaluate(UE::Dataflow::FContext& Context, const FDataflowOutput* Out) const override;
	virtual void Serialize(FArchive& Archive) override;
	//~ End FDataflowNode interface

	// Clear the imported data properties
	void ResetImport();
	// Import USD data if provided in the original legacy schemaless USD format
	bool ImportUsdFile_Schemaless(const FString& UsdPath, const FString& AssetPath, class FText& OutErrorText);
	// Import USD data if provided with a valid cloth schema
	bool ImportUsdFile(const FString& UsdPath, const FString& AssetPath, class FText& OutErrorText);

	// Import the given static mesh as a simulation mesh into the cloth collection
	bool ImportSimStaticMesh(const TSharedRef<FManagedArrayCollection> ClothCollection, class FText& OutErrorText);
	// Import the given static mesh as a render mesh into the cloth collection
	bool ImportRenderStaticMesh(const TSharedRef<FManagedArrayCollection> ClothCollection, class FText& OutErrorText);

	// Find the two imported static meshes
	void UpdateImportedAssets(const FString& SimMeshName, const FString& RenderMeshName);

	// Find an imported static mesh from the given usd prim path
	TObjectPtr<UStaticMesh> FindImportedStaticMesh(const TArrayView<TObjectPtr<UObject>> ImportedAssetsconst, const FString& MeshPrimPath);

	// Find the asset dependencies for the specified static mesh
	TArray<TObjectPtr<UObject>> GetImportedAssetDependencies(const UObject* StaticMesh);

	/** Content folder where all the USD assets are imported. */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	FString PackagePath;

	/**
	 * The static mesh created from the USD import process used as simulation mesh.
	 * Note that this property can still be empty after successfully importing a simulation mesh depending
	 * on whether the simulation mesh is imported from an older version of USD cloth schema.
	 */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	TObjectPtr<UStaticMesh> ImportedSimStaticMesh;

	/** The UV scale used to import the patterns from the imported static mesh UV coordinates. */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	FVector2f ImportedUVScale = { 1.f, 1.f };

	/** The static mesh created from the USD import process used as render mesh. */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	TObjectPtr<UStaticMesh> ImportedRenderStaticMesh;

	/** List of all the simulation static mesh's dependent assets. This does not include any Engine, or Engine plugin content. */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	TArray<TObjectPtr<UObject>> ImportedSimAssets;

	/** List of all the render static mesh's dependent assets. This does not include any Engine, or Engine plugin content. */
	UPROPERTY(VisibleAnywhere, Category = "USD Import")
	TArray<TObjectPtr<UObject>> ImportedRenderAssets;

	/** Support struct for storage of the extra data that doesn't fit inside a static mesh. */
	UPROPERTY()
	FChaosClothAssetUsdClothData UsdClothData;

	UPROPERTY()
	TArray<TObjectPtr<UObject>> ImportedAssets_DEPRECATED;
};
