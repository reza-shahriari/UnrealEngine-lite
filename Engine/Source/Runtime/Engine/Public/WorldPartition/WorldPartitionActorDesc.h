// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_EDITOR
#include "CoreMinimal.h"
#include "PropertyPairsMap.h"
#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "UObject/WeakObjectPtr.h"
#include "Templates/SubclassOf.h"
#include "Misc/Guid.h"
#include "Misc/TVariant.h"
#include "WorldPartition/WorldPartitionActorDescType.h"
#include "WorldPartition/WorldPartitionActorContainerID.h"
#include "WorldPartition/Filter/WorldPartitionActorFilter.h"

class FActorDescArchive;

// Struct used to create actor descriptor
struct FWorldPartitionActorDescInitData
{
	FWorldPartitionActorDescInitData()
		: DataSource(TInPlaceType<TArray<uint8>>(), TArray<uint8>())
	{}

	FWorldPartitionActorDescInitData(FActorDescArchive* InArchive)
		: DataSource(TInPlaceType<FActorDescArchive*>(), InArchive)
	{}

	UClass* NativeClass;
	FName PackageName;
	FSoftObjectPath ActorPath;

	TArray<uint8>& GetSerializedData()
	{
		check(DataSource.IsType<TArray<uint8>>());
		return DataSource.Get<TArray<uint8>>();
	}

	const TArray<uint8>& GetSerializedData() const
	{
		check(DataSource.IsType<TArray<uint8>>());
		return DataSource.Get<TArray<uint8>>();
	}

	FActorDescArchive* GetArchive() const
	{
		check(DataSource.IsType<FActorDescArchive*>());
		return DataSource.Get<FActorDescArchive*>();
	}

	bool IsUsingArchive() const
	{
		return DataSource.IsType<FActorDescArchive*>();
	}
		
	FWorldPartitionActorDescInitData& SetNativeClass(UClass* InNativeClass) { NativeClass = InNativeClass; return *this; }
	FWorldPartitionActorDescInitData& SetPackageName(FName InPackageName) { PackageName = InPackageName; return *this; }
	FWorldPartitionActorDescInitData& SetActorPath(const FSoftObjectPath& InActorPath) { ActorPath = InActorPath; return *this; }

private:
	// Provide SerializedData or an already initialized Archive
	TVariant<TArray<uint8>, FActorDescArchive*> DataSource;
};

struct FWorldPartitionRelativeBounds
{
	FWorldPartitionRelativeBounds()
		: bIsValid(false)
	{ }

	FWorldPartitionRelativeBounds(EForceInit ForceInit)
		: Center(ForceInit)
		, Rotation(ForceInit)
		, Extents(ForceInit)
		, bIsValid(false)
	{ }

	FWorldPartitionRelativeBounds(const FVector& InCenter, const FQuat& InRotation, const FVector& InExtents)
		: Center(InCenter)
		, Rotation(InRotation)
		, Extents(InExtents)
		, bIsValid(true)
	{ }

	FWorldPartitionRelativeBounds(const FBox& InBox)
		: FWorldPartitionRelativeBounds(ForceInit)
	{
		if (InBox.IsValid)
		{
			InBox.GetCenterAndExtents(Center, Extents);
			Rotation = FQuat::Identity;
			bIsValid = true;
		}
	}

	bool Equals(const FWorldPartitionRelativeBounds& Other, double Tolerance = UE_KINDA_SMALL_NUMBER) const
	{
		return (!bIsValid && !Other.bIsValid) 
			|| ((bIsValid && Other.bIsValid) && Center.Equals(Other.Center, Tolerance) && Rotation.Equals(Other.Rotation, Tolerance) && Extents.Equals(Other.Extents, Tolerance));
	}

	bool IsValid() const
	{
		return bIsValid;
	}

	FBox ToAABB() const
	{
		FBox Result(ForceInit);

		if (bIsValid)
		{
			double X = Extents.X;
			double Y = Extents.Y;
			double Z = Extents.Z;

			Result += Rotation * FVector(-X, -Y, -Z) + Center;
			Result += Rotation * FVector( X, -Y, -Z) + Center;
			Result += Rotation * FVector(-X,  Y, -Z) + Center;
			Result += Rotation * FVector( X,  Y, -Z) + Center;
			Result += Rotation * FVector(-X, -Y,  Z) + Center;
			Result += Rotation * FVector( X, -Y,  Z) + Center;
			Result += Rotation * FVector(-X,  Y,  Z) + Center;
			Result += Rotation * FVector( X,  Y,  Z) + Center;
		}

		return Result;
	}

