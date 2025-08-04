// Copyright Epic Games, Inc. All Rights Reserved.

#include "Cooker/DiffPackageWriter.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Cooker/CookTypes.h"
#include "CookOnTheSide/CookLog.h"
#include "CoreGlobals.h"
#include "EditorDomain/EditorDomainUtils.h"
#include "Engine/Engine.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CString.h"
#include "Misc/FeedbackContext.h"
#include "Misc/Parse.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceHelper.h"
#include "ProfilingDebugging/CookStats.h"
#include "Serialization/LargeMemoryWriter.h"
#include "Templates/UniquePtr.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/LinkerSave.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogDiff, Log, All);

FDiffPackageWriter::FDiffPackageWriter(TUniquePtr<ICookedPackageWriter>&& InInner,
	UE::Cook::FDeterminismManager* InDeterminismManager)
	: Inner(MoveTemp(InInner))
{
	AccumulatorGlobals.Reset(new UE::DiffWriter::FAccumulatorGlobals(Inner.Get()));

	GConfig->GetInt(TEXT("CookSettings"), TEXT("MaxDiffsToLog"), MaxDiffsToLog, GEditorIni);
	// Command line override for MaxDiffsToLog
	FParse::Value(FCommandLine::Get(), TEXT("MaxDiffstoLog="), MaxDiffsToLog);

	bSaveForDiff = FParse::Param(FCommandLine::Get(), TEXT("SaveForDiff"));
	bDiffOptional = FParse::Param(FCommandLine::Get(), TEXT("DiffOptional"));

	GConfig->GetBool(TEXT("CookSettings"), TEXT("IgnoreHeaderDiffs"), bIgnoreHeaderDiffs, GEditorIni);
	// Command line override for IgnoreHeaderDiffs
	if (bIgnoreHeaderDiffs)
	{
		bIgnoreHeaderDiffs = !FParse::Param(FCommandLine::Get(), TEXT("HeaderDiffs"));
	}
	else
	{
		bIgnoreHeaderDiffs = FParse::Param(FCommandLine::Get(), TEXT("IgnoreHeaderDiffs"));
	}

	ParseCmds();

	Indent = FCString::Spc(FOutputDeviceHelper::FormatLogLine(ELogVerbosity::Warning,
		LogDiff.GetCategoryName(), TEXT(""), GPrintLogTimes).Len());
	NewLine = TEXT("\n"); // OutputDevices are responsible for remapping to LINE_TERMINATOR if desired

	check(InDeterminismManager);
	DeterminismManager = InDeterminismManager;
}

void FDiffPackageWriter::ParseCmds()
{
	const TCHAR* DumpObjListParam = TEXT("dumpobjlist");
	const TCHAR* DumpObjectsParam = TEXT("dumpobjects");

	FString CmdsText;
	const TCHAR* const CommandLine = FCommandLine::Get();

	if (FParse::Value(CommandLine, TEXT("-diffcmds="), CmdsText, false))
	{
		CmdsText = CmdsText.TrimQuotes();
		TArray<FString> CmdsList;
		CmdsText.ParseIntoArray(CmdsList, TEXT(","));
		for (const FString& Cmd : CmdsList)
		{
			if (Cmd.StartsWith(DumpObjListParam))
			{
				bDumpObjList = true;
				ParseDumpObjList(*Cmd + FCString::Strlen(DumpObjListParam));
			}
			else if (Cmd.StartsWith(DumpObjectsParam))
			{
				bDumpObjects = true;
				ParseDumpObjects(*Cmd + FCString::Strlen(DumpObjectsParam));
			}
		}
	}

	if (FParse::Param(CommandLine, TEXT("DiffDenyList")))
	{
		for (FTopLevelAssetPath& DenyBaseClassPath : UE::EditorDomain::ConstructTargetIncrementalClassBlockList())
		{
			UClass* DenyBaseClass = FindObject<UClass>(DenyBaseClassPath);
			if (DenyBaseClass)
			{
				CompareDenyListClasses.Add(DenyBaseClassPath);
				TArray<UClass*> DerivedClasses;
				GetDerivedClasses(DenyBaseClass, DerivedClasses);
				for (UClass* DerivedClass : DerivedClasses)
				{
					CompareDenyListClasses.Add(DerivedClass->GetClassPathName());
				}
			}
		}
	}
}

