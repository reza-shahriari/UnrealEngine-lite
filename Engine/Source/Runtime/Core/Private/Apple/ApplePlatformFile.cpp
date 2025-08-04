// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ApplePlatformFile.mm: Apple platform implementations of File functions
=============================================================================*/

#include "Apple/ApplePlatformFile.h"

#include "Containers/UnrealString.h"
#include "Containers/StringConv.h"
#include "CoreGlobals.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "ProfilingDebugging/PlatformFileTrace.h"
#include "Templates/Function.h"
#include <sys/stat.h>

#include "Async/MappedFileHandle.h"
#include <sys/mman.h>

// make an FTimeSpan object that represents the "epoch" for time_t (from a stat struct)
const FDateTime MacEpoch(1970, 1, 1);

namespace
{
	FFileStatData MacStatToUEFileData(struct stat& FileInfo)
	{
		const bool bIsDirectory = S_ISDIR(FileInfo.st_mode);

		int64 FileSize = -1;
		if (!bIsDirectory)
		{
			FileSize = FileInfo.st_size;
		}

		return FFileStatData(
			MacEpoch + FTimespan::FromSeconds(FileInfo.st_ctime), 
			MacEpoch + FTimespan::FromSeconds(FileInfo.st_atime), 
			MacEpoch + FTimespan::FromSeconds(FileInfo.st_mtime), 
			FileSize,
			bIsDirectory,
			!(FileInfo.st_mode & S_IWUSR)
		);
	}
}

/** 
 * Mac file handle implementation which limits number of open files per thread. This
 * is to prevent running out of system file handles (250). Should not be neccessary when
 * using pak file (e.g., SHIPPING?) so not particularly optimized. Only manages
 * files which are opened READ_ONLY.
**/
#define MANAGE_FILE_HANDLES PLATFORM_MAC // !UE_BUILD_SHIPPING

class CORE_API FFileHandleApple : public IFileHandle
{
	enum {READWRITE_SIZE = 1024 * 1024};

public:
	FFileHandleApple(int32 InFileHandle, const TCHAR* InFilename, bool bIsReadOnly)
		: FileHandle(InFileHandle)
#if MANAGE_FILE_HANDLES
		, Filename(InFilename)
		, HandleSlot(-1)
		, FileOffset(0)
		, FileSize(0)
#endif
        , bReadOnly(bIsReadOnly)
	{
		check(FileHandle > -1);

#if MANAGE_FILE_HANDLES
		// Only files opened for read will be managed
		if (bIsReadOnly)
		{
			ReserveSlot();
			ActiveHandles[HandleSlot] = this;
			struct stat FileInfo;
			fstat(FileHandle, &FileInfo);
			FileSize = FileInfo.st_size;
		}
#endif
	}
	virtual ~FFileHandleApple()
	{
#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			if( ActiveHandles[ HandleSlot ] == this )
			{
				TRACE_PLATFORMFILE_BEGIN_CLOSE(FileHandle);
				flock(FileHandle, LOCK_UN | LOCK_NB);
				int CloseResult = close(FileHandle);
				if (CloseResult >= 0)
				{
					TRACE_PLATFORMFILE_END_CLOSE(FileHandle);
				}
				else
				{
					TRACE_PLATFORMFILE_FAIL_CLOSE(FileHandle);
					UE_LOG(LogInit, Warning, TEXT("Failed to properly close readable file: %s with errno: %d: %s"),
						*Filename, errno, UTF8_TO_TCHAR(strerror(errno)));
				}
				ActiveHandles[ HandleSlot ] = nullptr;
			}
		}
		else
#endif
		{
            if (!bReadOnly)
            {
                int Result = fsync(FileHandle);
				if (Result < 0)
				{
					UE_LOG(LogInit, Error, TEXT("Failed to properly flush writable file with errno: %d: %s"),
						errno, UTF8_TO_TCHAR(strerror(errno)));
				}
            }
			TRACE_PLATFORMFILE_BEGIN_CLOSE(FileHandle);
			flock(FileHandle, LOCK_UN | LOCK_NB);
			int CloseResult = close(FileHandle);
			if (CloseResult >= 0)
			{
				TRACE_PLATFORMFILE_END_CLOSE(FileHandle);
			}
			else
			{
				TRACE_PLATFORMFILE_FAIL_CLOSE(FileHandle);
				UE_LOG(LogInit, Warning, TEXT("Failed to properly close file with errno: %d: %s"),
					errno, UTF8_TO_TCHAR(strerror(errno)));
			}
		}
		FileHandle = -1;
	}
	virtual int64 Tell() override
	{
#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			return FileOffset;
		}
		else
