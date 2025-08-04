// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreTypes.h"
#include "Misc/Guid.h"

#define UE_API RIGVM_API

// Custom serialization version for changes made in Dev-Anim stream
struct FRigVMObjectVersion
{
	enum Type
	{
		// Before any version changes were made
		BeforeCustomVersionWasAdded,
		
		// ControlRig & RigVMHost compute and checks VM Hash
		AddedVMHashChecks,

		// Predicates added to execute operations
		PredicatesAddedToExecuteOps,

		// Storing paths to user defined structs map
		VMStoringUserDefinedStructMap,

		// Storing paths to user defined enums map
		VMStoringUserDefinedEnumMap,

		// Storing paths to user defined enums map
		HostStoringUserDefinedData,

		// VM Memory Storage Struct serialized
		VMMemoryStorageStructSerialized,

		// VM Memory Storage Defaults generated at VM
		VMMemoryStorageDefaultsGeneratedAtVM,

		// VM Bytecode Stores the Public Context Path
		VMBytecodeStorePublicContextPath,

		// Removing unused tooltip property from frunction header
		VMRemoveTooltipFromFunctionHeader,

		// Removing library node FSoftObjectPath from FRigVMGraphFunctionIdentifier
		RemoveLibraryNodeReferenceFromFunctionIdentifier,

		// Adding variant struct to function identifier
		AddVariantToFunctionIdentifier,

		// Adding variant to every RigVM asset
		AddVariantToRigVMAssets,

		// Storing user interface layout within function header
		FunctionHeaderStoresLayout,

		// Storing user interface relevant pin index in category
		FunctionHeaderLayoutStoresPinIndexInCategory,

		// Storing user interface relevant category expansion
		FunctionHeaderLayoutStoresCategoryExpansion,

		// Storing function graph collapse node content as part of the header
		RigVMSaveSerializedGraphInGraphFunctionDataAsByteArray,

		// VM Bytecode Stores the Public Context Path as a FTopLevelAssetPath 
     	VMBytecodeStorePublicContextPathAsTopLevelAssetPath,

		// Serialized instruction offsets are now int32 rather than uint16, NumBytes has been removed
		// from RigVMCopyOp
		ByteCodeCleanup,

		// -----<new versions can be added above this line>-------------------------------------------------
		VersionPlusOne,
		LatestVersion = VersionPlusOne - 1
	};

	// The GUID for this custom version number
	UE_API const static FGuid GUID;

private:
	FRigVMObjectVersion() {}
};

#undef UE_API
