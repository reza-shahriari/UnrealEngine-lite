// Copyright Epic Games, Inc. All Rights Reserved.

struct FILE_NETWORK_OPEN_INFORMATION
{
	LARGE_INTEGER CreationTime;
	LARGE_INTEGER LastAccessTime;
	LARGE_INTEGER LastWriteTime;
	LARGE_INTEGER ChangeTime;
	LARGE_INTEGER AllocationSize;
	LARGE_INTEGER EndOfFile;
	ULONG FileAttributes;
};

bool IsContentWrite(u32 desiredAccess, u32 createDisposition)
{
	if (desiredAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE))
		return true;
	if (createDisposition == FILE_CREATE || createDisposition == FILE_OVERWRITE || createDisposition == FILE_OVERWRITE_IF)
		return true;
	return false;
}

bool IsContentRead(u32 desiredAccess, u32 createDisposition)
{
	return (desiredAccess & (GENERIC_READ | FILE_READ_DATA)) != 0;
}

bool IsContentUse(u32 desiredAccess, u32 createDisposition)
{
	return IsContentRead(desiredAccess, createDisposition) || IsContentWrite(desiredAccess, createDisposition);
}

bool IsWrite(u32 desiredAccess, u32 createDisposition)
{
	return IsContentWrite(desiredAccess, createDisposition) || (desiredAccess & (FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA)) != 0;
}

u8 GetFileAccessFlags(DWORD desiredAccess, u32 createDisposition)
{
	u8 access = 0;
	if (IsContentRead(desiredAccess, createDisposition))
		access |= AccessFlag_Read;
	if (IsWrite(desiredAccess, createDisposition))
		access |= AccessFlag_Write;
	return access;
}

#if UBA_DEBUG_LOG_ENABLED
StringBuffer<32> ToString(NTSTATUS s)
{
	StringBuffer<32> res;
	if (NT_SUCCESS(s))
		return res.Append(L"Success");
	if (s == STATUS_OBJECT_NAME_NOT_FOUND)
		return res.Append(L"STATUS_OBJECT_NAME_NOT_FOUND");
	if (s == STATUS_OBJECT_PATH_NOT_FOUND)
		return res.Append(L"STATUS_OBJECT_PATH_NOT_FOUND");
	if (s == STATUS_INVALID_HANDLE)
		return res.Append(L"STATUS_INVALID_HANDLE");
	if (s == STATUS_SHARING_VIOLATION)
		return res.Append(L"STATUS_SHARING_VIOLATION");
	if (s == STATUS_ACCESS_DENIED)
		return res.Append(L"STATUS_ACCESS_DENIED");
	return res.Appendf(L"Error (0x%x)", u32(s));
}
#endif

struct FILE_FS_DEVICE_INFORMATION
{
	DEVICE_TYPE DeviceType;
	ULONG Characteristics;
};

struct FILE_FS_ATTRIBUTE_INFORMATION
{
	ULONG FileSystemAttributes;
	LONG  MaximumComponentNameLength;
	ULONG FileSystemNameLength;
	WCHAR FileSystemName[1];
};

NTSTATUS Detoured_NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FsInformation, ULONG Length, FS_INFORMATION_CLASS FsInformationClass)
{
	DETOURED_CALL(NtQueryVolumeInformationFile);
	HANDLE TrueHandle = FileHandle;
	if (isDetouredHandle(FileHandle))
	{
		auto& dh = asDetouredHandle(FileHandle);
		if (dh.fileObject->fileInfo->memoryFile)
		{
			if (FsInformationClass == 4) // FileFsDeviceInformation
			{
				auto& info = *(FILE_FS_DEVICE_INFORMATION*)FsInformation;
				info.DeviceType = FILE_DEVICE_FILE_SYSTEM;
				info.Characteristics = 0;
				return STATUS_SUCCESS;
			}
		}
		TrueHandle = dh.trueHandle;
		if (TrueHandle == INVALID_HANDLE_VALUE)
		{
			if (FsInformationClass == 1) // FileFsVolumeInformation
			{
				// TODO This code path is here to handle nodejs queries..
				
				auto& info = *(FILE_FS_VOLUME_INFORMATION*)FsInformation;
				UBA_ASSERT(dh.dirTableOffset != ~0u);
				DirectoryTable::EntryInformation entryInfo;
				g_directoryTable.GetEntryInformation(entryInfo, dh.dirTableOffset);
				UBA_ASSERT(entryInfo.attributes != 0);
				info.VolumeCreationTime.QuadPart = 0;
				info.VolumeSerialNumber = entryInfo.volumeSerial;
				info.VolumeLabelLength = 0;
				info.SupportsObjects = false;
				info.VolumeLabel[0] = 0;
				return STATUS_SUCCESS;
			}
			UBA_ASSERTF(false, L"NtQueryVolumeInformationFile using class %u not handled %ls (%ls)", FsInformationClass, dh.fileObject->fileInfo->name, dh.fileObject->fileInfo->originalName);
		}
	}
	else if (isListDirectoryHandle(FileHandle))
	{
		if (FsInformationClass == 4) // FileFsDeviceInformation
		{
			auto& info = *(FILE_FS_DEVICE_INFORMATION*)FsInformation;
			info.DeviceType = FILE_DEVICE_FILE_SYSTEM;
			info.Characteristics = 0;
			return STATUS_SUCCESS;
		}
		UBA_ASSERTF(false, L"NtQueryVolumeInformationFile called in ListDirectoryHandle using class %u which is not implemented (%ls)", FsInformationClass, HandleToName(FileHandle));
	}
	auto res = True_NtQueryVolumeInformationFile(TrueHandle, IoStatusBlock, FsInformation, Length, FsInformationClass);
	DEBUG_LOG_TRUE(L"NtQueryVolumeInformationFile", L"%llu (%ls) -> %ls", uintptr_t(FileHandle), HandleToName(FileHandle), ToString(res).data);
	return res;
}

NTSTATUS Detoured_NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
	DETOURED_CALL(NtQueryInformationFile);
	if (isListDirectoryHandle(FileHandle))
	{
		auto& listHandle = asListDirectoryHandle(FileHandle);
		if (FileInformationClass == 51) // FileIsRemoteDeviceInformation
		{
			auto& info = *(FILE_IS_REMOTE_DEVICE_INFORMATION*)FileInformation;
			info.IsRemote = FALSE;
			DEBUG_LOG_DETOURED(L"NtQueryInformationFile", L"(FileIsRemoteDeviceInformation) %llu (%ls) -> Success", uintptr_t(FileHandle), HandleToName(FileHandle));
			return STATUS_SUCCESS;
		}
		else if (FileInformationClass == 59) // FileIdInformation
		{
			auto& info = *(FILE_ID_INFORMATION*)FileInformation;
			if (listHandle.dir.tableOffset != InvalidTableOffset)
			{
				u32 entryOffset = listHandle.dir.tableOffset | 0x80000000;
				DirectoryTable::EntryInformation entryInfo;
				g_directoryTable.GetEntryInformation(entryInfo, entryOffset);
				info.VolumeSerialNumber = entryInfo.volumeSerial;
				u64* id = (u64*)&info.FileId;
				id[0] = 0;
				id[1] = entryInfo.fileIndex;
			}
			else
			{
				UBA_ASSERT(false);
				info.VolumeSerialNumber = 0;//attr.volumeSerial;
				memcpy(info.FileId.Identifier, &listHandle.dirNameKey, 16);
			}
			DEBUG_LOG_DETOURED(L"NtQueryInformationFile", L"(FileIdInformation) %llu (%ls) -> Success", uintptr_t(FileHandle), HandleToName(FileHandle));
			return STATUS_SUCCESS;
		}
		/*
		else if (FileInformationClass == 9) // FileNameInformation
		{
			auto& info = *(FILE_NAME_INFORMATION*)FileInformation;
			u32 nameLen = u32(wcslen(listHandle.name));
			//UBA_ASSERT(info.FileNameLength/2 > nameLen);
			memcpy(info.FileName, listHandle.name, nameLen*2+2);
			info.FileNameLength = nameLen*2;
			UBA_ASSERT(false);
			return STATUS_SUCCESS;
		}
		else if (FileInformationClass == 55) // Undefined, some old compilers using this it seems
		{
			DEBUG_LOG_DETOURED(L"NtQueryInformationFile", L"TODO_THIS (55) %llu (%ls) -> Error", uintptr_t(FileHandle), HandleToName(FileHandle));
			return STATUS_NOT_SUPPORTED;
		}
		*/
		else
		{
			FatalError(1348, L"NtQueryInformationFile with class %u not implemented", FileInformationClass);
		}
	}

	HANDLE TrueHandle = FileHandle;
	if (isDetouredHandle(FileHandle))
	{
		auto& dh = asDetouredHandle(FileHandle);
		TrueHandle = dh.trueHandle;

		if (TrueHandle == INVALID_HANDLE_VALUE)
		{
			if (FileInformationClass == 18) // FileAllInformation 
			{
				UBA_ASSERT(dh.dirTableOffset != ~0u);
				DirectoryTable::EntryInformation entryInfo;
				g_directoryTable.GetEntryInformation(entryInfo, dh.dirTableOffset);
				UBA_ASSERT(entryInfo.attributes != 0);

				// TODO This code path is here to handle nodejs queries.. Is not properly implemented and miss things
				auto& info = *(FILE_ALL_INFORMATION*)FileInformation;
				info.BasicInformation.CreationTime.QuadPart = entryInfo.lastWrite;
				info.BasicInformation.LastAccessTime.QuadPart = entryInfo.lastWrite;
				info.BasicInformation.LastWriteTime.QuadPart = entryInfo.lastWrite;
				info.BasicInformation.ChangeTime.QuadPart = entryInfo.lastWrite;
				info.BasicInformation.FileAttributes = entryInfo.attributes;
				info.StandardInformation.AllocationSize.QuadPart = entryInfo.size;
				info.StandardInformation.EndOfFile.QuadPart = entryInfo.size;
				info.StandardInformation.NumberOfLinks = 0;
				info.StandardInformation.DeletePending = false;
				info.StandardInformation.Directory = false;
				info.InternalInformation.IndexNumber.QuadPart = entryInfo.fileIndex;
				return STATUS_SUCCESS;
			}
			if (FileInformationClass == 34) // FileNetworkOpenInformation
			{
				UBA_ASSERT(dh.dirTableOffset != ~0u);
				DirectoryTable::EntryInformation entryInfo;
				g_directoryTable.GetEntryInformation(entryInfo, dh.dirTableOffset);
				UBA_ASSERT(entryInfo.attributes != 0);

				// TODO This code path is here to handle nodejs queries.. Is not properly implemented and miss things
				auto& info = *(FILE_NETWORK_OPEN_INFORMATION*)FileInformation;
				info.CreationTime.QuadPart = entryInfo.lastWrite;
				info.LastAccessTime.QuadPart = entryInfo.lastWrite;
				info.LastWriteTime.QuadPart = entryInfo.lastWrite;
				info.ChangeTime.QuadPart = entryInfo.lastWrite;
				info.AllocationSize.QuadPart = entryInfo.size;
				u64 fileSize = dh.fileObject->fileInfo->size;
				if (fileSize == InvalidValue)
					fileSize = entryInfo.size;
				info.EndOfFile.QuadPart = fileSize;
				info.FileAttributes = entryInfo.attributes;
				return STATUS_SUCCESS;
			}

			//if (FileInformationClass == 9) // FileNameInformation
			//{
			//	const wchar_t* name = dh.fileObject->fileInfo->originalName;
			//	auto& info = *(FILE_NAME_INFORMATION*)FileInformation;
			//	info.FileName[0] = '\\';
			//	info.FileNameLength = 2;
			//	//return STATUS_SUCCESS;
			//	u32 nameLen = u32(wcslen(name));
			//	//UBA_ASSERT(info.FileNameLength/2 > nameLen);
			//	//memcpy(info.FileName, name, nameLen*2+2);
			//	//info.FileNameLength = nameLen;
			//	memcpy(info.FileName, L"\\\\", 4);
			//	info.FileNameLength = 2;
			//	return STATUS_SUCCESS;
			//}
			UBA_ASSERTF(false, L"NtQueryInformationFile (%u) failed using detoured handle %ls (%ls)", FileInformationClass, dh.fileObject->fileInfo->name, dh.fileObject->fileInfo->originalName);
		}
	}

	TimerScope ts(g_kernelStats.getFileInfo);
	auto res = True_NtQueryInformationFile(TrueHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
	DEBUG_LOG_TRUE(L"NtQueryInformationFile", L"(%u) %llu (%ls) -> %ls", FileInformationClass, uintptr_t(FileHandle), HandleToName(FileHandle), ToString(res).data);
	return res;
}