#endif
		{
			check(IsValid());
			return lseek(FileHandle, 0, SEEK_CUR);
		}
	}
	virtual bool Seek(int64 NewPosition) override
	{
		check(NewPosition >= 0);

#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			FileOffset = NewPosition >= FileSize ? FileSize - 1 : NewPosition;
			return IsValid() && ActiveHandles[ HandleSlot ] == this ? lseek(FileHandle, FileOffset, SEEK_SET) != -1 : true;
		}
		else
#endif
		{
			check(IsValid());
			return lseek(FileHandle, NewPosition, SEEK_SET) != -1;
		}
	}
	virtual bool SeekFromEnd(int64 NewPositionRelativeToEnd = 0) override
	{
		check(NewPositionRelativeToEnd <= 0);

#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			FileOffset = (NewPositionRelativeToEnd >= FileSize) ? 0 : ( FileSize + NewPositionRelativeToEnd - 1 );
			return IsValid() && ActiveHandles[ HandleSlot ] == this ? lseek(FileHandle, FileOffset, SEEK_SET) != -1 : true;
		}
		else
#endif
		{
			check(IsValid());
			return lseek(FileHandle, NewPositionRelativeToEnd, SEEK_END) != -1;
		}
	}
	virtual bool Read(uint8* Destination, int64 BytesToRead) override
	{
#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			ActivateSlot();
			int64 BytesRead = ReadInternal(Destination, BytesToRead);
			FileOffset += BytesRead;
			return BytesRead == BytesToRead;
		}
		else
#endif
		{
			return ReadInternal(Destination, BytesToRead) == BytesToRead;
		}
	}
	virtual bool ReadAt(uint8* Destination, int64 BytesToRead, int64 Offset) override
	{
		if (BytesToRead < 0 || Offset < 0)
		{
			return false;
		}

		if (BytesToRead == 0)
		{
			return true;
		}

#if MANAGE_FILE_HANDLES
		if (IsManaged())
		{
			ActivateSlot();
		}
#endif //MANAGE_FILE_HANDLES

		int64 TotalBytesRead = 0;
		TRACE_PLATFORMFILE_BEGIN_READ(this, FileHandle, Offset, BytesToRead);

		do
		{
			size_t BytesToRead32 = static_cast<size_t>(FMath::Min<int64>(READWRITE_SIZE, BytesToRead));
			ssize_t BytesRead = pread(FileHandle, Destination, BytesToRead, Offset);

			TotalBytesRead += BytesRead;

			if (BytesRead != BytesToRead32)
			{
				TRACE_PLATFORMFILE_END_READ(this, TotalBytesRead);
				return false;
			}

			Offset += BytesRead;
			BytesToRead -= BytesToRead32;

		} while (BytesToRead > 0);

		TRACE_PLATFORMFILE_END_READ(this, TotalBytesRead);

		return true;
	}
	virtual bool Write(const uint8* Source, int64 BytesToWrite) override
	{
		check(IsValid());
		TRACE_PLATFORMFILE_BEGIN_WRITE(this, FileHandle, 0, BytesToWrite);
		int64 TotalBytesWritten = 0;
		while (BytesToWrite > 0)
		{
			int64 ThisSize = FMath::Min<int64>(READWRITE_SIZE, BytesToWrite);
			check(Source);
			int64 BytesWritten = write(FileHandle, Source, ThisSize);
            if (BytesWritten <= 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }
                else
                {
                    TRACE_PLATFORMFILE_END_WRITE(this, TotalBytesWritten);
                    return false;
                }
            }
            TotalBytesWritten += BytesWritten;
			Source += BytesWritten;
			BytesToWrite -= BytesWritten;
		}
		TRACE_PLATFORMFILE_END_WRITE(this, TotalBytesWritten);
		return true;
	}
	virtual bool Flush(const bool bFullFlush = false) override
	{
		check(IsValid());
#if MANAGE_FILE_HANDLES
		if (IsManaged())
		{
			return false;
		}
#endif
		if (bFullFlush)
		{
			// OS X needs fcntl with F_FULLFSYNC to guarantee a full flush,
			// but still fallback to fsync if fcntl fails
			if (fcntl(FileHandle, F_FULLFSYNC) == 0)
			{
				return true;
			}	
		}
		// HFS+ apparently doesn't always write the updated file size when using fdatasync, so use fsync to be safe
		return fsync(FileHandle) == 0;
	}
	virtual bool Truncate(int64 NewSize) override
	{
		check(IsValid());
#if MANAGE_FILE_HANDLES
		if (IsManaged())
		{
			return false;
		}
#endif
		int Result = 0;
		do { Result = ftruncate(FileHandle, NewSize); } while (Result < 0 && errno == EINTR);
		return Result == 0;
	}
	virtual int64 Size() override
	{
#if MANAGE_FILE_HANDLES
		if( IsManaged() )
		{
			return FileSize;
		}
		else
#endif
		{
			struct stat FileInfo;
			fstat(FileHandle, &FileInfo);
			return FileInfo.st_size;
		}
	}

