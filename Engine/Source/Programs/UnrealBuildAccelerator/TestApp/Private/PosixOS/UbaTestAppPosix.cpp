// Copyright Epic Games, Inc. All Rights Reserved.

#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <spawn.h>

#if PLATFORM_LINUX
#include <link.h>
#include <wait.h>
#else
#include <mach-o/dyld.h>
extern char** environ;
#endif


int LogError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	char buffer[1024];
	snprintf(buffer, 1024, format, args);
	printf("%s\n", buffer);
	va_end(args);
	return -1;
}

bool IsLibraryLoaded(const char* libraryToMatch)
	{
		bool foundLib = false;
#if PLATFORM_LINUX
		// The dl_iterate_phdr() function walks through the list of an
		// application's shared objects and calls the function callback once
		// for each object, until either all shared objects have been
		// processed or callback returns a nonzero value.
		// The last iteration of the callbacks return is propigated all the way up
		foundLib = dl_iterate_phdr([](struct dl_phdr_info* info, size_t size, void* libraryToMatch)
				{
					return strstr(info->dlpi_name, (const char*)libraryToMatch) != 0 ? 1 : 0;
				}, (void*)libraryToMatch);
#elif PLATFORM_MAC

		bool foundDetoursLib = false;
		unsigned int count = _dyld_image_count();
		for (int i = 0; i < count; i++)
		{
			const char* ImageName = _dyld_get_image_name(i);
			if (strstr(ImageName, libraryToMatch))
			{
				foundLib = true;
				break;
			}
		}
#endif
		return foundLib;
	}


