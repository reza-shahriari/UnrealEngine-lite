// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "ContentBrowserDataSource.h"
#include "NativeClassHierarchy.h"
#include "UObject/Package.h"
#include "ContentBrowserClassDataSource.generated.h"

class FContentBrowserClassFileItemDataPayload;
class FContentBrowserClassFolderItemDataPayload;

class IAssetTypeActions;
class UToolMenu;
class FNativeClassHierarchy;
struct FCollectionNameType;

USTRUCT()
struct CONTENTBROWSERCLASSDATASOURCE_API FContentBrowserCompiledClassDataFilter
{
	GENERATED_BODY()

public:
	UPROPERTY()
	TSet<TObjectPtr<UClass>> ValidClasses;

	UPROPERTY()
	TSet<FName> ValidFolders;
};

UCLASS()
class CONTENTBROWSERCLASSDATASOURCE_API UContentBrowserClassDataSource : public UContentBrowserDataSource
{
	GENERATED_BODY()

public:
	void Initialize(const bool InAutoRegister = true);

	virtual void Shutdown() override;

	virtual void CompileFilter(const FName InPath, const FContentBrowserDataFilter& InFilter, FContentBrowserDataCompiledFilter& OutCompiledFilter) override;

	virtual void EnumerateItemsMatchingFilter(const FContentBrowserDataCompiledFilter& InFilter, TFunctionRef<bool(FContentBrowserItemData&&)> InCallback) override;

	virtual void EnumerateItemsAtPath(const FName InPath, const EContentBrowserItemTypeFilter InItemTypeFilter, TFunctionRef<bool(FContentBrowserItemData&&)> InCallback) override;

	virtual bool EnumerateItemsForObjects(const TArrayView<UObject*> InObjects, TFunctionRef<bool(FContentBrowserItemData&&)> InCallback) override;

	virtual bool IsFolderVisible(const FName InPath, const EContentBrowserIsFolderVisibleFlags InFlags, const FContentBrowserFolderContentsFilter& InContentsFilter) override;

	virtual bool DoesItemPassFilter(const FContentBrowserItemData& InItem, const FContentBrowserDataCompiledFilter& InFilter) override;

	virtual bool GetItemAttribute(const FContentBrowserItemData& InItem, const bool InIncludeMetaData, const FName InAttributeKey, FContentBrowserItemDataAttributeValue& OutAttributeValue) override;

	virtual bool GetItemAttributes(const FContentBrowserItemData& InItem, const bool InIncludeMetaData, FContentBrowserItemDataAttributeValues& OutAttributeValues) override;

	virtual bool GetItemPhysicalPath(const FContentBrowserItemData& InItem, FString& OutDiskPath) override;

	virtual bool CanEditItem(const FContentBrowserItemData& InItem, FText* OutErrorMsg) override;

	virtual bool EditItem(const FContentBrowserItemData& InItem) override;

	virtual bool BulkEditItems(TArrayView<const FContentBrowserItemData> InItems) override;

	virtual bool AppendItemReference(const FContentBrowserItemData& InItem, FString& InOutStr) override;

	virtual bool AppendItemObjectPath(const FContentBrowserItemData& InItem, FString& InOutStr) override;

	virtual bool AppendItemPackageName(const FContentBrowserItemData& InItem, FString& InOutStr) override;

	virtual bool UpdateThumbnail(const FContentBrowserItemData& InItem, FAssetThumbnail& InThumbnail) override;

	virtual bool TryGetCollectionId(const FContentBrowserItemData& InItem, FSoftObjectPath& OutCollectionId) override;

	virtual bool Legacy_TryGetPackagePath(const FContentBrowserItemData& InItem, FName& OutPackagePath) override;

	virtual bool Legacy_TryGetAssetData(const FContentBrowserItemData& InItem, FAssetData& OutAssetData) override;

	virtual bool Legacy_TryConvertPackagePathToVirtualPath(const FName InPackagePath, FName& OutPath) override;

	virtual bool Legacy_TryConvertAssetDataToVirtualPath(const FAssetData& InAssetData, const bool InUseFolderPaths, FName& OutPath) override;

protected:
	TSharedPtr<IAssetTypeActions> GetClassTypeActions();
	
	virtual void BuildRootPathVirtualTree() override;
	bool RootClassPathPassesFilter(const FName InRootClassPath, const bool bIncludeEngineClasses, const bool bIncludePluginClasses) const;

private:
	bool IsKnownClassPath(const FName InPackagePath) const;

	bool GetClassPathsForCollections(TArrayView<const FCollectionRef> InCollections, const bool bIncludeChildCollections, TArray<FTopLevelAssetPath>& OutClassPaths);

	FContentBrowserItemData CreateClassFolderItem(const FName InFolderPath);

	FContentBrowserItemData CreateClassFileItem(UClass* InClass, FNativeClassHierarchyGetClassPathCache& InCache);

	FContentBrowserItemData CreateClassFolderItem(const FName InFolderPath, const TSharedPtr<const FNativeClassHierarchyNode>& InFolderNode);

	FContentBrowserItemData CreateClassFileItem(const FName InClassPath, const TSharedPtr<const FNativeClassHierarchyNode>& InClassNode);

	TSharedPtr<const FContentBrowserClassFolderItemDataPayload> GetClassFolderItemPayload(const FContentBrowserItemData& InItem) const;

	TSharedPtr<const FContentBrowserClassFileItemDataPayload> GetClassFileItemPayload(const FContentBrowserItemData& InItem) const;

	void OnNewClassRequested(const FName InSelectedPath);

	void PopulateAddNewContextMenu(UToolMenu* InMenu);

	void ConditionalCreateNativeClassHierarchy();

	void OnFoldersAdded(const TArrayView<TSharedRef<const FNativeClassHierarchyNode>> InFolders);

	void OnFoldersRemoved(const TArrayView<TSharedRef<const FNativeClassHierarchyNode>> InFolders);

	void OnClassesAdded(const TArrayView<TSharedRef<const FNativeClassHierarchyNode>> InClasses);

	void OnClassesRemoved(const TArrayView<TSharedRef<const FNativeClassHierarchyNode>> InClasses);

	TSharedPtr<FNativeClassHierarchy> NativeClassHierarchy;
	FNativeClassHierarchyGetClassPathCache NativeClassHierarchyGetClassPathCache;

	TSharedPtr<IAssetTypeActions> ClassTypeActions;
};