private:

#if MANAGE_FILE_HANDLES
	FORCEINLINE bool IsManaged()
	{
		return HandleSlot != -1;
	}

	void ActivateSlot()
	{
		if( IsManaged() )
		{
			if( ActiveHandles[ HandleSlot ] != this || (ActiveHandles[ HandleSlot ] && ActiveHandles[ HandleSlot ]->FileHandle == -1) )
			{
				ReserveSlot();

				TRACE_PLATFORMFILE_BEGIN_OPEN(*Filename);
				FileHandle = open(TCHAR_TO_UTF8(*Filename), O_RDONLY | O_SHLOCK | O_CLOEXEC);
				if( FileHandle != -1 )
				{
					TRACE_PLATFORMFILE_END_OPEN(FileHandle);
					lseek(FileHandle, FileOffset, SEEK_SET);
					ActiveHandles[ HandleSlot ] = this;
				}
				else
				{
					TRACE_PLATFORMFILE_FAIL_OPEN(*Filename);
				}
			}
			else
			{
				AccessTimes[ HandleSlot ] = FPlatformTime::Seconds();
			}
		}
	}

	void ReserveSlot()
	{
		HandleSlot = -1;

		// Look for non-reserved slot
		for( int32 i = 0; i < ACTIVE_HANDLE_COUNT; ++i )
		{
			if( ActiveHandles[ i ] == nullptr )
			{
				HandleSlot = i;
				break;
			}
		}

		// Take the oldest handle
		if( HandleSlot == -1 )
		{
			int32 Oldest = 0;
			for( int32 i = 1; i < ACTIVE_HANDLE_COUNT; ++i )
			{
				if( AccessTimes[ Oldest ] > AccessTimes[ i ] )
				{
					Oldest = i;
				}
			}

			int32 OldestFileHandle = ActiveHandles[ Oldest ]->FileHandle;
			TRACE_PLATFORMFILE_BEGIN_CLOSE(OldestFileHandle);
			flock(OldestFileHandle, LOCK_UN | LOCK_NB);
			int CloseResult = close(OldestFileHandle);
#if PLATFORMFILETRACE_ENABLED
			if (CloseResult >= 0)
			{
				TRACE_PLATFORMFILE_END_CLOSE(OldestFileHandle);
			}
			else
			{
				TRACE_PLATFORMFILE_FAIL_CLOSE(OldestFileHandle);
			}
#else
			(void)CloseResult;
#endif
			ActiveHandles[ Oldest ]->FileHandle = -1;
			HandleSlot = Oldest;
		}

		ActiveHandles[ HandleSlot ] = nullptr;
		AccessTimes[ HandleSlot ] = FPlatformTime::Seconds();
	}
#endif

	int64 ReadInternal(uint8* Destination, int64 BytesToRead)
	{
		check(IsValid());
		int64 MaxReadSize = READWRITE_SIZE;
		int64 BytesRead = 0;
		TRACE_PLATFORMFILE_BEGIN_READ(this, FileHandle, 0, BytesToRead);
		while (BytesToRead)
		{
			check(BytesToRead >= 0);
			int64 ThisSize = FMath::Min<int64>(MaxReadSize, BytesToRead);
			check(Destination);
			int64 ThisRead = read(FileHandle, Destination, ThisSize);
			if (ThisRead == -1)
			{
				// Reading from smb can sometimes result in a EINVAL error. Try again a few times with a smaller read buffer.
				if (errno == EINVAL && MaxReadSize > 1024)
				{
					MaxReadSize /= 2;
					continue;
				}
				TRACE_PLATFORMFILE_END_READ(this, BytesRead);
				return BytesRead;
			}
			BytesRead += ThisRead;
			if (ThisRead != ThisSize)
			{
				TRACE_PLATFORMFILE_END_READ(this, BytesRead);
				return BytesRead;
			}
			Destination += ThisSize;
			BytesToRead -= ThisSize;
		}
		TRACE_PLATFORMFILE_END_READ(this, BytesRead);
		return BytesRead;
	}

	// Holds the internal file handle.
	int32 FileHandle;

