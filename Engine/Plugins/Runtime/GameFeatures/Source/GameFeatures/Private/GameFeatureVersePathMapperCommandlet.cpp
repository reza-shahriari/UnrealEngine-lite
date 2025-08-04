// Copyright Epic Games, Inc. All Rights Reserved.

#include "GameFeatureVersePathMapperCommandlet.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "GameFeatureData.h"
#include "GameFeaturesSubsystem.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Misc/PathViews.h"
#include "HAL/FileManager.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/IPluginManager.h"
#include "Logging/StructuredLog.h"
#include "InstallBundleUtils.h"
#include "JsonObjectConverter.h"
#include "Algo/Transform.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameFeatureVersePathMapper, Log, All);

namespace GameFeatureVersePathMapper
{
	struct FArgs
	{
		FString DevARPath;

		FString OutputPath;

		const ITargetPlatform* TargetPlatform = nullptr;

		static TOptional<FArgs> Parse(const TCHAR* CmdLineParams)
		{
			UE_LOGFMT(LogGameFeatureVersePathMapper, Display, "Parsing command line");

			FArgs Args;

			// Optional path to dev asset registry
			FString DevARFilename;
			if (FParse::Value(CmdLineParams, TEXT("-DevAR="), DevARFilename))
			{
				if (IFileManager::Get().FileExists(*DevARFilename) && FPathViews::GetExtension(DevARFilename) == TEXTVIEW("bin"))
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Display, "Using dev asset registry path '{Path}'", DevARFilename);
					Args.DevARPath = DevARFilename;
				}
				else
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "-DevAR did not specify a valid path.");
					return {};
				}
			}

			// Required output path
			if (!FParse::Value(CmdLineParams, TEXT("-Output="), Args.OutputPath))
			{
				UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "-Output is required.");
				return {};
			}

			// Required target platform
			FString TargetPlatformName;
			if (FParse::Value(CmdLineParams, TEXT("-Platform="), TargetPlatformName))
			{
				if (const ITargetPlatform* TargetPlatform = GetTargetPlatformManagerRef().FindTargetPlatform(TargetPlatformName))
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Display, "Using target platform '{Platform}'", TargetPlatformName);
					Args.TargetPlatform = TargetPlatform;
				}
				else
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Could not find target platfom '{Platform}'.", TargetPlatformName);
					return {};
				}
			}
			else
			{
				UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "-Platform is required.");
				return {};
			}

			return Args;
		}
	};

	FString GetVerseAppDomain()
	{
		FString AppDomain;
		if (!GConfig->GetString(TEXT("Verse"), TEXT("AppDomain"), AppDomain, GGameIni))
		{
			AppDomain = FPaths::Combine(TEXTVIEW("/"), FString(FApp::GetProjectName()) + TEXTVIEW(".com"));
		}
		AppDomain.RemoveFromEnd(TEXTVIEW("/"));
		return AppDomain;
	}

	FString GetAltVerseAppDomain()
	{
		FString AppDomain;
		if (!GConfig->GetString(TEXT("Verse"), TEXT("AltAppDomain"), AppDomain, GGameIni))
		{
			AppDomain = {};
		}
		AppDomain.RemoveFromEnd(TEXTVIEW("/"));
		return AppDomain;
	}

	class FInstallBundleResolver
	{
		TArray<TPair<FString, TArray<FRegexPattern>>> BundleRegexList;
		TMap<FString, FString> RegexMatchCache;

	public:
		FInstallBundleResolver(const TCHAR* IniPlatformName = nullptr)
		{
			FConfigFile MaybeLoadedConfig;
			const FConfigFile* InstallBundleConfig = IniPlatformName ?
				GConfig->FindOrLoadPlatformConfig(MaybeLoadedConfig, *GInstallBundleIni, IniPlatformName) :
				GConfig->FindConfigFile(GInstallBundleIni);

			// We want to load regex even if PlatformChunkID=-1 to make sure we map GFPs that are not packaged
			BundleRegexList = InstallBundleUtil::LoadBundleRegexFromConfig(*InstallBundleConfig);
		}

		FString Resolve(const FStringView& PluginName, const FString& ChunkPattern)
		{
			FString InstallBundleName = UGameFeatureData::GetInstallBundleName(PluginName);
			if (InstallBundleName.IsEmpty() && !ChunkPattern.IsEmpty())
			{
				if (FString* CachedInstallBundleName = RegexMatchCache.Find(ChunkPattern))
				{
					InstallBundleName = *CachedInstallBundleName;
				}
				else if (InstallBundleUtil::MatchBundleRegex(BundleRegexList, ChunkPattern, InstallBundleName))
				{
					RegexMatchCache.Add(ChunkPattern, InstallBundleName);
				}
			}

			return InstallBundleName;
		}
	};

	FConfigCacheIni* GetPlatformConfigCacheIni(const FString& IniPlatformName)
	{
#if WITH_EDITOR
		FConfigCacheIni* ConfigCache = FConfigCacheIni::ForPlatform(FName(IniPlatformName));
		if (!ConfigCache)
		{
			UE_LOGFMT(LogGameFeatureVersePathMapper, Warning, "Failed to find config for {PlatformName}", *IniPlatformName);
			ConfigCache = GConfig;
		}
#else
		FConfigCacheIni* ConfigCache = GConfig;
#endif
		return ConfigCache;
	}

	static bool PlatformChunksAreAlwaysResident(const ITargetPlatform* TargetPlatform /*= nullptr*/)
	{
		const FString& IniPlatformName = TargetPlatform ? TargetPlatform->IniPlatformName() : FPlatformProperties::IniPlatformName();
		FConfigCacheIni* ConfigCache = GetPlatformConfigCacheIni(IniPlatformName);

		bool bPlatformAlwaysResident = false;
		if (!ConfigCache->GetBool(TEXT("GameFeaturePlugins"), TEXT("bGFPAreAlwaysResident"), bPlatformAlwaysResident, GInstallBundleIni))
		{
			if (TargetPlatform)
			{
				bPlatformAlwaysResident = TargetPlatform->IsServerOnly();
			}
			else
			{
				bPlatformAlwaysResident = IsRunningDedicatedServer();
			}
		}

		return bPlatformAlwaysResident;
	}

	static FString GetChunkPatternFormat(const FString& IniPlatformName)
	{
		FConfigCacheIni* ConfigCache = GetPlatformConfigCacheIni(IniPlatformName);

		FString ChunkPatternFormat;
		if (!ConfigCache->GetString(TEXT("GameFeaturePlugins"), TEXT("GFPBundleRegexMatchPatternFormat"), ChunkPatternFormat, GInstallBundleIni))
		{
			ChunkPatternFormat = TEXTVIEW("chunk{Chunk}.pak");
		}

		return ChunkPatternFormat;
	}

	static FString GetChunkPattern(const FString& ChunkPatternFormat, int32 Chunk)
	{
		return FString::Format(*ChunkPatternFormat, FStringFormatNamedArguments{ {TEXT("Chunk"), Chunk} });
	}

	static TArray<int32> GetAlwaysResidentChunks(const FString& IniPlatformName)
	{
		FConfigCacheIni* ConfigCache = GetPlatformConfigCacheIni(IniPlatformName);

		TArray<int32> AlwaysResidentChunks;
		TArray<FString> AlwaysResidentChunksStr;

		if (!ConfigCache->GetArray(TEXT("GameFeaturePlugins"), TEXT("GFPAlwaysResidentChunks"), AlwaysResidentChunksStr, GInstallBundleIni))
		{
			AlwaysResidentChunks.Empty(1);
			AlwaysResidentChunks.Add(0);
		}
		else
		{
			AlwaysResidentChunks.Empty(AlwaysResidentChunksStr.Num());
			for (const FString& ChunkStr : AlwaysResidentChunksStr)
			{
				AlwaysResidentChunks.Add(FCString::Atoi(*ChunkStr));
			}
		}

		return AlwaysResidentChunks;
	}

	static TArray<FString> GetAlwaysResidentBundles(const FString& IniPlatformName)
	{
		FConfigCacheIni* ConfigCache = GetPlatformConfigCacheIni(IniPlatformName);

		TArray<FString> AlwaysResidentBundles;
		if (!ConfigCache->GetArray(TEXT("GameFeaturePlugins"), TEXT("GFPAlwaysResidentBundles"), AlwaysResidentBundles, GInstallBundleIni))
		{
			AlwaysResidentBundles.Empty();
		}

		return AlwaysResidentBundles;
	}

	static FString GetDevARPathForPlatform(FStringView PlatformName)
	{
		return FPaths::Combine(
			FPaths::ProjectSavedDir(), 
			TEXTVIEW("Cooked"), 
			PlatformName, 
			FApp::GetProjectName(), 
			TEXTVIEW("Metadata"), 
			TEXTVIEW("DevelopmentAssetRegistry.bin"));
	}

	static FString GetDevARPath(const FArgs& Args)
	{
		if (!Args.DevARPath.IsEmpty())
		{
			return Args.DevARPath;
		}

		if (Args.TargetPlatform)
		{
			return GetDevARPathForPlatform(Args.TargetPlatform->PlatformName());
		}

		return {};
	}

	template<class EnumeratorFunc>
	static TMap<FString, int32> FindGFPChunksImpl(const EnumeratorFunc& Enumerator)
	{
		const IAssetRegistry& AR = IAssetRegistry::GetChecked();

		FARFilter RawFilter;
#if !WITH_EDITORONLY_DATA
		// work-around for in-memory FAssetData not having chunks set
		RawFilter.bIncludeOnlyOnDiskAssets = true;
#endif
		RawFilter.bRecursiveClasses = true;
		RawFilter.ClassPaths.Add(UGameFeatureData::StaticClass()->GetClassPathName());

		FARCompiledFilter Filter;
		AR.CompileFilter(RawFilter, Filter);

		TMap<FString, int32> GFPChunks;

		FNameBuilder PackagePathBuilder;
		auto FindGFDChunks = [&PackagePathBuilder, &GFPChunks](const FAssetData& AssetData) -> bool
		{
			int32 ChunkId = -1;
			if (AssetData.GetChunkIDs().Num() > 0)
			{
				ChunkId = AssetData.GetChunkIDs()[0];
				if (AssetData.GetChunkIDs().Num() > 1)
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Warning, "Multiple Chunks found for {Package}, using chunk {Chunk}", AssetData.PackageName, ChunkId);
				}
			}
			AssetData.PackageName.ToString(PackagePathBuilder);
			FStringView PackageRoot = FPathViews::GetMountPointNameFromPath(PackagePathBuilder);
			GFPChunks.Emplace(PackageRoot, ChunkId);

			return true;
		};

		Enumerator(Filter, FindGFDChunks);

		return GFPChunks;
	}

	static TMap<FString, int32> FindGFPChunks(const FAssetRegistryState& DevAR)
	{
		return FindGFPChunksImpl([&DevAR](const FARCompiledFilter& Filter, TFunctionRef<bool(const FAssetData&)> Callback)
		{
			DevAR.EnumerateAssets(Filter, {}, Callback, 
				UE::AssetRegistry::EEnumerateAssetsFlags::AllowUnmountedPaths | 
				UE::AssetRegistry::EEnumerateAssetsFlags::AllowUnfilteredArAssets);
		});
	}

	static TMap<FString, int32> FindGFPChunks()
	{
		const IAssetRegistry& AR = IAssetRegistry::GetChecked();
		return FindGFPChunksImpl([&AR](const FARCompiledFilter& Filter, TFunctionRef<bool(const FAssetData&)> Callback)
		{
			AR.EnumerateAssets(Filter, Callback, UE::AssetRegistry::EEnumerateAssetsFlags::AllowUnmountedPaths);
		});
	}

	static bool IsChunkAlwaysResident(TConstArrayView<int32> AlwaysResidentChunks, int32 Chunk)
	{
		return Chunk < 0 || AlwaysResidentChunks.Contains(Chunk);
	}

	// Filter GFPs cooked of out of band
	static bool IsGFPUpluginInBaseBuild(FStringView GFPName)
	{
		// Consider a GFP part of the base build if its plugin was added outside of the
		// GFP statemachine. If there are cases where this doesn't hold, then its probably
		// better to generate an explicit manifest.

		UGameFeaturesSubsystem& GFPSys = UGameFeaturesSubsystem::Get();

		bool bGFPAddedUplugin = false;

		FString GFPURL;
		if (GFPSys.GetPluginURLByName(GFPName, GFPURL))
		{
			GFPSys.GetGameFeatureControlsUPlugin(GFPURL, bGFPAddedUplugin);
		}

		return !bGFPAddedUplugin;
	}

	bool FDepthFirstGameFeatureSorter::Visit(const FName Plugin, TFunctionRef<void(FName, const FString&)> AddOutput)
	{
		const FGameFeaturePluginInfo* MaybePluginInfo = GfpInfoMap.Find(Plugin);
		if (!MaybePluginInfo)
		{
			UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "DepthFirstGameFeatureSorter: could not find {PluginName}", Plugin);
			return false;
		}
		const FGameFeaturePluginInfo& PluginInfo = *MaybePluginInfo;

		// Add a scope here to make sure VisitState isn't used later. It can become invalid if VisitedPlugins is resized
		{
			EVisitState& VisitState = VisitedPlugins.FindOrAdd(Plugin, EVisitState::None);
			if (VisitState == EVisitState::Visiting)
			{
				UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "DepthFirstGameFeatureSorter: Cycle detected in plugin dependencies with {PluginName}", Plugin);
				return false;
			}

			if (VisitState == EVisitState::Visited)
			{
				return true;
			}

			VisitState = EVisitState::Visiting;
		}

		for (const FName DepPlugin : PluginInfo.Dependencies)
		{
			if (!Visit(DepPlugin, AddOutput))
			{
				return false;
			}
		}

		VisitedPlugins.FindChecked(Plugin) = EVisitState::Visited;
		if (bIncludeVirtualNodes || !PluginInfo.GfpUri.IsEmpty()) // An empty URI means this is virtual node that only exists for Verse path resolution
		{
			AddOutput(Plugin, PluginInfo.GfpUri);
		}
		return true;
	}

	bool FDepthFirstGameFeatureSorter::Sort(TFunctionRef<FName()> GetNextRootPlugin, TFunctionRef<void(FName, const FString&)> AddOutput)
	{
		for (FName RootPlugin = GetNextRootPlugin(); !RootPlugin.IsNone(); RootPlugin = GetNextRootPlugin())
		{
			if (!Visit(RootPlugin, AddOutput))
			{
				return false;
			}
		}
		return true;
	}

	bool FDepthFirstGameFeatureSorter::Sort(TConstArrayView<FName> RootPlugins, TFunctionRef<void(FName, const FString&)> AddOutput)
	{
		return Sort(
			[RootPlugins, i = int32(0)]() mutable -> FName
			{
				if (!RootPlugins.IsValidIndex(i))
				{
					return {};
				}
				return RootPlugins[i++];
			},
			AddOutput);
	}

	bool FDepthFirstGameFeatureSorter::Sort(TConstArrayView<FName> RootPlugins, TArray<FName>& OutPlugins)
	{
		return Sort(
			[RootPlugins, i = int32(0)]() mutable -> FName
			{
				if (!RootPlugins.IsValidIndex(i))
				{
					return {};
				}
				return RootPlugins[i++];
			}, 
			[&OutPlugins](FName OutPlugin, const FString& URI)
			{
				OutPlugins.Add(OutPlugin);
			});
	}

	TOptional<FGameFeatureVersePathLookup> BuildLookup(
		const ITargetPlatform* TargetPlatform /*= nullptr*/,
		const FAssetRegistryState* DevAR /*= nullptr*/,
		EBuildLookupOptions Options /*= EBuildLookupOptions::None*/)
	{
		const TMap<FString, int32> GFPChunks = DevAR ? FindGFPChunks(*DevAR) : FindGFPChunks();

		IPluginManager& PluginMan = IPluginManager::Get();

		FInstallBundleResolver InstallBundleResolver(TargetPlatform ? *TargetPlatform->IniPlatformName() : nullptr);

		const FString AppDomain = GameFeatureVersePathMapper::GetVerseAppDomain();
		const FString GameFeatureRootVersePath = UGameFeatureVersePathMapperCommandlet::GetGameFeatureRootVersePath();
		
		const FString& IniPlatformName = TargetPlatform ? TargetPlatform->IniPlatformName() : FPlatformProperties::IniPlatformName();
		const FString ChunkPatternFormat = GetChunkPatternFormat(IniPlatformName);
		const TArray<int32> AlwaysResidentChunks = GetAlwaysResidentChunks(IniPlatformName);
		const TArray<FString> AlwaysResidentBundles = GetAlwaysResidentBundles(IniPlatformName);
		const bool bPlatformChunksAreAlwaysResident = PlatformChunksAreAlwaysResident(TargetPlatform);

		FString TargetPlatformName = TargetPlatform ? TargetPlatform->IniPlatformName() : FPlatformMisc::GetUBTPlatform();
		if (TargetPlatformName.Equals(TEXT("Windows"), ESearchCase::IgnoreCase))
		{
			// legacy change of windows -> win64 as that's how SupportedTargetPlatforms expects windows.
			TargetPlatformName = TEXT("Win64");
		}
		FGameFeatureVersePathLookup Output;
		for (const TPair<FString, int32>& Pair : GFPChunks)
		{
			TSharedPtr<IPlugin> Plugin = PluginMan.FindPlugin(Pair.Key);
			if (!Plugin)
			{
				UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Could not find uplugin {PluginName}", Pair.Key);
				return {};
			}

			FStringView PluginNameView(Plugin->GetName());
			FName PluginName(PluginNameView);

			if (EnumHasAnyFlags(Options, EBuildLookupOptions::OnlyBaseBuildPlugins))
			{
				if (!IsGFPUpluginInBaseBuild(PluginNameView))
				{
					continue;
				}
			}

			// Skip plugins that won't be enabled on the platform.
			if (!Plugin->GetDescriptor().SupportsTargetPlatform(TargetPlatformName))
			{
				continue;
			}

			Output.VersePathToGfpMap.Add(FPaths::Combine(GameFeatureRootVersePath, PluginNameView), PluginName);

			// Add a virtual GFP to support plugin specified Verse paths
			if (!Plugin->GetVersePath().IsEmpty() && 
				Plugin->GetVersePath() != AppDomain) // Filter out references to the root path, we don't wan't to allow resolving all content (and we don't register sub-paths)
			{
				// Add a virtual GFP with this Verse path that depends on this GFP
				FName& VirtualGFPName = Output.VersePathToGfpMap.FindOrAdd(Plugin->GetVersePath());
				if (VirtualGFPName.IsNone())
				{
					VirtualGFPName = FName(FStringView(TEXTVIEW("V_") + Plugin->GetVersePath()));
				}

				FGameFeaturePluginInfo& GfpInfo = Output.GfpInfoMap.FindOrAdd(VirtualGFPName);
				GfpInfo.Dependencies.Add(PluginName);
			}

			FGameFeaturePluginInfo& GfpInfo = Output.GfpInfoMap.Add(PluginName);

			const FString DescriptorFileName = FPaths::CreateStandardFilename(Plugin->GetDescriptorFileName());

			const int32 Chunk = Pair.Value;
			const bool bIsChunkAlwaysResident = bPlatformChunksAreAlwaysResident || IsChunkAlwaysResident(AlwaysResidentChunks, Chunk);
			const FString ChunkPattern = bIsChunkAlwaysResident ? FString() : GetChunkPattern(ChunkPatternFormat, Chunk);
			const FString InstallBundleName = bIsChunkAlwaysResident ? FString() : InstallBundleResolver.Resolve(PluginNameView, ChunkPattern);

			GfpInfo.GfpUri = (InstallBundleName.IsEmpty() || AlwaysResidentBundles.Contains(InstallBundleName)) ?
				UGameFeaturesSubsystem::GetPluginURL_FileProtocol(DescriptorFileName) :
				UGameFeaturesSubsystem::GetPluginURL_InstallBundleProtocol(DescriptorFileName, InstallBundleName);

			for (const FPluginReferenceDescriptor& Dependency : Plugin->GetDescriptor().Plugins)
			{
				// Currently GameFeatureSubsystem only checks bEnabled to determine if it should wait on a dependency, so match that logic here
				if (!Dependency.bEnabled)
				{
					continue;
				}

				if (!GFPChunks.Contains(Dependency.Name))
				{
					// Dependency is not a GFP
					continue;
				}

				if (!Dependency.IsSupportedTargetPlatform(TargetPlatformName))
				{
					continue;
				}
				TSharedPtr<IPlugin> DepPlugin = PluginMan.FindPlugin(Dependency.Name);
				if (!DepPlugin)
				{
					UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Could not find uplugin dependency {PluginName}", Dependency.Name);
					continue;
				}
				if (!DepPlugin->GetDescriptor().SupportsTargetPlatform(TargetPlatformName))
				{
					continue;
				}

				GfpInfo.Dependencies.Emplace(FStringView(Dependency.Name));
			}
		}

		check(Output.VersePathToGfpMap.Num() == Output.GfpInfoMap.Num());

		return Output;
	}
}

