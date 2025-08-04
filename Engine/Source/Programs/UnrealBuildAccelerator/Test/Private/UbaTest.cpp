// Copyright Epic Games, Inc. All Rights Reserved.

#include "UbaTestAll.h"

namespace uba
{
	bool WrappedMain(int argc, tchar* argv[])
	{
		AddExceptionHandler();
		return RunTests(argc, argv);
	}
}

#if PLATFORM_WINDOWS
int wmain(int argc, wchar_t* argv[])
{
	return uba::WrappedMain(argc, argv) ? 0 : -1;
}
#else
int main(int argc, char* argv[])
{
	return uba::WrappedMain(argc, argv) ? 0 : -1;
}
#endif