#if MANAGE_FILE_HANDLES
	// Holds the name of the file that this handle represents. Kept around for possible reopen of file.
	FString Filename;

    // Most recent valid slot index for this handle; >=0 for handles which are managed.
    int32 HandleSlot;

    // Current file offset; valid if a managed handle.
    int64 FileOffset;

    // Cached file size; valid if a managed handle.
    int64 FileSize;

    // Each thread keeps a collection of active handles with access times.
    static const int32 ACTIVE_HANDLE_COUNT = 192;
    static __thread FFileHandleApple* ActiveHandles[ ACTIVE_HANDLE_COUNT ];
    static __thread double AccessTimes[ ACTIVE_HANDLE_COUNT ];
#endif

    // Whether the file is read-only or permits writes.
    bool bReadOnly;
    
	FORCEINLINE bool IsValid()
	{
		return FileHandle != -1;
	}
};

#if MANAGE_FILE_HANDLES
__thread FFileHandleApple* FFileHandleApple::ActiveHandles[ FFileHandleApple::ACTIVE_HANDLE_COUNT ];
__thread double FFileHandleApple::AccessTimes[ FFileHandleApple::ACTIVE_HANDLE_COUNT ];
#endif

static SIZE_T FileMappingAlignment = FPlatformMemory::GetConstants().PageSize;

class FMappedFileRegion final : public IMappedFileRegion
{
public:
	class FMappedFileHandle* Parent;
	const uint8* AlignedPtr;
	uint64 AlignedSize;
	
	FMappedFileRegion(const uint8* InMappedPtr, const uint8* InAlignedPtr, size_t InMappedSize, uint64 InAlignedSize, const FString& InDebugFilename, size_t InDebugOffsetIntoFile, class FMappedFileHandle* InParent)
		: IMappedFileRegion(InMappedPtr, InMappedSize, InDebugFilename, InDebugOffsetIntoFile)
		, Parent(InParent)
		, AlignedPtr(InAlignedPtr)
		, AlignedSize(InAlignedSize)
	{
	}
	
	~FMappedFileRegion();
	
	virtual void PreloadHint(int64 PreloadOffset = 0, int64 BytesToPreload = MAX_int64) override
	{
		int64 Size = GetMappedSize();
		const uint8* Ptr = GetMappedPtr();
		int32 FoolTheOptimizer = 0;
		while (Size > 0)
		{
			FoolTheOptimizer += Ptr[0];
			Size -= 4096;
			Ptr += 4096;
		}
		if (FoolTheOptimizer == 0xbadf00d)
		{
			FPlatformProcess::Sleep(0.0f); // this will more or less never happen, but we can't let the optimizer strip these reads
		}
	}
	
};

class FMappedFileHandle final : public IMappedFileHandle
{
	const uint8* MappedPtr;
	FString Filename;
	std::atomic<int32> NumOutstandingRegions;
	int FileHandle;
	
public:
	
	FMappedFileHandle(int InFileHandle, int64 FileSize, const FString& InFilename)
		: IMappedFileHandle(FileSize)
		, MappedPtr(nullptr)
#if !UE_BUILD_SHIPPING
		, Filename(InFilename)
#endif
		, NumOutstandingRegions(0)
		, FileHandle(InFileHandle)
	{
	}

	~FMappedFileHandle()
	{
		// can't delete the file before you delete all outstanding regions
		UE_CLOG(
				NumOutstandingRegions.load(std::memory_order_relaxed),
				LogHAL,
#if UE_BUILD_SHIPPING
				Error,
#else
				Fatal,
#endif
				TEXT("Cleaning mapped file with alive mapped regions: %s"),
				*Filename);	
		close(FileHandle);
	}
	