NTSTATUS NTAPI Detoured_NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry, PUNICODE_STRING FileName, BOOLEAN RestartScan)
{
	DETOURED_CALL(NtQueryDirectoryFile);

	if (isListDirectoryHandle(FileHandle))
	{
		IoStatusBlock->Information = 0;

		auto& listHandle = asListDirectoryHandle(FileHandle);
		NTSTATUS res = STATUS_NO_MORE_FILES;

		UBA_ASSERT(Event == 0 && ApcRoutine == nullptr && ApcContext == nullptr);

		if (RestartScan)
			listHandle.it = 0;

		u8* prevInformation = nullptr;
		u8* it = (u8*)FileInformation;
		u8* bufferEnd = it + Length;

		while (true)
		{
			if (listHandle.it == listHandle.fileTableOffsets.size())
				break;

			u32 fileOffset = listHandle.fileTableOffsets[listHandle.it++];

			DirectoryTable::EntryInformation entryInfo;
			wchar_t fileName[512];
			g_directoryTable.GetEntryInformation(entryInfo, fileOffset, fileName, sizeof_array(fileName));
			if (entryInfo.attributes == 0) // File was deleted
				continue;

			if (FileName && wcsncmp(FileName->Buffer, fileName, FileName->Length / 2) != 0)
				continue;

			u32 fileNameBytes = u32(wcslen(fileName) * 2);

			wchar_t* fileNamePos = nullptr;
			u32 structSize = 0;
			if (FileInformationClass == FileDirectoryInformation)
			{
				structSize = sizeof(FILE_DIRECTORY_INFORMATION);
				fileNamePos = ((FILE_DIRECTORY_INFORMATION*)it)->FileName;
			}
			else if (FileInformationClass == 2)//FileFullDirectoryInformation)
			{
				structSize = sizeof(FILE_FULL_DIR_INFORMATION);
				fileNamePos = ((FILE_FULL_DIR_INFORMATION*)it)->FileName;
			}
			else
			{
				UBA_ASSERT(false);
				IoStatusBlock->Status = STATUS_OBJECT_NAME_NOT_FOUND;
				return STATUS_OBJECT_NAME_NOT_FOUND;
			}

			u8* writeEnd = (u8*)fileNamePos + fileNameBytes;
			if (writeEnd > bufferEnd)
			{
				--listHandle.it;
				if (!prevInformation)
					res = STATUS_BUFFER_OVERFLOW;
				break;
			}

			memset(it, 0, structSize);
			auto& info = *(FILE_DIRECTORY_INFORMATION*)it;

			memcpy(fileNamePos, fileName, fileNameBytes);

			info.FileNameLength = fileNameBytes;
			info.FileAttributes = entryInfo.attributes;
			info.LastWriteTime.QuadPart = entryInfo.lastWrite;
			info.EndOfFile.QuadPart = entryInfo.size;
			//info.FileIndex = entryInfo.fileIndex; // This needs serialno too?
			info.AllocationSize.QuadPart = entryInfo.size;
			info.CreationTime.QuadPart = entryInfo.lastWrite;

			if (prevInformation)
			{
				((FILE_DIRECTORY_INFORMATION*)prevInformation)->NextEntryOffset = u32(it - prevInformation);
			}

			prevInformation = it;
			it = (u8*)fileNamePos + info.FileNameLength + 2;

			DEBUG_LOG_DETOURED(L"NtQueryDirectoryFile", L"%llu %ls", u64(FileHandle), fileNamePos);

			res = STATUS_SUCCESS;

			if (ReturnSingleEntry)
				break;
		}

		IoStatusBlock->Status = res;
		IoStatusBlock->Information = it - (u8*)FileInformation;

#if 0//UBA_DEBUG_VALIDATE
		if (false) // Sorting can mismatch
		{
			u8 info2Mem[1024];
			UBA_ASSERT(Length <= sizeof(info2Mem));
			auto& info2 = *(FILE_DIRECTORY_INFORMATION*)info2Mem;
			NTSTATUS res2;
			do
			{
				res2 = True_NtQueryDirectoryFile(listHandle.validateHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, &info2, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
				if (res2 >= 0)
				{
					info2.FileName[info2.FileNameLength / 2] = 0;
					ToLower(info2.FileName);
				}
			} while (wcscmp(info2.FileName, L".") == 0 || wcscmp(info2.FileName, L"..") == 0);
			UBA_ASSERT(res < 0 && res2 < 0 || res >= 0 && res2 >= 0);
			UBA_ASSERT(res < 0 || wcscmp(info.FileName, info2.FileName) == 0);
		}
#endif
		return res;
	}

	HANDLE trueHandle = FileHandle;
	if (isDetouredHandle(FileHandle))
	{
		DetouredHandle& h = asDetouredHandle(FileHandle);
		trueHandle = h.trueHandle;
		UBA_ASSERTF(trueHandle != INVALID_HANDLE_VALUE, L"NtQueryDirectoryFile for using class %u not implemented for detoured handles (%ls)", FileInformationClass, HandleToName(FileHandle));
	}

	NTSTATUS res = True_NtQueryDirectoryFile(trueHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);

#if UBA_DEBUG_LOG_ENABLED
	if (res == STATUS_SUCCESS)
	{
		u8* it = (u8*)FileInformation;
		while (true)
		{
			const wchar_t* fileNamePos;
			if (FileInformationClass == FileDirectoryInformation)
				fileNamePos = ((FILE_DIRECTORY_INFORMATION*)it)->FileName;
			else if (FileInformationClass == 2)//FileFullDirectoryInformation)
				fileNamePos = ((FILE_FULL_DIR_INFORMATION*)it)->FileName;
			else
				break;
			StringBuffer<> b;
			b.Append(fileNamePos, ((FILE_DIRECTORY_INFORMATION*)it)->FileNameLength / 2);
			DEBUG_LOG_TRUE(L"NtQueryDirectoryFile", L"%llu %ls", u64(FileHandle), b.data);

			u32 nextOffset = ((FILE_DIRECTORY_INFORMATION*)it)->NextEntryOffset;
			if (!nextOffset)
				break;
			it += nextOffset;
			//DEBUG_LOG_TRUE(L"NtQueryDirectoryFile", L"(%u) %llu (%ls) -> %ls", FileInformationClass, uintptr_t(FileHandle), HandleToName(FileHandle), ToString(res));
		}
		//DEBUG_LOG_TRUE(L"NtQueryDirectoryFile", L"(%u) %llu (%ls) -> %ls", FileInformationClass, uintptr_t(FileHandle), HandleToName(FileHandle), ToString(res));
	}
#endif
	return res;
}

NTSTATUS Detoured_NtQueryFullAttributesFile(POBJECT_ATTRIBUTES ObjectAttributes, PVOID Attributes)
{
	auto fileName = (const wchar_t*)ObjectAttributes->ObjectName->Buffer;
	u32 fileNameLen = ObjectAttributes->ObjectName->Length/sizeof(tchar);(void)fileNameLen;
	UBA_ASSERT(fileName[fileNameLen] == 0);

	if (!CanDetour(fileName) || Contains(fileName, L"::")) // Some weird .net path used by dotnet.exe ... ignore for now!
	{
		TimerScope ts(g_kernelStats.getFileInfo);
		auto res = True_NtQueryFullAttributesFile(ObjectAttributes, Attributes);
		DEBUG_LOG_TRUE(L"NtQueryFullAttributesFile", L"(%.*s) -> %s", fileNameLen, fileName, ToString(res).data);
		return res;
	}


	StringBuffer<MaxPath> fixedName;
	FixPath(fixedName, fileName);
	/*
	if (fixedName.StartsWith(g_systemTemp.data))
	{
		auto res = True_NtQueryFullAttributesFile(ObjectAttributes, Attributes);
		DEBUG_LOG_TRUE(L"NtQueryFullAttributesFile", L"(%.*s) -> %s", fileNameLen, fileName, ToString(res).data);
		return res;
	}
	*/

	DevirtualizePath(fixedName);

	FileAttributes attr;
	Shared_GetFileAttributes(attr, fixedName.data);

	if (!attr.useCache)
	{
		auto res = True_NtQueryFullAttributesFile(ObjectAttributes, Attributes);
		DEBUG_LOG_TRUE(L"NtQueryFullAttributesFile", L"(%.*s) -> %s", fileNameLen, fileName, ToString(res).data);
		return res;
	}

	UBA_ASSERT(!ObjectAttributes->RootDirectory);

	NTSTATUS res = STATUS_OBJECT_NAME_NOT_FOUND;
	if (attr.exists && attr.lastError == ErrorSuccess)
	{
		WIN32_FILE_ATTRIBUTE_DATA& data = attr.data;
		res = STATUS_SUCCESS;
		auto& info = *(FILE_NETWORK_OPEN_INFORMATION*)Attributes;;
		info.CreationTime = ToLargeInteger(data.ftCreationTime.dwHighDateTime, data.ftCreationTime.dwLowDateTime);
		info.LastAccessTime = ToLargeInteger(data.ftLastAccessTime.dwHighDateTime, data.ftLastAccessTime.dwLowDateTime);
		info.LastWriteTime = ToLargeInteger(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
		info.ChangeTime = ToLargeInteger(data.ftLastWriteTime.dwHighDateTime, data.ftLastWriteTime.dwLowDateTime);
		info.AllocationSize = ToLargeInteger(data.nFileSizeHigh, data.nFileSizeLow);
		info.EndOfFile = info.AllocationSize;
		info.FileAttributes = data.dwFileAttributes;
	}

	DEBUG_LOG_DETOURED(L"NtQueryFullAttributesFile", L"(%.*s) -> %s (Size: %llu)", fileNameLen, fileName, ToString(res).data, ToLargeInteger(attr.data.nFileSizeHigh, attr.data.nFileSizeLow).QuadPart);
	return res;
}

NTSTATUS Detoured_NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
	DETOURED_CALL(NtSetInformationFile);


	u8 tempBuffer[sizeof(FILE_RENAME_INFORMATION) + 512];

	HANDLE trueHandle = FileHandle;
	if (isDetouredHandle(FileHandle))
	{
		DetouredHandle& h = asDetouredHandle(FileHandle);

		if (FileInformationClass == 10) // FileRenameInformation 
		{
			// We can end up in here through MoveFileEx
			auto& info = *(FILE_RENAME_INFORMATION*)FileInformation;
			const wchar_t* newNamePtr = t_renameFileNewName;
			StringBuffer<> newNameTemp;
			if (!newNamePtr)
			{
				newNameTemp.Append(info.FileName, info.FileNameLength / 2);
				newNamePtr = newNameTemp.data;
			}
			if (StartsWith(newNamePtr, L"\\??\\"))
				newNamePtr += 4;
			StringBuffer<> newName;
			FixPath(newName, newNamePtr);
			DevirtualizePath(newName);
			FileObject& fo = *h.fileObject;
			fo.newName = newName.data;

			StringKey newFileNameKey = ToStringKeyLower(newName);

			// TODO: Revisit this.. don't know what could go wrong with this
#if 0
			FileInfo* newFileInfo = nullptr;
			{
				SCOPED_READ_LOCK(g_mappedFileTable.m_lookupLock, _);
				auto findIt = g_mappedFileTable.m_lookup.find(newFileNameKey);
				if (findIt != g_mappedFileTable.m_lookup.end())
					newFileInfo = &findIt->second;
			}

			UBA_ASSERTF(newFileInfo, L"File info already exists for the rename we are doing from %ls to %ls", fo.fileInfo->originalName, newName.data); // TODO: Implement when we find a test case
#endif

			if (!fo.closeId)
			{
				wchar_t temp[1024];
				u64 size;
				StringBuffer<> fixedPath;
				FixPath(fixedPath, newName.data);
				Rpc_CreateFileW(fixedPath, newFileNameKey, AccessFlag_Write, temp, sizeof_array(temp), size, fo.closeId, true);
			}
			DEBUG_LOG_DETOURED(L"NtSetInformationFile", L"File is set to be renamed on close (from %ls to %ls)", HandleToName(FileHandle), fo.newName.c_str());

			if (auto memoryFile = fo.fileInfo->memoryFile)
			{
				memoryFile->isReported = false;
				return STATUS_SUCCESS;
			}

			UBA_ASSERT(!fo.fileInfo->isFileMap);

#if 0
			// This is for the odd rename logic in clang.. foo.so -> foo.so123123.tmp
			// Clang first create a tmp file, to see if it exists..
			// It then renames the old file to that file..
			// ..and finally opens it again with attributes only and deleteonclose flag.. and closes it.
			// TODO: This is wrong.. we just delete the file.. but what we should do is copy it over in memory to the memory file before setting it to delete
			SCOPED_READ_LOCK(g_mappedFileTable.m_lookupLock, _);
			auto findIt = g_mappedFileTable.m_lookup.find(newFileNameKey);
			if (findIt != g_mappedFileTable.m_lookup.end())
			{
				FileInfo& info = findIt->second;
				if (info.memoryFile)
				{
					Rpc_WriteLogf(L"HNNMMMM");
					FILE_DISPOSITION_INFO info;
					info.DeleteFileW = true;
					True_SetFileInformationByHandle(FileHandle, FileDispositionInfo, &info, sizeof(info));
					return STATUS_SUCCESS;
				}
			}
#endif

			if (g_runningRemote) // This needs a proper solution as the comments above.
				return STATUS_SUCCESS;

			// In case we are using vfs we need to replace the information before calling the true NtSetInformationFile
			auto& info2 = *(FILE_RENAME_INFORMATION*)tempBuffer;
			memcpy(&info2, &info, sizeof(FILE_RENAME_INFORMATION));
			memcpy(info2.FileName, TC("\\??\\"), 8);
			memcpy(info2.FileName + 4, newName.data, newName.count * 2);
			info2.FileNameLength = (newName.count+4) * 2;
			
			FileInformation = &info2;
			Length = sizeof(FILE_RENAME_INFORMATION) + info2.FileNameLength + 2;
		}

		trueHandle = h.trueHandle;
		if (trueHandle == INVALID_HANDLE_VALUE)
		{
			//UBA_ASSERT(!g_runningRemote);
			// TODO: This needs to be sent back to Session.. so session can set whatever needs to be set.
			DEBUG_LOG_DETOURED(L"NtSetInformationFile", L"(%u) SKIPPED!!!!!!!!! %llu (%ls) -> Skipped", FileInformationClass, uintptr_t(FileHandle), HandleToName(FileHandle));
			return STATUS_SUCCESS;
		}
	}

	TimerScope ts(g_kernelStats.setFileInfo);
	auto res = True_NtSetInformationFile(trueHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
	DEBUG_LOG_TRUE(L"NtSetInformationFile", L"(%u) %llu (%ls) -> %ls", FileInformationClass, uintptr_t(FileHandle), HandleToName(FileHandle), ToString(res).data);
	return res;
}

NTSTATUS NTAPI Detoured_NtSetInformationObject(HANDLE ObjectHandle, OBJECT_INFORMATION_CLASS ObjectInformationClass, PVOID ObjectInformation, ULONG Length)
{
	if (isDetouredHandle(ObjectHandle))
	{
		DetouredHandle& h = asDetouredHandle(ObjectHandle);
		ObjectHandle = h.trueHandle;
		UBA_ASSERT(ObjectHandle != INVALID_HANDLE_VALUE);
	}
	auto res = True_NtSetInformationObject(ObjectHandle, ObjectInformationClass, ObjectInformation, Length);
	DEBUG_LOG_TRUE(L"NtSetInformationObject", L"(%u) %llu (%ls) -> %ls", ObjectInformationClass, uintptr_t(ObjectHandle), HandleToName(ObjectHandle), ToString(res).data);
	return res;
}

NTSTATUS NTAPI Detoured_NtCreateSection(PHANDLE SectionHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PLARGE_INTEGER MaximumSize, ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle)
{
	DETOURED_CALL(NtCreateSection);
	if (isDetouredHandle(FileHandle))
	{
		DetouredHandle& h = asDetouredHandle(FileHandle);
		FileHandle = h.trueHandle;
		UBA_ASSERT(FileHandle != INVALID_HANDLE_VALUE);
	}
	return True_NtCreateSection(SectionHandle, DesiredAccess, ObjectAttributes, MaximumSize, SectionPageProtection, AllocationAttributes, FileHandle);
}

bool g_checkRtlHeap = true;

SIZE_T Detoured_RtlSizeHeap(HANDLE HeapPtr, ULONG Flags, PVOID Ptr)
{
#if UBA_USE_MIMALLOC
	if (g_checkRtlHeap && IsInMiMalloc(Ptr))
		return mi_usable_size(Ptr);
#endif
	return True_RtlSizeHeap(HeapPtr, Flags, Ptr);
}

BOOLEAN Detoured_RtlFreeHeap(PVOID HeapHandle, ULONG Flags, PVOID BaseAddress)
{
#if UBA_USE_MIMALLOC
	if (g_checkRtlHeap && IsInMiMalloc(BaseAddress))
	{
		mi_free(BaseAddress);
		return true;
	}
#endif
	return True_RtlFreeHeap(HeapHandle, Flags, BaseAddress);
}

NTSTATUS Detoured_RtlAnsiStringToUnicodeString(PUNICODE_STRING DestinationString, PCANSI_STRING SourceString, BOOLEAN AllocateDestinationString)
{
#if UBA_USE_MIMALLOC
	if (AllocateDestinationString && g_useMiMalloc)
	{
		DestinationString->MaximumLength = SourceString->MaximumLength * 2;
		DestinationString->Buffer = (wchar_t*)mi_malloc(DestinationString->MaximumLength);
		AllocateDestinationString = false;
	}
#endif
	auto res = True_RtlAnsiStringToUnicodeString(DestinationString, SourceString, AllocateDestinationString);
	return res;
}

NTSTATUS Detoured_RtlUnicodeStringToAnsiString(PANSI_STRING DestinationString, PCUNICODE_STRING SourceString, BOOLEAN AllocateDestinationString)
{
#if UBA_USE_MIMALLOC
	if (AllocateDestinationString && g_useMiMalloc)
	{
		DestinationString->MaximumLength = SourceString->MaximumLength / 2;
		DestinationString->Buffer = (char*)mi_malloc(DestinationString->MaximumLength);
		AllocateDestinationString = false;
	}
#endif
	return True_RtlUnicodeStringToAnsiString(DestinationString, SourceString, AllocateDestinationString);
}

NTSTATUS NTAPI Local_NtCreateFile(bool IsCreateFunc, PHANDLE hFileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
	ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
#if 0
	if (IsContentWrite(DesiredAccess, CreateDisposition))
	{
		StringBuffer<> b;
		b.Append(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / 2);
		if (!b.Contains(L"\\Device\\") && !b.EndsWith(L"\\nul"))
			Rpc_WriteLogf(L"[%ls] WRITTEN: %ls", g_rulesIndex ? GetApplicationRules()[g_rulesIndex].app : wcsrchr(g_virtualApplication.data, '\\') + 1, ObjectAttributes->ObjectName->Buffer);
	}
#endif

	TimerScope ts(g_kernelStats.createFile);

	constexpr u32 retryCount = 15;
	u32 retriesLeft = retryCount;
	while (true)
	{
		//if (!Contains(ObjectAttributes->ObjectName->Buffer, L".dll") && !Contains(ObjectAttributes->ObjectName->Buffer, L".mui"))
		//Rpc_WriteLogf(L"NtCreateFile: %ls", ObjectAttributes->ObjectName->Buffer);
		auto res = True_NtCreateFile(hFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);

		UBA_ASSERTF(res != STATUS_SUCCESS || u64(*hFileHandle) < DetouredHandleStart - 10000, L"Normal handle range is closing in on detoured. Bump detour range (normal: %llu, detour start: %llu)", u64(*hFileHandle), DetouredHandleStart);

		// I have no idea why we get this sometimes when trying to open pch for read after recently being written..
		// All scenarios I've seen this succeeds after 1 second.
		// Only theory I have is some antivirus or something. 
		if (res == STATUS_SHARING_VIOLATION)
		{
			if (!--retriesLeft)
				return res;

			StringView fileName(ObjectAttributes->ObjectName->Buffer, ObjectAttributes->ObjectName->Length / 2);
			if (!fileName.EndsWith(TCV(".pch")))
				return res;

			#if UBA_DEBUG
			StringBuffer<> b;
			b.Appendf(L"Got access denied trying to open %.*s. Retrying in one second", ObjectAttributes->ObjectName->Length / 2, ObjectAttributes->ObjectName->Buffer);
			Rpc_WriteLog(b.data, b.count, true, false);
			#endif
			Sleep(1000);
			continue;
		}

		#if UBA_DEBUG
		if (retriesLeft != retryCount)
		{
			StringBuffer<> b;
			b.Appendf(L"SUCCEEDED to open %.*s after %u retries.", ObjectAttributes->ObjectName->Length / 2, ObjectAttributes->ObjectName->Buffer, retryCount - retriesLeft);
			Rpc_WriteLog(b.data, b.count, true, true);
		}
		#endif

		return res;
	}
}

NTSTATUS NTAPI Shared_NtCreateFile(bool IsCreateFunc, PHANDLE hFileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
	ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
	*hFileHandle = INVALID_HANDLE_VALUE;

#if UBA_DEBUG_LOG_ENABLED
	const wchar_t* funcName = IsCreateFunc ? L"NtCreateFile" : L"NtOpenFile"; (void)funcName;
#endif

	const wchar_t* createFileName = t_createFileFileName;

	// NOTE - ObjectAttributes->ObjectName->Buffer might not be null terminated, so we need to copy over to another buffer
	StringBuffer<> fileName;
	bool suppressCreateFileDetour = t_disallowCreateFileDetour || t_disallowDetour;
	HANDLE rootDir = ObjectAttributes->RootDirectory;
	{
		const wchar_t* buf = ObjectAttributes->ObjectName->Buffer;
		u32 bufChars = ObjectAttributes->ObjectName->Length / 2;

		if (suppressCreateFileDetour)
		{
		}
		else if (!buf)
		{
			suppressCreateFileDetour = true;
		}
		else if ((bufChars >= 7u && wcsncmp(buf, L"\\Device", 7u) == 0) || (bufChars >= 10u && wcsncmp(buf, L"\\Global??\\", 10u) == 0)) // \Global is for FilterConnectCommunicationPort and friends
		{
			suppressCreateFileDetour = true;
		}
		else if (createFileName)
		{
			if (!CanDetour(createFileName))
				suppressCreateFileDetour = true;
			else if (!FixPath(fileName, createFileName))
				UBA_ASSERTF(false, L"FixPath failed for string '%ls'", createFileName);

			// TODO: Instead of using t_createFileFileName.. should we just resolve the path from ObjectAttributes->RootDirectory?
			ObjectAttributes->RootDirectory = nullptr;
		}
		else if (bufChars >= 8 && memcmp(buf, L"\\??\\", 8) == 0)
		{
			if (!CanDetour(buf))
				suppressCreateFileDetour = true;
			else if (!FixPath(fileName, buf))
				UBA_ASSERTF(false, L"FixPath failed for string '%ls'", buf + 4);
		}
		else
		{
			if (ObjectAttributes->RootDirectory)
			{
				if (isDetouredHandle(ObjectAttributes->RootDirectory))
				{
					auto& dh = asDetouredHandle(ObjectAttributes->RootDirectory);
					fileName.Append(dh.fileObject->fileInfo->originalName).EnsureEndsWithSlash().Append(buf, bufChars);
					rootDir = dh.trueHandle;
					ObjectAttributes->RootDirectory = nullptr;
				}
				else if (isListDirectoryHandle(ObjectAttributes->RootDirectory))
				{
					auto& lh = asListDirectoryHandle(ObjectAttributes->RootDirectory);
					fileName.Append(lh.originalName).EnsureEndsWithSlash().Append(buf, bufChars);
					ObjectAttributes->RootDirectory = nullptr;
				}
				else
				{
					// TODO: Revisit!
					//UBA_ASSERT(false);
					suppressCreateFileDetour = true;
				}
			}
			else
			{
				fileName.Append(buf, bufChars);
				if (fileName.StartsWith(L"\\DosDevices")) // Something used in msbuild.. 
					suppressCreateFileDetour = true;
			}
		}
	}

	if (!suppressCreateFileDetour && fileName[fileName.count - 1] == '$')
	{
		constexpr const wchar_t* stdStr[] = { L"conerr$", L"conout$", L"conin$" };
		for (u32 i = 0; i != 3; ++i)
		{
			if (!fileName.EndsWith(stdStr[i]))
				continue;
			if (g_isDetachedProcess)
			{
				*hFileHandle = g_stdHandle[i];
				return STATUS_SUCCESS;
			}

			suppressCreateFileDetour = true;
			break;
		}
	}

	if (suppressCreateFileDetour)
	{
		NTSTATUS res = Local_NtCreateFile(IsCreateFunc, hFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
		DEBUG_LOG_TRUE(funcName, L"(SUPPRESSDETOUR) %llu (%ls) -> %ls", uintptr_t(*hFileHandle), ObjectAttributes->ObjectName->Buffer, ToString(res).data);
		if (createFileName && Equals(createFileName, L"NUL"))
			g_nullFile = *hFileHandle;
		return res;
	}

	DevirtualizePath(fileName);

	u32 dirTableOffset = ~u32(0);

	//UBA_ASSERT(CreateDisposition != FILE_SUPERSEDE);
	bool isDeleteOnClose = (CreateOptions & FILE_DELETE_ON_CLOSE) != 0; // clang is using CreateFile with DeleteOnClose to delete files after build errors

	bool useContent = IsContentUse(DesiredAccess, CreateDisposition);
	bool isWrite = IsWrite(DesiredAccess, CreateDisposition);
	bool isThrowAway = g_rules->IsThrowAway(fileName, g_runningRemote);
	bool keepInMemory = KeepInMemory(fileName, isWrite) || ((isWrite || isDeleteOnClose) && g_rules->IsOutputFile(fileName)) || isThrowAway;

#if UBA_DEBUG_LOG_ENABLED
	const wchar_t* isWriteStr = isWrite ? L" WRITE" : L""; (void)isWriteStr;
#endif

	bool isSystemFile = fileName.StartsWith(g_systemRoot.data);
	bool checkIfDir = false;
	// This is here just to avoid getting a NtQueryVolumeInformationFile to get volume information 
	if (fileName[3] == 0 && fileName[1] == ':')
	{
		isSystemFile = ToLower(fileName[0]) == g_systemRoot[0];
		checkIfDir = true;
	}

	bool isSystemOrTempFile = isSystemFile || fileName.StartsWith(g_systemTemp.data);

	if (isSystemFile || (isSystemOrTempFile && !keepInMemory))
	{
		ObjectAttributes->RootDirectory = rootDir;
		NTSTATUS res = Local_NtCreateFile(IsCreateFunc, hFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
		if (NT_ERROR(res))
			*hFileHandle = INVALID_HANDLE_VALUE;
		DEBUG_LOG_TRUE(funcName, L"(NODETOUR)%ls %llu (%.*ls) -> %ls", isWriteStr, uintptr_t(*hFileHandle), ObjectAttributes->ObjectName->Length / 2, ObjectAttributes->ObjectName->Buffer, ToString(res).data);
		if (NT_ERROR(res))
			return res;

		if (!isSystemOrTempFile && !isWrite && !t_disallowDetour && fileName[fileName.count-1] != ':')
			TrackInput(fileName.data);
		else
			SkipTrackInput(fileName.data);
		return res;
	}

	StringBuffer<> fileNameLower(fileName);
	fileNameLower.MakeLower();
	StringKey fileNameKey = ToStringKey(fileNameLower);

	if (createFileName)// && (keepInMemory || canDetour))
	{
		if (g_allowDirectoryCache)
		{
			// This is an optimization where we populate directory table and use that to figure out if file exists or not..
			// .. in msvc's case it doesn't matter much because these tables are already up to date when msvc use CreateFile.
			// .. clang otoh is using CreateFile with tooons of different paths trying to open files.. in remote worker case this becomes super expensive
			if (!isWrite && !isSystemOrTempFile) // We need to skip SystemTemp.. lots of stuff going on there.
			{
				CHECK_PATH(fileNameLower);
				dirTableOffset = Rpc_GetEntryOffset(fileNameKey, fileName.data, fileName.count, checkIfDir);

				bool allowEarlyOut = true;
				if (dirTableOffset == ~u32(0))
				{
					// This could be a written file not reported to server yet
					{
						SCOPED_READ_LOCK(g_mappedFileTable.m_lookupLock, lock);
						auto findIt = g_mappedFileTable.m_lookup.find(fileNameKey);
						if (findIt != g_mappedFileTable.m_lookup.end())
							allowEarlyOut = findIt->second.deleted;
					}
					if (allowEarlyOut)
					{
						//SetLastError(ERROR_FILE_NOT_FOUND); // Don't think this is needed
						*hFileHandle = INVALID_HANDLE_VALUE;
						DEBUG_LOG_DETOURED(funcName, L"NOTFOUND_USINGTABLE (%ls) -> Error", fileName.data);

#if UBA_DEBUG_VALIDATE
						if (g_validateFileAccess)
						{
							SuppressDetourScope _;
							UBA_ASSERTF(True_GetFileAttributesW(fileName.data) == INVALID_FILE_ATTRIBUTES, L"DIRTABLE claims file %ls does not exist but it does", fileName.data);
						}
#endif

						return STATUS_OBJECT_NAME_NOT_FOUND;
					}
				}
				else if (!checkIfDir)
				{
					// File could have been deleted.
					DirectoryTable::EntryInformation entryInfo;
					g_directoryTable.GetEntryInformation(entryInfo, dirTableOffset);
					if (entryInfo.attributes == 0)
					{
						DEBUG_LOG_DETOURED(funcName, L"DELETED %llu, (%ls) -> Success", uintptr_t(*hFileHandle), fileName.data);
						return STATUS_OBJECT_NAME_NOT_FOUND;
					}
					else if (useContent && IsDirectory(entryInfo.attributes))
					{
						DEBUG_LOG_DETOURED(funcName, L"%llu, (%ls) -> STATUS_FILE_IS_A_DIRECTORY", uintptr_t(*hFileHandle), fileName.data);
						return STATUS_FILE_IS_A_DIRECTORY;
					}
				}

				bool isWriteAttributes = (DesiredAccess & FILE_WRITE_ATTRIBUTES) != 0;

				// If file is an output file we still allllow this path and accept wrong (compressed) size.
				// This is a bit hacky but we don't want to transfer and decompress file just to get the size
				if (allowEarlyOut && !useContent && !isWriteAttributes && (!CouldBeCompressedFile(fileName) || g_rules->IsOutputFile(fileName)))
				{
					auto dh = new DetouredHandle(HandleType_File);
					dh->fileObject = new FileObject();
					dh->fileObject->desiredAccess = DesiredAccess;
					dh->dirTableOffset = dirTableOffset;

					FileInfo* tempFileInfo = new FileInfo();
					dh->fileObject->fileInfo = tempFileInfo;
					dh->fileObject->ownsFileInfo = true;
					dh->fileObject->deleteOnClose = isDeleteOnClose;
					tempFileInfo->originalName = _wcsdup(fileName.data);
					tempFileInfo->name = L"GETATTRIBUTES";
					tempFileInfo->refCount = 1;
					*hFileHandle = makeDetouredHandle(dh);
					//SetLastError(ERROR_SUCCESS); // Don't think this is needed
					DEBUG_LOG_DETOURED(funcName, L"GETATTRIBUTES %llu, (%ls) -> Success", uintptr_t(*hFileHandle), fileName.data);
					return STATUS_SUCCESS;
				}
			}
		}
	}

	if (isSystemOrTempFile)
	{
	}
	else if ((DesiredAccess & FILE_LIST_DIRECTORY) != 0 && (CreateOptions & FILE_DIRECTORY_FILE) != 0)
	{
		if (isWrite || !g_allowListDirectoryHandle)
		{
			TimerScope ts(g_kernelStats.createFile);
			UBA_ASSERT(!g_runningRemote);
			NTSTATUS res = True_NtCreateFile(hFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);

			DEBUG_LOG_DETOURED(funcName, L"(CREATE_DIR) %llu, (%ls) -> %ls", uintptr_t(*hFileHandle), fileName.data, ToString(res).data);
			return res;
		}

		UBA_ASSERT(fileNameLower.data[fileNameLower.count - 1] != '\\');
		DirHash hash(fileNameLower);

		SCOPED_WRITE_LOCK(g_directoryTable.m_lookupLock, lookupLock);
		auto insres = g_directoryTable.m_lookup.try_emplace(hash.key, g_memoryBlock);
		DirectoryTable::Directory& dir = insres.first->second;
		if (insres.second)
		{
			auto existsResult = g_directoryTable.EntryExistsNoLock(hash.key, fileNameLower);
			if (existsResult != DirectoryTable::Exists_No)
				Rpc_UpdateDirectory(hash.key, fileNameLower.data, fileNameLower.count, false);
		}

		bool exists = false;
		if (dir.tableOffset != InvalidTableOffset)
		{
			u32 entryOffset = dir.tableOffset | 0x80000000;
			DirectoryTable::EntryInformation entryInfo;
			g_directoryTable.GetEntryInformation(entryInfo, entryOffset);
			exists = entryInfo.attributes != 0;
		}

#if UBA_DEBUG_VALIDATE
		NTSTATUS res = exists ? 0 : -1; (void)res;
		HANDLE validateHandle = INVALID_HANDLE_VALUE;
		if (g_validateFileAccess && !isListDirectoryHandle(rootDir))
		{
			IO_STATUS_BLOCK IoStatusBlock2;
			ObjectAttributes->RootDirectory = rootDir;
			NTSTATUS res2 = True_NtCreateFile(&validateHandle, DesiredAccess, ObjectAttributes, &IoStatusBlock2, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength); (void)res2;
			UBA_ASSERT(res < 0 && res2 < 0 || res >= 0 && res2 >= 0);
			ObjectAttributes->RootDirectory = nullptr;
		}
#endif

		if (!exists)
		{
			DEBUG_LOG_DETOURED(funcName, L"(AS_DIRECTORY) (%ls) -> NOT EXISTS", fileName.data);
			return STATUS_OBJECT_NAME_NOT_FOUND;
		}
		g_directoryTable.PopulateDirectory(hash.open, dir);

		auto listHandle = new ListDirectoryHandle{ hash.key, insres.first->second };
		listHandle->it = 0;

		SCOPED_READ_LOCK(dir.lock, lock);
		listHandle->fileTableOffsets.resize(dir.files.size());
		u32 it = 0;
		for (auto& pair : dir.files)
			listHandle->fileTableOffsets[it++] = pair.second;
		lock.Leave();

#if UBA_DEBUG_VALIDATE
		if (g_validateFileAccess)
			listHandle->validateHandle = validateHandle;
#endif
		*hFileHandle = makeListDirectoryHandle(listHandle);

		listHandle->originalName = g_memoryBlock.Strdup(fileName).data;

		IoStatusBlock->Information = 1;
		IoStatusBlock->Pointer = nullptr;
		IoStatusBlock->Status = 0;
		DEBUG_LOG_DETOURED(funcName, L"(AS_DIRECTORY) (%ls) -> %llu", fileName.data, uintptr_t(*hFileHandle));

		return STATUS_SUCCESS;
	}

	if (!keepInMemory || !isWrite) // we might get \\pipe\ here... 
		CHECK_PATH(fileNameLower);

	const wchar_t* lpFileName = fileName.data;
	u32 closeId = 0;

	SCOPED_WRITE_LOCK(g_mappedFileTable.m_lookupLock, lookupLock);
	auto insres = g_mappedFileTable.m_lookup.try_emplace(fileNameKey);
	FileInfo& info = insres.first->second;
	u32 lastDesiredAccess = info.lastDesiredAccess;
	if (insres.second)
	{
		u64 size = InvalidValue;
		info.originalName = g_memoryBlock.Strdup(fileName).data;
		info.name = info.originalName;
		if (!keepInMemory && !isSystemOrTempFile)
		{
			u8 access = GetFileAccessFlags(DesiredAccess, CreateDisposition);
			wchar_t newFileName[512];
			Rpc_CreateFileW(fileName, fileNameKey, access, newFileName, sizeof_array(newFileName), size, closeId, false);
			info.name = g_memoryBlock.Strdup(newFileName);
			lpFileName = info.name;
		}

		info.size = size;
		info.fileNameKey = fileNameKey;
		info.lastDesiredAccess = DesiredAccess;
	}
	else
	{
		if (!info.originalName)
			info.originalName = g_memoryBlock.Strdup(fileName).data;
		if (isWrite)
		{
			bool lastWasWrite = IsContentWrite(info.lastDesiredAccess, 0);
			UBA_ASSERT(!info.isFileMap);
			bool shouldReport = !lastWasWrite || info.deleted || isDeleteOnClose;
			shouldReport = shouldReport && !keepInMemory;
			if (shouldReport)
			{
				u64 size = InvalidValue;
				info.deleted = false;
				wchar_t newFileName[1024];
				u8 access = GetFileAccessFlags(DesiredAccess, CreateDisposition);
				Rpc_CreateFileW(fileName, fileNameKey, access, newFileName, sizeof_array(newFileName), size, closeId, false);
				info.name = g_memoryBlock.Strdup(newFileName);
				//info.size = size; // TODO: Should this be set?
				lpFileName = info.name;
			}
			bool lastUseContent = IsContentUse(info.lastDesiredAccess, 0);
			if (!useContent || !lastUseContent)
				lpFileName = info.name;
			info.lastDesiredAccess |= DesiredAccess;
		}
		else if (info.deleted)
		{
			lpFileName = L"";
		}
		else
		{
			if (!info.mappingChecked && info.name[0] == '^' && !g_runningRemote && CouldBeCompressedFile(fileName))
			{
				Rpc_CheckRemapping(fileName, fileNameKey);
				info.mappingChecked = true;
			}
			lpFileName = info.name;
		}
	}

	if (!*lpFileName)
	{
		DEBUG_LOG_DETOURED(funcName, L"(deleted) not found (%ls)", fileName.data);
		//UBA_ASSERTF(dwFlagsAndAttributes != FILE_FLAG_BACKUP_SEMANTICS, L"Not finding %ls", fileName);
		//SetLastError(ERROR_FILE_NOT_FOUND);
		return STATUS_OBJECT_NAME_NOT_FOUND;
	}


	auto TrackFileInput = [&]()
		{
			if (!keepInMemory && useContent && !isWrite)
			{
				if (!info.tracked)
				{
					info.tracked = true;
					TrackInput(fileName.data);
				}
			}
			else
			{
				SkipTrackInput(fileName.data);
			}
		};

	auto CreateFileHandle = [&](HANDLE th = INVALID_HANDLE_VALUE)
		{
			auto fo = new FileObject();
			fo->desiredAccess = DesiredAccess;
			fo->closeId = closeId;
			fo->fileInfo = &info;
			InterlockedIncrement(&info.refCount);
			fo->deleteOnClose = isDeleteOnClose;
			auto dh = new DetouredHandle(HandleType_File, th);
			dh->dirTableOffset = dirTableOffset;
			dh->fileObject = fo;
			return makeDetouredHandle(dh);
		};

	if (lpFileName[0] == '$')
	{
		lookupLock.Leave();

		UBA_ASSERT(!lpFileName[2]);

		bool isDir = lpFileName[1] == 'd';
		if (isDir && useContent)
			return STATUS_FILE_IS_A_DIRECTORY;

		MemoryFile& mf = g_emptyMemoryFile;
		info.memoryFile = &mf;

		UBA_ASSERT(!isDeleteOnClose);
		*hFileHandle = CreateFileHandle();

		TrackFileInput();

		DEBUG_LOG_DETOURED(funcName, L"(EMPTY) %llu (%ls) (%ls)", uintptr_t(*hFileHandle), lpFileName, (fileName.data != lpFileName ? fileName.data : L""));
		return STATUS_SUCCESS;
	}

	if (lpFileName[0] == '^') // It is a HANDLE from session process
	{
		UBA_ASSERTF(!isWrite, TC("Mapped file cant be open for write (%s)"), info.originalName);

		const wchar_t* handleStr = lpFileName + 1;
		const wchar_t* handleStrEnd = wcschr(handleStr, '-');
		if (!handleStrEnd)
		{
			UBA_ASSERT(handleStrEnd);
			return STATUS_UNSUCCESSFUL;
		}
		HANDLE mappingHandle = FileMappingHandle::FromU64(StringToValue(handleStr, handleStrEnd - handleStr)).mh;
		const wchar_t* mappingOffsetStr = handleStrEnd + 1;
		u64 mappingOffset = StringToValue(mappingOffsetStr, wcslen(mappingOffsetStr));
		info.trueFileMapOffset = mappingOffset;

		info.isFileMap = true;
		if (!True_DuplicateHandle(g_hostProcess, mappingHandle, GetCurrentProcess(), &info.trueFileMapHandle, 0, FALSE, DUPLICATE_SAME_ACCESS))
		{
			Rpc_WriteLogf(L"Can't duplicate handle 0x%llx (%ls) for file %ls (Error %u)", uintptr_t(mappingHandle), lpFileName, info.originalName, GetLastError());
			UBA_ASSERTF(info.trueFileMapHandle, L"Can't duplicate handle 0x%llx (%ls) for file %ls (Error %u)", uintptr_t(mappingHandle), lpFileName, info.originalName, GetLastError());
			return STATUS_UNSUCCESSFUL;
		}

		lookupLock.Leave();

		UBA_ASSERT(info.size != InvalidValue);
		UBA_ASSERTF(!isDeleteOnClose, TC("Creating file mapping %s that has delete on close"), info.originalName);
		*hFileHandle = CreateFileHandle();

		TrackFileInput();

		DEBUG_LOG_DETOURED(funcName, L"(MAPPED)%ls %llu (%ls) (%ls) -> Success", isWriteStr, uintptr_t(*hFileHandle), lpFileName, (fileName.data != lpFileName ? fileName.data : L""));
		return STATUS_SUCCESS;
	}

	auto TakeFileOwnershipFromSession = [&](FileMappingHandle mappingHandle, u64 mappingHandleSize)
		{
			info.memoryFile = new MemoryFile(nullptr, false);
			MemoryFile& mf = *info.memoryFile;
			HANDLE newHandle = 0;
			True_DuplicateHandle(g_hostProcess, mappingHandle.mh, GetCurrentProcess(), &newHandle, 0, FALSE, DUPLICATE_SAME_ACCESS);
			UBA_ASSERTF(newHandle, L"DuplicateHandle failed when opening temp file %ls (%u)", fileName.data, GetLastError());
			mf.writtenSize = mappingHandleSize;
			mf.committedSize = AlignUp(mappingHandleSize, g_pageSize);
			mf.mappedSize = mf.committedSize;
			TimerScope ts2(g_kernelStats.mapViewOfFile);
			mf.baseAddress = (u8*)True_MapViewOfFile(newHandle, FILE_MAP_READ | FILE_MAP_ALL_ACCESS, 0, 0, mf.mappedSize);
			UBA_ASSERTF(mf.baseAddress, L"MapViewOfFile failed when opening temp file %ls (%u)", fileName.data, GetLastError());
			mf.reserveSize = FileTypeMaxSize(fileName, isSystemOrTempFile);
			mf.mappingHandle.mh = newHandle;
		};

	if (lpFileName[0] == ':') // It is a HANDLE from session process. A written file that is writable
	{
		if (!info.memoryFile)
		{
			const wchar_t* handleStr = lpFileName + 1;
			const wchar_t* handleStrEnd = wcschr(handleStr, '-');
			if (!handleStrEnd)
			{
				UBA_ASSERT(handleStrEnd);
				return STATUS_UNSUCCESSFUL;
			}
			auto mappingHandle = FileMappingHandle::FromU64(StringToValue(handleStr, handleStrEnd - handleStr));
			UBA_ASSERT(!StringToValue(handleStrEnd + 1, wcslen(handleStrEnd + 1)));
			UBA_ASSERT(info.size != InvalidValue);

			TakeFileOwnershipFromSession(mappingHandle, info.size);
		}

		if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF)
			info.memoryFile->writtenSize = 0;

		lookupLock.Leave();
		*hFileHandle = CreateFileHandle();
		TrackFileInput();
		DEBUG_LOG_DETOURED(funcName, L"(WRITTENFILE)%ls %llu (%ls) (%ls) -> Success", isWriteStr, uintptr_t(*hFileHandle), lpFileName, (fileName.data != lpFileName ? fileName.data : L""));
		return STATUS_SUCCESS;
	}
	
	if (keepInMemory || info.memoryFile)
	{
		#if UBA_DEBUG_LOG_ENABLED
		const wchar_t* memoryType = L"MEMORY";
		#endif

		if (!info.memoryFile)
		{
			if (NeedsSharedMemory(fileName.data))
			{
				#if UBA_DEBUG_LOG_ENABLED
				memoryType = L"SHAREDMEMORY";
				#endif
				{
					RPC_MESSAGE(OpenTempFile, openTempFile)
					writer.WriteStringKey(fileNameKey);
					writer.WriteString(fileNameLower);
					writer.Flush();
					BinaryReader reader;
					FileMappingHandle mappingHandle = FileMappingHandle::FromU64(reader.ReadU64());
					u64 mappingHandleSize = reader.ReadU64();
					pcs.Leave();
					if (mappingHandle.IsValid())
					{
						TakeFileOwnershipFromSession(mappingHandle, mappingHandleSize);
					}
					else if (isWrite)
					{
						info.memoryFile = new MemoryFile(false, FileTypeMaxSize(fileName, isSystemOrTempFile), false, fileName.data);
					}
					else
					{
						DEBUG_LOG_DETOURED(funcName, L"(memory) not found (%ls)", fileName.data);
						return STATUS_OBJECT_NAME_NOT_FOUND;
					}
				}
			}
			else
			{
				if (!isThrowAway && CreateDisposition == FILE_OPEN)
				{
					if (!info.memoryFile)
					{
						*hFileHandle = INVALID_HANDLE_VALUE;
						DEBUG_LOG_DETOURED(funcName, L"ALREADYEXISTS (%ls) -> Error", fileName.data);
						return STATUS_OBJECT_NAME_EXISTS;//STATUS_OBJECT_NAME_NOT_FOUND;
					}
				}
				else
				{
					bool isOutput = (isWrite || isDeleteOnClose) && g_rules->IsOutputFile(fileName);
					if (isOutput && (CreateDisposition & FILE_OPEN_IF) == 0)
					{
						UBA_ASSERTF(false, TC("Trying to open %s with openif. This is not supported"), fileName.data);
					}

					bool isLocal = !isOutput;
					//UBA_ASSERTF(CreateDisposition != FILE_OPEN || Contains(fileName.data, L"vctip_"), TC("Unsupported disposition %u for file %s"), CreateDisposition, fileName.data);
					info.memoryFile = new MemoryFile(isLocal, FileTypeMaxSize(fileName, isSystemOrTempFile), isThrowAway, fileName.data);
				}
			}

			// TODO: Time should be in sync with host machine!
			FILETIME ft;
			SYSTEMTIME st;
			GetSystemTime(&st);
			SystemTimeToFileTime(&st, &ft);
			info.memoryFile->fileTime = (u64&)ft;
			info.memoryFile->volumeSerial = 1;
			info.memoryFile->fileIndex = InterlockedDecrement(&g_memoryFileIndexCounter);
		}
		else
		{
			#if UBA_DEBUG_LOG_ENABLED
			if (!info.memoryFile->isLocalOnly)
				memoryType = L"SHAREDMEMORY";
			#endif
		}

		lookupLock.Leave();

		*hFileHandle = CreateFileHandle();

		TrackFileInput();

		DEBUG_LOG_DETOURED(funcName, L"(%s)%ls %llu (%ls) (%ls) -> Success", memoryType, isWriteStr, uintptr_t(*hFileHandle), lpFileName, (fileName.data != lpFileName ? fileName.data : L""));
		return STATUS_SUCCESS;
	}

	lookupLock.Leave();

	StringView tempFileName;
	if (lpFileName[0] == '#')
		tempFileName = fileName;
	else
		tempFileName = ToView(info.name);

	StringBuffer<> temp;
	temp.Append(TCV("\\??\\"));
	if (IsUncPath(tempFileName.data))
		temp.Append(TCV("UNC")).Append(StringView(tempFileName).Skip(1));
	else
		temp.Append(tempFileName);

	UNICODE_STRING* old = ObjectAttributes->ObjectName;
	UNICODE_STRING str;
	str.Buffer = temp.data;
	str.Length = u16(temp.count * 2);
	str.MaximumLength = str.Length + 2;
	ObjectAttributes->ObjectName = &str;
	// TODO!!! THIS NEEDS TO set the ObjectAttributes->ObjectName->Buffer and ObjectAttributes->ObjectName->Length;
	//wcscpy_s(ObjectAttributes->ObjectName->Buffer + 4, ObjectAttributes->ObjectName->MaximumLength/2 - 8, lpFileName);
	//ObjectAttributes->ObjectName->Length = u16(wcslen(lpFileName)*2) + 8;

	NTSTATUS res = Local_NtCreateFile(IsCreateFunc, hFileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);

	ObjectAttributes->ObjectName = old;

	if (NT_ERROR(res))
	{
		if (closeId)
		{
			info.lastDesiredAccess = lastDesiredAccess;
			Rpc_UpdateCloseHandle(L"", closeId, false, L"", {}, 0, false);
		}
		DEBUG_LOG_TRUE(funcName, L"%ls (%ls) (%ls) -> %ls", isWriteStr, lpFileName, (fileName.data != lpFileName ? fileName.data : L""), ToString(res).data);
		return res;
	}

	TrackFileInput();

	UBA_ASSERT(info.originalName);
	*hFileHandle = CreateFileHandle(*hFileHandle);
	DEBUG_LOG_TRUE(funcName, L"%ls %llu (%ls)%s -> %ls", isWriteStr, uintptr_t(*hFileHandle), tempFileName.data, isDeleteOnClose ? TC(" DeleteOnClose") : TC(""), ToString(res).data);
	return res;
}

NTSTATUS NTAPI Detoured_NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
	ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
{
	DETOURED_CALL(NtCreateFile);
	return Shared_NtCreateFile(true, FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
}

NTSTATUS NTAPI Detoured_NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)
{
	DETOURED_CALL(NtOpenFile);
	return Shared_NtCreateFile(false, FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, (PLARGE_INTEGER)NULL, 0L, ShareAccess, FILE_OPEN, OpenOptions, (PVOID)NULL, 0L);
}

NTSTATUS NTAPI Detoured_NtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength)
{
	DETOURED_CALL(NtFsControlFile);
	HANDLE trueHandle = FileHandle;
	if (isDetouredHandle(FileHandle))
	{
		auto& dh = asDetouredHandle(FileHandle);
		trueHandle = dh.trueHandle;
		//if (trueHandle == INVALID_HANDLE_VALUE)
		//{
		//	return STATUS_UNSUCCESSFUL;
		//}
		UBA_ASSERTF(trueHandle != INVALID_HANDLE_VALUE, L"NtFsControlFile code %u (%ls)", FsControlCode, HandleToName(FileHandle));
	}
	UBA_ASSERT(!isListDirectoryHandle(FileHandle));

	return True_NtFsControlFile(trueHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FsControlCode, InputBuffer, InputBufferLength, OutputBuffer, OutputBufferLength);
}

NTSTATUS NTAPI Detoured_NtCopyFileChunk(HANDLE Source, HANDLE Dest, HANDLE Event, PIO_STATUS_BLOCK IoStatusBlock, ULONG Length, PULONG SourceOffset, PULONG DestOffset, PULONG SourceKey, PULONG DestKey, ULONG Flags)
{
	DETOURED_CALL(NtCopyFileChunk);
	HANDLE trueSourceHandle = Source;
	if (isDetouredHandle(Source))
	{
		auto& dh = asDetouredHandle(Source);
		trueSourceHandle = dh.trueHandle;
		UBA_ASSERT(trueSourceHandle != INVALID_HANDLE_VALUE);
	}
	HANDLE trueDestHandle = Dest;
	if (isDetouredHandle(Dest))
	{
		auto& dh = asDetouredHandle(Dest);
		trueDestHandle = dh.trueHandle;
		UBA_ASSERT(trueDestHandle != INVALID_HANDLE_VALUE);
	}
	return True_NtCopyFileChunk(trueSourceHandle, trueDestHandle, Event, IoStatusBlock, Length, SourceOffset, DestOffset, SourceKey, DestKey, Flags);
}
NTSTATUS NTAPI Detoured_NtClose(HANDLE handle)
{
	DETOURED_CALL(NtClose);

	if (handle == INVALID_HANDLE_VALUE || handle == PseudoHandle)
	{
		TimerScope ts(g_kernelStats.closeHandle);
		return True_NtClose(handle);
	}

	if (isListDirectoryHandle(handle))
	{
		auto& listHandle = asListDirectoryHandle(handle);

#if UBA_DEBUG_VALIDATE
		if (g_validateFileAccess)
		{
			auto res = True_NtClose(listHandle.validateHandle);
			if (res != 0)
				ToInvestigate(L"NtClose failed for validate handle");
		}
#endif

		delete& listHandle;
		return STATUS_SUCCESS;
	}

	if (!isDetouredHandle(handle))
	{
		TimerScope ts(g_kernelStats.closeHandle);
		auto res = True_NtClose(handle);
#if !defined(_M_ARM64) // For some reason this log line crashes on arm64 with access violation on internal tls variable
		DEBUG_LOG_TRUE(L"NtClose", L"%llu (%ls) -> %ls", uintptr_t(handle), HandleToName(handle), ToString(res).data);
#endif
		return res;
	}

	DetouredHandle& dh = asDetouredHandle(handle);

	NTSTATUS res = STATUS_SUCCESS;

	if (dh.trueHandle != INVALID_HANDLE_VALUE)
	{
		TimerScope ts(g_kernelStats.closeFile);
		res = True_NtClose(dh.trueHandle);
	}

	FileObject* fo = dh.fileObject;
	if (!fo)
	{
		if (dh.type >= HandleType_StdErr) // TODO: This might leak if handle is duplicated.. but ignore for now
			return res;
		DEBUG_LOG_TRUE(L"NtClose", L"%llu (%ls) -> %ls", uintptr_t(handle), HandleToName(handle), ToString(res).data);
		delete& dh;
		return res;
	}

	auto foRefCount = InterlockedDecrement(&fo->refCount);
	UBA_ASSERT(foRefCount != ~u64(0));
	if (foRefCount)
	{
		DEBUG_LOG_TRUE(L"NtClose", L"%llu (%ls) -> %ls", uintptr_t(handle), HandleToName(handle), ToString(res).data);
		delete& dh;
		return res;
	}

	FileMappingHandle mappingHandle;
	u64 mappingWritten = 0;
	FileInfo& fi = *fo->fileInfo;
	const wchar_t* path = fi.name;
	wchar_t temp[512];
	if (MemoryFile* mf = fi.memoryFile)
	{
		if (IsWrite(fo->desiredAccess, 0))
		{
			// TODO: There are race conditions in this code. There could be other file handles accessing the same piece of memory (allthough unlikely)
			u64 alignedWritten = AlignUp(mf->writtenSize, 64 * 1024);
			if (alignedWritten < mf->committedSize)
			{
				u64 decommitSize = u64(mf->committedSize - alignedWritten);
				if (mf->isLocalOnly)
				{
#pragma warning(push)
#pragma warning(disable:6250)
					if (!::VirtualFree(mf->baseAddress + alignedWritten, decommitSize, MEM_DECOMMIT))
						ToInvestigate(L"Failed to decommit memory (%u)", GetLastError());
#pragma warning(pop)
				}
				else
				{
					// Speculative change. According to stackoverflow (I know) this can hint the system that this memory is not needed anymore.
					// Building UnrealEditor and friends put huge pressure on committed space and anything that can reduce that is valuable
					if (!::VirtualUnlock(mf->baseAddress + alignedWritten, decommitSize))
						if (GetLastError() != ERROR_NOT_LOCKED)
							ToInvestigate(L"Failed to unlock memory (%u)", GetLastError());
				}
				mf->committedSize = alignedWritten;
			}
		}

		mappingHandle = mf->mappingHandle;
		mappingWritten = mf->writtenSize;

		u32 orginalNameLen = TStrlen(fi.originalName);
		if ((fo->deleteOnClose || IsWrite(fo->desiredAccess, 0)) && g_rules->IsOutputFile({fi.originalName, orginalNameLen}) && !g_rules->IsThrowAway(StringView(fi.originalName, orginalNameLen), g_runningRemote))
		{
			// Need to report this file to host so it can be tracked in directory table
			if (!mf->isReported)
			{
				path = temp;
				mf->isReported = true;
				const wchar_t* fileName = fi.originalName;
				if (!fo->newName.empty())
					fileName = fo->newName.c_str();
				StringBuffer<> fixedName;
				FixPath(fixedName, fileName);
				StringKey fileNameKey = fi.fileNameKey;
				if (!fo->newName.empty())
					fileNameKey = ToStringKeyLower(fixedName);

				u64 size;
				Rpc_CreateFileW(fixedName, fileNameKey, AccessFlag_Write, temp, sizeof_array(temp), size, fo->closeId, true);
			}

			if (!fo->newName.empty())
			{
				// It might be that same process will open it again, so we will need to update the mapping table
				StringBuffer<> fixedNewName;
				FixPath(fixedNewName, fo->newName.c_str());
				fixedNewName.MakeLower();
				StringKey fileNameKey = ToStringKey(fixedNewName);
				SCOPED_WRITE_LOCK(g_mappedFileTable.m_lookupLock, _);
				auto insres = g_mappedFileTable.m_lookup.try_emplace(fileNameKey);
				FileInfo& newInfo = insres.first->second;
				newInfo = fi;
				newInfo.originalName = g_memoryBlock.Strdup(fo->newName).data;
				newInfo.name = newInfo.originalName;
				newInfo.fileNameKey = fileNameKey;
				UBA_ASSERT(!fo->deleteOnClose);
				fi = {};
				fi.deleted = true;
				fo->ownsFileInfo = false;
				fo->newName.clear();
			}
		}
		else if (NeedsSharedMemory(fi.originalName) && IsWrite(fo->desiredAccess, 0))
		{
			UBA_ASSERT(!mf->isLocalOnly);
			StringBuffer<> fixedName;
			FixPath(fixedName, path);

			RPC_MESSAGE(CreateTempFile, createTempFile)
			writer.WriteStringKey(ToStringKeyLower(fixedName));
			writer.WriteString(fixedName);
			writer.WriteU64(mappingHandle.ToU64());
			writer.WriteU64(mappingWritten);
			writer.Flush();
			BinaryReader reader;
		}
	}
	else if (fo->deleteOnClose && dh.trueHandle == INVALID_HANDLE_VALUE) // We have used an optimized handle that actually never opens the file so we need to delete it manually
	{
		DeleteFileW(fi.originalName);
	}

	if (fo->closeId)
	{
		Rpc_UpdateCloseHandle(path, fo->closeId, fo->deleteOnClose, fo->newName.c_str(), mappingHandle, mappingWritten, true);
	}
	else
	{
		// TODO: Update g_mappedFileTable.m_lookup?
		//UBA_ASSERTF(fo->newName.empty(), L"Got close of file that was renamed but had no closeId. Old: %ls New: %ls", fi.originalName, fo->newName.c_str());
	}

	InterlockedDecrement(&fi.refCount);

	DEBUG_LOG_DETOURED(L"NtClose", L"%llu (%ls) -> %ls", uintptr_t(handle), HandleToName(handle), ToString(res).data);

	if (fo->ownsFileInfo)
	{
		UBA_ASSERT(!fi.memoryFile);
		if (fi.fileMapMem)
		{
			bool success = True_UnmapViewOfFile(fi.fileMapMem); (void)success;
			DEBUG_LOG_TRUE(L"INTERNAL UnmapViewOfFile", L"%llu (%ls) (%ls) -> %ls", uintptr_t(fi.fileMapMem), fi.name, fi.originalName, ToString(success));
		}

		free((void*)fi.originalName);
		delete& fi;
	}

	delete fo;
	delete& dh;
	return res;
}

NTSTATUS Detoured_NtQueryObject(HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength)
{
	DETOURED_CALL(NtQueryObject);

	HANDLE trueHandle = Handle;

	// This can be other things than FILES.. Is used by GetHandleInformation
	if (isDetouredHandle(Handle))
	{
		DetouredHandle& dh = asDetouredHandle(Handle);
		trueHandle = dh.trueHandle;

		if (trueHandle == INVALID_HANDLE_VALUE)
		{
			if (ObjectInformationClass == 1) // ObjectNameInformation
			{
				auto fo = dh.fileObject;
				UBA_ASSERT(fo);
				auto& fi = *fo->fileInfo;
				UBA_ASSERT(fi.originalName);
				const wchar_t* fileName = fi.originalName;

				StringBuffer<> fixedPath;
				FixPath(fileName, g_virtualWorkingDir.data, g_virtualWorkingDir.count, fixedPath);

				StringBuffer<> buffer;

				g_directoryTable.GetFinalPath(buffer, fixedPath.data);
				VirtualizePath(buffer);

				if (g_runningRemote || buffer[0] != fixedPath[0])
				{
					// It was remote or virtualized.. let's make up device drive.
					// TODO: Maybe we need to replicate device drive names to remotes
					buffer.Prepend(AsView(L"\\Device\\HarddiskVolume100"), 2);
				}
				else
				{
					wchar_t drive[3] = { buffer[0], ':', 0 };
					wchar_t device[256];
					DWORD deviceLen = QueryDosDeviceW(drive, device, sizeof_array(device));
					UBA_ASSERT(deviceLen);
					buffer.Prepend(StringView(device, deviceLen), 2);
				}

				u64 bufferSize = (buffer.count+1)*sizeof(tchar);
				u64 totalSize = sizeof(UNICODE_STRING) + bufferSize;

				if (ObjectInformationLength < totalSize)
				{
					DEBUG_LOG_DETOURED(L"NtQueryObject", L"(ObjectNameInformation) %s -> STATUS_BUFFER_OVERFLOW", HandleToName(Handle));
					return STATUS_BUFFER_OVERFLOW;
				}
				auto& ustr = *(PUNICODE_STRING)ObjectInformation;
				ustr.Length = u16(bufferSize - sizeof(tchar));
				ustr.MaximumLength = u16(bufferSize);
				ustr.Buffer = (wchar_t*)((&ustr) + 1);
				memcpy(ustr.Buffer, buffer.data, bufferSize);
				*ReturnLength = (ULONG)totalSize;

				DEBUG_LOG_DETOURED(L"NtQueryObject", L"(ObjectNameInformation) %llu -> Success (%s)", uintptr_t(Handle), buffer.data);
				return STATUS_SUCCESS;
			}

			UBA_ASSERTF(false, L"NtQueryObject NOT_IMPLEMENTED (class %i) (%s)", ObjectInformationClass, HandleToName(Handle));
		}
	}
	auto res = True_NtQueryObject(trueHandle, ObjectInformationClass, ObjectInformation, ObjectInformationLength, ReturnLength);
	DEBUG_LOG_TRUE(L"NtQueryObject", L"(%i) %llu -> %ls", ObjectInformationClass, uintptr_t(Handle), ToString(res).data);
	return res;
}

NTSTATUS Detoured_NtQueryInformationProcess(HANDLE ProcessHandle, PROCESSINFOCLASS ProcessInformationClass, PVOID ProcessInformation, ULONG ProcessInformationLength, PULONG ReturnLength)
{
	DETOURED_CALL(NtQueryInformationProcess);
	if (isDetouredHandle(ProcessHandle))
	{
		ProcessHandle = asDetouredHandle(ProcessHandle).trueHandle;
	}

	NTSTATUS res = True_NtQueryInformationProcess(ProcessHandle, ProcessInformationClass, ProcessInformation, ProcessInformationLength, ReturnLength);
	DEBUG_LOG_TRUE(L"NtQueryInformationProcess", L"(class %u) %llu -> %ls", ProcessInformationClass, uintptr_t(ProcessHandle), ToString(res).data);
	return res;
}

#if defined(DETOURED_INCLUDE_DEBUG)

NTSTATUS NTAPI Detoured_NtCreateIoCompletion(PHANDLE IoCompletionHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, ULONG Count)
{
	UBA_ASSERT(!isDetouredHandle(*IoCompletionHandle));
	return True_NtCreateIoCompletion(IoCompletionHandle, DesiredAccess, ObjectAttributes, Count);
}

NTSTATUS NTAPI Detoured_NtFlushBuffersFileEx(HANDLE FileHandle, ULONG Flags, PVOID Parameters, ULONG ParametersSize, PIO_STATUS_BLOCK IoStatusBlock)
{
	DETOURED_CALL(NtFlushBuffersFileEx);
	UBA_ASSERT(!isDetouredHandle(FileHandle));
	return True_NtFlushBuffersFileEx(FileHandle, Flags, Parameters, ParametersSize, IoStatusBlock);
}

NTSTATUS NTAPI Detoured_NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key)
{
	DETOURED_CALL(NtReadFile);
	UBA_ASSERT(!isDetouredHandle(FileHandle));
	return True_NtReadFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, Buffer, Length, ByteOffset, Key);
}

