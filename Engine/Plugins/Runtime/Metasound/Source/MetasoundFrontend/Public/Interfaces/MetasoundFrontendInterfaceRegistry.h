// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MetasoundFrontendController.h"
#include "MetasoundFrontendDocument.h"
#include "UObject/NoExportTypes.h"

#define UE_API METASOUNDFRONTEND_API

namespace Metasound::Frontend
{
	using FInterfaceRegistryKey = FString;
	using FRegistryTransactionID = int32;

	METASOUNDFRONTEND_API bool IsValidInterfaceRegistryKey(const FInterfaceRegistryKey& InKey);
	METASOUNDFRONTEND_API FInterfaceRegistryKey GetInterfaceRegistryKey(const FMetasoundFrontendVersion& InInterfaceVersion);
	METASOUNDFRONTEND_API FInterfaceRegistryKey GetInterfaceRegistryKey(const FMetasoundFrontendInterface& InInterface);

	class IInterfaceRegistryEntry
	{
	public:
		virtual ~IInterfaceRegistryEntry() = default;

		// MetaSound Interface definition
		virtual const FMetasoundFrontendInterface& GetInterface() const = 0;

		// Whether or not entry is deprecated or not. If false, entry is checked for validity on registration in editor builds.
		virtual bool IsDeprecated() const { return false; }

		// Name of routing system used to update interface inputs (ex. ParameterInterface or DataReference).
		virtual FName GetRouterName() const = 0;

		// How to update a given document if versioning is required to this interface from a deprecated version.
		virtual bool UpdateRootGraphInterface(FDocumentHandle InDocument) const = 0;
	};

	class FInterfaceRegistryTransaction
	{
	public:
		using FTimeType = uint64;

		/** Describes the type of transaction. */
		enum class ETransactionType : uint8
		{
			InterfaceRegistration,     //< Something was added to the registry.
			InterfaceUnregistration,  //< Something was removed from the registry.
			Invalid
		};

		UE_API FInterfaceRegistryTransaction(ETransactionType InType, const FInterfaceRegistryKey& InKey, const FMetasoundFrontendVersion& InInterfaceVersion, FTimeType InTimestamp);

		UE_API ETransactionType GetTransactionType() const;
		UE_API const FMetasoundFrontendVersion& GetInterfaceVersion() const;
		UE_API const FInterfaceRegistryKey& GetInterfaceRegistryKey() const;
		UE_API FTimeType GetTimestamp() const;

	private:

		ETransactionType Type;
		FInterfaceRegistryKey Key;
		FMetasoundFrontendVersion InterfaceVersion;
		FTimeType Timestamp;
	};

	class IInterfaceRegistry
	{
	public:
		static UE_API IInterfaceRegistry& Get();

		virtual ~IInterfaceRegistry() = default;

		// Register an interface
		virtual bool RegisterInterface(TUniquePtr<IInterfaceRegistryEntry>&& InEntry) = 0;

		// Find an interface entry with the given key. Returns null if entry not found with given key.
		virtual const IInterfaceRegistryEntry* FindInterfaceRegistryEntry(const FInterfaceRegistryKey& InKey) const = 0;

		// Find an interface with the given key. Returns true if interface is found, false if not.
		virtual bool FindInterface(const FInterfaceRegistryKey& InKey, FMetasoundFrontendInterface& OutInterface) const = 0;
	};
} // namespace Metasound::Frontend

#undef UE_API