	virtual IMappedFileRegion* MapRegion(int64 Offset = 0, int64 BytesToMap = MAX_int64, FFileMappingFlags Flags = EMappedFileFlags::ENone) override
	{
		LLM_PLATFORM_SCOPE(ELLMTag::PlatformMMIO);
		check(Offset < GetFileSize()); // don't map zero bytes and don't map off the end of the file
		BytesToMap = FMath::Min<int64>(BytesToMap, GetFileSize() - Offset);
		check(BytesToMap > 0); // don't map zero bytes
		
		const int64 AlignedOffset = AlignDown(Offset, FileMappingAlignment);
		//File mapping can extend beyond file size. It's OK, kernel will just fill any leftover page data with zeros
		const int64 AlignedSize = Align(BytesToMap + Offset - AlignedOffset, FileMappingAlignment);
		
		int Protection = PROT_READ;
		int InternalFlags = 0;
		if (EnumHasAnyFlags(Flags.Flags, EMappedFileFlags::EFileWritable))
		{
			Protection |= PROT_WRITE;
			InternalFlags |= MAP_SHARED;
		}
		else
		{
			InternalFlags |= MAP_PRIVATE;
		}

		const uint8* AlignedMapPtr = (const uint8 *)mmap(NULL, AlignedSize, Protection, InternalFlags, FileHandle, AlignedOffset);
		if (AlignedMapPtr == (const uint8*)-1 || AlignedMapPtr == nullptr)
		{
			UE_LOG(LogHAL, Warning, TEXT("Failed to map memory %s, error is %d"), *Filename, errno);
			return nullptr;
		}
		LLM_IF_ENABLED(FLowLevelMemTracker::Get().OnLowLevelAlloc(ELLMTracker::Platform, AlignedMapPtr, AlignedSize));

		// create a mapping for this range
		const uint8* MapPtr = AlignedMapPtr + Offset - AlignedOffset;
		FMappedFileRegion* Result = new FMappedFileRegion(MapPtr, AlignedMapPtr, BytesToMap, AlignedSize, Filename, Offset, this);

		NumOutstandingRegions.fetch_add(1, std::memory_order_relaxed);

		return Result;
	}
	
	void UnMap(FMappedFileRegion* Region)
	{
		LLM_PLATFORM_SCOPE(ELLMTag::PlatformMMIO);
		
		int32 OldNumOutstandingRegions = NumOutstandingRegions.fetch_sub(1, std::memory_order_relaxed);
		check(OldNumOutstandingRegions > 0);	
		
		LLM_IF_ENABLED(FLowLevelMemTracker::Get().OnLowLevelFree(ELLMTracker::Platform, (void*)Region->AlignedPtr));
		int Res = munmap((void*)Region->AlignedPtr, Region->AlignedSize);
		checkf(Res == 0, TEXT("Failed to unmap, error is %d, errno is %d [params: %x, %d]"), Res, errno, MappedPtr, GetFileSize());
	}
};

FMappedFileRegion::~FMappedFileRegion()
{
	Parent->UnMap(this);
}

/**
 * Mac File I/O implementation
**/
FString FApplePlatformFile::NormalizeFilename(const TCHAR* Filename)
{
	FString Result(Filename);
	Result.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	return Result;
}
FString FApplePlatformFile::NormalizeDirectory(const TCHAR* Directory)
{
	FString Result(Directory);
	Result.ReplaceInline(TEXT("\\"), TEXT("/"), ESearchCase::CaseSensitive);
	return Result;
}

bool FApplePlatformFile::FileExists(const TCHAR* Filename)
{
	struct stat FileInfo;
	if (Stat(Filename, &FileInfo) != -1)
	{
		return S_ISREG(FileInfo.st_mode);
	}
	return false;
}
int64 FApplePlatformFile::FileSize(const TCHAR* Filename)
{
	struct stat FileInfo;
	FileInfo.st_size = -1;
	if (Stat(Filename, &FileInfo) != 0)
	{
		return -1;
	}
	
	// make sure to return -1 for directories
	if (S_ISDIR(FileInfo.st_mode))
	{
		FileInfo.st_size = -1;
	}
	return FileInfo.st_size;
}
bool FApplePlatformFile::DeleteFile(const TCHAR* Filename)
{
	return unlink(TCHAR_TO_UTF8(*NormalizeFilename(Filename))) == 0;
}
bool FApplePlatformFile::IsReadOnly(const TCHAR* Filename)
{
	if (access(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), F_OK) == -1)
	{
		return false; // file doesn't exist
	}
	if (access(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), W_OK) == -1)
	{
		return errno == EACCES;
	}
	return false;
}
bool FApplePlatformFile::MoveFile(const TCHAR* To, const TCHAR* From)
{
	int32 Result = rename(TCHAR_TO_UTF8(*NormalizeFilename(From)), TCHAR_TO_UTF8(*NormalizeFilename(To)));
	if (Result == -1 && errno == EXDEV)
	{
		// Copy the file if rename failed because To and From are on different file systems
		if (CopyFile(To, From))
		{
			DeleteFile(From);
			Result = 0;
		}
	}
	return Result != -1;
}
bool FApplePlatformFile::SetReadOnly(const TCHAR* Filename, bool bNewReadOnlyValue)
{
	struct stat FileInfo;
	if (Stat(Filename, &FileInfo) == 0)
	{
		if (bNewReadOnlyValue)
		{
			FileInfo.st_mode &= ~S_IWUSR;
		}
		else
		{
			FileInfo.st_mode |= S_IWUSR;
		}
		return chmod(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), FileInfo.st_mode) == 0;
	}
	return false;
}


