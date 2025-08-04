// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AssetRegistry/AssetRegistryState.h"
#include "Cooker/CookDeterminismManager.h"
#include "Cooker/DiffWriterArchive.h"
#include "Serialization/PackageWriter.h"

namespace UE::DiffWriter { struct FAccumulatorGlobals; }

/** A CookedPackageWriter that diffs output from the current cook with the file that was saved in the previous cook. */
class FDiffPackageWriter : public ICookedPackageWriter
{
public:
	FDiffPackageWriter(TUniquePtr<ICookedPackageWriter>&& InInner, UE::Cook::FDeterminismManager* InDeterminismManager);

	// IPackageWriter
	virtual FCapabilities GetCapabilities() const override
	{
		FCapabilities Result = Inner->GetCapabilities();
		Result.bIgnoreHeaderDiffs = bIgnoreHeaderDiffs;
		Result.bDeterminismDebug = true;
		return Result;
	}
	virtual void BeginPackage(const FBeginPackageInfo& Info) override;
	virtual void CommitPackage(FCommitPackageInfo&& Info) override;
	virtual void WritePackageData(const FPackageInfo& Info, FLargeMemoryWriter& ExportsArchive,
		const TArray<FFileRegion>& FileRegions) override;
	virtual void WriteBulkData(const FBulkDataInfo& Info, const FIoBuffer& BulkData,
		const TArray<FFileRegion>& FileRegions) override
	{
		Inner->WriteBulkData(Info, BulkData, FileRegions);
	}
	virtual void WriteAdditionalFile(const FAdditionalFileInfo& Info, const FIoBuffer& FileData) override
	{
		Inner->WriteAdditionalFile(Info, FileData);
	}
	virtual void WriteLinkerAdditionalData(const FLinkerAdditionalDataInfo& Info, const FIoBuffer& Data,
		const TArray<FFileRegion>& FileRegions) override
	{
		Inner->WriteLinkerAdditionalData(Info, Data, FileRegions);
	}
	virtual void WritePackageTrailer(const FPackageTrailerInfo& Info, const FIoBuffer& Data) override
	{
		Inner->WritePackageTrailer(Info, Data);
	}
	virtual int64 GetExportsFooterSize() override
	{
		return Inner->GetExportsFooterSize();
	}
	virtual TUniquePtr<FLargeMemoryWriter> CreateLinkerArchive(FName PackageName, UObject* Asset, uint16 MultiOutputIndex) override;
	virtual TUniquePtr<FLargeMemoryWriter> CreateLinkerExportsArchive(FName PackageName, UObject* Asset, uint16 MultiOutputIndex) override;
	virtual bool IsPreSaveCompleted() const override
	{
		return bHasStartedSecondSave;
	}
	virtual void RegisterDeterminismHelper(UObject* SourceObject,
		const TRefCountPtr<UE::Cook::IDeterminismHelper>& DeterminismHelper) override;

