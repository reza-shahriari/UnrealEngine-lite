// Copyright Epic Games, Inc. All Rights Reserved.

#include "IO/PackageStore.h"

#include "Containers/StringConv.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "CoreGlobals.h"
#include "HAL/Platform.h"
#include "IO/PackageId.h"
#include "Misc/AssertionMacros.h"
#include "Serialization/Archive.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"
#include "Templates/Greater.h"

FArchive& operator<<(FArchive& Ar, FPackageStoreEntryResource& PackageStoreEntry)
{
	uint32 Flags = static_cast<uint32>(PackageStoreEntry.Flags);

	Ar << Flags;
	Ar << PackageStoreEntry.PackageName;
	Ar << PackageStoreEntry.ImportedPackageIds;
	Ar << PackageStoreEntry.OptionalSegmentImportedPackageIds;
	Ar << PackageStoreEntry.SoftPackageReferences;

	if (Ar.IsLoading())
	{
		PackageStoreEntry.PackageId = FPackageId::FromName(PackageStoreEntry.PackageName);
		PackageStoreEntry.Flags = static_cast<EPackageStoreEntryFlags>(Flags);
	}

	return Ar;
}

FCbWriter& operator<<(FCbWriter& Writer, const FPackageStoreEntryResource& PackageStoreEntry)
{
	Writer.BeginObject();

	Writer << "flags" << static_cast<uint32>(PackageStoreEntry.Flags);
	Writer << "packagename" << PackageStoreEntry.PackageName.ToString();

	if (PackageStoreEntry.ImportedPackageIds.Num())
	{
		Writer.BeginArray("importedpackageids");
		for (const FPackageId& ImportedPackageId : PackageStoreEntry.ImportedPackageIds)
		{
			Writer << ImportedPackageId.Value();
		}
		Writer.EndArray();
	}

	if (PackageStoreEntry.ShaderMapHashes.Num())
	{
		Writer.BeginArray("shadermaphashes");
		for (const FSHAHash& ShaderMapHash : PackageStoreEntry.ShaderMapHashes)
		{
			Writer << ShaderMapHash.ToString();
		}
		Writer.EndArray();
	}

	if (PackageStoreEntry.OptionalSegmentImportedPackageIds.Num())
	{
		Writer.BeginArray("optionalsegmentimportedpackageids");
		for (const FPackageId& ImportedPackageId : PackageStoreEntry.OptionalSegmentImportedPackageIds)
		{
			Writer << ImportedPackageId.Value();
		}
		Writer.EndArray();
	}

	if (PackageStoreEntry.SoftPackageReferences.Num())
	{
		Writer.BeginArray("softpackagereferences");
		for (const FPackageId& SoftRef : PackageStoreEntry.SoftPackageReferences)
		{
			Writer << SoftRef.Value();
		}
		Writer.EndArray();
	}

	Writer.EndObject();

	return Writer;
}

FPackageStoreEntryResource FPackageStoreEntryResource::FromCbObject(FCbObjectView Obj)
{
	FPackageStoreEntryResource Entry;

	Entry.Flags				= static_cast<EPackageStoreEntryFlags>(Obj["flags"].AsUInt32());
	Entry.PackageName		= FName(Obj["packagename"].AsString());
	Entry.PackageId			= FPackageId::FromName(Entry.PackageName);
	
	if (Obj["importedpackageids"])
	{
		for (FCbFieldView ArrayField : Obj["importedpackageids"])
		{
			Entry.ImportedPackageIds.Add(FPackageId::FromValue(ArrayField .AsUInt64()));
		}
	}
	
	if (Obj["shadermaphashes"])
	{
		for (FCbFieldView& ArrayField : Obj["shadermaphashes"].AsArrayView())
		{
			FSHAHash& ShaderMapHash = Entry.ShaderMapHashes.AddDefaulted_GetRef();
			ShaderMapHash.FromString(FUTF8ToTCHAR(ArrayField.AsString()));
		}
	}

	if (Obj["optionalsegmentimportedpackageids"])
	{
		for (FCbFieldView ArrayField : Obj["optionalsegmentimportedpackageids"])
		{
			Entry.OptionalSegmentImportedPackageIds.Add(FPackageId::FromValue(ArrayField.AsUInt64()));
		}
	}

	if (Obj["softpackagereferences"])
	{
		for (FCbFieldView ArrayField : Obj["softpackagereferences"])
		{
			Entry.SoftPackageReferences.Add(FPackageId::FromValue(ArrayField.AsUInt64()));
		}
	}

	return Entry;
}