NTSTATUS NTAPI Detoured_NtAlpcCreatePort(PHANDLE PortHandle, POBJECT_ATTRIBUTES ObjectAttributes, PALPC_PORT_ATTRIBUTES PortAttributes)
{
	//DEBUG_LOG_TRUE(L"NtAlpcCreatePort", L"");
	return True_NtAlpcCreatePort(PortHandle, ObjectAttributes, PortAttributes);
}

NTSTATUS NTAPI Detoured_NtAlpcConnectPort(PHANDLE PortHandle, PUNICODE_STRING PortName, POBJECT_ATTRIBUTES ObjectAttributes, PALPC_PORT_ATTRIBUTES PortAttributes, DWORD ConnectionFlags, PSID RequiredServerSid, PPORT_MESSAGE ConnectionMessage, PSIZE_T ConnectMessageSize, PALPC_MESSAGE_ATTRIBUTES OutMessageAttributes, PALPC_MESSAGE_ATTRIBUTES InMessageAttributes, PLARGE_INTEGER Timeout)
{
	//DEBUG_LOG_TRUE(L"NtAlpcConnectPort", L"");
	return True_NtAlpcConnectPort(PortHandle, PortName, ObjectAttributes, PortAttributes, ConnectionFlags, RequiredServerSid, ConnectionMessage, ConnectMessageSize, OutMessageAttributes, InMessageAttributes, Timeout);
}

