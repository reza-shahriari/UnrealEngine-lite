// Copyright Epic Games, Inc. All Rights Reserved.

#include "Modules/ModuleManager.h"

#include "IpAddressDetailsCustomization.h"
#include "MetaHumanCaptureSource.h"
#include "MetaHumanCaptureSourceSync.h"

#include "Settings/EditorLoadingSavingSettings.h"

bool CompareProperties(const UClass* const InLeftClass, const UClass* const InRightClass)
{
	// We need to exclude the UMetaHumanCaptureSourceSync::MetaHumanCaptureSource property, as this exists only in the synchronous form of the capture 
	// source and is needed to manage garbage collection.
	const TArray<FName> ExcludedPropertyNames = {
		TEXT("MetaHumanCaptureSource"),
	};

	for (TFieldIterator<FProperty> PropertyIter(InLeftClass, EFieldIterationFlags::None); PropertyIter; ++PropertyIter)
	{
		const FName PropertyName = PropertyIter->GetFName();
		const bool bIsExcludedProperty = ExcludedPropertyNames.Contains(PropertyName);

		if (!bIsExcludedProperty && InRightClass->FindPropertyByName(PropertyName) == nullptr)
		{
			return false;
		}
	}

	return true;
}

#define CHECK_AND_RETURN(Func) if (bool Condition = Func; !Condition) { return Condition; }

class FMetaHumanCaptureSourceModule
	: public IModuleInterface
{
public:

	//~Begin IModuleInterface interface
	virtual void StartupModule() override
	{
		checkf(VerifyCaptureSourcesProperties(), TEXT("There is a mismatch between MetaHumanCaptureSource and MetaHumanCaptureSourceSync properties"));

		// Add an exemption for files this module will create to the auto import setting. This places a restriction of
		// the name of the directory we can use for import
		FAutoReimportDirectoryConfig DirectoryConfig;
		DirectoryConfig.SourceDirectory = TEXT("/Game/");

		FAutoReimportWildcard Wildcard;
		Wildcard.Wildcard = TEXT("*_Ingested/*");
		Wildcard.bInclude = false;
		DirectoryConfig.Wildcards.Add(Wildcard);

		UEditorLoadingSavingSettings* Settings = GetMutableDefault<UEditorLoadingSavingSettings>();

		bool bSettingPresent = false;
		for (int32 ConfigIndex = 0; ConfigIndex < Settings->AutoReimportDirectorySettings.Num() && !bSettingPresent; ++ConfigIndex)
		{
			const FAutoReimportDirectoryConfig& Config = Settings->AutoReimportDirectorySettings[ConfigIndex];

			bSettingPresent = (Config.SourceDirectory == DirectoryConfig.SourceDirectory &&
							   Config.MountPoint == DirectoryConfig.MountPoint &&
							   Config.Wildcards.Num() == DirectoryConfig.Wildcards.Num() &&
							   Config.Wildcards[0].Wildcard == DirectoryConfig.Wildcards[0].Wildcard &&
							   Config.Wildcards[0].bInclude == DirectoryConfig.Wildcards[0].bInclude);
		}

		if (!bSettingPresent)
		{
			Settings->AutoReimportDirectorySettings.Add(DirectoryConfig);
			Settings->SaveConfig();
			Settings->OnSettingChanged().Broadcast(GET_MEMBER_NAME_CHECKED(UEditorLoadingSavingSettings, AutoReimportDirectorySettings));
		}

		FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.RegisterCustomPropertyTypeLayout("DeviceAddress", FOnGetPropertyTypeCustomizationInstance::CreateStatic(&FIpAddressDetailsCustomization::MakeInstance));

		PropertyModule.NotifyCustomizationModuleChanged();
	}

	virtual void ShutdownModule() override
	{
		if (FModuleManager::Get().IsModuleLoaded("PropertyEditor"))
		{
			// Unregister properties when the module is shutdown
			FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
			PropertyModule.UnregisterCustomPropertyTypeLayout("DeviceAddress");

			PropertyModule.NotifyCustomizationModuleChanged();
		}
	}
	//~End IModuleInterface interface

	static bool VerifyCaptureSourcesProperties()
	{
		CHECK_AND_RETURN(CompareProperties(UMetaHumanCaptureSourceSync::StaticClass(), UMetaHumanCaptureSource::StaticClass()));
		CHECK_AND_RETURN(CompareProperties(UMetaHumanCaptureSource::StaticClass(), UMetaHumanCaptureSourceSync::StaticClass()));

		return true;
	}

};

IMPLEMENT_MODULE(FMetaHumanCaptureSourceModule, MetaHumanCaptureSource)