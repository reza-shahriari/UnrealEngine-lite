// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using EpicGames.Core;
using Microsoft.Extensions.Logging;
using UnrealBuildBase;

namespace UnrealBuildTool
{
	class AndroidToolChain : ClangToolChain, IAndroidToolChain
	{
		// Minimum NDK API level to allow
		public const int MinimumNDKAPILevel = 26;

		// this is architectures with the dash, which we match in filenames that have inlined arch name
		public static readonly Dictionary<UnrealArch, string> AllCpuSuffixes = new()
		{
			{ UnrealArch.Arm64, "-arm64" },
			{ UnrealArch.X64,   "-x64" },
		};

		// short names for the above suffixes
		public static readonly Dictionary<string, string> ShortArchNames = new Dictionary<string, string>()
		{
			{ "", "" },
			{ "-arm64", "a8" },
			{ "-x64", "x6" },
		};

		public enum ClangSanitizer
		{
			None,
			Address,
			HwAddress,
			UndefinedBehavior,
			UndefinedBehaviorMinimal,
			Thread,
		};

		public static string GetCompilerOption(ClangSanitizer Sanitizer)
		{
			switch (Sanitizer)
			{
				case ClangSanitizer.Address: return "address";
				case ClangSanitizer.HwAddress: return "hwaddress";
				case ClangSanitizer.UndefinedBehavior:
				case ClangSanitizer.UndefinedBehaviorMinimal: return "undefined";
				case ClangSanitizer.Thread: return "thread";
				default: return "";
			}
		}

		// Version string from the Android specific build of clang. E.g in Android (6317467 based on r365631c1) clang version 9.0.8
		// this would be 6317467)
		protected static string? AndroidClangBuild;

		// the "-android" suffix paths here are vcpkg triplets for the android platform
		private static Dictionary<UnrealArch, string[]> AllArchNames = new() {
			{ UnrealArch.Arm64, new string[] { "arm64", "arm64-v8a", "arm64-android" } },
			{ UnrealArch.X64,   new string[] { "x64", "x86_64", "x64-android" } }
		};

		// architecture paths to use for filtering include and lib paths
		private static Dictionary<UnrealArch, string[]> AllFilterArchNames = new() {
			{ UnrealArch.Arm64, new string[] { "arm64", "arm64-v8a", "arm64-android" } },
			{ UnrealArch.X64,   new string[] { "x64", "x86_64", "x64-android" } },
			// using Default as a placeholder to remove old folders for arches we no longer support, but licensees may have in their Build rules that we need to strip out
			{ UnrealArch.Deprecated  , new string[] { "armv7", "armeabi-v7a", "arm-android", "x86", "x86-android" } }
		};

		private static Dictionary<UnrealArch, string[]> LibrariesToSkip = new() {
			{ UnrealArch.Arm64, new string[] { "nvToolsExt", "nvToolsExtStub", "vorbisenc", } },
			{ UnrealArch.X64,   new string[] { "nvToolsExt", "nvToolsExtStub", "oculus", "OVRPlugin", "vrapi", "ovrkernel", "systemutils", "openglloader", "ovrplatformloader", "vorbisenc", } }
		};

		private static Dictionary<UnrealArch, string[]> ModulesToSkip = new() {
			{ UnrealArch.Arm64, Array.Empty<string>() },
			{ UnrealArch.X64,   new string[] { "OnlineSubsystemOculus", "OculusHMD", "OculusMR" } }
		};

		private static Dictionary<UnrealArch, string[]> GeneratedModulesToSkip = new() {
			{ UnrealArch.Arm64, Array.Empty<string>() },
			{ UnrealArch.X64,   new string[] { "OculusEntitlementCallbackProxy", "OculusCreateSessionCallbackProxy", "OculusFindSessionsCallbackProxy", "OculusIdentityCallbackProxy", "OculusNetConnection", "OculusNetDriver", "OnlineSubsystemOculus_init" } }
		};

		public string? NDKToolchainVersion;
		public ulong NDKVersionInt;

		int ClangVersionMajor = -1;
		int ClangVersionMinor = -1;
		int ClangVersionPatch = -1;

		protected void SetClangVersion(int Major, int Minor, int Patch)
		{
			ClangVersionMajor = Major;
			ClangVersionMinor = Minor;
			ClangVersionPatch = Patch;
		}

		public string GetClangVersionString()
		{
			return String.Format("{0}.{1}.{2}", ClangVersionMajor, ClangVersionMinor, ClangVersionPatch);
		}

		public bool IsNewNDKModel()
		{
			// Google changed NDK structure in r22+
			return NDKVersionInt >= 220000;
		}

		public bool HasEmbeddedHWASanSupport()
		{
			return NDKVersionInt >= 260000;
		}

		public AndroidToolChain(FileReference? InProjectFile, ILogger InLogger)
			: this(InProjectFile, ClangToolChainOptions.None, InLogger)
		{
		}

		DirectoryReference GCCToolchainPath;
		DirectoryReference SysrootPath;

