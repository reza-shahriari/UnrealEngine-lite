// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetDefinitionDefault.h"
#include "Engine/DataAsset.h"

#include "AssetDefinition_DataAsset.generated.h"

UCLASS()
class ENGINEASSETDEFINITIONS_API UAssetDefinition_DataAsset : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	// UAssetDefinition Implementation
	UAssetDefinition_DataAsset()
	{
		IncludeClassInFilter = EIncludeClassInFilter::Always;
	}
	virtual FText GetAssetDisplayName() const override { return NSLOCTEXT("AssetDefinition", "DataAsset", "Data Asset"); }
	virtual FText GetAssetDisplayName(const FAssetData& AssetData) const override;
	virtual FLinearColor GetAssetColor() const override { return FLinearColor(FColor(201, 29, 85)); }
	virtual TSoftClassPtr<UObject> GetAssetClass() const override { return UDataAsset::StaticClass(); }
	
	virtual EAssetCommandResult PerformAssetDiff(const FAssetDiffArgs& DiffArgs) const override;

	virtual bool CanMerge() const override;
	virtual EAssetCommandResult Merge(const FAssetAutomaticMergeArgs& MergeArgs) const override;
	virtual EAssetCommandResult Merge(const FAssetManualMergeArgs& MergeArgs) const override;
};
