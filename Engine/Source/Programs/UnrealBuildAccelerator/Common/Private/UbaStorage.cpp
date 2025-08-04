// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaStorage.h"
#include "UbaApplicationRules.h"
#include "UbaBottleneck.h"
#include "UbaCompressedFileHeader.h"
#include "UbaConfig.h"
#include "UbaFileAccessor.h"
#include "UbaBinaryReaderWriter.h"
#include "UbaDirectoryIterator.h"
#include "UbaStorageUtils.h"
#include "UbaWorkManager.h"
#include <oodle2.h>

#define UBA_CHECK_BUFFER_SLOTS 0

namespace uba
{
	constexpr u32 CasTableVersion = IsWindows ? 32 : 34;
	constexpr u32 MaxWorkItemsPerAction = 128; // Cap this to not starve other things

    CasKey EmptyFileKey = []() { CasKeyHasher hasher; return ToCasKey(hasher, false); }();

	void Storage::GetMappingString(StringBufferBase& out, FileMappingHandle mappingHandle, u64 offset)
	{
		out.Append('^').AppendHex(mappingHandle.ToU64()).Append('-').AppendHex(offset);
	}

	u8* BufferSlots::Pop()
	{
		SCOPED_FUTEX(m_slotsLock, lock);
		if (!m_slots.empty())
		{
			auto back = m_slots.back();
			m_slots.pop_back();
			return back;
		}
#if UBA_CHECK_BUFFER_SLOTS
		u8* res = new u8[BufferSlotSize + 12];
		res += 8;
		*(u32*)(res - 4) = 0xdeadbeef;
		*(u32*)(res + BufferSlotSize) = 0xdeadbeef;
		return res;
#else
		return new u8[BufferSlotSize];
#endif
	}

	void BufferSlots::Push(u8* slot)
	{
		if (!slot)
			return;

#if UBA_CHECK_BUFFER_SLOTS
		UBA_ASSERT(*(u32*)(slot - 4) == 0xdeadbeef);
		UBA_ASSERT(*(u32*)(slot + BufferSlotSize) == 0xdeadbeef);
#endif

		SCOPED_FUTEX(m_slotsLock, lock);
		m_slots.push_back(slot);
	}

	BufferSlots::~BufferSlots()
	{
#if UBA_CHECK_BUFFER_SLOTS
		for (u8* slot : m_slots)
			delete[] (slot-8);
#else
		for (u8* slot : m_slots)
			delete[] slot;
#endif
	}

	StorageCreateInfo::StorageCreateInfo(const tchar* rootDir_, LogWriter& w)
	:	writer(w)
	,	rootDir(rootDir_)
	,	casCompressor(DefaultCompressor)
	,	casCompressionLevel(DefaultCompressionLevel)
	{
	}

	void StorageCreateInfo::Apply(const Config& config)
	{
		const ConfigTable* tablePtr = config.GetTable(TC("Storage"));
		if (!tablePtr)
			return;
		const ConfigTable& table = *tablePtr;
		table.GetValueAsString(rootDir, TC("RootDir"));
		table.GetValueAsBool(asyncUnmapViewOfFile, TC("AsyncUnmapViewOfFile"));
		table.GetValueAsU64(casCapacityBytes, TC("CasCapacityBytes"));
		table.GetValueAsBool(keepTransientDataMapped, TC("KeepTransientDataMapped"));

		const tchar* compressor;
		if (table.GetValueAsString(compressor, TC("Compressor")))
			casCompressor = GetCompressor(compressor);

		const tchar* compressionLevel;
		if (table.GetValueAsString(compressionLevel, TC("CompressionLevel")))
			casCompressionLevel = GetCompressionLevel(compressionLevel);
	}

	struct FileMappingScope
	{
		u8* MapView(const tchar* func, FileMappingHandle mappingHandle, u64 mappingOffset, u64 fileSize, const tchar* fileName_)
		{
			fileName = fileName_;
			constexpr u64 pageSize = 64*1024;
			u64 endOffset = mappingOffset + fileSize;
			u64 alignedOffsetStart = AlignUp(mappingOffset - (pageSize - 1), pageSize);
			u64 alignedOffsetEnd = AlignUp(endOffset, pageSize);
			mapSize = alignedOffsetEnd - alignedOffsetStart;
			mappedMem = MapViewOfFile(logger, mappingHandle, FILE_MAP_READ, alignedOffsetStart, mapSize);
			if (!mappedMem)
				mappedMem = MapViewOfFile(logger, mappingHandle, FILE_MAP_READ, alignedOffsetStart, 0);
			if (mappedMem)
				return mappedMem + (mappingOffset - alignedOffsetStart);
			logger.Error(TC("[%s] Failed to map view of file %s at offset %llu and size %llu (%s)"), func, fileName, alignedOffsetStart, mapSize, LastErrorToText().data);
			return nullptr;
		}

		FileMappingScope(Logger& l) : logger(l) {}
		~FileMappingScope()
		{
			if (mappedMem)
				if (!UnmapViewOfFile(logger, mappedMem, mapSize, fileName))
					logger.Error(TC("Failed to unmap memory %s at 0x%llx with size %llu (%s)"), fileName, mappedMem, mapSize, LastErrorToText().data);
		}

		Logger& logger;
		const tchar* fileName;
		u8* mappedMem = nullptr;
		u64 mapSize = 0;
	};

	void StorageImpl::CasEntryAccessed(CasEntry& casEntry)
	{
		{
			SCOPED_READ_LOCK(casEntry.lock, l); // Note, this lock is taken again outside CasEntryAccessed.. so if this takes a long time it won't help to remove this lock
			if (casEntry.dropped)
				return;
			//if (casEntry.mappingHandle.IsValid())
			//	return;
		}

		SCOPED_FUTEX(m_accessLock, lock);

		CasEntry* prevAccessed = casEntry.prevAccessed;
		if (prevAccessed == nullptr)
		{
			if (m_newestAccessed == &casEntry) // We are already first
				return;
		}
		else
			prevAccessed->nextAccessed = casEntry.nextAccessed;

		if (casEntry.nextAccessed)
			casEntry.nextAccessed->prevAccessed = prevAccessed;
		else if (prevAccessed)
			m_oldestAccessed = prevAccessed;
		else if (!m_oldestAccessed)
			m_oldestAccessed = &casEntry;

		if (m_newestAccessed)
			m_newestAccessed->prevAccessed = &casEntry;
		casEntry.nextAccessed = m_newestAccessed;
		casEntry.prevAccessed = nullptr;
		m_newestAccessed = &casEntry;
	}

	void StorageImpl::CasEntryWritten(CasEntry& casEntry, u64 size)
	{
		SCOPED_FUTEX(m_accessLock, lock);

		m_casTotalBytes += size - casEntry.size;
		m_casMaxBytes = Max(m_casTotalBytes, m_casMaxBytes);

		casEntry.size = size;

		UBA_ASSERT(!m_casCapacityBytes || !casEntry.mappingHandle.IsValid());

		if (!m_casCapacityBytes || m_overflowReported || m_manuallyHandleOverflow)
			return;

		if (m_casTotalBytes <= m_casCapacityBytes)
		{
			if (!casEntry.prevAccessed && !casEntry.nextAccessed && (!m_newestAccessed || m_newestAccessed != &casEntry))
			{
				UBA_ASSERT(!casEntry.dropped);
				AttachEntry(casEntry);
			}
			return;
		}

		TimerScope ts(Stats().handleOverflow);
		UBA_ASSERT(!m_newestAccessed || !m_newestAccessed->prevAccessed);
		UBA_ASSERT(!m_oldestAccessed || !m_oldestAccessed->nextAccessed);

		struct Rec { CasEntry& casEntry; u64 size; };
		Vector<Rec> toDelete;

		for (CasEntry* it = m_oldestAccessed; it;)
		{
			CasEntry& ce = *it;
			ce.lock.Enter();

			if (ce.verified && !m_allowDeleteVerified) // Can't remove these since they might be in mapping tables and actively being used. It is tempting to delete dropped cas files but we can't
			{
				ce.lock.Leave();
				break;
			}

			if (ce.beingWritten || ce.readCount)
			{
				if (ce.beingWritten)
					m_logger.Info(TC("Want to delete file that is begin written. We should never end up here"));
				ce.lock.Leave();
				it = ce.prevAccessed;
				continue;
			}

			UBA_ASSERT(ce.key != CasKeyZero);
			toDelete.push_back({*it, ce.size});

			if (m_trackedDeletes)
				m_trackedDeletes->insert(ce.key);

			m_casEvictedBytes += ce.size;
			m_casEvictedCount++;
			m_casTotalBytes -= ce.size;

			ce.exists = false;
			ce.size = 0;


			it = ce.prevAccessed;
			DetachEntry(ce);
			if (m_casTotalBytes <= m_casCapacityBytes)
				break;
		}

		if (m_casTotalBytes > m_casCapacityBytes)
		{
			m_overflowReported = true;
			m_logger.Info(TC("Exceeding maximum size set for cas (%s). Current session needs more storage to be able to finish (will now overflow). User memory reported on session exit"), BytesToText(m_casCapacityBytes).str);
		}

		lock.Leave();

		for (Rec& rec : toDelete)
		{
			StringBuffer<> casFile;
			StorageImpl::GetCasFileName(casFile, rec.casEntry.key);

			if (!DeleteFileW(casFile.data))
			{
				u32 error = GetLastError();
				if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
				{
					m_logger.Error(TC("Failed to delete %s while handling overflow (%s)"), casFile.data, LastErrorToText(error).data);
					rec.casEntry.exists = true;
					rec.casEntry.size = rec.size;
					rec.casEntry.lock.Leave();

					// TODO: Should this instead set some overflow
					/*
					SCOPED_FUTEX(m_accessLock, lock);
					m_casEvictedBytes -= rec.size;
					--m_casEvictedCount;
					m_casTotalBytes += rec.size;
					AttachEntry(&rec.casEntry); // TODO: This should be re-added in the end
					*/
					continue;
				}
			}

			//m_logger.Warning(TC("Evicted %s due to overflow"), casFile.data);

			casEntry.verified = true; // Verified to be deleted

			rec.casEntry.lock.Leave();
		}
	}

	void StorageImpl::CasEntryDeleted(CasEntry& casEntry, u64 size)
	{
		SCOPED_FUTEX(m_accessLock, lock);
		m_casTotalBytes -= size;
		casEntry.size = 0;
		DetachEntry(casEntry);
	}

	void StorageImpl::AttachEntry(CasEntry& casEntry)
	{
		if (m_oldestAccessed)
			m_oldestAccessed->nextAccessed = &casEntry;
		casEntry.prevAccessed = m_oldestAccessed;
		casEntry.nextAccessed = nullptr;
		if (!m_newestAccessed)
			m_newestAccessed = &casEntry;
		m_oldestAccessed = &casEntry;
	}

	void StorageImpl::DetachEntry(CasEntry& casEntry)
	{
		CasEntry* prevAccessed = casEntry.prevAccessed;
		if (prevAccessed)
			prevAccessed->nextAccessed = casEntry.nextAccessed;
		else if (m_newestAccessed == &casEntry)
			m_newestAccessed = casEntry.nextAccessed;

		if (casEntry.nextAccessed)
			casEntry.nextAccessed->prevAccessed = prevAccessed;
		else if (m_oldestAccessed == &casEntry)
			m_oldestAccessed = prevAccessed;

		casEntry.prevAccessed = nullptr;
		casEntry.nextAccessed = nullptr;
	}

	void StorageImpl::RegisterExternalFileMappingsProvider(ExternalFileMappingsProvider&& provider)
	{
		m_externalFileMappingsProvider = std::move(provider);
	}

	bool StorageImpl::WriteCompressed(WriteResult& out, const tchar* from, FileHandle readHandle, u8* readMem, u64 fileSize, const tchar* to, const void* header, u64 headerSize, u64 lastWriteTime)
	{
		StorageStats& stats = Stats();

		u64 totalWritten = 0;

		u64 diff = (u64)OodleLZ_GetCompressedBufferSizeNeeded((OodleLZ_Compressor)m_casCompressor, BufferSlotHalfSize) - BufferSlotHalfSize;
		u64 maxUncompressedBlock = BufferSlotHalfSize - diff - 8; // 8 bytes for the little header
		u32 workCount = u32((fileSize + maxUncompressedBlock - 1) / maxUncompressedBlock);

		FileAccessor destinationFile(m_logger, to);
		if (!destinationFile.CreateWrite(false, DefaultAttributes(), 0, m_tempPath.data))
			return false;
		if (headerSize)
			if (!destinationFile.Write(header, headerSize))
				return false;
		if (!destinationFile.Write(&fileSize, sizeof(u64))) // Store file size first in compressed file
			return false;

		totalWritten += sizeof(u64);

		u64 left = fileSize;

		if (m_workManager && workCount > 1)
		{
			if (!readMem)
			{
				FileMappingHandle fileMapping = uba::CreateFileMappingW(m_logger, readHandle, PAGE_READONLY, fileSize, from);
				if (!fileMapping.IsValid())
					return m_logger.Error(TC("Failed to create file mapping for %s (%s)"), from, LastErrorToText().data);

				auto fmg = MakeGuard([&]() { CloseFileMapping(m_logger, fileMapping, from); });
				u8* uncompressedData = MapViewOfFile(m_logger, fileMapping, FILE_MAP_READ, 0, fileSize);
				if (!uncompressedData)
					return m_logger.Error(TC("Failed to map view of file mapping for %s (%s)"), from, LastErrorToText().data);

				auto udg = MakeGuard([&]()
					{
						if (m_asyncUnmapViewOfFile)
							m_workManager->AddWork([this, uncompressedData, fileSize, f = TString(from)](const WorkContext&) { UnmapViewOfFile(m_logger, uncompressedData, fileSize, f.c_str()); }, 1, TC("UnmapFile"));
						else
							UnmapViewOfFile(m_logger, uncompressedData, fileSize, from);
					});

				if (!WriteMemToCompressedFile(destinationFile, workCount, uncompressedData, fileSize, maxUncompressedBlock, totalWritten))
					return false;
			}
			else
			{
				if (!WriteMemToCompressedFile(destinationFile, workCount, readMem, fileSize, maxUncompressedBlock, totalWritten))
					return false;
			}
		}
		else
		{
			u8* slot = m_bufferSlots.Pop();
			auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });
			u8* uncompressedData = slot;
			u8* compressBuffer = slot + BufferSlotHalfSize;