void FDiffPackageWriter::ParseDumpObjList(FString InParams)
{
	const TCHAR* PackageFilterParam = TEXT("-packagefilter=");
	FParse::Value(*InParams, PackageFilterParam, PackageFilter);
	RemoveParam(InParams, PackageFilterParam);

	// Add support for more parameters here
	// After all parameters have been parsed and removed, pass the remaining string as objlist params
	DumpObjListParams = InParams;
}

void FDiffPackageWriter::ParseDumpObjects(FString InParams)
{
	const TCHAR* PackageFilterParam = TEXT("-packagefilter=");
	FParse::Value(*InParams, PackageFilterParam, PackageFilter);
	RemoveParam(InParams, PackageFilterParam);

	const TCHAR* SortParam = TEXT("sort");
	bDumpObjectsSorted = FParse::Param(*InParams, SortParam);
	RemoveParam(InParams, SortParam);
}

void FDiffPackageWriter::RemoveParam(FString& InOutParams, const TCHAR* InParamToRemove)
{
	int32 ParamIndex = InOutParams.Find(InParamToRemove);
	if (ParamIndex >= 0)
	{
		int32 NextParamIndex = InOutParams.Find(TEXT(" -"),
			ESearchCase::CaseSensitive, ESearchDir::FromStart, ParamIndex + 1);
		if (NextParamIndex < ParamIndex)
		{
			NextParamIndex = InOutParams.Len();
		}
		InOutParams = InOutParams.Mid(0, ParamIndex) + InOutParams.Mid(NextParamIndex);
	}
}

void FDiffPackageWriter::BeginPackage(const FBeginPackageInfo& Info)
{
	bIsDifferent = false;
	bNewPackage = false;
	bHasStartedSecondSave = false;
	Accumulators[0].SafeRelease();
	Accumulators[1].SafeRelease();

	BeginInfo = Info;
	Package = FindObjectFast<UPackage>(nullptr, BeginInfo.PackageName);
	ConditionallyDumpObjList();
	ConditionallyDumpObjects();
	Inner->BeginPackage(Info);
}

void FDiffPackageWriter::CommitPackage(FCommitPackageInfo&& Info)
{
	if (bHasStartedSecondSave && bSaveForDiff)
	{
		// Write the package to _ForDiff, but do not write any sidecars
		EnumRemoveFlags(Info.WriteOptions, EWriteOptions::WriteSidecars);
		EnumAddFlags(Info.WriteOptions, EWriteOptions::SaveForDiff);
	}
	else
	{
		EnumRemoveFlags(Info.WriteOptions, EWriteOptions::Write);
	}
	Inner->CommitPackage(MoveTemp(Info));
	Package = nullptr;
}

void FDiffPackageWriter::WritePackageData(const FPackageInfo& Info, FLargeMemoryWriter& ExportsArchive,
	const TArray<FFileRegion>& FileRegions)
{
	check(Info.MultiOutputIndex < 2);
	check(Accumulators[Info.MultiOutputIndex].IsValid()); // Should have been constructed by CreateLinkerArchive
	UE::DiffWriter::FAccumulator& Accumulator = *Accumulators[Info.MultiOutputIndex];
	UE::DiffWriter::FDiffArchive& ExportsArchiveInternal =static_cast<UE::DiffWriter::FDiffArchive&>(ExportsArchive);
	check(&ExportsArchiveInternal.GetAccumulator() == &Accumulator);

	FPackageInfo LocalInfo(Info);
	Inner->CompleteExportsArchiveForDiff(LocalInfo, ExportsArchive);

	if (!bHasStartedSecondSave)
	{
		ICookedPackageWriter::FPreviousCookedBytesData PreviousInnerData;
		if (!Inner->GetPreviousCookedBytes(LocalInfo, PreviousInnerData))
		{
			PreviousInnerData.Data.Reset();
			PreviousInnerData.HeaderSize = 0;
			PreviousInnerData.Size = 0;
		}
		check(PreviousInnerData.Data.Get() != nullptr || (PreviousInnerData.Size == 0 && PreviousInnerData.HeaderSize == 0));

		bNewPackage = PreviousInnerData.Size == 0;
		Accumulator.OnFirstSaveComplete(LocalInfo.LooseFilePath, LocalInfo.HeaderSize, Info.HeaderSize,
			MoveTemp(PreviousInnerData));
		bIsDifferent = Accumulator.HasDifferences();
		if (bIsDifferent && !IsPackageDiffAllowed())
		{
			bIsDifferent = false;
		}
	}
	else
	{
		// Avoid an assert when calling StaticFindObject during save, which we do to list the "exports" from a package.
		// We are not writing the discovered objects into the saved package, so the call to StaticFindObject is legal.
		TGuardValue<bool> GIsSavingPackageGuard(GIsSavingPackage, false);

		Accumulator.OnSecondSaveComplete(LocalInfo.HeaderSize);

		TMap<FName, FArchiveDiffStats> PackageDiffStats;
		const TCHAR* CutoffString = TEXT("UEditorEngine::Save()");
		Accumulator.SetDeterminismManager(*DeterminismManager);
		Accumulator.CompareWithPrevious(CutoffString, PackageDiffStats);

		//COOK_STAT(FSavePackageStats::NumberOfDifferentPackages++);
		//COOK_STAT(FSavePackageStats::MergeStats(PackageDiffStats));
	}

	Inner->WritePackageData(LocalInfo, ExportsArchive, FileRegions);
}