/******************** WARNING ********************/
// UbaTestAppPosix cannot be run standalone. It is extremely
// dependent on the UbaTest runner. Please see details in:
// UbaTestSession.h
int main(int argc, char* argv[])
{
	bool runningRemote = false;
	char* tmp = getenv("UBA_REMOTE");
	if (tmp && tmp[0] == '1')
		runningRemote = true;

	// Make the assumption that if we're running remote the detour lib will be there.
	bool foundDetoursLib = IsLibraryLoaded(UBA_DETOURS_LIBRARY) || runningRemote;


	if (!foundDetoursLib)
		return LogError("libUbaDetours not loaded. This app is designed to only start from inside UnrealBuildAccelerator.");

	if (argc == 1)
	{
		char cwd[1024];
		if (!getcwd(cwd, sizeof(cwd)))
			return LogError("getcwd failed");

		struct stat attrR;
		if (stat("FileR.h", &attrR) == -1)
			return LogError("stat for FileR.h failed");
		if (S_ISREG(attrR.st_mode) == 0)
			return LogError("stat for FileR.h did not return normal file");

		int fdr = open("FileR.h", O_RDONLY);
		if (fdr == -1)
			return LogError("open FileR.h failed");
	
		char buf[4];
		if (read(fdr, buf, 4) != 4)
			return LogError("Failed to read FileR.h");

		if (strcmp(buf, "Foo") != 0)
			return LogError("FileR.h content was wrong");

		if (close(fdr) == -1)
			return LogError("close FileR.h failed");

		int fdw = open("FileW", O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
		if (fdw == -1)
			return LogError("open FileW failed");

		if (write(fdw, "hello", 6) == -1)
			return LogError("write FileW failed");

		struct stat attrW1;
		if (fstat(fdw, &attrW1) == -1)
			return LogError("fstat FileW failed");

		if (close(fdw) == -1)
			return LogError("closed FileW failed");

		struct stat attrW2;
		if (stat("FileW", &attrW2) == -1)
			return LogError("stat for FileW failed");
		if (S_ISREG(attrW2.st_mode) == 0)
			return LogError("stat for FileW did not return normal file (%u)", attrW2.st_mode);

		if (rename("FileW", "FileW2") == -1)
			return LogError("rename for FileW to FileW2 failed");


		struct stat attrD1;
		if (stat("Dir1", &attrD1) == -1)
			return LogError("stat for Dir1 failed");
		if (S_ISREG(attrD1.st_mode) != 0)
			return LogError("stat for Dir1 did not return directory");
		
		if (mkdir("Dir2/Dir3", S_IRUSR | S_IWUSR) == 0)
			return LogError("mkdir for dir2 did not fail even though it exists");
		if (errno != EEXIST)
			return LogError("mkdir for dir2 did not return error that it exists");

		struct stat attrD2;
		if (stat("Dir2/Dir3/Dir4/Dir5", &attrD2) == -1)
			return LogError("stat for Dir2/Dir3 failed");

		struct stat attr3;
		if (stat("/usr", &attr3) == -1)
			return LogError("stat for /usr failed");

		FILE* f = fopen("FileWF", "w+");
		if (f == nullptr)
			return LogError("fopen FileWF failed");
		if (fwrite("Hello", 1, 6, f) != 6)
			return LogError("fwrite FileWF failed");
		if (fclose(f) != 0)
			return LogError("fclose FileWF failed");
		struct stat attrWF;
		if (stat("FileWF", &attrWF) == -1)
			return LogError("stat for FileW failed");

	#if !PLATFORM_LINUX // Farm linux machines do not have clang installed... need to revisit this
		char fullPath[PATH_MAX];
		if (realpath("/usr/bin/clang", fullPath) == nullptr)
			return LogError("realpath for 'clang' failed");
	#endif

		struct stat attrRoot;
		if (stat("/", &attrRoot) != 0)
			return LogError("stat for '/' failed");

		if (mkdir("FooDir", S_IRUSR | S_IWUSR) != 0)
			return LogError("mkdir 'FooDir' failed");

		struct stat attrFoo;
		if (stat("FooDir", &attrFoo) != 0)
			return LogError("stat for 'FooDir' failed");
		if (!S_ISDIR(attrFoo.st_mode))
			return LogError("stat for dir 'FooDir' returned wrong type");

		if (rmdir("FooDir") != 0)
			return LogError("rmdir 'FooDir' failed");

		if (stat("FooDir", &attrFoo) == 0)
			return LogError("stat for 'FooDir' failed to not find removed directory");

		auto dir = opendir(".");
		if (!dir)
			return LogError("opendir failed");
		while (true)
		{
			dirent* ent = readdir(dir);
			if (!ent)
				break;
		}
		closedir(dir);

		{
			char execPath[1024];
			if (readlink("/proc/self/exe", execPath, sizeof(execPath)) == -1)
				return LogError("readlink failed while getting executable path");

			pid_t childPid = 0;
			const char* args[] { execPath, "-child", nullptr };
			if (posix_spawn(&childPid, execPath, nullptr, nullptr, (char**)args, environ) != 0)
				return LogError("posix_spawn failed");
			int status = 0;
			do
			{
				if (waitpid(childPid, &status, WUNTRACED | WCONTINUED) == -1)
					return LogError("waitpid on child process failed (pid %i)", childPid);
				if (WIFSIGNALED(status))
					return LogError("Child process killed by signal %d", WTERMSIG(status));
				if (WIFSTOPPED(status))
					return LogError("Child process stopped by signal %d", WSTOPSIG(status));
				if (WIFCONTINUED(status))
					return LogError("Child process continued");
			} while (!WIFEXITED(status));
			if (WEXITSTATUS(status) != 0)
				return LogError("Child process failed");
		}

		return 0;
	}
	else if (strcmp(argv[1], "-child") == 0)
	{
		struct stat attr;
		if (stat("FileW2", &attr) != 0)
			return LogError("stat for 'FileW' in child process failed");
		if (stat("FileW", &attr) != -1)
			return LogError("stat for 'FileW' in child process failed");
		return 0;
	}
	else if (strncmp(argv[1], "-GetFileAttributes=", 6) == 0)
	{
		const char* str = argv[1] + 19;
		struct stat attr;
		if (stat(str, &attr) == -1)
			return 255;
		return attr.st_mode;
	}
	else if (strcmp(argv[1], "-popen") == 0)
	{
		FILE* file = popen("xdg-user-dir DOCUMENTS", "r");
		if (!file)
			return -3;
		char docPath[256];
		if (!fgets(docPath, 256, file))
			return -4;
		auto docLen = strlen(docPath) - 1;
		if (docLen <= 0)
			return -5;
		if (docPath[docLen] != '\n')
			return -6;
		pclose(file);
		return 0;
	}
	else if (strncmp(argv[1], "-file=", 6) == 0)
	{
		void* detoursHandle = dlopen(UBA_DETOURS_LIBRARY, RTLD_LAZY);
		if (!detoursHandle)
			return -3;
		using UbaRequestNextProcessFunc = bool(unsigned int prevExitCode, char* outArguments, unsigned int outArgumentsCapacity);
		static UbaRequestNextProcessFunc* requestNextProcess = (UbaRequestNextProcessFunc*)(void*)dlsym(detoursHandle, "UbaRequestNextProcess");
		if (!requestNextProcess)
			return -8;

		char arguments[1024];
		const char* file = argv[1] + 6;
		while (true)
		{
			int rh = open(file, O_RDONLY);
			if (rh == -1)
				return LogError("Failed to open file %s", file);
			if (close(rh) == -1)
				return LogError("Failed to close file %s", file);

			srand(getpid());
			int milliseconds = rand() % 2000;
			struct timespec ts;
			ts.tv_sec = milliseconds / 1000;
			ts.tv_nsec = (milliseconds % 1000) * 1000000;
			nanosleep(&ts, NULL);

			char outFile[1024];
			strcpy(outFile, file);
			outFile[strlen(file)-3] = 0;
			strcat(outFile, ".out");

			int wh = open(outFile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
			if (wh == -1)
				return LogError("Failed to create file %s", file);
			if (close(wh) == -1)
				return LogError("Failed to close created file %s", file);

			// Request new process
			if (!requestNextProcess(0, arguments, 1024))
				break; // No process available, exit loop
			file = arguments + 6;
		}

		return 0;
	}
	return -2;
}