			auto& memoryCompressTime = KernelStats::GetCurrent().memoryCompress;

			TimerScope cts(stats.compressWrite);
			while (left)
			{
				u64 uncompressedBlockSize = Min(left, maxUncompressedBlock);
				
				void* scratchMem = nullptr;
				u64 scratchSize = 0;

				if (readMem)
				{
					scratchMem = uncompressedData;
					scratchSize = BufferSlotHalfSize;
					uncompressedData = readMem + fileSize - left;
				}
				else
				{
					if (!ReadFile(m_logger, from, readHandle, uncompressedData, uncompressedBlockSize))
						return false;
					scratchMem = uncompressedData + uncompressedBlockSize;
					scratchSize = BufferSlotHalfSize - uncompressedBlockSize;
				}
				u8* destBuf = compressBuffer;
				OO_SINTa compressedBlockSize;
				{
					TimerScope kts(memoryCompressTime);
					compressedBlockSize = OodleLZ_Compress((OodleLZ_Compressor)m_casCompressor, uncompressedData, (OO_SINTa)uncompressedBlockSize, destBuf + 8, (OodleLZ_CompressionLevel)m_casCompressionLevel, NULL, NULL, NULL, scratchMem, scratchSize);
					if (compressedBlockSize == OODLELZ_FAILED)
						return m_logger.Error(TC("Failed to compress %llu bytes for %s"), uncompressedBlockSize, from);
					memoryCompressTime.bytes += compressedBlockSize;
				}

				*(u32*)destBuf =  u32(compressedBlockSize);
				*(u32*)(destBuf+4) =  u32(uncompressedBlockSize);

				u32 writeBytes = u32(compressedBlockSize) + 8;
				if (!destinationFile.Write(destBuf, writeBytes))
					return false;

				totalWritten += writeBytes;

				left -= uncompressedBlockSize;
			}
		}

		if (lastWriteTime)
			if (!SetFileLastWriteTime(destinationFile.GetHandle(), lastWriteTime))
				return m_logger.Error(TC("Failed to set file time on filehandle for %s"), to);
		if (!destinationFile.Close())
			return false;

		stats.createCasBytesRaw += fileSize;
		stats.createCasBytesComp += totalWritten;

		out.size = totalWritten;
		return true;
	}

	bool StorageImpl::WriteMemToCompressedFile(FileAccessor& destination, u32 workCount, const u8* uncompressedData, u64 fileSize, u64 maxUncompressedBlock, u64& totalWritten)
	{
		#ifndef __clang_analyzer__

		struct WorkRec
		{
			WorkRec() = delete;
			WorkRec(const WorkRec&) = delete;
			WorkRec(u32 wc)
			{
				workCount = wc;

				// For some very unknown reason ASAN on linux triggers on the "new[]" call when doing delete[]
				// while doing it manually works properly. Will stop investigating this and move on
				#if PLATFORM_WINDOWS
				events = new Event[workCount];
				#else
				events = (Event*)aligned_alloc(alignof(Event), sizeof(Event)*workCount);
				for (auto i=0;i!=workCount; ++i)
					new (events + i) Event();
				#endif
			}
			~WorkRec()
			{
				#if PLATFORM_WINDOWS
				delete[] events;
				#else
				for (auto i=0;i!=workCount; ++i)
					events[i].~Event();
				aligned_free(events);
				#endif
			}
			Atomic<u64> refCount;
			Atomic<u64> compressCounter;
			Event* events;
			const u8* uncompressedData = nullptr;
			FileAccessor* destination = nullptr;
			u64 written = 0;
			u64 workCount = 0;
			u64 maxUncompressedBlock = 0;
			u64 fileSize = 0;
			bool error = false;
		};

		WorkRec* rec = new WorkRec(workCount);
		rec->uncompressedData = uncompressedData;
		rec->maxUncompressedBlock = maxUncompressedBlock;
		rec->fileSize = fileSize;
		rec->destination = &destination;

		for (u32 i=0; i!=workCount; ++i)
			rec->events[i].Create(true);

		StorageStats& stats = Stats();
		TimerScope cts(stats.compressWrite);

		auto& kernelStats = KernelStats::GetCurrent();

		auto work = [this, rec, &stats, &kernelStats](const WorkContext&)
		{
			KernelStatsScope kss(kernelStats);

			u8* slot = m_bufferSlots.Pop();
			auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });
			u8* compressSlotBuffer = slot + BufferSlotHalfSize;
			while (true)
			{
				u64 index = rec->compressCounter++;
				if (index >= rec->workCount)
				{
					if (!--rec->refCount)
						delete rec;
					return;
				}
				u64 startOffset = rec->maxUncompressedBlock*index;
				const u8* uncompressedDataSlot = rec->uncompressedData + startOffset;
				OO_SINTa uncompressedBlockSize = (OO_SINTa)Min(rec->maxUncompressedBlock, rec->fileSize - startOffset);
				OO_SINTa compressedBlockSize;
				{
					void* scratchMem = slot;
					u64 scratchSize = BufferSlotHalfSize;
					TimerScope kts(kernelStats.memoryCompress);
					compressedBlockSize = OodleLZ_Compress((OodleLZ_Compressor)m_casCompressor, uncompressedDataSlot, uncompressedBlockSize, compressSlotBuffer + 8, (OodleLZ_CompressionLevel)m_casCompressionLevel, NULL, NULL, NULL, scratchMem, scratchSize);
					if (compressedBlockSize == OODLELZ_FAILED)
					{
						m_logger.Error(TC("Failed to compress %llu bytes for %s"), u64(uncompressedBlockSize), rec->destination->GetFileName());
						rec->error = true;
						return;
					}
					kernelStats.memoryCompress.bytes += compressedBlockSize;
				}
				*(u32*)compressSlotBuffer =  u32(compressedBlockSize);
				*(u32*)(compressSlotBuffer+4) =  u32(uncompressedBlockSize);

				if (index)
					rec->events[index-1].IsSet();

				u32 writeBytes = u32(compressedBlockSize) + 8;

				if (!rec->destination->Write(compressSlotBuffer, writeBytes))
					rec->error = true;

				rec->written += writeBytes;
				if (index < rec->workCount)
					rec->events[index].Set();
			}
		};

		u32 workerCount = Min(workCount, m_workManager->GetWorkerCount());
		workerCount = Min(workerCount, MaxWorkItemsPerAction);

		rec->refCount = workerCount + 1; // We need to keep refcount up 1 to make sure it is not deleted before we read rec->written
		m_workManager->AddWork(work, workerCount-1, TC("Compress")); // We are a worker ourselves
		{
			TrackWorkScope tws;
			work({tws});
		}
		rec->events[rec->workCount - 1].IsSet();

		totalWritten += rec->written;
		bool error = rec->error;

		if (!--rec->refCount)
			delete rec;

		if (error)
			return false;
		#endif // __clang_analyzer__
		return true;
	}

	bool StorageImpl::WriteCasFileNoCheck(WriteResult& out, const StringKey& fileNameKey, const tchar* fileName, const CasKey& casKey, const tchar* casFile, bool storeCompressed)
	{
		StorageStats& stats = Stats();
		TimerScope ts(stats.createCas);

		if (m_externalFileMappingsProvider)
		{
			ExternalFileMapping externalMapping;
			if (m_externalFileMappingsProvider(externalMapping, fileNameKey, fileName))
			{
				FileMappingScope scope(m_logger);
				u8* fileMem = scope.MapView(TC("WriteCompressed"), externalMapping.handle, externalMapping.offset, externalMapping.size, fileName);
				if (!fileMem)
					return false;
				return StorageImpl::WriteCompressed(out, fileName, InvalidFileHandle, fileMem, externalMapping.size, casFile, nullptr, 0);
			}
		}

		FileHandle readHandle;
		if (!OpenFileSequentialRead(m_logger, fileName, readHandle))
			return m_logger.Error(TC("[WriteCasFileNoCheck] Failed to open file %s for read (%s)"), fileName, LastErrorToText().data);
		auto fileGuard = MakeGuard([&](){ CloseFile(fileName, readHandle); });

		u64 readFileSize = 0;
		if (!uba::GetFileSizeEx(readFileSize, readHandle))
			return m_logger.Error(TC("[WriteCasFileNoCheck] GetFileSize failed for %s (%s)"), fileName, LastErrorToText().data);

		bool readIsCompressed = false;
		if (readFileSize >= sizeof(CompressedFileHeader) && g_globalRules.FileCanBeCompressed(ToView(fileName)))
		{
			CompressedFileHeader header(CasKeyZero);
			if (!ReadFile(m_logger, fileName, readHandle, &header, sizeof(header)))
				return m_logger.Error(TC("Failed to read header of compressed file %s (%s)"), fileName, LastErrorToText().data);
			if (header.IsValid())
			{
				if (AsCompressed(casKey, false) != AsCompressed(header.casKey, false))
					return m_logger.Error(TC("Compressed file has different caskey than what was expected (%s)"), fileName);
				readIsCompressed = true;
				readFileSize -= sizeof(header);
			}
			else
			{
				if (!SetFilePointer(m_logger, fileName, readHandle, 0))
					return false;
			}
		}

		if (!readIsCompressed && storeCompressed)
			return WriteCompressed(out, fileName, readHandle, 0, readFileSize, casFile, nullptr, 0);

		if (readIsCompressed && !storeCompressed)
			return m_logger.Error(TC("[WriteCasFileNoCheck] Writing compressed file to non-compressed storage not supported (%s)"), fileName);

		FileAccessor destinationFile(m_logger, casFile);
		if (!destinationFile.CreateWrite(false, DefaultAttributes(), readFileSize, m_tempPath.data))
			return false;

		u8* slot= m_bufferSlots.Pop();
		auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });
		u64 left = readFileSize;

		while (left)
		{
			u32 toRead = u32(Min(left, BufferSlotSize));
			if (!ReadFile(m_logger, fileName, readHandle, slot, toRead))
				return false;
			if (!destinationFile.Write(slot, toRead))
				return false;

			left -= toRead;
		}

		if (!destinationFile.Close())
			return false;

		out.size = readFileSize;
		return true;
	}

	bool StorageImpl::WriteCasFile(WriteResult& out, const tchar* fileName, const CasKey& casKey)
	{
		UBA_ASSERT(IsCompressed(casKey) == m_storeCompressed);
		StringBuffer<> casFile;
		if (!StorageImpl::GetCasFileName(casFile, casKey))
			return false;
		if (FileExists(m_logger, casFile.data))
			return true;

		StringBuffer<> temp;
		temp.Append(fileName);
		if (CaseInsensitiveFs)
			temp.MakeLower();
		StringKey fileNameKey = ToStringKey(temp);
		return WriteCasFileNoCheck(out, fileNameKey, fileName, casKey, casFile.data, IsCompressed(casKey));
	}

	void StorageImpl::CasEntryAccessed(const CasKey& casKey)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lookupLock);
		auto findIt = m_casLookup.find(casKey);
		if (findIt == m_casLookup.end())
			return;
		CasEntry& casEntry = findIt->second;
		lookupLock.Leave();
		CasEntryAccessed(casEntry);
	}

	bool StorageImpl::IsDisallowedPath(const tchar* fileName)
	{
		return false;
	}

	bool StorageImpl::DecompressMemoryToMemory(const u8* compressedData, u64 compressedSize, u8* writeData, u64 decompressedSize, const tchar* readHint, const tchar* writeHint)
	{
		UBA_ASSERTF(compressedData, TC("DecompressMemoryToMemory got readmem nullptr (%s)"), readHint);
		UBA_ASSERTF(writeData, TC("DecompressMemoryToMemory got writemem nullptr (%s)"), writeHint);

		StorageStats& stats = Stats();

		if (decompressedSize > BufferSlotSize * 4) // Arbitrary size threshold. We want to at least catch the pch here
		{
			struct WorkRec
			{
				Logger* logger;
				const tchar* hint;
				Atomic<u64> refCount;
				const u8* readPos = nullptr;
				u8* writePos = nullptr;

				Futex lock;
				u64 decompressedSize = 0;
				u64 decompressedLeft = 0;
				u64 written = 0;
				Event done;
				Atomic<bool> error;
			};

			WorkRec* rec = new WorkRec();
			rec->logger = &m_logger;
			rec->hint = readHint;
			rec->readPos = compressedData;
			rec->writePos = writeData;
			rec->decompressedSize = decompressedSize;
			rec->decompressedLeft = decompressedSize;
			rec->done.Create(true);
			rec->refCount = 2;

			auto work = [rec](const WorkContext&)
				{
					u64 lastWritten = 0;
					while (true)
					{
						SCOPED_FUTEX(rec->lock, lock);
						rec->written += lastWritten;
						if (!rec->decompressedLeft)
						{
							if (rec->written == rec->decompressedSize)
								rec->done.Set();
							lock.Leave();
							if (!--rec->refCount)
								delete rec;
							return;
						}
						const u8* readPos = rec->readPos;
						u8* writePos = rec->writePos;
						u32 compressedBlockSize = ((const u32*)readPos)[0];
						u32 decompressedBlockSize = ((const u32*)readPos)[1];

						if (decompressedBlockSize == 0 || decompressedBlockSize > rec->decompressedSize)
						{
							bool f = false;
							if (rec->error.compare_exchange_strong(f, true))
								rec->logger->Error(TC("Decompressed block size %u is invalid. Decompressed file is %u (%s)"), decompressedBlockSize, rec->decompressedSize, rec->hint);
							if (!--rec->refCount)
								delete rec;
							rec->done.Set();
							return;
						}

						readPos += sizeof(u32) * 2;
						rec->decompressedLeft -= decompressedBlockSize;
						rec->readPos = readPos + compressedBlockSize;
						rec->writePos += decompressedBlockSize;
						lock.Leave();

						void* decoderMem = nullptr;
						u64 decoderMemSize = 0;
						OO_SINTa decompLen = OodleLZ_Decompress(readPos, (OO_SINTa)compressedBlockSize, writePos, (OO_SINTa)decompressedBlockSize, OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_None, NULL, 0, NULL, NULL, decoderMem, decoderMemSize);
						if (decompLen != decompressedBlockSize)
						{
							bool f = false;
							if (rec->error.compare_exchange_strong(f, true))
								rec->logger->Error(TC("Expecting to be able to decompress %u bytes to %u bytes but got %llu (%s)"), compressedBlockSize, decompressedBlockSize, decompLen, rec->hint);
							if (!--rec->refCount)
								delete rec;
							rec->done.Set();
							return;
						}
						lastWritten = u64(decompLen);
					}
				};

			if (m_workManager)
			{
				u32 workCount = u32(decompressedSize / BufferSlotSize) + 1;
				u32 workerCount = Min(workCount, m_workManager->GetWorkerCount() - 1); // We are a worker ourselves
				workerCount = Min(workerCount, MaxWorkItemsPerAction); // Cap this to not starve other things
				rec->refCount += workerCount;
				m_workManager->AddWork(work, workerCount, TC("DecompressMemToMem"));
			}

			TimerScope ts(stats.decompressToMem);
			{
				TrackWorkScope tws;
				work({tws});
			}
			rec->done.IsSet();
			bool success = !rec->error;
			if (!success)
				while (rec->refCount > 1)
					Sleep(10);

			if (!--rec->refCount)
				delete rec;
			return success;
		}
		else
		{
			const u8* readPos = compressedData;
			u8* writePos = writeData;

			u64 left = decompressedSize;
			while (left)
			{
				u32 compressedBlockSize = ((const u32*)readPos)[0];
				if (!compressedBlockSize)
					break;
				u32 decompressedBlockSize = ((const u32*)readPos)[1];
				if (decompressedBlockSize == 0 || decompressedBlockSize > left)
					return m_logger.Error(TC("Decompressed block size %u is invalid. Decompressed file is %u (%s -> %s)"), decompressedBlockSize, decompressedSize, readHint, writeHint);
				readPos += sizeof(u32) * 2;

				void* decoderMem = nullptr;
				u64 decoderMemSize = 0;
				TimerScope ts(stats.decompressToMem);
				OO_SINTa decompLen = OodleLZ_Decompress(readPos, (OO_SINTa)compressedBlockSize, writePos, (OO_SINTa)decompressedBlockSize, OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_None, NULL, 0, NULL, NULL, decoderMem, decoderMemSize);
				if (decompLen != decompressedBlockSize)
					return m_logger.Error(TC("Expecting to be able to decompress %u to %u bytes at pos %llu but got %llu. File compressed size: %llu (%s -> %s)"), compressedBlockSize, decompressedBlockSize, decompressedSize - left, decompLen, compressedSize, readHint, writeHint);
				writePos += decompressedBlockSize;
				readPos += compressedBlockSize;
				left -= decompressedBlockSize;
			}
		}
		return true;
	}

	bool StorageImpl::DecompressMemoryToFile(u8* compressedData, FileAccessor& destination, u64 decompressedSize, bool useNoBuffering)
	{
		StorageStats& stats = Stats();
		u8* readPos = compressedData;

		u8* slot = m_bufferSlots.Pop();
		auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });

		u64 left = decompressedSize;
		u64 overflow = 0;
		while (left)
		{
			u32 compressedBlockSize = ((u32*)readPos)[0];
			if (!compressedBlockSize)
				break;
			u32 decompressedBlockSize = ((u32*)readPos)[1];

			readPos += sizeof(u32)*2;

			OO_SINTa decompLen;
			{
				TimerScope ts(stats.decompressToMem);
				decompLen = OodleLZ_Decompress(readPos, (OO_SINTa)compressedBlockSize, slot + overflow, (OO_SINTa)decompressedBlockSize);
			}
			UBA_ASSERT(decompLen == decompressedBlockSize);
			
			u64 available = overflow + u64(decompLen);

			if (left - available > 0 && available < BufferSlotHalfSize)
			{
				overflow += u64(decompLen);
				readPos += compressedBlockSize;
				continue;
			}

			if (useNoBuffering)
			{
				u64 writeSize = AlignUp(available - 4096 + 1, 4096);

				if (!destination.Write(slot, writeSize))
					return false;

				overflow = available - writeSize;
				readPos += compressedBlockSize;
				left -= writeSize;

				if (overflow == left)
				{
					if (!destination.Write(slot + writeSize, 4096))
						return false;
					break;
				}

				memcpy(slot, slot + writeSize, overflow);
			}
			else
			{
				u64 writeSize = available;
				if (!destination.Write(slot, writeSize))
					return false;
				readPos += compressedBlockSize;
				left -= writeSize;
				overflow = 0;
			}
		}

		if (useNoBuffering)
			if (!SetEndOfFile(m_logger, destination.GetFileName(), destination.GetHandle(), decompressedSize))
				return false;
		return true;
	}

	bool StorageImpl::VerifyExisting(bool& outReturnValue, ScopedWriteLock& entryLock, const CasKey& casKey, CasEntry& casEntry, StringBufferBase& casFile)
	{
		u64 outFileSize = 0;
		u32 outAttributes = 0;
		if (!FileExists(m_logger, casFile.data, &outFileSize, &outAttributes))
			return false;

		bool isBad = (outFileSize == 0 && casKey != EmptyFileKey);

		if (isBad)
		{
			#if !PLATFORM_WINDOWS
			if (!outAttributes)
				m_logger.Info(TC("Found file %s with attributes 0 which means it was never written fully. Deleting"), casFile.data);
			else
			#endif
				m_logger.Info(TC("Found file %s with size 0 which did not have the zero-size-caskey. Deleting"), casFile.data);

			if (!DeleteFileW(casFile.data))
			{
				outReturnValue = false;
				m_logger.Error(TC("Failed to delete %s. Clean cas folder and restart"), casFile.data);
				return false;
			}
		}
		else
		{
			UBA_ASSERT(!casEntry.verified || casEntry.size);
			casEntry.verified = true;
			casEntry.exists = true;
			entryLock.Leave();
			CasEntryWritten(casEntry, outFileSize);
			outReturnValue = true;
			return true;
		}

		return false;
	}


	bool StorageImpl::AddCasFile(StringKey fileNameKey, const tchar* fileName, const CasKey& casKey, bool deferCreation)
	{
		UBA_ASSERTF(IsCompressed(casKey) == m_storeCompressed, TC("CasKey compress mode must match storage compress mode. Compress mode Key: %i, Store: %i Cas: %s (%s)"), (IsCompressed(casKey) ? 1 : 0), (m_storeCompressed ? 1 : 0), CasKeyString(casKey).str, fileName);
		CasEntry& casEntry = GetOrCreateCasEntry(casKey);
		CasEntryAccessed(casEntry);

		SCOPED_WRITE_LOCK(casEntry.lock, entryLock);

		if (casEntry.beingWritten)
		{
			m_logger.Warning(TC("Cas %s is being written from network while being added by host (%s)"), CasKeyString(casKey).str, fileName);
			int waitCount = 0;
			while (casEntry.beingWritten)
			{
				CasKey key = casEntry.key;
				entryLock.Leave();
				Sleep(100);
				entryLock.Enter();

				if (++waitCount < 12*60*10)
					continue;
				return m_logger.Error(TC("Host waited more than 12 minutes for file %s (%s) to be written by network (Written %llu/%llu)"), CasKeyString(key).str, fileName);
			}
		}

		if (casEntry.verified)
			if (casEntry.exists)
				return true;

		casEntry.disallowed = IsDisallowedPath(fileName);

		StringBuffer<> casFile;
		if (!StorageImpl::GetCasFileName(casFile, casKey)) // Force to not use subclassed version
			return false;
	
		bool verifyReturnValue;
		if (!casEntry.verified && VerifyExisting(verifyReturnValue, entryLock, casKey, casEntry, casFile))
			return verifyReturnValue;

		if (deferCreation)
		{
			SCOPED_WRITE_LOCK(m_deferredCasCreationLookupLock, deferredLock);
			auto res = m_deferredCasCreationLookup.try_emplace(casKey);
			if (res.second)
			{
				DeferedCasCreation& dcc = res.first->second;
				dcc.fileName = fileName;
			}
			auto res2 = m_deferredCasCreationLookupByName.try_emplace(fileNameKey, casKey);
			if (res2.second)
				res.first->second.names.push_back(res2.first);
			else if (res2.first->second != casKey)
				return m_logger.Error(TC("Same file %s is registered with different caskey (%s vs %s)"), fileName, CasKeyString(res2.first->second).str, CasKeyString(casKey).str);
			return true;
		}

		casEntry.verified = true;
		casEntry.exists = false;

		WriteResult res;
		if (!WriteCasFileNoCheck(res, fileNameKey, fileName, casKey, casFile.data, IsCompressed(casKey)))
			return false;

		casEntry.exists = true;
		entryLock.Leave();
		CasEntryWritten(casEntry, res.size);
		return true;
	}

	void StorageImpl::TraverseAllCasFiles(const tchar* dir, const Function<void(const StringBufferBase& fullPath, const DirectoryEntry& e)>& func, bool allowParallel)
	{
		Atomic<u32> workLeft;

		auto traverseCasFileDir = [&func, &workLeft, this](const StringView& casFileDir)
			{
				TraverseDir(m_logger, casFileDir,
					[&](const DirectoryEntry& e)
					{
						UBA_ASSERT(!IsDirectory(e.attributes));
						StringBuffer<> fullPath(casFileDir);
						fullPath.EnsureEndsWithSlash().Append(e.name);
						func(fullPath, e);
					});
				--workLeft;
			};

		TraverseDir(m_logger, ToView(dir),
			[&](const DirectoryEntry& e)
			{
				if (!IsDirectory(e.attributes))
					return;
				StringBuffer<> fullPath(dir);
				fullPath.EnsureEndsWithSlash().Append(e.name);
				++workLeft;

				if (allowParallel && m_workManager)
				{
					m_workManager->AddWork([&traverseCasFileDir, p = fullPath.ToString()](const WorkContext&) { traverseCasFileDir(p); }, 1, TC("TraverseCasFiles"));
				}
				else
				{
					traverseCasFileDir(fullPath);
				}
			});

		while (workLeft)
			m_workManager->DoWork();
	}

	void StorageImpl::TraverseAllCasFiles(const Function<void(const CasKey& key, u64 size)>& func, bool allowParallel)
	{
		StringBuffer<> casRoot;
		casRoot.Append(m_rootDir.data, m_rootDir.count - 1);
		TraverseAllCasFiles(casRoot.data, [&](const StringBufferBase& fullPath, const DirectoryEntry& e)
			{
				func(CasKeyFromString(e.name), e.size);
			}, allowParallel);
	}

	bool StorageImpl::CheckAllCasFiles(u64 checkContentOfFilesNewerThanTime)
	{
		u64 startTime = GetTime();
		u64 before = m_casTotalBytes;
		m_casTotalBytes = 0;
		// Need to scan all files to see so there are no orphans in the folders
		StringBuffer<> casRoot;
		casRoot.Append(m_rootDir.data, m_rootDir.count - 1);

		Atomic<bool> success = true;
		TraverseAllCasFiles(casRoot.data, [&](const StringBufferBase& fullPath, const DirectoryEntry& e)
			{
				CasKey casKey = CasKeyFromString(e.name);
				bool deleteFile = false;
				u64 size = e.size;

				// Do quick check on new files so the have good content (ignore files with zero size, they are tested further down)
				if (size && e.lastWritten >= checkContentOfFilesNewerThanTime)
				{
					FileAccessor fa(m_logger, fullPath.data);
					if (fa.OpenMemoryRead())
					{
						if (IsCompressed(casKey))
						{
							BinaryReader reader(fa.GetData(), 0, fa.GetSize());
							if (reader.GetLeft() < 12)
							{
								m_logger.Detail(TC("Corrupt cas. Is %llu bytes, must be at least 12 (%s)"), reader.GetLeft(), e.name);
								deleteFile = true;
							}
							else
							{
								reader.ReadU64(); // Decompressed size
								while (true)
								{
									if (reader.GetLeft() <= 8)
									{
										m_logger.Detail(TC("Corrupt cas. Missing beginning of block (%s)"), e.name);
										deleteFile = true;
										break;
									}

									u32 compressedBlockSize = reader.ReadU32();
									reader.ReadU32();

									if (!compressedBlockSize || compressedBlockSize > reader.GetLeft())
									{
										m_logger.Detail(TC("Corrupt cas. Bad block (%s)"), e.name);
										deleteFile = true;
										break;
									}

									reader.Skip(compressedBlockSize);

									if (!reader.GetLeft())
										break;
								}
							}
						}
						else
						{
							// TODO: Some simple validation of uncompressed file.. maybe even rehash and check? (since it is not many uncompressed files.. zero on server and a few on client)
						}
					}
					else
					{
						deleteFile = true;
					}
				}


				SCOPED_WRITE_LOCK(m_casLookupLock, lookupLock);
				auto insres = m_casLookup.try_emplace(casKey, casKey);
				CasEntry& casEntry = insres.first->second;
				casEntry.verified = true;
				casEntry.exists = true;

				m_casTotalBytes += size;
				if (insres.second)
				{
					casEntry.size = size;
					AttachEntry(casEntry);
				}
				else
				{
					UBA_ASSERT(casEntry.key == casKey);
					// We should probably delete this one.. something is wrong
					if (casEntry.size != 0 && casEntry.size != size && !deleteFile)
						m_logger.Detail(TC("Found cas entry which has a different size than what the table thought! Was %llu, is %llu (%s)"), casEntry.size, size, e.name);
					casEntry.size = size;
				}

				if (!size && casKey != ToCasKey(CasKeyHasher(), IsCompressed(casKey)))
				{
					m_logger.Detail(TC("Found file that has size 0 but does not have correct caskey (%s)"), e.name);
					deleteFile = true;
				}

				if (!deleteFile)
					return;

				DetachEntry(casEntry);
				m_casLookup.erase(insres.first);
				m_casTotalBytes -= size;
				lookupLock.Leave();

				if (DeleteFileW(fullPath.data))
					return;

				m_logger.Error(TC("Failed to delete file %s (%s)"), fullPath.data, LastErrorToText().data);
				success = false;

			}, true);

		if (!success)
			return false;

		u32 didNotExistCount = 0;
		// All files we saw is tagged as "handled") so let's see if there are cas entries we didn't find
		for (auto it = m_casLookup.begin(); it!=m_casLookup.end();)
		{
			CasEntry& casEntry = it->second;
			if (casEntry.verified)
			{
				++it;
				casEntry.verified = false; // Unhandle the entries to be able to evict later
				continue;
			}
			casEntry.size = 0;
			DetachEntry(casEntry);
			it = m_casLookup.erase(it);
			didNotExistCount++;
		}

		if (didNotExistCount)
			m_logger.Info(TC("Found %u cas entries that didn't have a file"), didNotExistCount);

		u64 duration = GetTime() - startTime;

		u64 after = m_casTotalBytes;
		if (before != after)
			m_logger.Info(TC("Corrected storage size from %s to %s in %s"), BytesToText(before).str, BytesToText(after).str, TimeToText(duration).str);
		else
			m_logger.Info(TC("Validated storage (size %s) in %s"), BytesToText(after).str, TimeToText(duration).str);
		m_casMaxBytes = m_casTotalBytes;
		return true;
	}

	void StorageImpl::HandleOverflow(UnorderedSet<CasKey>* outDeletedFiles)
	{
		if (!m_casCapacityBytes)
			return;
		u64 startTime = GetTime();
		u64 before = m_casTotalBytes;
		while (m_casTotalBytes > m_casCapacityBytes)
		{
			CasEntry* casEntry = m_oldestAccessed;
			if (!casEntry)
			{
				UBA_ASSERT(m_casLookup.empty());
				m_casTotalBytes = 0;
				break;
			}
			DropCasFile(casEntry->key, true, TC("HandleOverflow"));
			if (outDeletedFiles)
				outDeletedFiles->insert(casEntry->key);
			DetachEntry(*casEntry);
			m_casLookup.erase(casEntry->key);
		}
		m_overflowReported = false;
		u64 after = m_casTotalBytes;
		if (before != after)
			m_logger.Info(TC("Evicted %s from storage (%s). Estimated new storage is now %s (there might be files db is not aware of)"), BytesToText(before - after).str, TimeToText(GetTime() - startTime).str, BytesToText(after).str);
	}

	bool StorageImpl::DeleteIsRunningFile()
	{
		StringBuffer<256> isRunningName;
		isRunningName.Append(m_rootDir).Append(TCV(".isRunning"));
		if (DeleteFileW(isRunningName.data))
			return true;
		u32 lastError = GetLastError();
		if (lastError == ERROR_FILE_NOT_FOUND || lastError == ERROR_PATH_NOT_FOUND)
			return true;
		return m_logger.Warning(TC("Failed to delete %s (%s)"), isRunningName.data, LastErrorToText(lastError).data);
	}

	#if UBA_USE_MIMALLOC
	void* Oodle_MallocAligned(OO_SINTa bytes, OO_S32 alignment)
	{
		return mi_malloc_aligned(bytes, alignment);
	}

	void Oodle_Free(void* ptr)
	{
		mi_free(ptr);
	}
	#endif

	StorageImpl::StorageImpl(const StorageCreateInfo& info, const tchar* logPrefix)
	:	m_workManager(info.workManager)
	,	m_logger(info.writer, logPrefix)
	,	m_maxParallelCopyOrLinkBottleneck(info.maxParallelCopyOrLink)
	,	m_casDataBuffer(m_logger, info.workManager)
	{
		m_casCapacityBytes = info.casCapacityBytes;
		m_storeCompressed = info.storeCompressed;
		m_manuallyHandleOverflow = info.manuallyHandleOverflow;
		m_asyncUnmapViewOfFile = info.asyncUnmapViewOfFile && m_workManager;
		m_allowDeleteVerified = info.allowDeleteVerified;
		m_writeToDisk = info.writeToDisk;
		if (!m_writeToDisk)
			m_casCapacityBytes = 0;

		m_exclusiveMutex = info.exclusiveMutex;

		m_rootDir.count = GetFullPathNameW(info.rootDir, m_rootDir.capacity, m_rootDir.data, NULL);
		m_rootDir.Replace('/', PathSeparator).EnsureEndsWithSlash();

		if (m_writeToDisk)
		{
			m_tempPath.Append(m_rootDir).Append(TCV("castemp"));
			CreateDirectory(m_tempPath.data);
			DeleteAllFiles(m_logger, m_tempPath.data, false);
			m_tempPath.EnsureEndsWithSlash();
		}

		m_rootDir.Append(TCV("cas")).EnsureEndsWithSlash();

		m_casDataBuffer.AddTransient(TC("CasData"), info.keepTransientDataMapped);

		m_casCompressor = info.casCompressor;
		m_casCompressionLevel = info.casCompressionLevel;

		#if UBA_USE_MIMALLOC
		OodleCore_Plugins_SetAllocators(Oodle_MallocAligned, Oodle_Free);
		#endif
	}

	StorageImpl::~StorageImpl()
	{
		SaveCasTable(true, true);
		CloseMutex(m_exclusiveMutex);
	}

	MutexHandle StorageImpl::GetExclusiveAccess(Logger& logger, const StringView& rootDir, bool reportError)
	{
		StringKey key = ToStringKeyNoCheck(rootDir.data, rootDir.count);
		KeyToString keyStr(key);
		MutexHandle exclusiveMutex = CreateMutexW(true, keyStr.data);
		u32 lastError = GetLastError();
		if (exclusiveMutex == InvalidMutexHandle)
		{
			if (reportError)
				logger.Error(TC("Failed to create mutex %s for path %s (%s)"), keyStr.data, rootDir.data, LastErrorToText(lastError).data);
			return InvalidMutexHandle;
		}

		if (lastError != ERROR_ALREADY_EXISTS)
			return exclusiveMutex;

		CloseMutex(exclusiveMutex);
		if (reportError)
			logger.Error(TC("Needs exclusive access to storage %s. Another process is running"), rootDir.data);
		return InvalidMutexHandle;
	}

	StorageImpl::FileEntry& StorageImpl::GetOrCreateFileEntry(const StringKey& fileNameKey)
	{
		SCOPED_READ_LOCK(m_fileTableLookupLock, lock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt != m_fileTableLookup.end())
			return findIt->second;
		lock.Leave();
		SCOPED_WRITE_LOCK(m_fileTableLookupLock, lock2);
		return m_fileTableLookup.try_emplace(fileNameKey).first->second;
	}

	StorageImpl::CasEntry& StorageImpl::GetOrCreateCasEntry(const CasKey& casKey)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lock);
		auto findIt = m_casLookup.find(casKey);
		if (findIt != m_casLookup.end())
			return findIt->second;
		lock.Leave();
		SCOPED_WRITE_LOCK(m_casLookupLock, lock2);
		return m_casLookup.try_emplace(casKey, casKey).first->second;
	}

	bool StorageImpl::LoadCasTable(bool logStats, bool alwaysCheckAllFiles, bool* outWasTerminated)
	{
		if (!m_writeToDisk)
			return true;

		if (m_exclusiveMutex == InvalidMutexHandle)
		{
			m_exclusiveMutex = GetExclusiveAccess(m_logger, m_rootDir, true);
			if (m_exclusiveMutex == InvalidMutexHandle)
				return false;
		}

		CreateDirectory(m_rootDir.data);

		SCOPED_FUTEX(m_casTableLoadSaveLock, loadSaveLock);

		UBA_ASSERT(!m_casTableLoaded);
		m_casTableLoaded = true;
		u64 startTime = GetTime();
		tchar isRunningName[256];
		TStrcpy_s(isRunningName, sizeof_array(isRunningName), m_rootDir.data);
		TStrcat_s(isRunningName, sizeof_array(isRunningName), TC(".isRunning"));
		bool wasTerminated = FileExists(m_logger, isRunningName);

		if (outWasTerminated)
			*outWasTerminated = wasTerminated;

		if (!wasTerminated)
		{
			FileAccessor isRunningFile(m_logger, isRunningName);
			if (!isRunningFile.CreateWrite(false, DefaultAttributes(), 0, m_tempPath.data) || !isRunningFile.Close())
				return m_logger.Error(TC("Failed to create temporary \".isRunning\" file"));
		}

		StringBuffer<> fileName;
		fileName.Append(m_rootDir).Append(TCV("casdb"));

		FileHandle fileHandle;
		if (!OpenFileSequentialRead(m_logger, fileName.data, fileHandle, false))
			return false;
		if (fileHandle == InvalidFileHandle)
			return true;

		auto fileGuard = MakeGuard([&](){ CloseFile(fileName.data, fileHandle); });

		u64 fileSize;
		if (!uba::GetFileSizeEx(fileSize, fileHandle))
			return m_logger.Error(TC("GetFileSize failed for %s (%s)"), fileName.data, LastErrorToText().data);

		if (fileSize < sizeof(u32))
			return m_logger.Warning(TC("CasTable file %s is corrupt (size: %u)"), fileName.data, fileSize);

		u8* buffer = new u8[fileSize];
		auto memGuard = MakeGuard([&](){ delete[] buffer; });

		if (!ReadFile(m_logger, fileName.data, fileHandle, buffer, fileSize))
			return false;

		BinaryReader reader(buffer);
		u32 version = reader.ReadU32();
		if (version != CasTableVersion)
		{
			fileGuard.Execute();
			m_logger.Info(TC("New CasTable version (%u). Deleting all cas files..."), CasTableVersion);
			DeleteAllCas();
			loadSaveLock.Leave();
			if (!SaveCasTable(false, false)) // Save it just to get the version down
				return false;
			return true;
		}

		u32 fileTableSize = reader.ReadU32();
		u32 casLookupSize = reader.ReadU32();

		{
			SCOPED_WRITE_LOCK(m_fileTableLookupLock, fileTableLock); // Note, this can be reached from InvalidateCachedFileInfo which is called directly from session, so use a lock

			UBA_ASSERT(m_casLookup.empty());
			UBA_ASSERT(m_fileTableLookup.empty());

			m_fileTableLookup.reserve(fileTableSize);
			for (u32 i=0; i!=fileTableSize; ++i)
			{
				StringKey fileNameKey = reader.ReadStringKey();
				if (reader.GetPosition() + 24 > fileSize)
				{
					m_fileTableLookup.clear();
					return m_logger.Warning(TC("CasTable file %s is corrupt"), fileName.data);
				}
				u64 size = reader.ReadU64();
				u64 lastWritten = reader.ReadU64();
				CasKey casKey = reader.ReadCasKey();
				if (casKey != CasKeyZero)
					casKey = AsCompressed(casKey, m_storeCompressed);
				else if (size || lastWritten)
					continue; // This should not happen

				auto insres = m_fileTableLookup.try_emplace(fileNameKey);
				FileEntry& fileEntry = insres.first->second;
				fileEntry.verified = false;
				fileEntry.size = size;
				fileEntry.lastWritten = lastWritten;
				fileEntry.casKey = casKey;
			}
		}

		m_casLookup.reserve(casLookupSize);
		CasEntry* prev = nullptr;
		while (true)
		{
			CasKey casKey = reader.ReadCasKey();
			if (casKey == CasKeyZero)
				break;
			auto insres = m_casLookup.try_emplace(casKey, casKey);
			if (!insres.second)
			{
				m_logger.Warning(TC("CasTable file %s is corrupt, it contains same cas key multiple times (%s)"), fileName.data, CasKeyString(casKey).str);
				m_fileTableLookup.clear();
				m_casLookup.clear();
				m_newestAccessed = nullptr;
				return false;
			}
			CasEntry& casEntry = insres.first->second;
			casEntry.size = reader.ReadU64();
			casEntry.exists = true;
			m_casTotalBytes += casEntry.size;

			if (prev)
			{
				prev->nextAccessed = &casEntry;
				casEntry.prevAccessed = prev;
			}
			else
				m_newestAccessed = &casEntry;
			prev = &casEntry;
		}
		m_oldestAccessed = prev;

		bool resave = false;
		if (wasTerminated)
		{
			u64 fileTime = 0;
			FileHandle fh = uba::CreateFileW(isRunningName, 0, 0x00000007, 0x00000003, FILE_FLAG_BACKUP_SEMANTICS);
			auto fhg = MakeGuard([&]() { uba::CloseFile(isRunningName, fh); });
			if (fh != InvalidFileHandle)
				GetFileLastWriteTime(fileTime, fh);

			m_logger.Info(TC("Previous run was not gracefully shutdown. Reparsing cas directory %s to check for added/missing files"), m_rootDir.data);

			if (!CheckAllCasFiles(fileTime))
				return false;
			resave = true;

			if (fh != InvalidFileHandle)
				SetFileLastWriteTime(fh, GetSystemTimeAsFileTime());
		}
		else if (alwaysCheckAllFiles)
			CheckAllCasFiles();

		if (!m_manuallyHandleOverflow)
			HandleOverflow(nullptr);

		if (resave)
		{
			fileGuard.Execute();
			loadSaveLock.Leave();
			SaveCasTable(false, false);
		}

		if (logStats)
		{
			u64 duration = GetTime() - startTime;
			m_logger.Detail(TC("Database loaded from %s (v%u) in %s (contained %llu entries estimated to %s)"), fileName.data, version, TimeToText(duration).str, m_casLookup.size(), BytesToText(m_casTotalBytes).str);
		}

		return true;
	}

	bool StorageImpl::SaveCasTable(bool deleteIsRunningfile, bool deleteDropped)
	{
		SCOPED_FUTEX(m_casTableLoadSaveLock, loadSaveLock);
		if (!m_casTableLoaded)
			return true;

		u64 startTime = GetTime();

		StringBuffer<256> fileName;
		fileName.Append(m_rootDir).Append(TCV("casdb"));

		StringBuffer<256> tempFileName;
		tempFileName.Append(fileName).Append(TCV(".tmp"));

		u32 deleteCount = 0;
		{
			FileAccessor tempFile(m_logger, tempFileName.data);
			if (!tempFile.CreateWrite(false, DefaultAttributes(), 0, m_tempPath.data))
				return false;
			
			SCOPED_WRITE_LOCK(m_fileTableLookupLock, fileTableLock);
			SCOPED_WRITE_LOCK(m_casLookupLock, casLookupLock);
			SCOPED_FUTEX(m_accessLock, accessLock);

			Vector<u8> buffer;

			u64 casLookupEntrySize = sizeof(CasKey) + sizeof(u64);

			u64 headerSize = sizeof(u32)*3;
			u64 fileTableMaxWriteSize = m_fileTableLookup.size() * (sizeof(StringKey) + sizeof(u64) * 2 + sizeof(CasKey));
			u64 casLookupMaxWriteSize = m_casLookup.size() * casLookupEntrySize + sizeof(CasKey);

			buffer.reserve(headerSize + fileTableMaxWriteSize + casLookupMaxWriteSize);
			BinaryWriter writer(buffer.data(), 0, buffer.capacity());

			// Header
			writer.WriteU32(CasTableVersion);
			auto fileTableSizePtr = (u32*)writer.AllocWrite(4);
			auto casLookupSizePtr = (u32*)writer.AllocWrite(4);

			// File table
			u32 fileTableSize = 0;
			for (auto& pair : m_fileTableLookup)
			{
				FileEntry& fileEntry = pair.second;
				//SCOPED_READ_LOCK(fileEntry.lock, entryLock); // TODO: Remove this? tsan complains about it
				if (fileEntry.casKey == CasKeyZero || fileEntry.lastWritten == 0 || fileEntry.isTemp || fileEntry.casKey == CasKeyInvalid)
					continue;
				writer.WriteStringKey(pair.first);
				writer.WriteU64(fileEntry.size);
				writer.WriteU64(fileEntry.lastWritten);
				writer.WriteCasKey(fileEntry.casKey);
				++fileTableSize;
			}
			*fileTableSizePtr = fileTableSize;

			// Cas table
			u32 casTableSize = 0;
			CasEntry* last = nullptr;
			for (CasEntry* it = m_newestAccessed; it; it = it->nextAccessed)
			{
				last = it;
				CasEntry& casEntry = *it;
				if (casEntry.verified && !casEntry.exists)
					continue;
				if (deleteDropped && casEntry.dropped)
				{
					StringBuffer<512> casFileName;
					if (!StorageImpl::GetCasFileName(casFileName, casEntry.key))
						continue;
					if (DeleteFileW(casFileName.data))
						++deleteCount;
					continue;
				}

				if (writer.GetCapacityLeft() < casLookupEntrySize + sizeof(CasKey))
					return m_logger.Error(TC("This should not happen, somehow there are more valid entries in access list than lookup. (Lookup has %llu entries)"), m_casLookup.size());

				UBA_ASSERT(casEntry.key != CasKeyZero);
				writer.WriteCasKey(casEntry.key);
				writer.WriteU64(casEntry.size);
				++casTableSize;
			}
			writer.WriteCasKey(CasKeyZero);
			*casLookupSizePtr = casTableSize;

			if (!tempFile.Write(buffer.data(), writer.GetPosition()))
				return false;
			UBA_ASSERT(m_oldestAccessed == last); (void)last;
			if (!tempFile.Close())
				return false;
		}

		if (!MoveFileExW(tempFileName.data, fileName.data, MOVEFILE_REPLACE_EXISTING))
			return m_logger.Error(TC("Can't move file from %s to %s (%s)"), tempFileName.data, fileName.data, LastErrorToText().data);

		if (deleteIsRunningfile)
			DeleteIsRunningFile();

		if (m_overflowReported)
			m_logger.Info(TC("Session needs at least %s to not overflow."), BytesToText(m_casMaxBytes).str);

		u64 duration = GetTime() - startTime;
		m_logger.Detail(TC("Database saved in %s (deleted %u dropped cas files)"), TimeToText(duration).str, deleteCount);
		return true;
	}

	bool StorageImpl::CheckCasContent(u32 workerCount)
	{
		StringBuffer<> casRoot;
		casRoot.Append(m_rootDir.data, m_rootDir.count - 1);

		u64 fileTimeNow = GetFileTimeAsSeconds(GetSystemTimeAsFileTime());
		auto writeTimeAgo = [&](StringBufferBase& out, u64 lastWritten)
			{
				u64 secondsAgoTotal = fileTimeNow - GetFileTimeAsSeconds(lastWritten);
				//u64 secondsAgo = secondsAgoTotal % 60;
				u64 daysAgo = secondsAgoTotal / (60 * 60 * 24);
				u64 hoursAgo = secondsAgoTotal / (60 * 60) % 24;
				u64 minutesAgo = (secondsAgoTotal / 60) % 60;
				out.Appendf(TC("%llud %02lluh %02llum"), daysAgo, hoursAgo, minutesAgo);
				//if (hoursAgo)
				//	timeStr.Appendf(TC(" %lluhours"), hoursAgo);
				//if (minutesAgo)
				//	timeStr.Appendf(TC(" %llumin"), minutesAgo);
			};

		m_logger.Info(TC("Traverse cas database..."));
		WorkManagerImpl workManager(workerCount, TC("UbaWrk/ChkCasC"));
		u32 entryCount = 0;
		u32 errorCount = 0;
		u64 newestWrittenError = 0;
		Futex lock;
		TraverseAllCasFiles(casRoot.data, [&](const StringBufferBase& fullPath, const DirectoryEntry& e)
			{
				++entryCount;
				workManager.AddWork([&, filePath = fullPath.ToString(), name = TString(e.name), lastWritten = e.lastWritten](const WorkContext&)
					{
						StringBuffer<> timeStr;
						writeTimeAgo(timeStr, lastWritten);

						
						//m_logger.Info(TC("Validating %s (%s ago)"), name.c_str(), timeStr.data);
						CasKey casKey = CasKeyFromString(name.c_str());

						auto reportError = MakeGuard([&]()
							{
								SCOPED_FUTEX(lock, l);
								++errorCount;
								if (lastWritten > newestWrittenError)
									newestWrittenError = lastWritten;
							});

						CasKey checkedKey = EmptyFileKey;
						if (IsCompressed(casKey))
						{
							FileAccessor file(m_logger, filePath.c_str());
							if (!file.OpenMemoryRead())
								return;
							if (u64 fileSize = file.GetSize())
							{
								u8* mem = file.GetData();
								u64 decompressedSize = *(u64*)mem;
								mem += sizeof(u64);
								u8* dest = new u8[decompressedSize];
								auto g = MakeGuard([dest]() { delete[] dest; });
								if (!DecompressMemoryToMemory(mem, fileSize, dest, decompressedSize, filePath.c_str(), TC("Memory")))
									return;
								checkedKey = CalculateCasKey(dest, decompressedSize, true);
							}
						}
						else
						{
							if (!CalculateCasKey(checkedKey, filePath.c_str()))
								return;
							checkedKey = AsCompressed(checkedKey, false);
 						}
						if (casKey == checkedKey)
						{
							reportError.Cancel();
							return;
						}
						m_logger.Error(TC("Cas key and content mismatch for key %s (expected %s) (%s ago)"), CasKeyString(casKey).str, CasKeyString(checkedKey).str, timeStr.data);
						
					}, 1, TC("CheckCasContent"));
			});
		m_logger.Info(TC("Validating %u entries..."), entryCount);

		workManager.FlushWork();

		StringBuffer<> newestLastWrittenStr;
		writeTimeAgo(newestLastWrittenStr, newestWrittenError);

		if (errorCount == 0)
			m_logger.Info(TC("Done. No errors found"));
		else
			m_logger.Info(TC("Done. Found %u errors out of %u entries (Last written bad entry was %s)"), errorCount, entryCount, newestLastWrittenStr.data);
		return true;
	}

	bool StorageImpl::CheckFileTable(const tchar* searchPath, u32 workerCount)
	{
		m_logger.Info(TC("Searching %s to check files against file table..."), searchPath);
		WorkManagerImpl workManager(workerCount, TC("UbaWrk/ChkFTbl"));

		List<TString> directories;
		directories.push_back(searchPath);

		Atomic<u32> foundFiles;
		Atomic<u32> trackedFiles;
		Atomic<u32> testedFiles;
		Atomic<u32> errorCount;

		u64 startTime = GetTime();
		
		while (!directories.empty())
		{
			TString dir = directories.front();
			directories.pop_front();
			TraverseDir(m_logger, dir, [&](const DirectoryEntry& e)
				{
					StringBuffer<> path(dir);
					path.EnsureEndsWithSlash().Append(e.name);
					if (CaseInsensitiveFs)
						path.MakeLower();

					if (IsDirectory(e.attributes))
					{
						if (Equals(e.name, TC("Content")))
							return;
						directories.push_back(path.data);
						return;
					}
					u64 lastWritten = e.lastWritten;
					u64 size = e.size;

					workManager.AddWork([&, p = path.ToString(), lastWritten, size](const WorkContext&)
						{
							++foundFiles;
							StringKey key = ToStringKey(p.data(), p.size());
							
							auto findIt = m_fileTableLookup.find(key);
							if (findIt == m_fileTableLookup.end())
								return;
							++trackedFiles;

							FileEntry& fe = findIt->second;
							if (fe.lastWritten != lastWritten || fe.size != size)
								return;
							++testedFiles;

							CasKey casKey;

							{
								FileAccessor file(m_logger, p.data());
								if (!file.OpenMemoryRead())
									return;
								if (file.GetSize() > sizeof(CompressedFileHeader) && ((CompressedFileHeader*)file.GetData())->IsValid())
									casKey = ((CompressedFileHeader*)file.GetData())->casKey;
							}
							if (casKey == CasKeyZero)
								if (!CalculateCasKey(casKey, p.data()))
									m_logger.Warning(TC("Failed to calculate cas key for %s"), p.data());

							if (casKey != fe.casKey)
							{
								++errorCount;
								m_logger.Error(TC("CasKey mismatch for %s even though size and lastwritten were the same. Corrupt path table! (Correct: %s. Wrong: %s)"), p.data(), CasKeyString(casKey).str, CasKeyString(fe.casKey).str);
							}

						}, 1, TC("CheckFileTable"));
				});
		}
		workManager.FlushWork();

		m_logger.Info(TC("Done. %u errors found. Searched %u files where %u was tracked and %u matched table."), errorCount.load(), foundFiles.load(), trackedFiles.load(), testedFiles.load(), TimeToText(GetTime() - startTime).str);

		return errorCount == 0;
	}

	const tchar* StorageImpl::GetTempPath()
	{
		return m_tempPath.data;
	}

	u64 StorageImpl::GetStorageCapacity()
	{
		return m_casCapacityBytes;
	}

	u64 StorageImpl::GetStorageUsed()
	{
		return m_casTotalBytes;
	}

	bool StorageImpl::GetZone(StringBufferBase& out)
	{
		return false;
	}

	bool StorageImpl::Reset()
	{
		m_casLookup.clear();
		m_fileTableLookup.clear();
		m_newestAccessed = nullptr;
		m_oldestAccessed = nullptr;
		m_casTotalBytes = 0;
		m_casMaxBytes = 0;

		DeleteAllCas();
		return true;
	}

	bool StorageImpl::DeleteAllCas()
	{
		u32 deleteCount = 0;

		WorkManagerImpl workManager(GetLogicalProcessorCount(), TC("UbaWrk/DelCas"));
		{
			Atomic<u32> atomicDeleteCount;
			TraverseDir(m_logger, m_rootDir, [&](const DirectoryEntry& e)
				{
					if (!IsDirectory(e.attributes))
						return;
					workManager.AddWork([&, name = TString(e.name)](const WorkContext&)
						{
							StringBuffer<> fullPath;
							fullPath.Append(m_rootDir).Append(name);
							u32 deleteCountTemp = 0;
							DeleteAllFiles(m_logger, fullPath.data, true, &deleteCountTemp);
							atomicDeleteCount += deleteCountTemp;
						}, 1, TC("DeleteAllCas"));
				});
			workManager.FlushWork();
			deleteCount += atomicDeleteCount;
		}

		bool res = DeleteAllFiles(m_logger, m_rootDir.data, false, &deleteCount);
		m_logger.Info(TC("Deleted %u cas files"), deleteCount);
		m_dirCache.Clear();
		return res;
	}

	bool StorageImpl::RetrieveCasFile(RetrieveResult& out, const CasKey& casKey, const tchar* hint, FileMappingBuffer* mappingBuffer, u64 memoryMapAlignment, bool allowProxy, u32 clientId)
	{
		UBA_ASSERT(false);
		return false;
	}

	bool StorageImpl::VerifyAndGetCachedFileInfo(CachedFileInfo& out, StringKey fileNameKey, u64 verifiedLastWriteTime, u64 verifiedSize)
	{
		out.casKey = CasKeyZero;
		SCOPED_READ_LOCK(m_fileTableLookupLock, lookupLock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt == m_fileTableLookup.end())
			return false;
		FileEntry& fileEntry = findIt->second;
		lookupLock.Leave();

		SCOPED_FUTEX(fileEntry.lock, entryLock);
		
		fileEntry.verified = fileEntry.lastWritten == verifiedLastWriteTime && fileEntry.size == verifiedSize && fileEntry.casKey != CasKeyInvalid;

		if (!fileEntry.verified)
			return false;
		out.casKey = fileEntry.casKey;
		return true;
	}

	bool StorageImpl::InvalidateCachedFileInfo(StringKey fileNameKey)
	{
		SCOPED_READ_LOCK(m_fileTableLookupLock, lookupLock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt == m_fileTableLookup.end())
			return false;
		FileEntry& fileEntry = findIt->second;
		lookupLock.Leave();

		SCOPED_FUTEX(fileEntry.lock, entryLock);
		fileEntry.verified = false;
		fileEntry.casKey = CasKeyInvalid;
		return true;
	}

	bool StorageImpl::StoreCasFile(CasKey& out, const tchar* fileName, const CasKey& casKeyOverride, bool deferCreation)
	{
		StringBuffer<> forKey;
		forKey.Append(fileName);
		if (CaseInsensitiveFs)
			forKey.MakeLower();
		StringKey fileNameKey = ToStringKey(forKey);

		FileEntry& fileEntry = GetOrCreateFileEntry(fileNameKey);
		SCOPED_FUTEX(fileEntry.lock, entryLock);

		if (fileEntry.verified)
		{
			UBA_ASSERT(fileEntry.casKey != CasKeyInvalid);

			if (fileEntry.casKey != CasKeyZero)
			{
				UBA_ASSERT(casKeyOverride == CasKeyZero || casKeyOverride == fileEntry.casKey);
 				if (!AddCasFile(fileNameKey, fileName, fileEntry.casKey, deferCreation))
					return false;
			}
			out = fileEntry.casKey;
			return true;
		}
		fileEntry.verified = true;


		ExternalFileMapping externalMapping;

		u64 fileSize = 0;
		u64 lastWriteTime = 0;

		FileHandle fileHandle = InvalidFileHandle;
		auto fileGuard = MakeGuard([&](){ CloseFile(fileName, fileHandle); });

		if (m_externalFileMappingsProvider && m_externalFileMappingsProvider(externalMapping, fileNameKey, fileName))
		{
			fileSize = externalMapping.size;
			lastWriteTime = externalMapping.lastWriteTime;
		}
		else
		{
			// Use OpenFile+GetFileInformationByHandle+close instead of Getting file attributes because it is actually faster on cloud setups (weirdly enough)
			if (!OpenFileSequentialRead(m_logger, fileName, fileHandle))
			{
				fileEntry.casKey = CasKeyZero;
				out = CasKeyZero;
				return true;
			}

			FileBasicInformation info;
			if (!GetFileBasicInformationByHandle(info, m_logger, fileName, fileHandle))
			{
				fileEntry.casKey = CasKeyZero;
				return m_logger.Error(TC("GetFileInformationByHandle failed on %s"), fileName);
			}
			fileSize = info.size;
			lastWriteTime = info.lastWriteTime;
		}

		if (fileEntry.casKey != CasKeyZero)
		{
			if (casKeyOverride != CasKeyZero && casKeyOverride != fileEntry.casKey)
			{
				fileEntry.casKey = casKeyOverride;
				if (!AddCasFile(fileNameKey, fileName, fileEntry.casKey, deferCreation))
					return false;
				out = fileEntry.casKey;
				return true;
			}
			if (fileSize == fileEntry.size && (lastWriteTime && lastWriteTime == fileEntry.lastWritten) && fileEntry.casKey != CasKeyInvalid)
			{
				if (!AddCasFile(fileNameKey, fileName, fileEntry.casKey, deferCreation))
					return false;
				out = fileEntry.casKey;
				return true;
			}
		}

		fileEntry.size = fileSize;
		fileEntry.lastWritten = lastWriteTime;
		if (casKeyOverride == CasKeyZero)
		{
			if (externalMapping.handle.IsValid())
			{
				FileMappingScope scope(m_logger);
				u8* fileMem = scope.MapView(TC("StoreCasFile"), externalMapping.handle, externalMapping.offset, externalMapping.size, fileName);
				if (!fileMem)
					return false;
				if (fileSize > sizeof(CompressedFileHeader) && ((CompressedFileHeader*)fileMem)->IsValid())
					fileEntry.casKey = ((CompressedFileHeader*)fileMem)->casKey;
				else
					fileEntry.casKey = CalculateCasKey(fileMem, externalMapping.size, m_storeCompressed);
			}
			else
			{
				bool handled = false;
				if (fileSize >= sizeof(CompressedFileHeader) && g_globalRules.FileCanBeCompressed(ToView(fileName)))
				{
					CompressedFileHeader header(CasKeyZero);
					if (!ReadFile(m_logger, fileName, fileHandle, &header, sizeof(header)))
						return m_logger.Error(TC("Failed to read header of compressed file %s (%s)"), fileName, LastErrorToText().data);
					if (header.IsValid())
					{
						fileEntry.casKey = AsCompressed(header.casKey, m_storeCompressed);
						handled = true;
					}
					else
					{
						if (!SetFilePointer(m_logger, fileName, fileHandle, 0))
							return false;
					}
				}

				if (!handled)
					fileEntry.casKey = CalculateCasKey(fileName, fileHandle, fileSize, m_storeCompressed);
			}
		}
		else
			fileEntry.casKey = casKeyOverride;

		if (fileEntry.casKey == CasKeyZero)
			return false;

		if (!AddCasFile(fileNameKey, fileName, fileEntry.casKey, deferCreation))
			return false;

		out = fileEntry.casKey;
		return true;
	}

	bool StorageImpl::IsFileVerified(const StringKey& fileNameKey)
	{
		SCOPED_READ_LOCK(m_fileTableLookupLock, lookupLock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt == m_fileTableLookup.end())
			return false;
		FileEntry& fileEntry = findIt->second;
		lookupLock.Leave();
		SCOPED_FUTEX_READ(fileEntry.lock, entryLock);
		return fileEntry.verified;
	}

	void StorageImpl::ReportFileInfoWeak(const StringKey& fileNameKey, u64 verifiedLastWriteTime, u64 verifiedSize)
	{
		SCOPED_READ_LOCK(m_fileTableLookupLock, lookupLock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt == m_fileTableLookup.end())
			return;
		FileEntry& fileEntry = findIt->second;
		lookupLock.Leave();

		SCOPED_FUTEX(fileEntry.lock, entryLock);
		if (fileEntry.verified)
			return;
		fileEntry.verified = fileEntry.lastWritten == verifiedLastWriteTime && fileEntry.size == verifiedSize && fileEntry.casKey != CasKeyInvalid;
	}

	bool StorageImpl::HasBeenSeen(const CasKey& casKey)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lookupLock);
		return m_casLookup.find(casKey) != m_casLookup.end();
	}

	bool StorageImpl::StoreCasKey(CasKey& out, const tchar* fileName, const CasKey& casKeyOverride)
	{
		StringBuffer<> forKey;
		forKey.Append(fileName);
		if (CaseInsensitiveFs)
			forKey.MakeLower();
		StringKey fileNameKey = ToStringKey(forKey);
		return StoreCasKey(out, fileNameKey, fileName, casKeyOverride);
	}

	bool StorageImpl::StoreCasKey(CasKey& out, const StringKey& fileNameKey, const tchar* fileName, const CasKey& casKeyOverride)
	{
		FileEntry& fileEntry = GetOrCreateFileEntry(fileNameKey);

		SCOPED_FUTEX(fileEntry.lock, entryLock);
	
		if (fileEntry.verified)
		{
			out = fileEntry.casKey;
			return true;
		}
		fileEntry.verified = true;

		// Use OpenFile+GetFileInformationByHandle+close instead of Getting file attributes because it is actually faster on cloud setups (weirdly enough)
		FileHandle fileHandle;
		if (!OpenFileSequentialRead(m_logger, fileName, fileHandle))
		{
			fileEntry.casKey = CasKeyZero;
			out = CasKeyZero;
			return true;
		}
		auto fileGuard = MakeGuard([&](){ CloseFile(fileName, fileHandle); });

		FileBasicInformation info;
		if (!GetFileBasicInformationByHandle(info, m_logger, fileName, fileHandle))
		{
			fileEntry.casKey = CasKeyZero;
			return m_logger.Error(TC("GetFileInformationByHandle failed on %s"), fileName);
		}

		u64 fileSize = info.size;
		u64 lastWritten = info.lastWriteTime;

		if (fileEntry.casKey != CasKeyZero)
		{
			if (casKeyOverride != CasKeyZero && casKeyOverride != fileEntry.casKey)
			{
				fileEntry.casKey = casKeyOverride;
				out = fileEntry.casKey;
				return true;
			}
			if (fileSize == fileEntry.size && lastWritten == fileEntry.lastWritten && fileEntry.casKey != CasKeyInvalid)
			{
				out = fileEntry.casKey;
				return true;
			}
		}

		fileEntry.size = fileSize;
		fileEntry.lastWritten = lastWritten;
		if (casKeyOverride == CasKeyZero)
		{
			bool handled = false;
			if (fileSize >= sizeof(CompressedFileHeader) && g_globalRules.FileCanBeCompressed(ToView(fileName)))
			{
				CompressedFileHeader header(CasKeyZero);
				if (!ReadFile(m_logger, fileName, fileHandle, &header, sizeof(header)))
					return m_logger.Error(TC("Failed to read header of compressed file %s (%s)"), fileName, LastErrorToText().data);
				if (header.IsValid())
				{
					fileEntry.casKey = AsCompressed(header.casKey, m_storeCompressed);
					handled = true;
				}
				else
				{
					if (!SetFilePointer(m_logger, fileName, fileHandle, 0))
						return false;
				}
			}

			if (!handled)
				fileEntry.casKey = CalculateCasKey(fileName, fileHandle, fileSize, m_storeCompressed);
		}
		else
			fileEntry.casKey = casKeyOverride;

		if (fileEntry.casKey == CasKeyZero)
			return false;

		out = fileEntry.casKey;
		return true;
	}

	bool StorageImpl::StoreCasFileClient(CasKey& out, StringKey fileNameKey, const tchar* fileName, FileMappingHandle mappingHandle, u64 mappingOffset, u64 fileSize, const tchar* hint, bool keepMappingInMemory, bool storeCompressed)
	{
		UBA_ASSERT(false);
		return false;
	}

	bool StorageImpl::HasCasFile(const CasKey& casKey, CasEntry** out)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lookupLock);
		auto it = m_casLookup.find(casKey);
		if (it == m_casLookup.end())
			return false;
		CasEntry& casEntry = it->second;
		lookupLock.Leave();
		CasEntryAccessed(casEntry);

		if (out)
			*out = &casEntry;

		SCOPED_WRITE_LOCK(casEntry.lock, entryLock);

		if (casEntry.verified && casEntry.exists)
			return true;

		SCOPED_WRITE_LOCK(m_deferredCasCreationLookupLock, deferredLock);
		auto findIt = m_deferredCasCreationLookup.find(casKey);
		if (findIt == m_deferredCasCreationLookup.end())
			return false;
		StringKey fileNameKey = findIt->second.fileNameKey;
		StringBuffer<> deferredCreation(findIt->second.fileName);
		for (auto& it2 : findIt->second.names)
			m_deferredCasCreationLookupByName.erase(it2);
		m_deferredCasCreationLookup.erase(findIt);

		if (casEntry.beingWritten)
			m_logger.Warning(TC("Deferred cas %s is being written from network. This should never happen (%s)"), CasKeyString(casKey).str, deferredCreation.data);

		casEntry.verified = true;
		deferredLock.Leave();
		WriteResult res;
		if (!WriteCasFile(res, deferredCreation.data, casKey))
			return m_logger.Error(TC("Failed to write deferred cas %s (%s)"), CasKeyString(casKey).str, deferredCreation.data);

		casEntry.exists = true;
		entryLock.Leave();

		CasEntryWritten(casEntry, res.size);
		return true;
	}

	bool StorageImpl::EnsureCasFile(const CasKey& casKey, const tchar* fileName)
	{
		CasEntry& casEntry = GetOrCreateCasEntry(casKey);
		CasEntryAccessed(casEntry);

		{
			SCOPED_READ_LOCK(casEntry.lock, entryLock);
			if (casEntry.verified)
			{
				if (casEntry.exists)
					return true;
				if (!fileName)
					return false;
			}
		}

		SCOPED_WRITE_LOCK(casEntry.lock, entryLock);

		if (casEntry.verified)
		{
			if (casEntry.exists)
				return true;
			if (!fileName)
				return false;
		}

		StringBuffer<> casFile;
		if (!StorageImpl::GetCasFileName(casFile, casKey))
			return false;

		bool verifyReturnValue;
		if (VerifyExisting(verifyReturnValue, entryLock, casKey, casEntry, casFile))
			return verifyReturnValue;

		casEntry.exists = false;
		casEntry.verified = true;
		if (!fileName)
			return false;
		WriteResult res;
		if (!WriteCasFile(res, fileName, casKey))
			return false;
		casEntry.exists = true;
		entryLock.Leave();
		CasEntryWritten(casEntry, res.size);
		return true;
	}

	bool StorageImpl::GetCasFileName(StringBufferBase& out, const CasKey& casKey)
	{
		out.Append(m_rootDir.data).AppendHex(((const u8*)&casKey)[0]);
		if (m_writeToDisk)
			if (!CreateDirectory(out.data))
				return false;
		out.Append(PathSeparator).Append(CasKeyString(casKey).str);
		return true;
	}

	MappedView StorageImpl::MapView(const CasKey& casKey, const tchar* hint)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lookupLock);
		auto findIt = m_casLookup.find(casKey);
		bool foundEntry = findIt != m_casLookup.end();
		if (!foundEntry)
		{
			m_logger.Error(TC("Can't find %s inside cas database (%s)"), CasKeyString(casKey).str, hint);
			return {{},0,0 };
		}
		CasEntry& casEntry = findIt->second;
		lookupLock.Leave();
		SCOPED_WRITE_LOCK(casEntry.lock, entryLock);
		//if (!casEntry.verified)
		//{
		//	m_logger.Error(TC("Trying to use unverified mapping of %s (%s)"), CasKeyString(casKey).str, hint);
		//	return {{},0,0 };
		//}
		FileMappingHandle handle = casEntry.mappingHandle;
		u64 offset = casEntry.mappingOffset;
		u64 size = casEntry.mappingSize;
		entryLock.Leave();

		auto res = m_casDataBuffer.MapView(handle, offset, size, hint);
		if (!res.memory)
			m_logger.Error(TC("Failed to map view for %s (%s)"), CasKeyString(casKey).str, hint);
		return res;
	}

	void StorageImpl::UnmapView(const MappedView& view, const tchar* hint)
	{
		m_casDataBuffer.UnmapView(view, hint);
	}

	bool StorageImpl::DropCasFile(const CasKey& casKey, bool forceDelete, const tchar* hint)
	{
		SCOPED_READ_LOCK(m_casLookupLock, lookupLock);
		auto findIt = m_casLookup.find(casKey);
		bool foundEntry = findIt != m_casLookup.end();
		if (!foundEntry)
		{
			if (forceDelete)
			{
				StringBuffer<> casFile;
				if (!StorageImpl::GetCasFileName(casFile, casKey))
					return false;
				if (DeleteFileW(casFile.data) == 0)
				{
					u32 lastError = GetLastError();
					if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
						return m_logger.Error(TC("Failed to drop cas %s (%s) (%s)"), casFile.data, hint, LastErrorToText(lastError).data);
				}
			}
			return true;
		}
		CasEntry& casEntry = findIt->second;
		lookupLock.Leave();
	
		SCOPED_WRITE_LOCK(casEntry.lock, entryLock);

		if (forceDelete)
		{
			StringBuffer<> casFile;
			if (!StorageImpl::GetCasFileName(casFile, casKey))
				return false;

			u64 sizeDeleted = 0;
			if (DeleteFileW(casFile.data) == 0)
			{
				u32 lastError = GetLastError();
				if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND)
					return m_logger.Error(TC("Failed to drop cas %s (%s) (%s)"), casFile.data, hint, LastErrorToText(lastError).data);
			}
			else
			{
				m_casDroppedBytes += casEntry.size;
				m_casDroppedCount++;
				//m_logger.Debug(TC("Evicted %s from cache (%llukb)"), casFile, casEntry.size/1024);
				sizeDeleted = casEntry.size;
			}
			casEntry.verified = true;
			casEntry.exists = false;
			entryLock.Leave();
	
			CasEntryDeleted(casEntry, sizeDeleted);
		}
		else
		{
			casEntry.dropped = true;
		}

		return true;
	}

	bool StorageImpl::ReportBadCasFile(const CasKey& casKey)
	{
		DropCasFile(casKey, true, TC("BadCasFile"));
		return true;
	}

	bool StorageImpl::CalculateCasKey(CasKey& out, const tchar* fileName)
	{
		FileHandle fileHandle;
		if (!OpenFileSequentialRead(m_logger, fileName, fileHandle))
			return m_logger.Error(TC("[CalculateCasKey] OpenFileSequentialRead failed for %s (%s)"), fileName, LastErrorToText().data);

		auto fileGuard = MakeGuard([&](){ CloseFile(fileName, fileHandle); });

		u64 fileSize;
		if (!uba::GetFileSizeEx(fileSize, fileHandle))
			return m_logger.Error(TC("[CalculateCasKey] GetFileSize failed for %s (%s)"), fileName, LastErrorToText().data);

		out = CalculateCasKey(fileName, fileHandle, fileSize, true);
		return out != CasKeyZero;
	}

	bool StorageImpl::CopyOrLink(const CasKey& casKey, const tchar* destination, u32 fileAttributes, bool writeCompressed, const FormattingFunc& formattingFunc, bool isTemp, bool allowHardLink)
	{
		UBA_ASSERT(casKey != CasKeyZero);
		UBA_ASSERT(fileAttributes);

		auto& stats = Stats();
		
		#if PLATFORM_WINDOWS
		BottleneckScope bs(m_maxParallelCopyOrLinkBottleneck, stats.copyOrLinkWait);
		#endif

		StringBuffer<> forKey;
		forKey.Append(destination);
		if (CaseInsensitiveFs)
			forKey.MakeLower();
		StringKey key = ToStringKey(forKey);
		FileEntry& fileEntry = GetOrCreateFileEntry(key);

		TimerScope ts(stats.copyOrLink);

		CasKey actualKey = AsCompressed(casKey, writeCompressed);

		bool testCompressed = !writeCompressed;
		while (true)
		{
			CasEntry* casEntry = nullptr;
			if (!HasCasFile(actualKey, &casEntry))
			{
				if (!testCompressed)
					return m_logger.Error(TC("[CopyOrLink] Trying to copy cas %s to %s but can't find neither compressed or uncompressed version"), CasKeyString(actualKey).str, destination);

				actualKey = AsCompressed(casKey, true);
				testCompressed = false;
				continue;
			}

			SCOPED_READ_LOCK(casEntry->lock, casEntryLock);
			UBA_ASSERT(casEntry->verified);
			UBA_ASSERT(casEntry->exists);

			if (IsCompressed(actualKey))
			{
				FileHandle readHandle = InvalidFileHandle;
				auto rsg = MakeGuard([&](){ if (readHandle != InvalidFileHandle) CloseFile(nullptr, readHandle); });

				u8* compressedData = nullptr;
				u8* readData = nullptr;
				MappedView mappedView;
				auto mapViewGuard = MakeGuard([&](){ m_casDataBuffer.UnmapView(mappedView, destination); });

				StringBuffer<512> casFile;
				u64 decompressedSize;

				if (casEntry->mappingHandle.IsValid())
				{
					casFile.Append(CasKeyString(actualKey).str);
					mappedView = m_casDataBuffer.MapView(casEntry->mappingHandle, casEntry->mappingOffset, casEntry->mappingSize, casFile.data);
					compressedData = mappedView.memory;
					if (!compressedData)
						return m_logger.Error(TC("[CopyOrLink] Failed to map view of mapping %s (%s)"), casFile.data, LastErrorToText().data);

					decompressedSize = *(u64*)compressedData;
					readData = compressedData + sizeof(u64);
				}
				else
				{
					if (!StorageImpl::GetCasFileName(casFile, actualKey))
						return false;

					if (!OpenFileSequentialRead(m_logger, casFile.data, readHandle))
						return m_logger.Error(TC("[CopyOrLink] Failed to open file %s for read (%s)"), casFile.data, LastErrorToText().data);

					if (!ReadFile(m_logger, casFile.data, readHandle, &decompressedSize, sizeof(u64)))
						return m_logger.Error(TC("[CopyOrLink] Failed to read first 8 bytes from compressed file %s (%s)"), casFile.data, LastErrorToText().data);
				}

				bool writeDirectlyToFile = casEntry->mappingHandle.IsValid() && false; // Experiment to try to fix bottlenecks on cloud
				bool useNoBuffering = writeDirectlyToFile && false; // Experiment to try to fix bottlenecks on cloud

				u32 writeFlags = useNoBuffering ? FILE_FLAG_NO_BUFFERING : 0;
				bool allowRead = !useNoBuffering;

				FileAccessor destinationFile(m_logger, destination);

				SCOPED_FUTEX(fileEntry.lock, entryLock);
				fileEntry.verified = false;
				fileEntry.isTemp = isTemp;

				u64 sizeOnDisk = decompressedSize;

				auto decompressToMemory = [&](u8* destinationMemory)
					{
						if (readData)
						{
							if (!DecompressMemoryToMemory(readData, mappedView.size, destinationMemory, decompressedSize, casFile.data, destination))
								return false;
						}
						else
						{
							if (!DecompressFileToMemory(CasKeyString(actualKey).str, readHandle, destinationMemory, decompressedSize, destination, 0))
								return false;
						}
						return true;
					};


				if (formattingFunc)
				{
					UBA_ASSERT(!writeCompressed);
					u8* slot = m_bufferSlots.Pop();
					auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });

					UBA_ASSERT(decompressedSize < BufferSlotSize);
					if (!decompressToMemory(slot))
						return false;

					MemoryBlock block(5*1024*1024);

					if (!formattingFunc(block, slot, decompressedSize, destination))
						return false;

					if (!destinationFile.CreateWrite(false, DefaultAttributes(), block.writtenSize, m_tempPath.data))
						return false;
						
					if (!destinationFile.Write(block.memory, block.writtenSize))
						return false;
				}
				else if (writeCompressed)
				{
					u64 compressedFileSize = mappedView.size;
					if (!mappedView.memory)
						if (!GetFileSizeEx(compressedFileSize, readHandle))
							return m_logger.Error(TC("[CopyOrLink] Failed to get file size of compressed file %s (%s)"), casFile.data, LastErrorToText().data);
						
					sizeOnDisk = compressedFileSize + sizeof(CompressedFileHeader);

					if (!destinationFile.CreateMemoryWrite(false, fileAttributes, sizeOnDisk, m_tempPath.data))
						return false;
					u8* writePos = destinationFile.GetData();
					*(CompressedFileHeader*)writePos = CompressedFileHeader(casKey);
					writePos += sizeof(CompressedFileHeader);

					if (mappedView.memory)
					{
						TimerScope cts(stats.memoryCopy);
						MapMemoryCopy(writePos, mappedView.memory, compressedFileSize);
					}
					else
					{
						*(u64*)writePos = decompressedSize;
						writePos += sizeof(u64);
						if (!ReadFile(m_logger, casFile.data, readHandle, writePos, compressedFileSize - sizeof(u64)))
							return m_logger.Error(TC("[CopyOrLink] Failed to read compressed file %s (%s)"), casFile.data, LastErrorToText().data);
					}
						
				}
				else if (writeDirectlyToFile || !decompressedSize)
				{
					if (!destinationFile.CreateWrite(allowRead, writeFlags | fileAttributes, decompressedSize, m_tempPath.data))
						return false;
					if (decompressedSize)
						if (!DecompressMemoryToFile(readData, destinationFile, decompressedSize, useNoBuffering))
							return false;
				}
				else
				{
					if (!destinationFile.CreateMemoryWrite(allowRead, writeFlags | fileAttributes, decompressedSize, m_tempPath.data))
						return false;
					if (!decompressToMemory(destinationFile.GetData()))
						return false;
				}

				u64 lastWriteTime = 0;
				if (!destinationFile.Close(&lastWriteTime))
					return false;
				UBA_ASSERT(lastWriteTime);
				if (lastWriteTime)
				{
					fileEntry.casKey = casKey;
					fileEntry.lastWritten = lastWriteTime;
					fileEntry.size = sizeOnDisk;
					fileEntry.verified = true;
				}
				return true;
			}

			if (casEntry->mappingHandle.IsValid())
			{
				SCOPED_FUTEX(fileEntry.lock, entryLock);
				fileEntry.verified = false;

				MappedView mappedView = m_casDataBuffer.MapView(casEntry->mappingHandle, casEntry->mappingOffset, casEntry->mappingSize, TC(""));
				auto mapViewGuard = MakeGuard([&](){ m_casDataBuffer.UnmapView(mappedView, destination); });

				FileAccessor destinationFile(m_logger, destination);
				if (!destinationFile.CreateMemoryWrite(false, fileAttributes, mappedView.size, m_tempPath.data))
					return false;
				u8* writePos = destinationFile.GetData();
				TimerScope cts(stats.memoryCopy);
				MapMemoryCopy(writePos, mappedView.memory, mappedView.size);
				u64 lastWriteTime = 0;
				if (!destinationFile.Close(&lastWriteTime))
					return false;
				UBA_ASSERT(lastWriteTime);
				if (lastWriteTime)
				{
					fileEntry.casKey = casKey;
					fileEntry.lastWritten = lastWriteTime;
					fileEntry.size = mappedView.size;
					fileEntry.verified = true;
				}
				return true;
			}

			StringBuffer<> casFile;
			if (!GetCasFileName(casFile, actualKey))
				return false;

			SCOPED_FUTEX(fileEntry.lock, entryLock);
			fileEntry.verified = false;

			bool firstTry = true;
			while (true)
			{
				bool success = false;

				#if !PLATFORM_MAC // For some reason creating links on macos causes trouble when they are exec/dylibs and being executed.. sometimes it is like the link behaves like a symlink.. but not always
				if (allowHardLink)
					success = CreateHardLinkW(destination, casFile.data);
				#endif

				if (!success)
					success = uba::CopyFileW(casFile.data, destination, true) != 0;

				if (success)
				{
					#if !PLATFORM_WINDOWS
					if (fileAttributes & S_IXUSR)
					{
						struct stat destStat;
						int res = stat(destination, &destStat);
						UBA_ASSERTF(res == 0, TC("stat failed (%s) error: %s"), destination, strerror(errno));
						if ((destStat.st_mode & S_IXUSR) == 0)
						{
							res = chmod(destination, S_IRUSR | S_IWUSR | S_IXUSR); (void)res;
							UBA_ASSERTF(res == 0, TC("chmod failed (%s) error: %s"), destination, strerror(errno));
						}
					}
					#endif
					return true;
				}
				if (!firstTry)
					return m_logger.Error(TC("Failed link/copy %s to %s (%s)"), casFile.data, destination, LastErrorToText().data);

				firstTry = false;
				DeleteFileW(destination);
				continue;

				//SetFileTime(destination, DateTime.UtcNow);
			}
		}

	}

	bool StorageImpl::FakeCopy(const CasKey& casKey, const tchar* destination, u64 size, u64 lastWritten, bool deleteExisting)
	{
		if (deleteExisting) 
			DeleteFileW(destination);

		StringBuffer<> forKey;
		forKey.Append(destination);
		if (CaseInsensitiveFs)
			forKey.MakeLower();
		StringKey key = ToStringKey(forKey);
		FileEntry& fileEntry = GetOrCreateFileEntry(key);
		SCOPED_FUTEX(fileEntry.lock, lock2);
		fileEntry.casKey = casKey;
		fileEntry.lastWritten = lastWritten;
		fileEntry.size = size;
		fileEntry.verified = true;
		return true;
	}

	void StorageImpl::ReportFileWrite(StringKey fileNameKey, const tchar* fileName)
	{
		// If a defered cas creation is queued up while the source file is about to be modified we need to flush out the cas creation before modifying the file
		SCOPED_READ_LOCK(m_deferredCasCreationLookupLock, deferredLock);
		auto findIt = m_deferredCasCreationLookupByName.find(fileNameKey);
		if (findIt == m_deferredCasCreationLookupByName.end())
			return;
		CasKey key = findIt->second;
		deferredLock.Leave();
		HasCasFile(key);
	}
	/*
	void StorageImpl::PushStats(StorageStats* stats)
	{
		t_threadStats = stats;
	}

	void StorageImpl::PopStats(StorageStats* stats)
	{
		m_stats.calculateCasKey.Add(stats->calculateCasKey);
		m_stats.copyOrLink.Add(stats->copyOrLink);
		m_stats.copyOrLinkWait.Add(stats->copyOrLinkWait);
		m_stats.sendCas.Add(stats->sendCas);
		m_stats.recvCas.Add(stats->recvCas);
		m_stats.compressWrite.Add(stats->compressWrite);
		m_stats.compressSend.Add(stats->compressSend);
		m_stats.decompressRecv.Add(stats->decompressRecv);
		m_stats.decompressToMem.Add(stats->decompressToMem);
		m_stats.sendCasBytesRaw += stats->sendCasBytesRaw;
		m_stats.sendCasBytesComp += stats->sendCasBytesComp;
		m_stats.recvCasBytesRaw += stats->recvCasBytesRaw;
		m_stats.recvCasBytesComp += stats->recvCasBytesComp;
		m_stats.createCas.Add(stats->createCas);
		m_stats.createCasBytesRaw += stats->createCasBytesRaw;
		m_stats.createCasBytesComp += stats->createCasBytesComp;
		t_threadStats = nullptr;
	}
	*/
	StorageStats& StorageImpl::Stats()
	{
		if (StorageStats* s = StorageStats::GetCurrent())
			return *s;
		return m_stats;
	}

	void StorageImpl::AddStats(StorageStats& stats)
	{
		m_stats.Add(stats);
	}

	void StorageImpl::PrintSummary(Logger& logger)
	{
		logger.Info(TC("  ------- Storage stats summary -------"));
		if (m_casLookup.empty())
		{
			logger.Info(TC("  Storage not loaded"));
			logger.Info(TC(""));
			return;
		}
		
		logger.Info(TC("  WorkMemoryBuffers    %6u %9s"), u32(m_bufferSlots.m_slots.size()), BytesToText(m_bufferSlots.m_slots.size() * BufferSlotSize).str);
		logger.Info(TC("  FileTable            %6u"), u32(m_fileTableLookup.size()));

		StorageStats& stats = Stats();
		u64 casBufferSize;
		u32 casBufferCount;
		m_casDataBuffer.GetSizeAndCount(MappedView_Transient, casBufferSize, casBufferCount);
		logger.Info(TC("  CasDataBuffers       %6u %9s"), casBufferCount, BytesToText(casBufferSize).str);
		logger.Info(TC("  CasTable             %6u %9s"), u32(m_casLookup.size()), BytesToText(m_casTotalBytes).str);
		logger.Info(TC("     Dropped           %6u %9s"), m_casDroppedCount, BytesToText(m_casDroppedBytes).str);
		logger.Info(TC("     Evicted           %6u %9s"), m_casEvictedCount, BytesToText(m_casEvictedBytes).str);
		logger.Info(TC("     HandleOverflow    %6u %9s"), stats.handleOverflow.count.load(), TimeToText(stats.handleOverflow.time).str);
		stats.Print(logger);

		if (u64 deferredCount = m_deferredCasCreationLookup.size())
			logger.Info(TC("  DeferredCasSkipped   %6u"), u32(deferredCount));
		logger.Info(TC(""));
	}

	CasKey StorageImpl::CalculateCasKey(u8* fileMem, u64 fileSize, bool storeCompressed)
	{
		StorageStats& stats = Stats();
		TimerScope ts(stats.calculateCasKey);
		return uba::CalculateCasKey(fileMem, fileSize, storeCompressed, m_workManager, nullptr);
	}


	CasKey StorageImpl::CalculateCasKey(const tchar* fileName, FileHandle fileHandle, u64 fileSize, bool storeCompressed)
	{
		StorageStats& stats = Stats();
		TimerScope ts(stats.calculateCasKey);

		if (fileSize > BufferSlotSize) // Note that when filesize is larger than BufferSlotSize the hash becomes a hash of hashes
		{
			FileMappingHandle fileMapping = uba::CreateFileMappingW(m_logger, fileHandle, PAGE_READONLY, fileSize, fileName);
			if (!fileMapping.IsValid())
			{
				m_logger.Error(TC("Failed to create file mapping for %s (%s)"), fileName, LastErrorToText().data);
				return CasKeyZero;
			}
			auto fmg = MakeGuard([&]() { CloseFileMapping(m_logger, fileMapping, fileName); });
			u8* fileData = MapViewOfFile(m_logger, fileMapping, FILE_MAP_READ, 0, fileSize);
			if (!fileData)
			{
				m_logger.Error(TC("Failed to map view of file mapping for %s (%s)"), fileName, LastErrorToText().data);
				return CasKeyZero;
			}
			auto udg = MakeGuard([&]() { 
					if (m_asyncUnmapViewOfFile)
						m_workManager->AddWork([this, fileData, fileSize, fn = TString(fileName)](const WorkContext&) { UnmapViewOfFile(m_logger, fileData, fileSize, fn.c_str()); }, 1, TC("UnmapFile"));
					else
						UnmapViewOfFile(m_logger, fileData, fileSize, fileName);
				});

			return uba::CalculateCasKey(fileData, fileSize, storeCompressed, m_workManager, fileName);
		}

		CasKeyHasher hasher;
		u8* slot = m_bufferSlots.Pop();
		auto _ = MakeGuard([&](){ m_bufferSlots.Push(slot); });
		u64 left = fileSize;
		while (left)
		{
			u32 toRead = u32(Min(left, BufferSlotSize));
			if (!ReadFile(m_logger, fileName, fileHandle, slot, toRead))
				return CasKeyZero;
			hasher.Update(slot, toRead);
			left -= toRead;
		}

		return ToCasKey(hasher, storeCompressed);
	}

	bool StorageImpl::DecompressFileToMemory(const tchar* fileName, FileHandle fileHandle, u8* dest, u64 decompressedSize, const tchar* writeHint, u64 fileStartOffset)
	{
		if (m_workManager && decompressedSize > BufferSlotSize*4) // Arbitrary size threshold. We want to at least catch the pch here
		{
			u64 compressedSize;
			if (!uba::GetFileSizeEx(compressedSize, fileHandle))
				return m_logger.Error(TC("GetFileSize failed for %s (%s)"), fileName, LastErrorToText().data);
			FileMappingHandle fileMapping = uba::CreateFileMappingW(m_logger, fileHandle, PAGE_READONLY, compressedSize, fileName);
			if (!fileMapping.IsValid())
				return m_logger.Error(TC("Failed to create file mapping for %s (%s)"), fileName, LastErrorToText().data);
			auto fmg = MakeGuard([&]() { CloseFileMapping(m_logger, fileMapping, fileName); });
			u8* fileData = MapViewOfFile(m_logger, fileMapping, FILE_MAP_READ, 0, compressedSize);
			if (!fileData)
				return m_logger.Error(TC("Failed to map view of file mapping for %s (%s)"), fileName, LastErrorToText().data);
			auto udg = MakeGuard([&]() { UnmapViewOfFile(m_logger, fileData, compressedSize, fileName); });
			
			u8* readPos = fileData + 8 + fileStartOffset;
			if (!DecompressMemoryToMemory(readPos, compressedSize, dest, decompressedSize, fileName, writeHint))
				return false;
		}
		else
		{
			StorageStats& stats = Stats();
			u8* slot = m_bufferSlots.Pop();
			auto _ = MakeGuard([&]() { m_bufferSlots.Push(slot); });

			void* decoderMem = slot + BufferSlotHalfSize;
			u64 decoderMemSize = BufferSlotHalfSize;

			u64 bytesRead = 8; // We know size has already been read

			u8* readBuffer = slot;
			u8* writePos = dest;
			u64 left = decompressedSize;
			while (left)
			{
				u32 sizes[2];
				if (!ReadFile(m_logger, fileName, fileHandle, sizes, sizeof(u32) * 2))
				{
					u64 compressedSize;
					if (!uba::GetFileSizeEx(compressedSize, fileHandle))
						return m_logger.Error(TC("GetFileSize failed for %s (%s)"), fileName, LastErrorToText().data);
					if (bytesRead + 8 > compressedSize)
						return m_logger.Error(TC("File %s corrupt. Tried to read 8 bytes. File is smaller than expected (Read: %llu, Size: %llu)"), fileName, bytesRead, compressedSize);
					return false;
				}
				u32 compressedBlockSize = sizes[0];
				u32 decompressedBlockSize = sizes[1];

				bytesRead += 8;

				if (!ReadFile(m_logger, fileName, fileHandle, readBuffer, compressedBlockSize))
				{
					u64 compressedSize;
					if (!uba::GetFileSizeEx(compressedSize, fileHandle))
						return m_logger.Error(TC("GetFileSize failed for %s (%s)"), fileName, LastErrorToText().data);
					if (bytesRead + compressedBlockSize > compressedSize)
						return m_logger.Error(TC("File %s corrupt. Compressed block size (%u) is larger than what is left of file (%llu)"), fileName, compressedBlockSize, compressedSize - bytesRead);
					return false;
				}
				bytesRead += compressedBlockSize;

				TimerScope ts(stats.decompressToMem);
				OO_SINTa decompLen = OodleLZ_Decompress(readBuffer, (OO_SINTa)compressedBlockSize, writePos, (OO_SINTa)decompressedBlockSize, OodleLZ_FuzzSafe_Yes, OodleLZ_CheckCRC_No, OodleLZ_Verbosity_None, NULL, 0, NULL, NULL, decoderMem, decoderMemSize);
				if (decompLen != decompressedBlockSize)
					return m_logger.Error(TC("Failed to decompress data from file %s at pos %llu"), fileName, decompressedSize - left);
				writePos += decompressedBlockSize;
				left -= decompressedBlockSize;
			}
		}
		return true;
	}

	bool StorageImpl::CreateDirectory(const tchar* dir)
	{
		return m_dirCache.CreateDirectory(m_logger, dir);
	}

	bool StorageImpl::DeleteCasForFile(const tchar* file)
	{
		StringBuffer<> forKey;
		FixPath(file, nullptr, 0, forKey);
		if (CaseInsensitiveFs)
			forKey.MakeLower();
		StringKey fileNameKey = ToStringKey(forKey);

		SCOPED_READ_LOCK(m_fileTableLookupLock, lookupLock);
		auto findIt = m_fileTableLookup.find(fileNameKey);
		if (findIt == m_fileTableLookup.end())
			return false;
		FileEntry& fileEntry = findIt->second;
		lookupLock.Leave();

		SCOPED_FUTEX(fileEntry.lock, entryLock);
		fileEntry.verified = false;

		return DropCasFile(fileEntry.casKey, true, file);
	}

}