	FString ToString() const
	{
		return FString::Printf(TEXT("IsValid=%s, Center=(%s), Rotation=(%s), Extents=(%s)"), bIsValid ? TEXT("true") : TEXT("false"), *Center.ToString(), *Rotation.Euler().ToString(), *Extents.ToString());
	}

	friend FArchive& operator<<(FArchive& Ar, FWorldPartitionRelativeBounds& Bounds)
	{
		return Ar << Bounds.Center << Bounds.Rotation << Bounds.Extents << Bounds.bIsValid;
	}

	[[nodiscard]] FWorldPartitionRelativeBounds TransformBy(const FTransform& Transform) const
	{
		if (!bIsValid)
		{
			return FWorldPartitionRelativeBounds(ForceInit);
		}

		FWorldPartitionRelativeBounds NewBounds;
		NewBounds.Center = Transform.TransformPosition(Center);
		NewBounds.Rotation = Transform.GetRotation() * Rotation;
		NewBounds.Extents = Extents * Transform.GetScale3D();
		NewBounds.bIsValid = true;

		return NewBounds;
	}

	[[nodiscard]] FWorldPartitionRelativeBounds InverseTransformBy(const FTransform& Transform) const
	{
		if (!bIsValid)
		{
			return FWorldPartitionRelativeBounds(ForceInit);
		}

		FWorldPartitionRelativeBounds NewBounds;
		NewBounds.Center = Transform.InverseTransformPosition(Center);
		NewBounds.Rotation = Transform.GetRotation().Inverse() * Rotation;
		NewBounds.Extents = Extents * FTransform::GetSafeScaleReciprocal(Transform.GetScale3D());
		NewBounds.bIsValid = true;

		return NewBounds;
	}

private:
	FVector Center;
	FQuat Rotation;
	FVector Extents;
	bool bIsValid;
};

struct FWorldPartitionAssetDataPatcher
{
	virtual ~FWorldPartitionAssetDataPatcher() {}
	virtual bool DoPatch(FString& InOutString) = 0;
	virtual bool DoPatch(FName& InOutName) = 0;
	virtual bool DoPatch(FSoftObjectPath& InOutSoft) = 0;
	virtual bool DoPatch(FTopLevelAssetPath& InOutPath) = 0;
};

class AActor;
class UWorldPartition;
class UActorDescContainerInstance;
class FWorldPartitionActorDescInstance;
class UActorDescContainer;
class IWorldPartitionActorDescInstanceView;
class IStreamingGenerationErrorHandler;

enum class EContainerClusterMode : uint8
{
	Partitioned, // Per Actor Partitioning
};

template <typename T, class F>
inline bool CompareUnsortedArrays(const TArray<T>& Array1, const TArray<T>& Array2, F Func)
{
	if (Array1.Num() == Array2.Num())
	{
		TArray<T> SortedArray1(Array1);
		TArray<T> SortedArray2(Array2);
		SortedArray1.Sort(Func);
		SortedArray2.Sort(Func);
		return SortedArray1 == SortedArray2;
	}
	return false;
}

template <typename T>
inline bool CompareUnsortedArrays(const TArray<T>& Array1, const TArray<T>& Array2)
{
	return CompareUnsortedArrays(Array1, Array2, [](const T& A, const T& B) { return A < B; });
}

template <>
inline bool CompareUnsortedArrays(const TArray<FName>& Array1, const TArray<FName>& Array2)
{
	return CompareUnsortedArrays(Array1, Array2, [](const FName& A, const FName& B) { return A.LexicalLess(B); });
}

#endif // WITH_EDITOR

/**
 * Represents a Actor Metadata (editor-only)
 */