NTSTATUS NTAPI Detoured_NtAlpcCreatePortSection(HANDLE PortHandle, ULONG Flags, HANDLE SectionHandle, SIZE_T SectionSize, PHANDLE AlpcSectionHandle, PSIZE_T ActualSectionSize)
{
	//DEBUG_LOG_TRUE(L"NtAlpcCreatePortSection", L"");
	return True_NtAlpcCreatePortSection(PortHandle, Flags, SectionHandle, SectionSize, AlpcSectionHandle, ActualSectionSize);
}

NTSTATUS NTAPI Detoured_NtAlpcSendWaitReceivePort(HANDLE PortHandle, DWORD Flags, PPORT_MESSAGE SendMessage_, PALPC_MESSAGE_ATTRIBUTES SendMessageAttributes, PPORT_MESSAGE ReceiveMessage, PSIZE_T BufferLength, PALPC_MESSAGE_ATTRIBUTES ReceiveMessageAttributes, PLARGE_INTEGER Timeout)
{
	//u64 size = 0;
	//if (SendMessage_)
	//	size = SendMessage_->u1.s1.DataLength;
	//DEBUG_LOG_TRUE(L"NtAlpcSendWaitReceivePort", L"%llu", size);
	return True_NtAlpcSendWaitReceivePort(PortHandle, Flags, SendMessage_, SendMessageAttributes, ReceiveMessage, BufferLength, ReceiveMessageAttributes, Timeout);
}