int32 UGameFeatureVersePathMapperCommandlet::Main(const FString& CmdLineParams)
{
	const TOptional<GameFeatureVersePathMapper::FArgs> MaybeArgs = GameFeatureVersePathMapper::FArgs::Parse(*CmdLineParams);
	if (!MaybeArgs)
	{
		// Parse function should print errors
		return 1;
	}
	const GameFeatureVersePathMapper::FArgs& Args = MaybeArgs.GetValue();

	FString DevArPath = GameFeatureVersePathMapper::GetDevARPath(Args);
	if (DevArPath.IsEmpty() && !FPaths::FileExists(DevArPath))
	{
		UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Could not find development asset registry at '{Path}'", DevArPath);
		return 1;
	}

	FAssetRegistryState DevAR;
	if (!FAssetRegistryState::LoadFromDisk(*DevArPath, FAssetRegistryLoadOptions(), DevAR))
	{
		UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Failed to load development asset registry from {Path}", DevArPath);
		return 1;
	}

	TOptional<GameFeatureVersePathMapper::FGameFeatureVersePathLookup> MaybeLookup = GameFeatureVersePathMapper::BuildLookup(Args.TargetPlatform, &DevAR);
	if (!MaybeLookup)
	{
		// BuildLookup will emit errors
		return 1;
	}
	GameFeatureVersePathMapper::FGameFeatureVersePathLookup& Lookup = *MaybeLookup;

	TSharedRef<FJsonObject> OutJsonObject = MakeShared<FJsonObject>();
	{
		{
			// Reversing the VersePathToGfpMap makes it more natural for the registration API
			TMap<FName, TSharedRef<FJsonValueString>> TempGfpVersePathMap;
			TempGfpVersePathMap.Reserve(Lookup.VersePathToGfpMap.Num());
			for (const TPair<FString, FName>& Pair : Lookup.VersePathToGfpMap)
			{
				TempGfpVersePathMap.Emplace(Pair.Value, MakeShared<FJsonValueString>(Pair.Key));
			}

			TSharedRef<FJsonObject> GfpVersePathMap = MakeShared<FJsonObject>();

			// Sort the reversed map in dependency order
			GameFeatureVersePathMapper::FDepthFirstGameFeatureSorter Sorter(Lookup.GfpInfoMap, true /*bIncludeVirtualNodes*/);
			Sorter.Sort(
				[It = TempGfpVersePathMap.CreateConstIterator()]() mutable -> FName
				{
					if (!It)
					{
						return {};
					}
					FName Plugin = It.Key();
					++It;
					return Plugin;
				},
				[&TempGfpVersePathMap, GfpVersePathMap](FName OutPlugin, const FString& OutGfpUri)
				{
					GfpVersePathMap->Values.Add(OutPlugin.ToString(), TempGfpVersePathMap.FindChecked(OutPlugin));
				});

			OutJsonObject->Values.Add(TEXT("GfpVersePathMap"), MakeShared<FJsonValueObject>(GfpVersePathMap));
		}

		{
			TSharedRef<FJsonObject> GfpInfoMap = MakeShared<FJsonObject>();
			for (TPair<FName, GameFeatureVersePathMapper::FGameFeaturePluginInfo>& Pair : Lookup.GfpInfoMap)
			{
				TSharedRef<FJsonObject> GfpInfo = MakeShared<FJsonObject>();
				GfpInfo->Values.Add(TEXT("GfpUri"), MakeShared<FJsonValueString>(MoveTemp(Pair.Value.GfpUri)));

				TArray<TSharedPtr<FJsonValue>> Dependencies;
				Dependencies.Reserve(Pair.Value.Dependencies.Num());
				Algo::Transform(Pair.Value.Dependencies, Dependencies, [](FName Name) { return MakeShared<FJsonValueString>(Name.ToString()); });
				GfpInfo->Values.Add(TEXT("Dependencies"), MakeShared<FJsonValueArray>(MoveTemp(Dependencies)));

				GfpInfoMap->Values.Add(Pair.Key.ToString(), MakeShared<FJsonValueObject>(GfpInfo));
			}

			OutJsonObject->Values.Add(TEXT("GfpInfoMap"), MakeShared<FJsonValueObject>(GfpInfoMap));
		}
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Args.OutputPath));

	TUniquePtr<FArchive> FileWriter(IFileManager::Get().CreateFileWriter(*Args.OutputPath));
	TSharedRef<TJsonWriter<UTF8CHAR>> JsonWriter = TJsonWriterFactory<UTF8CHAR>::Create(FileWriter.Get());
	if (!FJsonSerializer::Serialize(OutJsonObject, JsonWriter))
	{
		UE_LOGFMT(LogGameFeatureVersePathMapper, Error, "Failed to save output file at {Path}", Args.OutputPath);
		return 1;
	}

	return 0;
}

/*static*/ FString UGameFeatureVersePathMapperCommandlet::GetGameFeatureRootVersePath()
{
	return FPaths::Combine(GameFeatureVersePathMapper::GetVerseAppDomain(), TEXTVIEW("GameFeatures"));
}