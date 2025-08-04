// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "DNAReader.h"

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"
#include "UObject/ObjectMacros.h"

namespace dna
{

enum class DataLayer : uint32;

}  // namespace dna

RIGLOGICMODULE_API TSharedPtr<IDNAReader> ReadDNAFromFile(const FString& Path, EDNADataLayer Layer = EDNADataLayer::All, uint16_t MaxLOD = 0);
RIGLOGICMODULE_API TSharedPtr<IDNAReader> ReadDNAFromFile(const FString& Path, EDNADataLayer Layer, TArrayView<uint16_t> LODs);
RIGLOGICMODULE_API TSharedPtr<IDNAReader> ReadDNAFromBuffer(TArray<uint8>* DNABuffer, EDNADataLayer Layer = EDNADataLayer::All, uint16_t MaxLOD = 0);
RIGLOGICMODULE_API TSharedPtr<IDNAReader> ReadDNAFromBuffer(TArray<uint8>* DNABuffer, EDNADataLayer Layer, TArrayView<uint16_t> LODs);
RIGLOGICMODULE_API TArray<uint8> ReadStreamFromDNA(const IDNAReader* Reader, EDNADataLayer Layer);
RIGLOGICMODULE_API void WriteDNAToFile(const IDNAReader* Reader, EDNADataLayer Layer, const FString& Path);
RIGLOGICMODULE_API dna::DataLayer CalculateDNADataLayerBitmask(EDNADataLayer Layer);

/** Creates a new DNA asset with behavior/geometry data set. FileHelper is used to load dna data from specified file */
RIGLOGICMODULE_API TObjectPtr<class UDNAAsset> GetDNAAssetFromFile(const FString& InFilePath, UObject* InOuter, EDNADataLayer InLayer = EDNADataLayer::All);