class FWorldPartitionActorDesc
{
#if WITH_EDITOR
	friend class AActor;
	friend class UWorldPartition;
	friend struct FWorldPartitionHandleImpl;
	friend struct FWorldPartitionReferenceImpl;
	friend struct FWorldPartitionActorDescUtils;
	friend struct FWorldPartitionActorDescUnitTestAcccessor;
	friend class FAssetHeaderPatcherInner;
	friend class FActorDescArchive;
	friend class FWorldPartitionActorDescInstance;
	friend class FStreamingGenerationTempActorDescInstance;
	friend class FPropertyOverrideUtils;
	template<class U> friend class TActorDescContainerCollection;

public:
	struct FContainerInstance
	{
		UActorDescContainerInstance* ContainerInstance = nullptr;
		FTransform Transform = FTransform::Identity;
		EContainerClusterMode ClusterMode;
		TMap<FActorContainerID, TSet<FGuid>> FilteredActors;

		UE_DEPRECATED(5.4, "Use FLoadedContainerInstance instead")
		ULevel* LoadedLevel = nullptr;

		UE_DEPRECATED(5.4, "Use FLoadedContainerInstance instead")
		bool bSupportsPartialEditorLoading = false;

		UE_DEPRECATED(5.4, "Use ContainerInstance instead")
		const UActorDescContainer* Container = nullptr;
	};

	virtual ~FWorldPartitionActorDesc() {}

	inline const FGuid& GetGuid() const { return Guid; }
	
	inline FTopLevelAssetPath GetBaseClass() const { return BaseClass; }
	inline FTopLevelAssetPath GetNativeClass() const { return NativeClass; }
	inline UClass* GetActorNativeClass() const { return ActorNativeClass; }

	inline FName GetRuntimeGrid() const { return RuntimeGrid; }
	inline bool GetIsSpatiallyLoaded() const { return RuntimeBounds.IsValid ? bIsSpatiallyLoaded : false; }
	inline bool GetIsSpatiallyLoadedRaw() const { return bIsSpatiallyLoaded; }
	inline bool GetActorIsEditorOnly() const { return bActorIsEditorOnly; }
	ENGINE_API bool GetActorIsEditorOnlyLoadedInPIE() const;
	inline bool GetActorIsRuntimeOnly() const { return bActorIsRuntimeOnly; }

	inline bool GetActorIsHLODRelevant() const { return bActorIsHLODRelevant; }
	inline FSoftObjectPath GetHLODLayer() const { return HLODLayer; }
	ENGINE_API TArray<FName> GetDataLayers(bool bIncludeExternalDataLayer = true) const;
	ENGINE_API FName GetExternalDataLayer() const;
	inline const FSoftObjectPath& GetExternalDataLayerAsset() const { return ExternalDataLayerAsset; }

	inline const TArray<FName>& GetTags() const { return Tags; }

	inline FName GetActorPackage() const { return ActorPackage; }
	inline FSoftObjectPath GetActorSoftPath() const { return ActorPath; }
	inline FName GetActorLabel() const { return ActorLabel; }
	inline FName GetFolderPath() const { return FolderPath; }
	inline const FGuid& GetFolderGuid() const { return FolderGuid; }
	inline const FTransform& GetActorTransform() const { return ActorTransform; }

	ENGINE_API FBox GetEditorBounds() const;
	ENGINE_API FBox GetRuntimeBounds() const;

	inline const FGuid& GetParentActor() const { return ParentActor; }
	inline bool IsUsingDataLayerAsset() const { return bIsUsingDataLayerAsset; }
	inline void AddProperty(FName PropertyName, FName PropertyValue = NAME_None) { Properties.AddProperty(PropertyName, PropertyValue); }
	inline bool GetProperty(FName PropertyName, FName* PropertyValue) const { return Properties.GetProperty(PropertyName, PropertyValue); }
	inline bool HasProperty(FName PropertyName) const { return Properties.HasProperty(PropertyName); }

	ENGINE_API FName GetActorName() const;
	ENGINE_API FName GetActorLabelOrName() const;
	ENGINE_API FName GetDisplayClassName() const;

	// Faster accessors for names as strings
	ENGINE_API const FString& GetActorNameString() const;
	ENGINE_API const FString& GetActorLabelString() const;
	ENGINE_API const FString& GetDisplayClassNameString() const;