FDateTime FApplePlatformFile::GetTimeStamp(const TCHAR* Filename)
{
	// get file times
	struct stat FileInfo;
	if(Stat(Filename, &FileInfo) != 0)
	{
		return FDateTime::MinValue();
	}

	// convert _stat time to FDateTime
	FTimespan TimeSinceEpoch(0, 0, FileInfo.st_mtime);
	return MacEpoch + TimeSinceEpoch;

}

void FApplePlatformFile::SetTimeStamp(const TCHAR* Filename, const FDateTime DateTime)
{
	// get file times
	struct stat FileInfo;
	if (Stat(Filename, &FileInfo) != 0)
	{
		return;
	}

	// change the modification time only
	struct utimbuf Times;
	Times.actime = FileInfo.st_atime;
	Times.modtime = (time_t)(DateTime - MacEpoch).GetTotalSeconds();
	utime(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), &Times);
}

FDateTime FApplePlatformFile::GetAccessTimeStamp(const TCHAR* Filename)
{
	// get file times
	struct stat FileInfo;
	if(Stat(Filename, &FileInfo) == -1)
	{
		return FDateTime::MinValue();
	}

	// convert _stat time to FDateTime
	FTimespan TimeSinceEpoch(0, 0, FileInfo.st_atime);
	return MacEpoch + TimeSinceEpoch;
}

FString FApplePlatformFile::GetFilenameOnDisk(const TCHAR* Filename)
{
	return Filename;
}

ESymlinkResult FApplePlatformFile::IsSymlink(const TCHAR* Filename)
{
	struct stat FileInfo;
	if (Stat(Filename, &FileInfo) != -1 && S_ISLNK(FileInfo.st_mode))
	{
		return ESymlinkResult::Symlink;
	}
	return ESymlinkResult::NonSymlink;
}

IFileHandle* FApplePlatformFile::OpenRead(const TCHAR* Filename, bool bAllowWrite)
{
	TRACE_PLATFORMFILE_BEGIN_OPEN(Filename);
	int32 Handle = open(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), O_RDONLY | O_CLOEXEC);
	if (Handle != -1)
	{
		TRACE_PLATFORMFILE_END_OPEN(Handle);
#if PLATFORM_MAC && UE_EDITOR && !UE_BUILD_SHIPPING
		// No blocking attempt shared lock, failure means we should not have opened the file for reading, protect against multiple instances and client/server versions
		if(flock(Handle, LOCK_NB | LOCK_SH) != 0)
		{
			TRACE_PLATFORMFILE_BEGIN_CLOSE(Handle);
			int CloseResult = close(Handle);
#if PLATFORMFILETRACE_ENABLED
			if (CloseResult >= 0)
			{
				TRACE_PLATFORMFILE_END_CLOSE(Handle);
			}
			else
			{
				TRACE_PLATFORMFILE_FAIL_CLOSE(Handle);
			}
#else
			(void)CloseResult;
#endif
			return nullptr;
		}
#endif
		
#if MANAGE_FILE_HANDLES
		return new FFileHandleApple(Handle, *NormalizeDirectory(Filename), true);
#else
		return new FFileHandleApple(Handle, Filename, true);
#endif
	}
	else
	{
		TRACE_PLATFORMFILE_FAIL_OPEN(Filename);
		return nullptr;
	}
}