NTSTATUS NTAPI Detoured_NtAlpcDisconnectPort(HANDLE PortHandle, ULONG Flags)
{
	//DEBUG_LOG_TRUE(L"NtAlpcDisconnectPort", L"");
	return True_NtAlpcDisconnectPort(PortHandle, Flags);
}

NTSTATUS NTAPI Detoured_ZwQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length,
	FILE_INFORMATION_CLASS FileInformationClass, BOOLEAN ReturnSingleEntry, PUNICODE_STRING FileName, BOOLEAN RestartScan)
{
	DETOURED_CALL(ZwQueryDirectoryFile);
	DEBUG_LOG_TRUE(L"ZwQueryDirectoryFile", L"(%ls)", HandleToName(FileHandle));
	UBA_ASSERT(!isDetouredHandle(FileHandle));
	return True_ZwQueryDirectoryFile(FileHandle, Event, ApcRoutine, ApcContext, IoStatusBlock, FileInformation, Length, FileInformationClass, ReturnSingleEntry, FileName, RestartScan);
}

//NTSTATUS NTAPI Detoured_ZwCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes, 
//									 ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions, PVOID EaBuffer, ULONG EaLength)
//{
//	DETOURED_CALL(ZwCreateFile);
//	DEBUG_LOG_TRUE(L"ZwCreateFile", L"");
//	UBA_ASSERT(!isDetouredHandle(FileHandle));
//	return True_ZwCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, AllocationSize, FileAttributes, ShareAccess, CreateDisposition, CreateOptions, EaBuffer, EaLength);
//}