FPackageStoreEntryResource FPackageStoreEntryResource::CreateEmptyPackage(FName PackageName, bool bHasCookError)
{
	FPackageStoreEntryResource Entry;
	Entry.PackageName = PackageName;
	Entry.PackageId = FPackageId::FromName(PackageName);
	Entry.Flags = bHasCookError ? EPackageStoreEntryFlags::HasCookError : EPackageStoreEntryFlags::None;
	return Entry;
}

thread_local int32 FPackageStore::ThreadReadCount = 0;

FPackageStoreReadScope::FPackageStoreReadScope(FPackageStore& InPackageStore)
	: PackageStore(InPackageStore)
{
	if (!PackageStore.ThreadReadCount)
	{
		for (const FPackageStore::FBackendAndPriority& Backend : PackageStore.Backends)
		{
			Backend.Value->BeginRead();
		}
	}
	++PackageStore.ThreadReadCount;
}

FPackageStoreReadScope::~FPackageStoreReadScope()
{
	check(PackageStore.ThreadReadCount > 0);
	if (--PackageStore.ThreadReadCount == 0)
	{
		for (const FPackageStore::FBackendAndPriority& Backend : PackageStore.Backends)
		{
			Backend.Value->EndRead();
		}
	}
}

FPackageStore::FPackageStore()
	: BackendContext(MakeShared<FPackageStoreBackendContext>())
{
	
}

FPackageStore& FPackageStore::Get()
{
	static FPackageStore Instance;
	return Instance;
}


void FPackageStore::Mount(TSharedRef<IPackageStoreBackend> Backend, int32 Priority)
{
	check(IsInGameThread());
	int32 Index = Algo::LowerBoundBy(Backends, Priority, &FBackendAndPriority::Key, TGreater<>());
	Backends.Insert(MakeTuple(Priority, Backend), Index);
	Backend->OnMounted(BackendContext);
}

EPackageStoreEntryStatus FPackageStore::GetPackageStoreEntry(FPackageId PackageId, FName PackageName,
	FPackageStoreEntry& OutPackageStoreEntry)
{
	check(ThreadReadCount);
	for (const FBackendAndPriority& Backend : Backends)
	{
		EPackageStoreEntryStatus Status = Backend.Value->GetPackageStoreEntry(PackageId, PackageName, OutPackageStoreEntry);
		const bool bContinueSearch = 
			Status == EPackageStoreEntryStatus::None || 
			Status == EPackageStoreEntryStatus::Missing;

		if (!bContinueSearch)
		{
			return Status;
		}
	}

	return EPackageStoreEntryStatus::Missing;
}

bool FPackageStore::GetPackageRedirectInfo(FPackageId PackageId, FName& OutSourcePackageName, FPackageId& OutRedirectedToPackageId)
{
	check(ThreadReadCount);
	for (const FBackendAndPriority& Backend : Backends)
	{
		if (Backend.Value->GetPackageRedirectInfo(PackageId, OutSourcePackageName, OutRedirectedToPackageId))
		{
			return true;
		}
	}
	return false;
}

TConstArrayView<uint32> FPackageStore::GetSoftReferences(FPackageId PackageId, TConstArrayView<FPackageId>& OutPackageIds)
{
	check(ThreadReadCount);
	for (const FBackendAndPriority& Backend : Backends)
	{
		if (TConstArrayView<uint32> SoftRefs = Backend.Value->GetSoftReferences(PackageId, OutPackageIds); !SoftRefs.IsEmpty())
		{
			return SoftRefs;
		}
	}
	return TConstArrayView<uint32>();
}

FPackageStoreBackendContext::FPendingEntriesAddedEvent& FPackageStore::OnPendingEntriesAdded()
{
	return BackendContext->PendingEntriesAdded;
}

bool FPackageStore::HasAnyBackendsMounted() const
{
	return !Backends.IsEmpty();
}