IFileHandle* FApplePlatformFile::OpenWrite(const TCHAR* Filename, bool bAppend, bool bAllowRead)
{
	int Flags = O_CREAT | O_CLOEXEC;
	
	if (bAllowRead)
	{
		Flags |= O_RDWR;
	}
	else
	{
		Flags |= O_WRONLY;
	}
	
	TRACE_PLATFORMFILE_BEGIN_OPEN(Filename);
	int32 Handle = open(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), Flags, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
	
	if (Handle != -1)
	{
		TRACE_PLATFORMFILE_END_OPEN(Handle);
#if PLATFORM_MAC && UE_EDITOR && !UE_BUILD_SHIPPING
		// No blocking attempt EXclusive lock, failure means we should not have opened the file for writing, protect against multiple instances and client/server versions
		if(flock(Handle, LOCK_NB | LOCK_EX) != 0)
		{
			TRACE_PLATFORMFILE_BEGIN_CLOSE(Handle);
			int CloseResult = close(Handle);
#if PLATFORMFILETRACE_ENABLED
			if (CloseResult >= 0)
			{
				TRACE_PLATFORMFILE_END_CLOSE(Handle);
			}
			else
			{
				TRACE_PLATFORMFILE_FAIL_CLOSE(Handle);
			}
#else
			(void)CloseResult;
#endif
			return nullptr;
		}
		
		// We have created the writer, if reading is required downgrade the lock to SHared
		if(bAllowRead)
		{
			flock(Handle, LOCK_NB | LOCK_SH);
		}
#endif
		
		// Truncate after locking as lock may fail - don't use O_TRUNC in open flags
		if(!bAppend)
		{
			ftruncate(Handle, 0);
		}

#if MANAGE_FILE_HANDLES
		FFileHandleApple* FileHandleApple = new FFileHandleApple(Handle, *NormalizeDirectory(Filename), false);
#else
		FFileHandleApple* FileHandleApple = new FFileHandleApple(Handle, Filename, false);
#endif
		if (bAppend)
		{
			FileHandleApple->SeekFromEnd(0);
		}
		return FileHandleApple;
	}
	else
	{
		TRACE_PLATFORMFILE_FAIL_OPEN(Filename);
		return nullptr;
	}
}

FOpenMappedResult FApplePlatformFile::OpenMappedEx(const TCHAR* Filename, EOpenReadFlags OpenOptions, int64 MaximumSize)
{
	FString NormalizedFilename = NormalizeFilename(Filename);
	
	// check the read path
	FString FinalPath = ConvertToPlatformPath(NormalizedFilename, false, false);
	FILE* FP;
	const char* OpenMode = EnumHasAnyFlags(OpenOptions, EOpenReadFlags::AllowWrite) ? "r+" : "r";
	FP = fopen(TCHAR_TO_UTF8(*FinalPath), OpenMode);
	if(FP == nullptr)
	{
		// if not in the read path, check the private write path
		FinalPath = ConvertToPlatformPath(NormalizedFilename, true, false);
		FP = fopen(TCHAR_TO_UTF8(*FinalPath), OpenMode);

		if(FP == nullptr)
		{
			// if not in the private write path, check the public write path
			FinalPath = ConvertToPlatformPath(NormalizedFilename, true, true);
			FP = fopen(TCHAR_TO_UTF8(*FinalPath), OpenMode);
		}
	}
	
	if (FP != nullptr)
	{
		int32 Handle = fileno(FP);
	
		if (Handle != -1)
		{
			struct stat FileInfo;
			FileInfo.st_size = -1;
			// check the read path
			if(fstat(Handle, &FileInfo) == -1)
			{
				return MakeError(FString::Printf(TEXT("FApplePlatformFile::OpenMappedEx failed to get file info for file '%s'"), *NormalizedFilename));
			}
			uint64 FileSize = FileInfo.st_size;

			return MakeValue(new FMappedFileHandle(Handle, FileSize, FinalPath));
		}
	}
	
	return MakeError(FString::Printf(TEXT("FApplePlatformFile::OpenMappedEx failed to open file '%s' in '%s' mode"), *NormalizedFilename, EnumHasAnyFlags(OpenOptions, EOpenReadFlags::AllowWrite) ? TEXT("write") : TEXT("read")));
}

bool FApplePlatformFile::DirectoryExists(const TCHAR* Directory)
{
	struct stat FileInfo;
	if (Stat(Directory, &FileInfo) != -1)
	{
		return S_ISDIR(FileInfo.st_mode);
	}
	return false;
}

bool FApplePlatformFile::CreateDirectory(const TCHAR* Directory)
{
	SCOPED_AUTORELEASE_POOL;

	// Using native functions on program targets to be able to use detour and remote helpers
#if IS_PROGRAM
	if (mkdir(TCHAR_TO_UTF8(*NormalizeFilename(Directory)), 0775) == 0)
		return true;
	return errno == EEXIST;
#else
	CFStringRef CFDirectory = FPlatformString::TCHARToCFString(*NormalizeFilename(Directory));
	bool Result = [[NSFileManager defaultManager] createDirectoryAtPath:(NSString*)CFDirectory withIntermediateDirectories:true attributes:nil error:nil];
	CFRelease(CFDirectory);
	return Result;
#endif
}

bool FApplePlatformFile::DeleteDirectory(const TCHAR* Directory)
{
	return rmdir(TCHAR_TO_UTF8(*NormalizeFilename(Directory))) == 0;
}