//NTSTATUS NTAPI Detoured_ZwOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions)
//{
//	DETOURED_CALL(ZwOpenFile);
//	DEBUG_LOG_TRUE(L"ZwOpenFile", L"");
//	UBA_ASSERT(!isDetouredHandle(FileHandle));
//	return True_ZwCreateFile(FileHandle, DesiredAccess, ObjectAttributes, IoStatusBlock, (PLARGE_INTEGER)NULL, 0L, ShareAccess, FILE_OPEN, OpenOptions, (PVOID)NULL, 0L);
//}

NTSTATUS NTAPI Detoured_ZwSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation, ULONG Length, FILE_INFORMATION_CLASS FileInformationClass)
{
	DETOURED_CALL(ZwSetInformationFile);
	DEBUG_LOG_TRUE(L"ZwSetInformationFile", L"%llu (%ls)", uintptr_t(FileHandle), HandleToName(FileHandle));
	UBA_ASSERT(!isDetouredHandle(FileHandle));
	return True_ZwSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length, FileInformationClass);
}

PVOID Detoured_RtlAllocateHeap(PVOID HeapHandle, ULONG Flags, SIZE_T Size)
{
	//if (Flags & HEAP_ZERO_MEMORY)
	//	return mi_zalloc(Size);
	//else
	//	return mi_malloc(Size);
	return True_RtlAllocateHeap(HeapHandle, Flags, Size);
}