	inline bool IsDefaultActorDesc() const { return bIsDefaultActorDesc; }
		
	virtual bool IsChildContainerInstance() const { return false; }
	virtual FName GetChildContainerPackage() const { return NAME_None; }
	virtual FString GetChildContainerName() const { return FString(); }
	virtual EWorldPartitionActorFilterType GetChildContainerFilterType() const { return EWorldPartitionActorFilterType::None; }
	virtual const FWorldPartitionActorFilter* GetChildContainerFilter() const { return nullptr; }
	virtual UActorDescContainer* GetChildContainer() const { return nullptr; }
	
	ENGINE_API FGuid GetContentBundleGuid() const;

	virtual const FGuid& GetSceneOutlinerParent() const { return GetParentActor(); }
	virtual bool IsResaveNeeded() const { return bIsSpatiallyLoaded && !RuntimeBounds.IsValid; }

	ENGINE_API virtual void CheckForErrors(const IWorldPartitionActorDescInstanceView* InActorDescView, IStreamingGenerationErrorHandler* ErrorHandler) const;

	bool operator==(const FWorldPartitionActorDesc& Other) const
	{
		return Guid == Other.Guid;
	}

	friend uint32 GetTypeHash(const FWorldPartitionActorDesc& Key)
	{
		return GetTypeHash(Key.Guid);
	}

	//~ Begin Deprecation
	UE_DEPRECATED(5.1, "SetIsSpatiallyLoadedRaw is deprecated and should not be used.")
	inline void SetIsSpatiallyLoadedRaw(bool bNewIsSpatiallyLoaded) { bIsSpatiallyLoaded = bNewIsSpatiallyLoaded; }

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	UE_DEPRECATED(5.2, "GetOrigin is deprecated.")
	inline FVector GetOrigin() const { return GetBounds().GetCenter(); }

	UE_DEPRECATED(5.2, "GetBounds is deprecated, GetEditorBounds or GetRuntimeBounds should be used instead.")
	FBox GetBounds() const { return GetEditorBounds(); }
PRAGMA_ENABLE_DEPRECATION_WARNINGS

	UE_DEPRECATED(5.2, "ShouldValidateRuntimeGrid is deprecated and should not be used")
	virtual bool ShouldValidateRuntimeGrid() const { return true; }

	UE_DEPRECATED(5.3, "GetLevelPackage is deprecated use GetChildContainerPackage instead")
	virtual FName GetLevelPackage() const { return GetChildContainerPackage(); }

	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance::HasResolvedDataLayerInstanceNames")
	inline bool HasResolvedDataLayerInstanceNames() const { return false; }

	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance::GetDataLayerInstanceNames")
	TArray<FName> GetDataLayerInstanceNames() const { return TArray<FName>(); }

	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance::SetDataLayerInstanceNames")
	inline void SetDataLayerInstanceNames(const TArray<FName>& InDataLayerInstanceNames) { }

	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance::IsRuntimeRelevant instead")
	virtual bool IsRuntimeRelevant(const FActorContainerID& InContainerID) const { return false; }
		
	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance::IsEditorRelevant instead")
	virtual bool IsEditorRelevant() const { return false; }
		
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::GetUnloadedReason")
	FText GetUnloadedReason() const { return FText(); }

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::SetUnloadedReason")
	void SetUnloadedReason(FText* InUnloadedReason) { }

	UE_DEPRECATED(5.4, "Use IsChildContainerInstance instead")
	virtual bool IsContainerInstance() const { return false; }

	UE_DEPRECATED(5.4, "Use GetChildContainerPackage instead")
	virtual FName GetContainerPackage() const { return NAME_None; }

	UE_DEPRECATED(5.4, "Use GetChildContainerFilterType instead")
	virtual EWorldPartitionActorFilterType GetContainerFilterType() const { return EWorldPartitionActorFilterType::None; }

	UE_DEPRECATED(5.4, "Use GetChildContainerFilter instead")
	virtual const FWorldPartitionActorFilter* GetContainerFilter() const { return nullptr; }

	UE_DEPRECATED(5.4, "Use GetChildContainerInstance instead")
	virtual bool GetContainerInstance(FContainerInstance& OutContainerInstance) const { return false; }

