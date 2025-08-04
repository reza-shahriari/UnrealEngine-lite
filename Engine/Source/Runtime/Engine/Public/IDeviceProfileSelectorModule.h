// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	IDeviceProfileSelectorModule.h: Declares the IDeviceProfileSelectorModule interface.
=============================================================================*/

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "DeviceProfiles/DeviceProfileMatching.h"

class UDeviceProfile;

/**
 * Device Profile Selector module
 */
class IDeviceProfileSelectorModule
	: public IModuleInterface
{
public:
	IDeviceProfileSelectorModule() :
		ConstrainedAspectRatio(0.0f), 
		SafeZones(FVector4f::Zero())
	{}
	
	/**
	 * Run the logic to choose an appropriate device profile for this session
	 *
	 * @return The name of the device profile to use for this session
	 */
	virtual const FString GetRuntimeDeviceProfileName() = 0;

#if WITH_EDITOR
	/**
	 * Save Device Jsons to the location provided
	 * @param FolderLocation Folder where to store the Jsons
	 */
	virtual void ExportDeviceParametersToJson(FString& FolderLocation){}
	
	/**
	 * Can we export device parameters to Jsons
	 *  @return Wheter we can export device parameters to Jsonsn
	 */
	virtual bool CanExportDeviceParametersToJson() { return false; }

	/**
	 * Get the Device Parameters from the Json
	* @Param JsonLocation Json location
	* @Param OutDeviceParameters the device parameters read from Json
	 */
	virtual void GetDeviceParametersFromJson(FString& JsonLocation, TMap<FName, FString>& OutDeviceParameters) {}
	
	/**
	 * Get the ConstrainedAspectRatio of the Device
	 *  @return ConstrinedAspectRatio of the Device
	 */
	virtual float GetConstrainedAspectRatio() { return ConstrainedAspectRatio; }
	
	/**
	 * Get the Safe Zones of the Device
	 *  @return Safe Zones of the Device
	 */
	virtual FVector4f GetSafeZones() { return SafeZones; }
	
	/**
	 * Can we get device parameters from Json
	 *  @return Wheter we can get device parameters from Json
	 */
	virtual bool CanGetDeviceParametersFromJson() { return false; }
#endif

	/**
	* Run the logic to choose an appropriate device profile for this session.
	* @param DeviceParameters	A map of parameters to be used by device profile logic.
	* @return The name of the device profile to use for this session
	*/
	virtual const FString GetDeviceProfileName() { return FString(); }

	/**
	* Set or override the selector specific properties.
	* @param SelectorProperties	A map of parameters to be used by device profile matching logic.
	*/
	virtual void SetSelectorProperties(const TMap<FName, FString>& SelectorProperties) { }

	/*
	* Find a custom profile selector property value.
	* @Param PropertyType The information requested
	* @Param PropertyValueOUT the value of PropertyType
	* @return Whether the PropertyType was recognized by the profile selector.
	*/
	virtual bool GetSelectorPropertyValue(const FName& PropertyType, FString& PropertyValueOUT) { PropertyValueOUT.Reset(); return false; }

	/**
	 * Virtual destructor.
	 */
	virtual ~IDeviceProfileSelectorModule()
	{
	}

protected:
	float ConstrainedAspectRatio;
	FVector4f SafeZones;
};
