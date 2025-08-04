// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "UObject/ObjectMacros.h"

#include "NetEnums.generated.h"

namespace UE::Net
{
	enum class EJoinFlags
	{
		None,
		NoPawn = 1,
	};
	ENUM_CLASS_FLAGS(EJoinFlags)
}

/** Types of network failures broadcast from the engine */
UENUM(BlueprintType)
namespace ENetworkFailure
{
	enum Type : int
	{
		/** A relevant net driver has already been created for this service */
		NetDriverAlreadyExists,
		/** The net driver creation failed */
		NetDriverCreateFailure,
		/** The net driver failed its Listen() call */
		NetDriverListenFailure,
		/** A connection to the net driver has been lost */
		ConnectionLost,
		/** A connection to the net driver has timed out */
		ConnectionTimeout,
		/** The net driver received an NMT_Failure message */
		FailureReceived,
		/** The client needs to upgrade their game */
		OutdatedClient,
		/** The server needs to upgrade their game */
		OutdatedServer,
		/** There was an error during connection to the game */
		PendingConnectionFailure,
		/** NetGuid mismatch */
		NetGuidMismatch,
		/** Network checksum mismatch */
		NetChecksumMismatch
	};
}

namespace ENetworkFailure
{
	inline const TCHAR* ToString(ENetworkFailure::Type FailureType)
	{
		switch (FailureType)
		{
		case NetDriverAlreadyExists:
			return TEXT("NetDriverAlreadyExists");
		case NetDriverCreateFailure:
			return TEXT("NetDriverCreateFailure");
		case NetDriverListenFailure:
			return TEXT("NetDriverListenFailure");
		case ConnectionLost:
			return TEXT("ConnectionLost");
		case ConnectionTimeout:
			return TEXT("ConnectionTimeout");
		case FailureReceived:
			return TEXT("FailureReceived");
		case OutdatedClient:
			return TEXT("OutdatedClient");
		case OutdatedServer:
			return TEXT("OutdatedServer");
		case PendingConnectionFailure:
			return TEXT("PendingConnectionFailure");
		case NetGuidMismatch:
			return TEXT("NetGuidMismatch");
		case NetChecksumMismatch:
			return TEXT("NetChecksumMismatch");
		}
		return TEXT("Unknown ENetworkFailure error occurred.");
	}
}

UENUM(BlueprintType)
enum class EReplicationSystem : uint8
{
	Default,
	Generic,
	Iris,
};

// (DEPRECATED) Security event types used for UE_SECURITY_LOG
namespace ESecurityEvent
{ 
	enum Type
	{
		Malformed_Packet = 0, // The packet didn't follow protocol
		Invalid_Data = 1,     // The packet contained invalid data
		Closed = 2            // The connection had issues (potentially malicious) and was closed
	};
	
	/** @return the stringified version of the enum passed in */
	inline const TCHAR* ToString(const ESecurityEvent::Type EnumVal)
	{
		switch (EnumVal)
		{
			case Malformed_Packet:
			{
				return TEXT("Malformed_Packet");
			}
			case Invalid_Data:
			{
				return TEXT("Invalid_Data");
			}
			case Closed:
			{
				return TEXT("Closed");
			}
		}
		return TEXT("");
	}
}

namespace UE::Net
{

/**
 * Error types used when reporting an error on a specific Iris handle
 */
enum class ENetRefHandleError : uint32
{
	None = 0x00,
	BitstreamCorrupted = 0x01, // An error while reading detected an irrecoverable bitstream corruption. Client was forced to disconnect
	ReplicationDisabled = 0x02, // An error while reading was recoverable but prevents this NetObject from replicating.
	BlockedByMustBeMapped = 0x03, // An object is blocked due to MustBeMapped references not resolving fast enough
};
ENUM_CLASS_FLAGS(ENetRefHandleError);

inline const TCHAR* LexToString(ENetRefHandleError NetRefHandleError)
{
	switch (NetRefHandleError)
	{
		case ENetRefHandleError::None: return TEXT("None");
		case ENetRefHandleError::BitstreamCorrupted: return TEXT("BitstreamCorrupted");
		case ENetRefHandleError::ReplicationDisabled: return TEXT("ReplicationDisabled");
		case ENetRefHandleError::BlockedByMustBeMapped: return TEXT("BlockedByMustBeMapped");
		default: return TEXT("missing");
	}
}

} // end namespace UE::Net