	UE_DEPRECATED(5.4, "Use SetContainer without World param")
	virtual void SetContainer(UActorDescContainer* InContainer, UWorld* InWorldContext)	{ }

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::IsLoaded")
	bool IsLoaded(bool bEvenIfPendingKill=false) const { return false; }
	
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::GetActor")
	AActor* GetActor(bool bEvenIfPendingKill=true, bool bEvenIfUnreachable=false) const { return nullptr; }

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::GetActor")
	TWeakObjectPtr<AActor>* GetActorPtr(bool bEvenIfPendingKill = true, bool bEvenIfUnreachable = false) const { return nullptr; }

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::Load")
	AActor* Load() const { return nullptr;}

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::Unload")
	virtual void Unload() {}

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance")
	void TransformInstance(const FString& From, const FString& To) {}

	UE_DEPRECATED(5.4, "Use FWorldPartitionActorDescInstance version instead")
	virtual void CheckForErrors(IStreamingGenerationErrorHandler* ErrorHandler) const {}

protected:
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::IncSoftRefCount")
	inline uint32 IncSoftRefCount() const { return 0; }

	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::DecSoftRefCount")
	inline uint32 DecSoftRefCount() const { return 0; }
	
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::IncHardRefCount")
	inline uint32 IncHardRefCount() const { return 0;}
	
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::DecHardRefCount")
	inline uint32 DecHardRefCount() const { return 0;}
	
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::GetSoftRefCount")
	inline uint32 GetSoftRefCount() const { return 0; }
	
	UE_DEPRECATED(5.4, "Please use FWorldPartitionActorDescInstance::GetHardRefCount")
	inline uint32 GetHardRefCount() const { return 0; }
	//~ End Deprecation
	
public:
	const TArray<FGuid>& GetReferences() const
	{
		return References;
	}

	const TArray<FGuid>& GetEditorOnlyReferences() const
	{
		return EditorOnlyReferences;
	}

	bool IsEditorOnlyReference(const FGuid& ReferenceGuid) const
	{
		return EditorOnlyReferences.Contains(ReferenceGuid);
	}

	UActorDescContainer* GetContainer() const
	{
		return Container;
	}

	virtual void SetContainer(UActorDescContainer* InContainer)
	{
		check(!Container || !InContainer);
		Container = InContainer;
	}

	ENGINE_API virtual void UpdateActorToWorld();

	enum class EToStringMode : uint8
	{
		Guid,
		Compact,
		Full,
		Verbose,
		ForDiff
	};

	ENGINE_API FString ToString(EToStringMode Mode = EToStringMode::Compact) const;

	ENGINE_API virtual void Init(const AActor* InActor);
	ENGINE_API virtual void Init(const FWorldPartitionActorDescInitData& DescData);

	static ENGINE_API void Patch(const FWorldPartitionActorDescInitData& DescData, TArray<uint8>& OutData, FWorldPartitionAssetDataPatcher* InAssetDataPatcher);

	ENGINE_API virtual bool Equals(const FWorldPartitionActorDesc* Other) const;	

	virtual bool HasStandaloneHLOD() const { return false; }

	/**
	 * Returns true if resaving this actor will have an impact on streaming generation. Before class descriptors, properties changed on a Blueprint 
	 * that would affect streaming generation weren't taken into account until the actors affected by the change were resaved.
	 */
	ENGINE_API virtual bool ShouldResave(const FWorldPartitionActorDesc* Other) const;

	ENGINE_API void SerializeTo(TArray<uint8>& OutData, FWorldPartitionActorDesc* BaseDesc = nullptr) const;

	using FActorDescDeprecator = TFunction<void(FArchive&, FWorldPartitionActorDesc*)>;
	static ENGINE_API void RegisterActorDescDeprecator(TSubclassOf<AActor> ActorClass, const FActorDescDeprecator& Deprecator);