bool FDiffPackageWriter::IsPackageDiffAllowed() const
{
	if (!CompareDenyListClasses.IsEmpty() && Package != nullptr)
	{
		bool bHasDenyClass = false;
		ForEachObjectWithPackage(Package, [this, &bHasDenyClass](UObject* Object)
			{
				FTopLevelAssetPath ClassPath = Object->GetClass()->GetClassPathName();
				if (CompareDenyListClasses.Contains(ClassPath))
				{
					bHasDenyClass = true;
					return false; // Stop iterating
				}
				return true; // Keep iterating
			});
		if (bHasDenyClass)
		{
			return false;
		}
	}
	return true;
}
UE::DiffWriter::FMessageCallback FDiffPackageWriter::GetDiffWriterMessageCallback()
{
	return UE::DiffWriter::FMessageCallback([this](ELogVerbosity::Type Verbosity, FStringView Message)
		{
			this->OnDiffWriterMessage(Verbosity, Message);
		});
}

void FDiffPackageWriter::OnDiffWriterMessage(ELogVerbosity::Type Verbosity, FStringView Message)
{
	FMsg::Logf(__FILE__, __LINE__, LogDiff.GetCategoryName(), Verbosity, TEXT("%s"), *ResolveText(Message));
}

FString FDiffPackageWriter::ResolveText(FStringView Message)
{
	FString ResolvedText(Message);
	check(this->Indent && this->NewLine);
	ResolvedText.ReplaceInline(UE::DiffWriter::IndentToken, this->Indent);
	ResolvedText.ReplaceInline(UE::DiffWriter::NewLineToken, this->NewLine);
	return ResolvedText;
}

UE::DiffWriter::FAccumulator& FDiffPackageWriter::ConstructAccumulator(FName PackageName, UObject* Asset,
	uint16 MultiOutputIndex)
{
	check(MultiOutputIndex < 2);
	TRefCountPtr<UE::DiffWriter::FAccumulator>& Accumulator = Accumulators[MultiOutputIndex];
	if (!Accumulator.IsValid())
	{
		check(!bHasStartedSecondSave); // Accumulator should already exist from CreateLinkerArchive in the first save
		Accumulator = new UE::DiffWriter::FAccumulator(*AccumulatorGlobals, Asset, *PackageName.ToString(), MaxDiffsToLog,
			bIgnoreHeaderDiffs, GetDiffWriterMessageCallback(), Inner->GetCookCapabilities().HeaderFormat);
	}
	return *Accumulator;
}

TUniquePtr<FLargeMemoryWriter> FDiffPackageWriter::CreateLinkerArchive(FName PackageName,
	UObject* Asset, uint16 MultiOutputIndex)
{
	UE::DiffWriter::FAccumulator& Accumulator = ConstructAccumulator(PackageName, Asset, MultiOutputIndex);
	return TUniquePtr<FLargeMemoryWriter>(new UE::DiffWriter::FDiffArchiveForLinker(Accumulator));
}