PVOID Detoured_RtlReAllocateHeap(PVOID HeapHandle, ULONG Flags, PVOID BaseAddress, SIZE_T Size)
{
#if UBA_USE_MIMALLOC
	if (g_checkRtlHeap && IsInMiMalloc(BaseAddress))
	{
		return mi_realloc(BaseAddress, Size);
		//Rpc_WriteLogf(L"ERROR: RtlReAllocateHeap - This is not implemented");
		//return 0;
	}
#endif
	//if (Flags & HEAP_ZERO_MEMORY)
	//	return mi_realloc(BaseAddress, Size);
	//else
	//	return mi_realloc(BaseAddress, Size);
	return True_RtlReAllocateHeap(HeapHandle, Flags, BaseAddress, Size);
}

BOOLEAN Detoured_RtlValidateHeap(HANDLE HeapPtr, ULONG Flags, PVOID Block)
{
	//return true;
	return True_RtlValidateHeap(HeapPtr, Flags, Block);
}

NTSTATUS Detoured_RtlDosPathNameToNtPathName_U_WithStatus(PCWSTR dos_path, PUNICODE_STRING ntpath, PWSTR* file_part, VOID* reserved)
{
	auto res = True_RtlDosPathNameToNtPathName_U_WithStatus(dos_path, ntpath, file_part, reserved);
	//DEBUG_LOG_TRUE(L"Detoured_RtlDosPathNameToNtPathName_U_WithStatus", L"-> %s", ToString(res).data);
	return res;
}


#endif
