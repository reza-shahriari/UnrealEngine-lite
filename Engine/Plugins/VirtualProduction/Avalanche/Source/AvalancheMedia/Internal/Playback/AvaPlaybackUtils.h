// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "Templates/SharedPointer.h"
#include "UObject/SoftObjectPath.h"

class UPackage;
struct FAssetData;

class AVALANCHEMEDIA_API FAvaPlaybackUtils
{
public:
	/**
	 * Reset the loaders of any loaded packages so the client can overwrite the files.
	*/
	static void FlushPackageLoading(UPackage* InPackage);

	/**
	 * Checks if the given packages has been deleted on disk.
	 * @remark With Editor only.
	 * @return true if the package was deleted, false if the package still exists on disk.
	 */
	static bool IsPackageDeleted(const UPackage* InExistingPackage);

	/**
	 *	Purge all the objects in memory owned by the given packages.
	 */
	static void PurgePackages(const TArray<UPackage*>& InExistingPackages);

	/**
	 * Reloads the given package.
	 * @remark With Editor only.
	 * @return true if the package was reloaded.
	 */
	static bool ReloadPackages(const TArray<UPackage*>& InExistingPackages);

	/**
	 * @brief Determines if the asset is a map by checking the file extension.
	 * @remark The file must exist on disk.
	 * @param InPackageName Package name.
	 * @return true if the file on disk is a .umap file.
	 */
	static bool IsMapAsset(const FString& InPackageName);

	/**
	 * @brief Determines if the asset is a playable (can be used as template) asset, using the asset class.
	 * @param InAssetData Asset Data
	 * @return true if the asset is an ava playable.
	 */
	static bool IsPlayableAsset(const FAssetData& InAssetData);
};

namespace UE::AvaPlayback::Utils
{
	/**
	 *	Returns a compactly formatted time stamp information for the current frame.
	 *	This is to used for logging and tracing.
	 */
	AVALANCHEMEDIA_API FString GetBriefFrameInfo();

	template<typename InEnumType>
	FString StaticEnumToString(InEnumType InValue)
	{
		return StaticEnum<InEnumType>()->GetNameStringByValue(static_cast<int64>(InValue));
	}

	/**
	 * Utility class to perform async load of assets. 
	 */
	class AVALANCHEMEDIA_API FAsyncAssetLoader : public TSharedFromThis<FAsyncAssetLoader>
	{
	public:
		/**
		 * @brief Issue the async load command. Returns immediately.
		 * @param InAssetsToLoad Assets to load.
		 *
		 * The specified asset to load will added to the current set of pending assets.
		 */
		void BeginLoadingAssets(const TArray<FSoftObjectPath>& InAssetsToLoad);

		/** Returns true when all the requested assets have finished loading. */
		bool IsLoadingCompleted() const
		{
			return PendingAssets.IsEmpty();
		}

		/** Delegate called when all assets have finished loading. */
		DECLARE_MULTICAST_DELEGATE(FOnLoadingCompleted);
		FOnLoadingCompleted OnLoadingCompleted;

	private:
		TSet<FSoftObjectPath> PendingAssets;
	};
}