	// ICookedPackageWriter
	virtual FCookCapabilities GetCookCapabilities() const override
	{
		FCookCapabilities Result = Inner->GetCookCapabilities();
		Result.bDiffModeSupported = false; // DiffPackageWriter can not be an inner of another DiffPackageWriter
		Result.bReadOnly = true;
		return Result;
	}
	virtual FDateTime GetPreviousCookTime() const
	{
		return Inner->GetPreviousCookTime();
	}
	virtual void Initialize(const FCookInfo& Info) override
	{
		Inner->Initialize(Info);
	}
	virtual void BeginCook(const FCookInfo& Info) override
	{
		Inner->BeginCook(Info);
	}
	virtual void EndCook(const FCookInfo& Info) override
	{
		Inner->EndCook(Info);
	}
	virtual TUniquePtr<FAssetRegistryState> LoadPreviousAssetRegistry() override
	{
		return Inner->LoadPreviousAssetRegistry();
	}
	virtual FCbObject GetOplogAttachment(FName PackageName, FUtf8StringView AttachmentKey) override
	{
		return Inner->GetOplogAttachment(PackageName, AttachmentKey);
	}
	virtual void GetOplogAttachments(TArrayView<FName> PackageNames,
		TArrayView<FUtf8StringView> AttachmentKeys,
		TUniqueFunction<void(FName PackageName, FUtf8StringView AttachmentKey, FCbObject&& Attachment)>&& Callback) override
	{
		return Inner->GetOplogAttachments(PackageNames, AttachmentKeys, MoveTemp(Callback));
	}
	virtual ECommitStatus GetCommitStatus(FName PackageName) override
	{
		return Inner->GetCommitStatus(PackageName);
	}
	virtual void RemoveCookedPackages(TArrayView<const FName> PackageNamesToRemove) override
	{
		Inner->RemoveCookedPackages(PackageNamesToRemove);
	}
	virtual void RemoveCookedPackages() override
	{
		Inner->RemoveCookedPackages();
	}
	virtual void UpdatePackageModificationStatus(FName PackageName, bool bIncrementallyUnmodified,
		bool& bInOutShouldIncrementallySkip) override
	{
		Inner->UpdatePackageModificationStatus(PackageName, bIncrementallyUnmodified, bInOutShouldIncrementallySkip);
	}
	virtual EPackageWriterResult BeginCacheForCookedPlatformData(FBeginCacheForCookedPlatformDataInfo& Info) override
	{
		return Inner->BeginCacheForCookedPlatformData(Info);
	}
	virtual void UpdateSaveArguments(FSavePackageArgs& SaveArgs) override;
	virtual bool IsAnotherSaveNeeded(FSavePackageResultStruct& PreviousResult, FSavePackageArgs& SaveArgs) override;
	virtual TFuture<FCbObject> WriteMPCookMessageForPackage(FName PackageName) override
	{
		return Inner->WriteMPCookMessageForPackage(PackageName);
	}
	virtual bool TryReadMPCookMessageForPackage(FName PackageName, FCbObjectView Message) override
	{
		return Inner->TryReadMPCookMessageForPackage(PackageName, Message);
	}
	virtual TMap<FName, TRefCountPtr<FPackageHashes>>& GetPackageHashes() override
	{
		return Inner->GetPackageHashes();
	}

protected:
	void ParseCmds();
	void ParseDumpObjList(FString InParams);
	void ParseDumpObjects(FString InParams);
	void RemoveParam(FString& InOutParams, const TCHAR* InParamToRemove);
	bool FilterPackageName(const FString& InWildcard);
	void ConditionallyDumpObjList();
	void ConditionallyDumpObjects();
	UE::DiffWriter::FMessageCallback GetDiffWriterMessageCallback();
	virtual void OnDiffWriterMessage(ELogVerbosity::Type Verbosity, FStringView Message);
	FString ResolveText(FStringView Message);
	UE::DiffWriter::FAccumulator& ConstructAccumulator(FName PackageName, UObject* Asset, uint16 MultiOutputIndex);
	bool IsPackageDiffAllowed() const;

	TRefCountPtr<UE::DiffWriter::FAccumulator> Accumulators[2];
	FBeginPackageInfo BeginInfo;
	TSet<FTopLevelAssetPath> CompareDenyListClasses;
	TUniquePtr<ICookedPackageWriter> Inner;
	TUniquePtr<UE::DiffWriter::FAccumulatorGlobals> AccumulatorGlobals;
	UE::Cook::FDeterminismManager* DeterminismManager = nullptr;
	/** Only non-null between BeginPackage and CommitPackage. */
	UPackage* Package = nullptr;
	const TCHAR* Indent = nullptr;
	const TCHAR* NewLine = nullptr;
	FString DumpObjListParams;
	FString PackageFilter;
	int32 MaxDiffsToLog = 5;
	bool bSaveForDiff = false;
	bool bDiffOptional = false;
	bool bIgnoreHeaderDiffs = false;
	bool bIsDifferent = false;
	bool bNewPackage = false;
	bool bHasStartedSecondSave = false;
	bool bDumpObjList = false;
	bool bDumpObjects = false;
	bool bDumpObjectsSorted = false;
};