	ENGINE_API bool IsMainWorldOnly() const;
	ENGINE_API bool IsListedInSceneOutliner() const;

protected:
	void InitTransientProperties(const FWorldPartitionActorDescInitData& DescData);
	virtual bool GetChildContainerInstance(const FWorldPartitionActorDescInstance* InActorDescInstance, FContainerInstance& OutContainerInstance) const { return false; }
	virtual UActorDescContainerInstance* CreateChildContainerInstance(const FWorldPartitionActorDescInstance* InActorDescInstance) const { return nullptr; }
	virtual UWorldPartition* GetLoadedChildWorldPartition(const FWorldPartitionActorDescInstance* InActorDescInstance) const { return nullptr; }
	ENGINE_API virtual bool IsRuntimeRelevant(const FWorldPartitionActorDescInstance* InActorDescInstance) const;
	ENGINE_API virtual bool IsEditorRelevant(const FWorldPartitionActorDescInstance* InActorDescInstance) const;

	ENGINE_API FWorldPartitionActorDesc();

	ENGINE_API virtual void TransferFrom(const FWorldPartitionActorDesc* From);

	virtual void TransferWorldData(const FWorldPartitionActorDesc* From)
	{
		ActorTransform = From->ActorTransform;
		RuntimeBounds = From->RuntimeBounds;
		EditorBounds = From->EditorBounds;


		ActorTransformRelative = From->ActorTransformRelative;
		RuntimeBoundsRelative = From->RuntimeBoundsRelative;
		EditorBoundsRelative = From->EditorBoundsRelative;
		bHasValidRelativeBounds = From->bHasValidRelativeBounds;
	}

	virtual uint32 GetSizeOf() const { return sizeof(FWorldPartitionActorDesc); }

	ENGINE_API virtual void Serialize(FArchive& Ar);

	ENGINE_API void SetEditorBounds(const FBox& InEditorBounds);
	ENGINE_API void SetRuntimeBounds(const FBox& InRuntimeBounds);

	// Persistent
	FGuid							Guid;
	FTopLevelAssetPath				BaseClass;
	FTopLevelAssetPath				NativeClass;	// Not serialized, comes from initialization data
	FName							ActorPackage;	// Not serialized, comes from initialization data
	FSoftObjectPath					ActorPath;		// Not serialized, comes from initialization data
	FName							ActorLabel;
	FTransform						ActorTransformRelative;			// Relative transform for actors with parent actor, actor-to-world transform otherwise
	FWorldPartitionRelativeBounds	RuntimeBoundsRelative;	// Runtime bounds relative to actor-to-world transform
	FWorldPartitionRelativeBounds	EditorBoundsRelative;	// Editor bounds relative to actor-to-world transform
	FName							RuntimeGrid;
	bool							bIsSpatiallyLoaded;
	bool							bActorIsEditorOnly;
	bool							bActorIsRuntimeOnly;
	bool							bActorIsMainWorldOnly;
	bool							bActorIsHLODRelevant;
	bool							bActorIsListedInSceneOutliner;
	bool							bIsUsingDataLayerAsset; // Used to know if DataLayers array represents DataLayers Asset paths or the FNames of the deprecated version of Data Layers
	FSoftObjectPath					HLODLayer;
	TArray<FName>					DataLayers;
	FSoftObjectPath					ExternalDataLayerAsset;
	TArray<FGuid>					References;
	TArray<FGuid>					EditorOnlyReferences; // References that aren't necessarily editor only but referenced through an editor only property.
	TArray<FName>					Tags;
	FPropertyPairsMap				Properties;
	FName							FolderPath;
	FGuid							FolderGuid;
	FGuid							ParentActor; // Used to validate settings against parent (to warn on layer/placement compatibility issues)
	FGuid							ContentBundleGuid;
	
	// Transient
	UClass*							ActorNativeClass;
	FName							ActorName;
	FString							ActorNameString;
	FString							ActorLabelString;
	FString							ActorDisplayClassNameString;
	UActorDescContainer*			Container;
	FTransform						ActorTransform;	// Current actor-to-world transform, considering parent transform
	FBox							RuntimeBounds;	// Current runtime bounds, considering parent transform
	FBox							EditorBounds;	// Current editor bounds, considering parent transform
	bool							bIsDefaultActorDesc;
	bool							bHasValidRelativeBounds;

	static ENGINE_API TMap<TSubclassOf<AActor>, FActorDescDeprecator> Deprecators;

private:
	void FixupStreamingBounds();
#endif // WITH_EDITOR
};