		public AndroidToolChain(FileReference? InProjectFile, ClangToolChainOptions ToolchainOptions, ILogger InLogger)
			: base(ToolchainOptions, InLogger)
		{
			Options = ToolchainOptions;
			ProjectFile = InProjectFile;

			string? NDKPath = AndroidPlatformSDK.GetNDKRoot();

			// don't register if we don't have an NDKROOT specified
			if (String.IsNullOrEmpty(NDKPath))
			{
				throw new BuildException("NDKROOT is not specified; cannot use Android toolchain.");
			}

			NDKPath = NDKPath.Replace("\"", "");

			string ArchitecturePath;
			string ArchitecturePathWindows32 = @"prebuilt/windows";
			string ArchitecturePathWindows64 = @"prebuilt/windows-x86_64";
			string ArchitecturePathMac = @"prebuilt/darwin-x86_64";
			string ArchitecturePathLinux = @"prebuilt/linux-x86_64";
			string ExeExtension = ".exe";

			if (Directory.Exists(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePathWindows64)))
			{
				Logger.LogDebug("        Found Windows 64 bit versions of toolchain");
				ArchitecturePath = ArchitecturePathWindows64;
			}
			else if (Directory.Exists(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePathWindows32)))
			{
				Logger.LogDebug("        Found Windows 32 bit versions of toolchain");
				ArchitecturePath = ArchitecturePathWindows32;
			}
			else if (Directory.Exists(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePathMac)))
			{
				Logger.LogDebug("        Found Mac versions of toolchain");
				ArchitecturePath = ArchitecturePathMac;
				ExeExtension = "";
			}
			else if (Directory.Exists(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePathLinux)))
			{
				Logger.LogDebug("        Found Linux versions of toolchain");
				ArchitecturePath = ArchitecturePathLinux;
				ExeExtension = "";
			}
			else
			{
				throw new BuildException("Couldn't find 32-bit or 64-bit versions of the Android toolchain with NDKROOT: " + NDKPath);
			}

			// get the installed version (in the form r10e and 100500)
			UEBuildPlatformSDK SDK = UEBuildPlatform.GetSDK(UnrealTargetPlatform.Android)!;
			NDKToolchainVersion = SDK.GetInstalledVersion();
			SDK.TryConvertVersionToInt(NDKToolchainVersion, out NDKVersionInt);

			// figure out clang version (will live in toolchains/llvm from NDK 21 forward
			if (Directory.Exists(Path.Combine(NDKPath, @"toolchains/llvm")))
			{
				// look for version in AndroidVersion.txt (fail if not found)
				string VersionFilename = Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, "AndroidVersion.txt");
				if (!File.Exists(VersionFilename))
				{
					throw new BuildException("Cannot find supported Android toolchain");
				}
				string[] VersionFile = File.ReadAllLines(VersionFilename);
				string[] VersionParts = VersionFile[0].Split('.');
				SetClangVersion(Int32.Parse(VersionParts[0]), (VersionParts.Length > 1) ? Int32.Parse(VersionParts[1]) : 0, (VersionParts.Length > 2) ? Int32.Parse(VersionParts[2]) : 0);
			}
			else
			{
				throw new BuildException("Cannot find supported Android toolchain with NDKPath:" + NDKPath);
			}

			// set up the path to our toolchains
			ClangPath = Utils.CollapseRelativeDirectories(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, @"bin/clang++" + ExeExtension));

			// Android (6317467 based on r365631c1) clang version 9.0.8 
			string AndroidClangBuildTmp = Utils.RunLocalProcessAndReturnStdOut(ClangPath, "--version", Logger);
			try
			{
				AndroidClangBuild = Regex.Match(AndroidClangBuildTmp, @"(\w+) based on").Groups[1].ToString();
				if (String.IsNullOrEmpty(AndroidClangBuild))
				{
					AndroidClangBuild = Regex.Match(AndroidClangBuildTmp, @"(\w+), based on").Groups[1].ToString();
				}
			}
			catch
			{
				Logger.LogWarning("Failed to retreive build version from {AndroidClangBuild}", AndroidClangBuild);
				AndroidClangBuild = "unknown";
			}

			// use lld for r21+
			ArPathArm64 = Utils.CollapseRelativeDirectories(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, @"bin/llvm-ar" + ExeExtension));
			ArPathx64 = ArPathArm64;

			// NDK setup (enforce minimum API level)
			int NDKApiLevel64Int = GetNdkApiLevelInt(MinimumNDKAPILevel);

			GCCToolchainPath = DirectoryReference.FromString(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath))!;
			SysrootPath = DirectoryReference.FromString(Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, "sysroot").Replace("\\", "/"))!;

			// toolchain params (note: use ANDROID=1 same as we define it)
			ToolchainLinkParamsArm64 = " --target=aarch64-none-linux-android" + NDKApiLevel64Int + " -DANDROID=1";
			ToolchainLinkParamsx64 = " --target=x86_64-none-linux-android" + NDKApiLevel64Int + " -DANDROID=1";

			ToolchainParamsArm64 = ToolchainLinkParamsArm64;
			ToolchainParamsx64 = ToolchainLinkParamsx64;

			if (!IsNewNDKModel())
			{
				// We need to manually provide -D__ANDROID_API__ for NDK versions prior to r22 only, for newer ones, --target=aarch64-none-linux-android + NDKApiLevel64Int does it for us
				ToolchainParamsArm64 += " -D__ANDROID_API__=" + NDKApiLevel64Int;
				ToolchainParamsx64 += " -D__ANDROID_API__=" + NDKApiLevel64Int;
			}

			ReadElfPath = Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, @"bin/llvm-readelf" + ExeExtension);
			ObjDumpPath = Path.Combine(NDKPath, @"toolchains/llvm", ArchitecturePath, @"bin/llvm-objdump" + ExeExtension);
		}

		IEnumerable<string> AdditionalPaths(CppRootPaths Roots)
		{
			return new[]
			{
				$"--gcc-toolchain=\"{NormalizeCommandLinePath(GCCToolchainPath, Roots)}\"",
				$"--sysroot=\"{NormalizeCommandLinePath(SysrootPath, Roots)}\"",
			};
		}

		protected override ClangToolChainInfo GetToolChainInfo()
		{
			return new ClangToolChainInfo(DirectoryReference.FromString(AndroidPlatformSDK.GetNDKRoot()), FileReference.FromString(ClangPath)!, FileReference.FromString(ArPathArm64)!, Logger);
		}

		public static string GetGLESVersion(bool bBuildForES31)
		{
			string GLESversion = "0x00030000";

			if (bBuildForES31)
			{
				GLESversion = "0x00030002";
			}

			return GLESversion;
		}

		private bool BuildWithHiddenSymbolVisibility()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bBuildWithHiddenSymbolVisibility", out bool bBuild) && bBuild;
		}

		private bool CompressDebugSymbols()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bCompressDebugSymbols", out bool bBuild) && bBuild;
		}
		
		private bool DisableFunctionDataSections()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			bool bDisableFunctionDataSections = false;
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bDisableFunctionDataSections", out bDisableFunctionDataSections) && bDisableFunctionDataSections;
		}

		private bool EnableAdvancedBinaryCompression()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			bool bEnableAdvancedBinaryCompression = false;
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bEnableAdvancedBinaryCompression", out bEnableAdvancedBinaryCompression) && bEnableAdvancedBinaryCompression;
		}

		private bool DisableStackProtector()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			bool bDisableStackProtector = false;
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bDisableStackProtector", out bDisableStackProtector) && bDisableStackProtector;
		}

		private bool DisableLibCppSharedDependencyValidation()
		{
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			bool bDisableLibCppSharedDependencyValidation = false;
			return Ini.GetBool("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "bDisableLibCppSharedDependencyValidation", out bDisableLibCppSharedDependencyValidation) && bDisableLibCppSharedDependencyValidation;
		}

		private string GetVersionScriptFilename(LinkEnvironment LinkEnvironment)
		{
			return Path.Combine(LinkEnvironment.IntermediateDirectory!.FullName, "ExportSymbols.ldscript");
		}

		public int GetNdkApiLevelInt(int MinNdk = MinimumNDKAPILevel)
		{
			string NDKVersion = GetNdkApiLevel();
			int NDKVersionInt = MinNdk;
			if (NDKVersion.Contains('-'))
			{
				int Version;
				if (Int32.TryParse(NDKVersion.Substring(NDKVersion.LastIndexOf('-') + 1), out Version))
				{
					if (Version > NDKVersionInt)
					{
						NDKVersionInt = Version;
					}
				}
			}
			return NDKVersionInt;
		}

		public ulong GetNdkVersionInt()
		{
			return NDKVersionInt;
		}

		static string CachedPlatformsFilename = "";
		static bool CachedPlatformsValid = false;
		static int CachedMinPlatform = -1;
		static int CachedMaxPlatform = -1;

		private bool ReadMinMaxPlatforms(string PlatformsFilename, out int MinPlatform, out int MaxPlatform)
		{
			if (!CachedPlatformsFilename.Equals(PlatformsFilename))
			{
				// reset cache to defaults
				CachedPlatformsFilename = PlatformsFilename;
				CachedPlatformsValid = false;
				CachedMinPlatform = -1;
				CachedMaxPlatform = -1;

				// try to read it
				try
				{
					JsonObject? PlatformsObj = null;
					if (JsonObject.TryRead(new FileReference(PlatformsFilename), out PlatformsObj))
					{
						CachedPlatformsValid = PlatformsObj.TryGetIntegerField("min", out CachedMinPlatform) && PlatformsObj.TryGetIntegerField("max", out CachedMaxPlatform);
					}
				}
				catch (Exception)
				{
				}
			}

			MinPlatform = CachedMinPlatform;
			MaxPlatform = CachedMaxPlatform;
			return CachedPlatformsValid;
		}

		//This doesn't take into account SDK version overrides in packaging
		public int GetMinSdkVersion(int MinSdk = MinimumNDKAPILevel)
		{
			int MinSDKVersion = MinSdk;
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			Ini.GetInt32("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "MinSDKVersion", out MinSDKVersion);
			return MinSDKVersion;
		}

		protected virtual bool ValidateNDK(string PlatformsFilename, string ApiString)
		{
			int MinPlatform, MaxPlatform;
			if (!ReadMinMaxPlatforms(PlatformsFilename, out MinPlatform, out MaxPlatform))
			{
				return false;
			}

			if (ApiString.Contains('-'))
			{
				int Version;
				if (Int32.TryParse(ApiString.Substring(ApiString.LastIndexOf('-') + 1), out Version))
				{
					return (Version >= MinPlatform && Version <= MaxPlatform);
				}
			}
			return false;
		}

		public virtual string GetNdkApiLevel()
		{
			// ask the .ini system for what version to use
			ConfigHierarchy Ini = ConfigCache.ReadHierarchy(ConfigHierarchyType.Engine, DirectoryReference.FromFile(ProjectFile), UnrealTargetPlatform.Android);
			string NDKLevel;
			Ini.GetString("/Script/AndroidPlatformEditor.AndroidSDKSettings", "NDKAPILevel", out NDKLevel!);

			// check for project override of NDK API level
			string ProjectNDKLevel;
			Ini.GetString("/Script/AndroidRuntimeSettings.AndroidRuntimeSettings", "NDKAPILevelOverride", out ProjectNDKLevel!);
			ProjectNDKLevel = ProjectNDKLevel.Trim();
			if (!String.IsNullOrEmpty(ProjectNDKLevel))
			{
				NDKLevel = ProjectNDKLevel;
			}

			string PlatformsFilename = Environment.ExpandEnvironmentVariables("%NDKROOT%/meta/platforms.json");
			if (!File.Exists(PlatformsFilename))
			{
				throw new BuildException("No NDK platforms found in {0}", PlatformsFilename);
			}

			if (NDKLevel == "latest")
			{
				int MinPlatform, MaxPlatform;
				if (!ReadMinMaxPlatforms(PlatformsFilename, out MinPlatform, out MaxPlatform))
				{
					throw new BuildException("No NDK platforms found in {0}", PlatformsFilename);
				}

				NDKLevel = "android-" + MaxPlatform.ToString();
			}

			// validate the platform NDK is installed
			if (!ValidateNDK(PlatformsFilename, NDKLevel))
			{
				throw new BuildException("The NDK API requested '{0}' not installed in {1}", NDKLevel, PlatformsFilename);
			}

			return NDKLevel;
		}

		public string GetLargestApiLevel()
		{
			string PlatformsFilename = Environment.ExpandEnvironmentVariables("%NDKROOT%/meta/platforms.json");
			if (!File.Exists(PlatformsFilename))
			{
				throw new BuildException("No NDK platforms found in {0}", PlatformsFilename);
			}

			int MinPlatform, MaxPlatform;
			if (!ReadMinMaxPlatforms(PlatformsFilename, out MinPlatform, out MaxPlatform))
			{
				throw new BuildException("No NDK platforms found in {0}", PlatformsFilename);
			}

			return "android-" + MaxPlatform.ToString();
		}

		protected override void GetCompileArguments_IncludePaths(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			// remove paths that are not meant for this architecture
			// @todo can remove this when we only add paths properly for architecture
			IEnumerable<DirectoryReference> FilteredPaths = CompileEnvironment.UserIncludePaths.Where(x => IsDirectoryForArch(x.FullName, CompileEnvironment.Architecture));
			Arguments.AddRange(FilteredPaths.Select(IncludePath => GetUserIncludePathArgument(IncludePath, CompileEnvironment)));

			FilteredPaths = CompileEnvironment.SystemIncludePaths.Where(x => IsDirectoryForArch(x.FullName, CompileEnvironment.Architecture));
			Arguments.AddRange(FilteredPaths.Select(IncludePath => GetSystemIncludePathArgument(IncludePath, CompileEnvironment)));
		}

		/// <inheritdoc/>
		protected override string EscapePreprocessorDefinition(string Definition)
		{
			return Definition.Contains('"') ? Definition.Replace("\"", "\\\"") : Definition;
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_WarningsAndErrors(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_WarningsAndErrors(CompileEnvironment, Arguments);

			// @todo unlikely all needed
			Arguments.Add("-Wno-unknown-warning-option");   // Clang has been forked for Android - some warnings may not apply.
			Arguments.Add("-Wno-local-type-template-args"); // engine triggers this
			Arguments.Add("-Wno-return-type-c-linkage");    // needed for PhysX
			Arguments.Add("-Wno-reorder");                  // member initialization order
			Arguments.Add("-Wno-logical-op-parentheses");   // needed for external headers we can't change
			Arguments.Add("-Wno-nonportable-include-path"); // not all of these are real
			Arguments.Add("-Wno-extra-qualification");      // metasound DECLARE_METASOUND_xxx sometimes have redundant ::Metasound prefix
			Arguments.Add("-Wno-shorten-64-to-32");         // too many right now, same on other platforms 
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_Optimizations(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			// @todo the base has PGO options that are a little different than what we want - it would be nice to merge these

			// optimization level
			if (!CompileEnvironment.bOptimizeCode)
			{
				Arguments.Add("-O0");
			}
			else
			{
				if (CompileEnvironment.OptimizationLevel == OptimizationMode.Size)
				{
					Arguments.Add("-Oz");
				}
				else if (CompileEnvironment.OptimizationLevel == OptimizationMode.SizeAndSpeed)
				{
					Arguments.Add("-Os");
					if (CompileEnvironment.Architecture == UnrealArch.Arm64)
					{
						Arguments.Add("-moutline");
					}
				}
				else
				{
					Arguments.Add("-O3");
				}
			}

			if (CompileEnvironment.bAllowLTCG)
			{
				if ((Options & ClangToolChainOptions.EnableThinLTO) != 0)
				{
					Arguments.Add("-flto=thin");
				}
				else
				{
					Arguments.Add("-flto");
				}
			}

			// Profile Guided Optimization (PGO) and Link Time Optimization (LTO)
			if (CompileEnvironment.bPGOOptimize)
			{
				//
				// Clang emits warnings for each compiled object file that doesn't have a matching entry in the profile data.
				// This can happen when the profile data is older than the binaries we're compiling.
				//
				// Disable these warnings. They are far too verbose.
				//
				Arguments.Add("-Wno-profile-instr-out-of-date");
				Arguments.Add("-Wno-profile-instr-unprofiled");
				// apparently there can be hashing conflicts with PGO which can result in:
				// 'Function control flow change detected (hash mismatch)' warnings. 
				Arguments.Add("-Wno-backend-plugin");
				Arguments.Add(String.Format(" -fprofile-use=\"{0}.profdata\"", Path.Combine(CompileEnvironment.PGODirectory!, CompileEnvironment.PGOFilenamePrefix!).Replace("\\", "/")));
			}
			else if (CompileEnvironment.bPGOProfile)
			{
				// Android supports only LLVM IR-based for instrumentation-based PGO.
				Arguments.Add("-fprofile-generate");
				//  for sampling-based profile collection to generate minimal debug information:
				Arguments.Add("-gline-tables-only");
			}

			if (!CompileEnvironment.bUseInlining)
			{
				Arguments.Add("-fno-inline-functions");
			}

			Arguments.Add("-fdebug-types-section");
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_Architecture(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_Architecture(CompileEnvironment, Arguments);

			Arguments.AddRange(AdditionalPaths(CompileEnvironment.RootPaths));

			if (CompileEnvironment.Architecture == UnrealArch.Arm64)
			{
				Arguments.Add(ToolchainParamsArm64);
				Arguments.Add("-D__arm64__");            // for some reason this isn't defined and needed for PhysX
				Arguments.Add("-DPLATFORM_ANDROID_ARM64=1");
				Arguments.Add("-march=armv8-a");
			}
			else if (CompileEnvironment.Architecture == UnrealArch.X64)
			{
				Arguments.Add(ToolchainParamsx64);
				Arguments.Add("-DPLATFORM_ANDROID_X64=1");
				Arguments.Add("-fno-omit-frame-pointer");
				Arguments.Add("-march=atom");
			}
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_Debugging(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_Debugging(CompileEnvironment, Arguments);

			// debug info
			if (CompileEnvironment.bCreateDebugInfo)
			{
				Arguments.Add("-g2");
				Arguments.Add("-gdwarf-4");

				if (CompressDebugSymbols())
				{
					Arguments.Add("-gz");
				}

				if (CompileEnvironment.bDebugLineTablesOnly)
				{
					Arguments.Add("-gline-tables-only");
				}
			}

			if (!DisableStackProtector())
			{
				Arguments.Add("-fstack-protector-strong");  // Emits extra code to check for buffer overflows
			}

			// Add flags for on-device debugging
			if (CompileEnvironment.Configuration == CppConfiguration.Debug)
			{
				Arguments.Add("-fno-omit-frame-pointer");   // Disable removing the save/restore frame pointer for better debugging
				if (CompilerVersionGreaterOrEqual(3, 6, 0))
				{
					Arguments.Add(" -fno-function-sections");    // Improve breakpoint location
				}
			}

			// Some switches interfere with on-device debugging
			if (CompileEnvironment.Configuration != CppConfiguration.Debug && !DisableFunctionDataSections())
			{
				Arguments.Add("-ffunction-sections");   // Places each function in its own section of the output file, linker may be able to perform opts to improve locality of reference
				Arguments.Add("-fdata-sections");       // Places each data item in its own section of the output file, linker may be able to perform opts to improve locality of reference
			}
		}

		/// <inheritdoc/>
		protected override void GetCompilerArguments_Sanitizers(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			// TODO: Reconcile with base
			//base.GetCompilerArguments_Sanitizers(CompileEnvironment, Arguments);

			ClangSanitizer Sanitizer = BuildWithSanitizer();
			if (Sanitizer != ClangSanitizer.None)
			{
				Arguments.Add("-fsanitize=" + GetCompilerOption(Sanitizer));

				if (Sanitizer == ClangSanitizer.Address || Sanitizer == ClangSanitizer.HwAddress)
				{
					Arguments.Add("-fno-omit-frame-pointer");
				}
			}

			//string? SanitizerMode = Environment.GetEnvironmentVariable("ENABLE_ADDRESS_SANITIZER");
			//if ((SanitizerMode != null && SanitizerMode == "YES") || (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer)))
			//{
			//	Arguments.Add("-fsanitize=address -fno-omit-frame-pointer");
			//}

			//string? UndefSanitizerMode = Environment.GetEnvironmentVariable("ENABLE_UNDEFINED_BEHAVIOR_SANITIZER");
			//if ((UndefSanitizerMode != null && UndefSanitizerMode == "YES") || (Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer)))
			//{
			//	Arguments.Add("-fsanitize=undefined -fno-sanitize=bounds,enum,return,float-divide-by-zero");
			//}

			//if (Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer))
			//{
			//	Arguments.Add("-fsanitize=thread");
			//}

			//if (Options.HasFlag(ClangToolChainOptions.EnableMemorySanitizer))
			//{
			//	Arguments.Add("-fsanitize=memory");
			//}
		}

		/// <inheritdoc/>
		protected override void GetCompileArguments_Global(CppCompileEnvironment CompileEnvironment, List<string> Arguments)
		{
			base.GetCompileArguments_Global(CompileEnvironment, Arguments);

			// NDK setup (enforce minimum API level)
			int NDKApiLevel64Int = GetNdkApiLevelInt(21);       // deliberately use 21 minimum to force NDKApiLevel64Bit to update if below minimum
			string NDKApiLevel64Bit = GetNdkApiLevel();
			if (NDKApiLevel64Int < MinimumNDKAPILevel)
			{
				NDKApiLevel64Int = MinimumNDKAPILevel;
				NDKApiLevel64Bit = "android-" + MinimumNDKAPILevel;
			}

			Log.TraceInformationOnce("Compiling Native 64-bit code with NDK API '{0}'", NDKApiLevel64Bit);

			string NativeGluePath = Path.GetFullPath(GetNativeGluePath());

			if (BuildWithHiddenSymbolVisibility())
			{
				Arguments.Add("-fvisibility=hidden");
				Arguments.Add("-fvisibility-inlines-hidden");
				//TODO: add when Android's clang will support this
				//Arguments.Add("-fvisibility-inlines-hidden-static-local-var");
			}

			Arguments.Add(GetRTTIFlag(CompileEnvironment));
			Arguments.Add("-no-canonical-prefixes");
			Arguments.Add("-fno-PIE");
			Arguments.Add("-funwind-tables");           // Just generates any needed static data, affects no code
			Arguments.Add("-fPIC");                     // Generates position-independent code (PIC) suitable for use in a shared library
			Arguments.Add("-fno-strict-aliasing");      // Prevents unwanted or invalid optimizations that could produce incorrect code
			Arguments.Add("-fno-short-enums");          // Do not allocate to an enum type only as many bytes as it needs for the declared range of possible values
			Arguments.Add("-fforce-emit-vtables");      // Helps with devirtualization
			Arguments.Add("-D_FORTIFY_SOURCE=2");       // FORTIFY default
			Arguments.Add($"-DPLATFORM_USED_NDK_VERSION_INTEGER={NDKApiLevel64Int}");       // NDK version
			Arguments.Add("-DPLATFORM_64BITS=1");       // NDK version
		}

		/// <inheritdoc/>
		protected override FileItem GetCompileArguments_FileType(CppCompileEnvironment CompileEnvironment, FileItem SourceFile, DirectoryReference OutputDir, List<string> Arguments, Action CompileAction, CPPOutput CompileResult)
		{
			FileItem TargetFile = base.GetCompileArguments_FileType(CompileEnvironment, SourceFile, OutputDir, Arguments, CompileAction, CompileResult);

			string Extension = Path.GetExtension(SourceFile.AbsolutePath).ToUpperInvariant();
			if (Extension == ".C")
			{
				if (SourceFile.AbsolutePath.Equals(GetNativeGluePath()))
				{
					// Remove visibility settings for android native glue. Since it doesn't decorate with visibility attributes.
					Arguments.RemoveAll(x => x.StartsWith("-fvisibility"));
				}
				// remove any PCH includes - mostly for the force-added .c files in Launch as those will attempt to have the PCH used that was made with .cpp language
				Arguments.RemoveAll(x => x.StartsWith("-include-pch"));
			}

			return TargetFile;
		}

		protected virtual string GetLinkArguments(LinkEnvironment LinkEnvironment, UnrealArch Architecture)
		{
			string Result = string.Join(' ', AdditionalPaths(LinkEnvironment.RootPaths));

			//Result += " -nostdlib";
			Result += " -static-libstdc++";
			Result += " -no-canonical-prefixes";
			Result += " -shared";
			Result += " -Wl,-Bsymbolic";
			Result += " -Wl,--no-undefined";
			if (!DisableFunctionDataSections())
			{
				Result += " -Wl,--gc-sections"; // Enable garbage collection of unused input sections. works best with -ffunction-sections, -fdata-sections
			}

			if (!LinkEnvironment.bCreateDebugInfo)
			{
				Result += " -Wl,--strip-debug";
			}
			else if (CompressDebugSymbols())
			{
				Result += " -gz";
			}

			if (Architecture == UnrealArch.X64)
			{
				Result += ToolchainLinkParamsx64;
				Result += " -march=atom";
			}
			else // if (Architecture == UnrealArch.Arm64)
			{
				Result += ToolchainLinkParamsArm64;
				Result += " -march=armv8-a";
			}

			if (LinkEnvironment.Configuration == CppConfiguration.Shipping)
			{
				Result += " -Wl,--icf=all"; // Enables ICF (Identical Code Folding). [all, safe] safe == fold functions that can be proven not to have their address taken.
				Result += " -Wl,-O3";
			}

			Result += " -Wl,-no-pie";

			// use lld as linker (requires llvm-strip)
			Result += " -fuse-ld=lld";

			// make sure the DT_SONAME field is set properly (or we can a warning toast at startup on new Android)
			Result += " -Wl,-soname,libUnreal.so";

			{
				string VersionScriptFile = GetVersionScriptFilename(LinkEnvironment);
				using (StreamWriter Writer = File.CreateText(VersionScriptFile))
				{
					// Make all symbols hidden (except new/delete operators and ones called from Java)
					Writer.WriteLine("{ global: _Znwm*; _Znam*; _ZdlPv*; _ZdaPv*; Java_*; ANativeActivity_onCreate; JNI_OnLoad; local: *; };");
				}
				Result += " -Wl,--version-script=\"" + VersionScriptFile + "\"";
			}

			Result += " -Wl,--build-id=sha1";               // add build-id to make debugging easier

			if (LinkEnvironment.bPGOOptimize)
			{
				//
				// Clang emits warnings for each compiled object file that doesn't have a matching entry in the profile data.
				// This can happen when the profile data is older than the binaries we're compiling.
				//
				// Disable these warnings. They are far too verbose.
				//
				Result += " -Wno-profile-instr-out-of-date";
				Result += " -Wno-profile-instr-unprofiled";

				Result += String.Format(" -fprofile-use=\"{0}.profdata\"", Path.Combine(LinkEnvironment.PGODirectory!, LinkEnvironment.PGOFilenamePrefix!).Replace("\\", "/"));
			}
			else if (LinkEnvironment.bPGOProfile)
			{
				// Android supports only LLVM IR-based for instrumentation-based PGO.
				Result += " -fprofile-generate";
				//  for sampling-based profile collection to generate minimal debug information:
				Result += " -gline-tables-only";
			}

			if (LinkEnvironment.bAllowLTCG)
			{
				if ((Options & ClangToolChainOptions.EnableThinLTO) != 0)
				{
					Result += " -flto=thin";
					Result += " -Wl,--thinlto-jobs=all";
				}
				else
				{
					Result += " -flto";
				}
			}

			// verbose output from the linker
			// Result += " -v";

			ClangSanitizer Sanitizer = BuildWithSanitizer();
			if (Sanitizer != ClangSanitizer.None)
			{
				Result += " -fsanitize=" + GetCompilerOption(Sanitizer);
			}

			if (EnableAdvancedBinaryCompression())
			{
				int MinSDKVersion = GetMinSdkVersion();
				if (MinSDKVersion >= 28)
				{
					//Pack relocations in RELR format and Android APS2 packed format for RELA relocations if they can't be expressed in RELR
					Result += " -Wl,--pack-dyn-relocs=android+relr,--use-android-relr-tags";
				}
				else if (MinSDKVersion >= 23)
				{
					Result += " -Wl,--pack-dyn-relocs=android";
				}

				if (MinSDKVersion >= 23)
				{
					Result += " -Wl,--hash-style=gnu";  // generate GNU style hashes, faster lookup and faster startup. Avoids generating old .hash section. Supported on >= Android M
				}
			}

			// Enable support for non-4k virtual page sizes
			Result += " -z max-page-size=16384";

			return Result;
		}

		protected virtual void ModifyLibraries(LinkEnvironment LinkEnvironment)
		{
			// @todo Lumin: verify this works with base android
			if (GetNdkApiLevelInt() >= 21)
			{
				// this file was added in NDK11 so use existence to detect (RELEASE.TXT no longer present)
				//string NDKRoot = Environment.GetEnvironmentVariable("NDKROOT").Replace("\\", "/");
			}
		}

		static string GetArArguments(LinkEnvironment LinkEnvironment)
		{
			string Result = "";

			Result += " -r";

			return Result;
		}

		static bool IsDirectoryForArch(string Dir, UnrealArch Arch)
		{
			// make sure paths use one particular slash
			Dir = Dir.Replace("\\", "/").ToLowerInvariant();

			// look for other architectures in the Dir path, and fail if it finds it
			foreach (KeyValuePair<UnrealArch, string[]> Pair in AllFilterArchNames)
			{
				if (Pair.Key != Arch)
				{
					foreach (string ArchName in Pair.Value)
					{
						// if there's a directory in the path with a bad architecture name, reject it
						if (Regex.IsMatch(Dir, "/" + ArchName + "$") || Regex.IsMatch(Dir, "/" + ArchName + "/") || Regex.IsMatch(Dir, "/" + ArchName + "_API[0-9]+_NDK[0-9]+", RegexOptions.IgnoreCase))
						{
							return false;
						}
					}
				}
			}

			// if nothing was found, we are okay
			return true;
		}

		static bool ShouldSkipModule(string ModuleName, UnrealArch Arch)
		{
			foreach (string ModName in ModulesToSkip[Arch])
			{
				if (ModName == ModuleName)
				{
					return true;
				}
			}

			// if nothing was found, we are okay
			return false;
		}

		bool ShouldSkipLib(string FullLib, UnrealArch Arch)
		{
			// strip any absolute path
			string Lib = Path.GetFileNameWithoutExtension(FullLib);
			if (Lib.StartsWith("lib"))
			{
				Lib = Lib.Substring(3);
			}

			// reject any libs we outright don't want to link with
			foreach (string LibName in LibrariesToSkip[Arch])
			{
				if (LibName == Lib)
				{
					return true;
				}
			}

			// deal with .so files with wrong architecture
			if (Path.GetExtension(FullLib) == ".so")
			{
				string ParentDirectory = Path.GetDirectoryName(FullLib)!;
				if (!IsDirectoryForArch(ParentDirectory, Arch))
				{
					return true;
				}
			}

			// apply the same directory filtering to libraries as we do to additional library paths
			if (!IsDirectoryForArch(Path.GetDirectoryName(FullLib)!, Arch))
			{
				return true;
			}

			// if another architecture is in the filename, reject it
			foreach (KeyValuePair<UnrealArch, string> ComboName in AllCpuSuffixes)
			{
				if (ComboName.Key != Arch)
				{
					string ArchitectureName = ComboName.Key.ToString();
					if (Lib.EndsWith(ComboName.Value) || Lib.EndsWith(ArchitectureName))
					{
						return true;
					}
				}
			}

			// if nothing was found, we are okay
			return false;
		}

		private static string GetNativeGluePath()
		{
			return Environment.GetEnvironmentVariable("NDKROOT") + "/sources/android/native_app_glue/android_native_app_glue.c";
		}

		private static string GetCpuFeaturesPath()
		{
			return Environment.GetEnvironmentVariable("NDKROOT") + "/sources/android/cpufeatures/cpu-features.c";
		}

		public override CppCompileEnvironment CreateSharedResponseFile(CppCompileEnvironment CompileEnvironment, FileReference OutResponseFile, IActionGraphBuilder Graph)
		{
			// Seems like Android clang toolchain does not handle response files including response files
			return CompileEnvironment;
		}

		void GenerateEmptyLinkFunctionsForRemovedModules(List<FileItem> SourceFiles, UnrealArch Arch, string ModuleName, DirectoryReference OutputDirectory, IActionGraphBuilder Graph, ILogger Logger)
		{
			// Only add to Launch module
			if (!ModuleName.Equals("Launch"))
			{
				return;
			}

			string LinkerExceptionsName = "../UELinkerExceptions";
			FileReference LinkerExceptionsCPPFilename = FileReference.Combine(OutputDirectory, LinkerExceptionsName + ".cpp");

			List<string> Result = new List<string>();
			Result.Add("#include \"CoreTypes.h\"");
			Result.Add("");
			if (Arch == UnrealArch.X64)
			{
				Result.Add("#if PLATFORM_ANDROID_X64");
			}
			else
			{
				Result.Add("#if PLATFORM_ANDROID_ARM64");
			}

			foreach (string ModName in ModulesToSkip[Arch])
			{
				Result.Add("  void EmptyLinkFunctionForStaticInitialization" + ModName + "(){}");
			}
			foreach (string ModName in GeneratedModulesToSkip[Arch])
			{
				Result.Add("  void EmptyLinkFunctionForGeneratedCode" + ModName + "(){}");
			}
			Result.Add("#endif");

			Graph.CreateIntermediateTextFile(LinkerExceptionsCPPFilename, Result);

			SourceFiles.Add(FileItem.GetItemByFileReference(LinkerExceptionsCPPFilename));
		}

		// cache the location of NDK tools
		protected static string? ClangPath;
		protected static string ToolchainParamsArm64 = "";
		protected static string ToolchainParamsx64 = "";
		protected static string ToolchainLinkParamsArm64 = "";
		protected static string ToolchainLinkParamsx64 = "";
		protected static string? ArPathArm64;
		protected static string? ArPathx64;
		protected static string? ReadElfPath;
		protected static string? ObjDumpPath;

		public static string GetStripExecutablePath(UnrealArch UnrealArch)
		{
			string StripPath = ArPathArm64!;
			if (UnrealArch == UnrealArch.X64)
			{
				StripPath = ArPathx64!;
			}
			return StripPath.Replace("-ar", "-strip");
		}

		public static string GetReadElfExecutablePath(UnrealArch UnrealArch)
		{
			return ReadElfPath!;
		}

		public static string GetObjDumpExecutablePath(UnrealArch UnrealArch)
		{
			return ObjDumpPath!;
		}

		private HashSet<UnrealArch> HasHandledLaunchModule = new();
		private HashSet<UnrealArch> HasHandledCoreModule = new();

		protected override CPPOutput CompileCPPFiles(CppCompileEnvironment CompileEnvironment, IEnumerable<FileItem> InputFiles, DirectoryReference OutputDir, string ModuleName, IActionGraphBuilder Graph)
		{
			if (ShouldSkipCompile(CompileEnvironment) || ShouldSkipModule(ModuleName, CompileEnvironment.Architecture))
			{
				return new CPPOutput();
			}

			List<FileItem> ModifiedInputFiles = new(InputFiles);

			// Deal with Launch module special if first time seen
			if (!HasHandledLaunchModule.Contains(CompileEnvironment.Architecture) && (ModuleName.Equals("Launch") || ModuleName.Equals("AndroidLauncher")))
			{
				// Directly added NDK files for NDK extensions
				ModifiedInputFiles.Add(FileItem.GetItemByPath(GetNativeGluePath()));
				// Deal with dynamic modules removed by architecture
				GenerateEmptyLinkFunctionsForRemovedModules(ModifiedInputFiles, CompileEnvironment.Architecture, ModuleName, OutputDir, Graph, Logger);

				HasHandledLaunchModule.Add(CompileEnvironment.Architecture);
			}

			if (!HasHandledCoreModule.Contains(CompileEnvironment.Architecture) && ModuleName.Equals("Core") && (CompileEnvironment.PrecompiledHeaderAction == PrecompiledHeaderAction.None))
			{
				// This is used by Crypto code in Core
				ModifiedInputFiles.Add(FileItem.GetItemByPath(GetCpuFeaturesPath()));
				HasHandledCoreModule.Add(CompileEnvironment.Architecture);

			}

			return base.CompileCPPFiles(CompileEnvironment, ModifiedInputFiles, OutputDir, ModuleName, Graph);
		}

		public static string InlineArchName(string Pathname, UnrealArch Arch, bool bUseShortNames = false)
		{
			string FinalArch = "-" + Arch.ToString().ToLower();
			if (bUseShortNames)
			{
				FinalArch = ShortArchNames[FinalArch];
			}
			return Path.Combine(Path.GetDirectoryName(Pathname)!, Path.GetFileNameWithoutExtension(Pathname) + FinalArch + Path.GetExtension(Pathname));
		}

		public string RemoveArchName(string Pathname)
		{
			// remove all architecture names
			foreach (string Arch in AllCpuSuffixes.Values)
			{
				Pathname = Path.Combine(Path.GetDirectoryName(Pathname)!, Path.GetFileName(Pathname).Replace(Arch, ""));
			}
			return Pathname;
		}

		public static DirectoryReference InlineArchIncludeFolder(DirectoryReference PathRef, UnrealArch Arch)
		{
			return DirectoryReference.Combine(PathRef, "include", Arch.ToString());
		}

		public override FileItem? LinkFiles(LinkEnvironment LinkEnvironment, bool bBuildImportLibraryOnly, IActionGraphBuilder Graph)
		{
			if (!LinkEnvironment.bIsBuildingLibrary)
			{
				// @todo Lumin: will this add them multiple times?
				ModifyLibraries(LinkEnvironment);
			}

			UnrealArch Arch = LinkEnvironment.Architecture;

			// Create an action that invokes the linker.
			Action LinkAction = Graph.CreateAction(ActionType.Link);
			LinkAction.RootPaths = LinkEnvironment.RootPaths;
			LinkAction.WorkingDirectory = Unreal.EngineSourceDirectory;

			if (LinkEnvironment.bIsBuildingLibrary)
			{
				if (Arch == UnrealArch.Arm64)
				{
					LinkAction.CommandPath = new FileReference(ArPathArm64!);
				}
				else
				{
					LinkAction.CommandPath = new FileReference(ArPathx64!);
				}
			}
			else
			{
				LinkAction.CommandPath = new FileReference(ClangPath!);
			}

			DirectoryReference LinkerPath = LinkAction.WorkingDirectory;

			LinkAction.WorkingDirectory = LinkEnvironment.IntermediateDirectory!;

			// Get link arguments.
			LinkAction.CommandArguments = LinkEnvironment.bIsBuildingLibrary ? GetArArguments(LinkEnvironment) : GetLinkArguments(LinkEnvironment, Arch);

			// Add the output file as a production of the link action.
			FileItem OutputFile;
			OutputFile = FileItem.GetItemByFileReference(LinkEnvironment.OutputFilePaths.Where(x => x.FullName.Contains(LinkEnvironment.Architecture.ToString(), StringComparison.InvariantCultureIgnoreCase)).First());
			LinkAction.ProducedItems.Add(OutputFile);
			LinkAction.StatusDescription = String.Format("{0}", Path.GetFileName(OutputFile.AbsolutePath));
			LinkAction.CommandVersion = AndroidClangBuild!;

			// LinkAction.bPrintDebugInfo = true;

			// Add the output file to the command-line.
			if (LinkEnvironment.bIsBuildingLibrary)
			{
				LinkAction.CommandArguments += String.Format(" \"{0}\"", OutputFile.AbsolutePath);
			}
			else
			{
				LinkAction.CommandArguments += String.Format(" -o \"{0}\"", OutputFile.AbsolutePath);
			}

			if (LinkEnvironment.bAllowLTCG && Options.HasFlag(ClangToolChainOptions.EnableThinLTO))
			{
				// Set the weight to number of logical cores as lld can max out the available cores
				LinkAction.Weight = Utils.GetLogicalProcessorCount();

				// Disallow remote to prevent this long running action from running on an agent if remote linking is enabled
				LinkAction.bCanExecuteRemotely = false;
			}

			// Add the input files to a response file, and pass the response file on the command-line.
			List<string> InputFileNames = new List<string>();
			foreach (FileItem InputFile in LinkEnvironment.InputFiles)
			{
				string InputPath;
				if (InputFile.Location.IsUnderDirectory(LinkEnvironment.IntermediateDirectory!))
				{
					InputPath = InputFile.Location.MakeRelativeTo(LinkEnvironment.IntermediateDirectory!);
				}
				else
				{
					InputPath = InputFile.Location.FullName;
				}
				InputFileNames.Add(String.Format("\"{0}\"", InputPath.Replace('\\', '/')));

				LinkAction.PrerequisiteItems.Add(InputFile);
			}

			string LinkResponseArguments = "";

			// libs don't link in other libs
			if (!LinkEnvironment.bIsBuildingLibrary)
			{
				// Make a list of library paths to search
				List<string> AdditionalLibraryPaths = new List<string>();
				List<string> AdditionalLibraries = new List<string>();

				// Add the library paths to the additional path list
				foreach (DirectoryReference LibraryPath in LinkEnvironment.SystemLibraryPaths)
				{
					// LinkerPaths could be relative or absolute
					string AbsoluteLibraryPath = Utils.ExpandVariables(LibraryPath.FullName);
					if (IsDirectoryForArch(AbsoluteLibraryPath, Arch))
					{
						// environment variables aren't expanded when using the $( style
						if (Path.IsPathRooted(AbsoluteLibraryPath) == false)
						{
							AbsoluteLibraryPath = Path.Combine(LinkerPath.FullName, AbsoluteLibraryPath);
						}
						AbsoluteLibraryPath = Utils.CollapseRelativeDirectories(AbsoluteLibraryPath);
						if (!AdditionalLibraryPaths.Contains(AbsoluteLibraryPath))
						{
							AdditionalLibraryPaths.Add(AbsoluteLibraryPath);
						}
					}
				}

				// discover additional libraries and their paths
				foreach (string SystemLibrary in LinkEnvironment.SystemLibraries)
				{
					if (!ShouldSkipLib(SystemLibrary, Arch))
					{
						if (String.IsNullOrEmpty(Path.GetDirectoryName(SystemLibrary)))
						{
							if (SystemLibrary.StartsWith("lib"))
							{
								AdditionalLibraries.Add(SystemLibrary);
							}
							else
							{
								AdditionalLibraries.Add("lib" + SystemLibrary);
							}
						}
					}
				}
				foreach (FileReference Library in LinkEnvironment.Libraries)
				{
					if (!ShouldSkipLib(Library.FullName, Arch))
					{
						string AbsoluteLibraryPath = Path.GetDirectoryName(Library.FullName)!;
						LinkAction.PrerequisiteItems.Add(FileItem.GetItemByFileReference(Library));

						string Lib = Path.GetFileNameWithoutExtension(Library.FullName);
						if (Lib.StartsWith("lib"))
						{
							AdditionalLibraries.Add(Lib);
							if (!AdditionalLibraryPaths.Contains(AbsoluteLibraryPath))
							{
								AdditionalLibraryPaths.Add(AbsoluteLibraryPath);
							}
						}
						else
						{
							AdditionalLibraries.Add(AbsoluteLibraryPath);
						}

						if (!DisableLibCppSharedDependencyValidation() && ReadElfPath != null)
						{
							string? Output = Utils.RunLocalProcessAndReturnStdOut(ReadElfPath, "--dynamic \"" + Library.FullName + "\"");
							if (Output != null)
							{
								if (Output.Contains("libc++_shared.so"))
								{
									if (IsNewNDKModel())
									{
										throw new BuildException("Lib {0} depends on libc++_shared.so. There are known incompatibility issues when linking libc++_shared.so with Unreal Engine built with NDK22+." +
											" Please rebuild your dependencies with static libc++!", Lib);
									}
									else
									{
										Logger.LogWarning("Lib {LibName} depends on libc++_shared.so. Unreal Engine is designed to be linked with libs that are built against static libc++ only. Please rebuild your dependencies with static libc++!", Lib);
									}
								}
							}
						}
					}
				}

				// add the library paths to response
				foreach (string LibaryPath in AdditionalLibraryPaths)
				{
					LinkResponseArguments += String.Format(" -L\"{0}\"", LibaryPath);
				}

				// add libraries in a library group
				LinkResponseArguments += String.Format(" -Wl,--start-group");
				foreach (string AdditionalLibrary in AdditionalLibraries)
				{
					if (AdditionalLibrary.StartsWith("lib"))
					{
						LinkResponseArguments += String.Format(" \"-l{0}\"", AdditionalLibrary.Substring(3));
					}
					else
					{
						LinkResponseArguments += String.Format(" \"{0}\"", AdditionalLibrary);
					}
				}
				LinkResponseArguments += String.Format(" -Wl,--end-group");

				// Write the MAP file to the output directory.
				if (LinkEnvironment.bCreateMapFile)
				{
					FileReference MAPFilePath = FileReference.Combine(LinkEnvironment.OutputDirectory!, Path.GetFileNameWithoutExtension(OutputFile.AbsolutePath) + ".map");
					FileItem MAPFile = FileItem.GetItemByFileReference(MAPFilePath);
					LinkResponseArguments += String.Format(" -Wl,--cref -Wl,-Map,\"{0}\"", MAPFilePath);
					LinkAction.ProducedItems.Add(MAPFile);

					// Export a list of object file paths, so we can locate the object files referenced by the map file
					ExportObjectFilePaths(LinkEnvironment, Path.ChangeExtension(MAPFilePath.FullName, ".objpaths"));
				}
			}

			// Add the additional arguments specified by the environment.
			LinkResponseArguments += LinkEnvironment.AdditionalArguments;

			// Write out a response file
			FileReference ResponseFileName = GetResponseFileName(LinkEnvironment, OutputFile);
			InputFileNames.Add(LinkResponseArguments.Replace("\\", "/"));

			FileItem ResponseFileItem = Graph.CreateIntermediateTextFile(ResponseFileName, InputFileNames);

			LinkAction.CommandArguments += String.Format(" @\"{0}\"", ResponseFileName);
			LinkAction.PrerequisiteItems.Add(ResponseFileItem);

			// Fix up the paths in commandline
			LinkAction.CommandArguments = LinkAction.CommandArguments.Replace("\\", "/");

			// Only execute linking on the local PC.
			LinkAction.bCanExecuteRemotely = false;

			string VersionScriptFileItem = GetVersionScriptFilename(LinkEnvironment);
			LinkAction.PrerequisiteItems.Add(FileItem.GetItemByPath(VersionScriptFileItem));

			Logger.LogInformation("Link: {LinkActionCommandPathFullName} {LinkActionCommandArguments}", LinkAction.CommandPath.FullName, LinkAction.CommandArguments);

			// Windows can run into an issue with too long of a commandline when clang tries to call ld to link.
			// To work around this we call clang to just get the command it would execute and generate a
			// second response file to directly call ld with the right arguments instead of calling through clang.
			/* disable while tracking down some linker errors this introduces
						if (OperatingSystem.IsWindows())
						{
							// capture the actual link command without running it
							ProcessStartInfo StartInfo = new ProcessStartInfo();
							StartInfo.WorkingDirectory = LinkEnvironment.IntermediateDirectory.FullName;
							StartInfo.FileName = LinkAction.CommandPath;
							StartInfo.Arguments = "-### " + LinkAction.CommandArguments;
							StartInfo.UseShellExecute = false;
							StartInfo.CreateNoWindow = true;
							StartInfo.RedirectStandardError = true;

							LinkerCommandline = "";

							Process Proc = new Process();
							Proc.StartInfo = StartInfo;
							Proc.ErrorDataReceived += new DataReceivedEventHandler(OutputReceivedForLinker);
							Proc.Start();
							Proc.BeginErrorReadLine();
							Proc.WaitForExit(5000);

							LinkerCommandline = LinkerCommandline.Trim();

							// the command should be in quotes; if not we'll just use clang to link as usual
							int FirstQuoteIndex = LinkerCommandline.IndexOf('"');
							if (FirstQuoteIndex >= 0)
							{
								int SecondQuoteIndex = LinkerCommandline.Substring(FirstQuoteIndex + 1).IndexOf('"');
								if (SecondQuoteIndex >= 0)
								{
									LinkAction.CommandPath = LinkerCommandline.Substring(FirstQuoteIndex + 1, SecondQuoteIndex - FirstQuoteIndex);
									LinkAction.CommandArguments = LinkerCommandline.Substring(FirstQuoteIndex + SecondQuoteIndex + 3);

									// replace double backslashes
									LinkAction.CommandPath = LinkAction.CommandPath.Replace("\\\\", "/");

									// now create a response file for the full command using ld directly
									FileReference FinalResponseFileName = FileReference.Combine(LinkEnvironment.IntermediateDirectory, OutputFile.Location.GetFileName() + ".responseFinal");
									FileItem FinalResponseFileItem = Graph.CreateIntermediateTextFile(FinalResponseFileName, LinkAction.CommandArguments);
									LinkAction.CommandArguments = string.Format("@\"{0}\"", FinalResponseFileName);
									LinkAction.PrerequisiteItems.Add(FinalResponseFileItem);
								}
							}
						}
			*/

			return OutputFile;
		}

		// captures stderr from clang
		private static string LinkerCommandline = "";
		public static void OutputReceivedForLinker(object Sender, DataReceivedEventArgs Line)
		{
			if ((Line != null) && (Line.Data != null) && (Line.Data.Contains("--sysroot")))
			{
				LinkerCommandline += Line.Data;
			}
		}

		private void ExportObjectFilePaths(LinkEnvironment LinkEnvironment, string FileName)
		{
			// Write the list of object file directories
			HashSet<DirectoryReference> ObjectFileDirectories = new HashSet<DirectoryReference>();
			foreach (FileItem InputFile in LinkEnvironment.InputFiles)
			{
				ObjectFileDirectories.Add(InputFile.Location.Directory);
			}
			foreach (FileReference Library in LinkEnvironment.Libraries)
			{
				ObjectFileDirectories.Add(Library.Directory);
			}
			foreach (DirectoryReference LibraryPath in LinkEnvironment.SystemLibraryPaths)
			{
				ObjectFileDirectories.Add(LibraryPath);
			}
			foreach (string LibraryPath in (Environment.GetEnvironmentVariable("LIB") ?? "").Split(new char[] { ';' }, StringSplitOptions.RemoveEmptyEntries))
			{
				ObjectFileDirectories.Add(new DirectoryReference(LibraryPath));
			}
			Directory.CreateDirectory(Path.GetDirectoryName(FileName)!);
			File.WriteAllLines(FileName, ObjectFileDirectories.Select(x => x.FullName).OrderBy(x => x).ToArray());
		}

		public override void ModifyBuildProducts(ReadOnlyTargetRules Target, UEBuildBinary Binary, IEnumerable<string> Libraries, IEnumerable<UEBuildBundleResource> BundleResources, Dictionary<FileReference, BuildProductType> BuildProducts)
		{
			// only the .so needs to be in the manifest; we always have to build the apk since its contents depend on the project

			/*
			// the binary will have all of the .so's in the output files, we need to trim down to the shared apk (which is what needs to go into the manifest)
			if (Target.bDeployAfterCompile && Binary.Config.Type != UEBuildBinaryType.StaticLibrary)
			{
				foreach (FileReference BinaryPath in Binary.Config.OutputFilePaths)
				{
					FileReference ApkFile = BinaryPath.ChangeExtension(".apk");
					BuildProducts.Add(ApkFile, BuildProductType.Package);
				}
			}
			*/
		}

		public static void OutputReceivedDataEventHandler(object Sender, DataReceivedEventArgs Line, ILogger Logger)
		{
			if ((Line != null) && (Line.Data != null))
			{
				Logger.LogInformation("{Output}", Line.Data);
			}
		}

		public virtual string GetStripPath(FileReference SourceFile)
		{
			string StripExe;
			if (SourceFile.FullName.Contains("-arm64"))
			{
				StripExe = ArPathArm64!;
			}
			else
			if (SourceFile.FullName.Contains("-x64"))
			{
				StripExe = ArPathx64!;
			}
			else
			{
				throw new BuildException("Couldn't determine Android architecture to strip symbols from {0}", SourceFile.FullName);
			}

			// fix the executable (replace the last -ar with -strip and keep any extension)
			int ArIndex = StripExe.LastIndexOf("-ar");
			StripExe = StripExe.Substring(0, ArIndex) + "-strip" + StripExe.Substring(ArIndex + 3);
			return StripExe;
		}

		public void StripSymbols(FileReference SourceFile, FileReference TargetFile, ILogger Logger)
		{
			if (SourceFile != TargetFile)
			{
				// Strip command only works in place so we need to copy original if target is different
				File.Copy(SourceFile.FullName, TargetFile.FullName, true);
			}

			ProcessStartInfo StartInfo = new ProcessStartInfo();
			StartInfo.FileName = GetStripPath(SourceFile).Trim('"');
			StartInfo.Arguments = " --strip-debug \"" + TargetFile.FullName + "\"";
			StartInfo.UseShellExecute = false;
			StartInfo.CreateNoWindow = true;
			Utils.RunLocalProcessAndLogOutput(StartInfo, Logger);
		}

		public ClangSanitizer BuildWithSanitizer()
		{
			if (Options.HasFlag(ClangToolChainOptions.EnableAddressSanitizer))
			{
				return ClangSanitizer.Address;
			}
			else if (Options.HasFlag(ClangToolChainOptions.EnableHWAddressSanitizer))
			{
				return ClangSanitizer.HwAddress;
			}
			else if (Options.HasFlag(ClangToolChainOptions.EnableThreadSanitizer))
			{
				return ClangSanitizer.Thread;
			}
			else if (Options.HasFlag(ClangToolChainOptions.EnableUndefinedBehaviorSanitizer))
			{
				return ClangSanitizer.UndefinedBehavior;
			}
			else if (Options.HasFlag(ClangToolChainOptions.EnableMinimalUndefinedBehaviorSanitizer))
			{
				return ClangSanitizer.UndefinedBehaviorMinimal;
			}

			return ClangSanitizer.None;
		}
	};
}