FFileStatData FApplePlatformFile::GetStatData(const TCHAR* FilenameOrDirectory)
{
	struct stat FileInfo;
	if (Stat(FilenameOrDirectory, &FileInfo) != -1)
	{
		return MacStatToUEFileData(FileInfo);
	}

	return FFileStatData();
}

bool FApplePlatformFile::IterateDirectory(const TCHAR* Directory, FDirectoryVisitor& Visitor)
{
	const FString DirectoryStr = Directory;
	const FString NormalizedDirectoryStr = NormalizeFilename(Directory);

	return IterateDirectoryCommon(Directory, [&Visitor, &DirectoryStr, &NormalizedDirectoryStr](struct dirent* InEntry) -> bool
	{
		SCOPED_AUTORELEASE_POOL;

		// Normalize any unicode forms so we match correctly
		const FString NormalizedFilename = UTF8_TO_TCHAR(([[[NSString stringWithUTF8String:InEntry->d_name] precomposedStringWithCanonicalMapping] cStringUsingEncoding:NSUTF8StringEncoding]));

		// Figure out whether it's a directory. Some protocols (like NFS) do not voluntarily return this as part of the directory entry, and need to be queried manually.
		bool bIsDirectory = (InEntry->d_type == DT_DIR);
		if (InEntry->d_type == DT_UNKNOWN || InEntry->d_type == DT_LNK)
		{
			struct stat StatInfo;
			if (stat(TCHAR_TO_UTF8(*(NormalizedDirectoryStr / NormalizedFilename)), &StatInfo) == 0)
			{
				bIsDirectory = S_ISDIR(StatInfo.st_mode);
			}
		}

		return Visitor.CallShouldVisitAndVisit(*(DirectoryStr / NormalizedFilename), bIsDirectory);
	});
}

bool FApplePlatformFile::IterateDirectoryStat(const TCHAR* Directory, FDirectoryStatVisitor& Visitor)
{
	const FString DirectoryStr = Directory;
	const FString NormalizedDirectoryStr = NormalizeFilename(Directory);

	return IterateDirectoryCommon(Directory, [&Visitor, &DirectoryStr, &NormalizedDirectoryStr](struct dirent* InEntry) -> bool
	{
		SCOPED_AUTORELEASE_POOL;

		// Normalize any unicode forms so we match correctly
		const FString NormalizedFilename = UTF8_TO_TCHAR(([[[NSString stringWithUTF8String:InEntry->d_name] precomposedStringWithCanonicalMapping] cStringUsingEncoding:NSUTF8StringEncoding]));

		struct stat StatInfo;
		if (stat(TCHAR_TO_UTF8(*(NormalizedDirectoryStr / NormalizedFilename)), &StatInfo) == 0)
		{
			return Visitor.CallShouldVisitAndVisit(*(DirectoryStr / NormalizedFilename), MacStatToUEFileData(StatInfo));
		}

		return true;
	});
}

bool FApplePlatformFile::IterateDirectoryCommon(const TCHAR* Directory, const TFunctionRef<bool(struct dirent*)>& Visitor)
{
	bool Result = false;
	DIR* Handle = opendir(Directory[0] ? TCHAR_TO_UTF8(Directory) : ".");
	if (Handle)
	{
		Result = true;
		struct dirent *Entry;
		while (Result && (Entry = readdir(Handle)) != NULL)
		{
			if (FCStringAnsi::Strcmp(Entry->d_name, ".") && FCStringAnsi::Strcmp(Entry->d_name, "..") && FCStringAnsi::Strcmp(Entry->d_name, ".DS_Store"))
			{
                Result = Visitor(Entry);
			}
		}
		closedir(Handle);
	}
	return Result;
}

bool FApplePlatformFile::CopyFile(const TCHAR* To, const TCHAR* From, EPlatformFileRead ReadFlags, EPlatformFileWrite WriteFlags)
{
	bool Result = IPlatformFile::CopyFile(To, From, ReadFlags, WriteFlags);
	if (Result)
	{
		struct stat FileInfo;
		if (Stat(From, &FileInfo) == 0)
		{
			FileInfo.st_mode |= S_IWUSR;
			chmod(TCHAR_TO_UTF8(*NormalizeFilename(To)), FileInfo.st_mode);
		}
	}
	return Result;
}

int32 FApplePlatformFile::Stat(const TCHAR* Filename, struct stat* OutFileInfo)
{
	return stat(TCHAR_TO_UTF8(*NormalizeFilename(Filename)), OutFileInfo);
}