TUniquePtr<FLargeMemoryWriter> FDiffPackageWriter::CreateLinkerExportsArchive(FName PackageName,
	UObject* Asset, uint16 MultiOutputIndex)
{
	UE::DiffWriter::FAccumulator& Accumulator = ConstructAccumulator(PackageName, Asset, MultiOutputIndex);
	return TUniquePtr<FLargeMemoryWriter>(new UE::DiffWriter::FDiffArchiveForExports(Accumulator));
}

void FDiffPackageWriter::UpdateSaveArguments(FSavePackageArgs& SaveArgs)
{
	// if we are diffing optional data, add it to the save args, otherwise strip it
	if (bDiffOptional)
	{
		SaveArgs.SaveFlags |= SAVE_Optional;
	}

	Inner->UpdateSaveArguments(SaveArgs);
}


bool FDiffPackageWriter::IsAnotherSaveNeeded(FSavePackageResultStruct& PreviousResult, FSavePackageArgs& SaveArgs)
{
	checkf(!Inner->IsAnotherSaveNeeded(PreviousResult, SaveArgs),
		TEXT("DiffPackageWriter does not support an Inner that needs multiple saves."));
	if (PreviousResult == ESavePackageResult::Timeout)
	{
		return false;
	}

	// When looking for deterministic cook issues, first serialize the package to memory and do a simple diff with the
	// existing package. If the simple memory diff was not identical, collect callstacks for all Serialize calls and
	// dump differences to log
	if (!bHasStartedSecondSave)
	{
		bHasStartedSecondSave = true;
		if (PreviousResult.Result == ESavePackageResult::Success && bIsDifferent && !bNewPackage)
		{
			// The contract with the Inner is that Begin is paired with a single commit;
			// send the old commit and the new begin
			FCommitPackageInfo CommitInfo;
			CommitInfo.Status = IPackageWriter::ECommitStatus::Success;
			CommitInfo.PackageName = BeginInfo.PackageName;
			CommitInfo.WriteOptions = EWriteOptions::None;
			Inner->CommitPackage(MoveTemp(CommitInfo));
			Inner->BeginPackage(BeginInfo);

			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		return false;
	}
}

void FDiffPackageWriter::RegisterDeterminismHelper(UObject* SourceObject,
	const TRefCountPtr<UE::Cook::IDeterminismHelper>& DeterminismHelper)
{
	DeterminismManager->RegisterDeterminismHelper(SourceObject, DeterminismHelper);
}

bool FDiffPackageWriter::FilterPackageName(const FString& InWildcard)
{
	bool bInclude = false;
	FString PackageName = BeginInfo.PackageName.ToString();
	if (PackageName.MatchesWildcard(InWildcard))
	{
		bInclude = true;
	}
	else if (FPackageName::GetShortName(PackageName).MatchesWildcard(InWildcard))
	{
		bInclude = true;
	}
	else
	{
		const FString& Filename = BeginInfo.LooseFilePath;
		bInclude = Filename.MatchesWildcard(InWildcard);
	}
	return bInclude;
}

void FDiffPackageWriter::ConditionallyDumpObjList()
{
	if (bDumpObjList)
	{
		if (FilterPackageName(PackageFilter))
		{
			FString ObjListExec = TEXT("OBJ LIST ");
			ObjListExec += DumpObjListParams;

			TGuardValue GuardLogTimes(GPrintLogTimes, ELogTimes::None);
			TGuardValue GuardLogCategory(GPrintLogCategory, false);
			TGuardValue GuardPrintLogVerbosity(GPrintLogVerbosity, false);

			GEngine->Exec(nullptr, *ObjListExec);
		}
	}
}

void FDiffPackageWriter::ConditionallyDumpObjects()
{
	if (bDumpObjects)
	{
		if (FilterPackageName(PackageFilter))
		{
			TArray<FString> AllObjects;
			for (FThreadSafeObjectIterator It; It; ++It)
			{
				AllObjects.Add(*It->GetFullName());
			}
			if (bDumpObjectsSorted)
			{
				AllObjects.Sort();
			}

			TGuardValue GuardLogTimes(GPrintLogTimes, ELogTimes::None);
			TGuardValue GuardLogCategory(GPrintLogCategory, false);
			TGuardValue GuardPrintLogVerbosity(GPrintLogVerbosity, false);

			for (const FString& Obj : AllObjects)
			{
				UE_LOG(LogCook, Display, TEXT("%s"), *Obj);
			}
		}
	}
}