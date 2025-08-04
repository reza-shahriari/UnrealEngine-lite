// Copyright Epic Games, Inc. All Rights Reserved.

#include "Metadata/PCGMetadataPartitionCommon.h"

#include "PCGContext.h"
#include "PCGModule.h"
#include "PCGParamData.h"
#include "Data/PCGBasePointData.h"
#include "Data/PCGSpatialData.h"
#include "Elements/Metadata/PCGMetadataElementCommon.h"
#include "Metadata/PCGMetadata.h"
#include "Metadata/PCGMetadataCommon.h"
#include "Metadata/PCGMetadataAttribute.h"
#include "Metadata/Accessors/IPCGAttributeAccessor.h"
#include "Metadata/Accessors/PCGAttributeAccessorHelpers.h"
#include "Metadata/Accessors/PCGAttributeAccessorKeys.h"

#include "Algo/IndexOf.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

#define LOCTEXT_NAMESPACE "PCGMetadataPartitionCommon"

namespace PCGMetadataPartitionCommon
{
	template <typename T> 
	constexpr bool IsBitArray = std::is_same_v<T, TBitArray<>>;

	/**
	* Partition a given attribute, by first partitioning all value keys that point to the same value
	* and then for each unique value key, list of index in the keys that match for this value.
	*/
	template <typename PartitionType>
	TArray<PartitionType> AttributePartition(const FPCGMetadataAttributeBase* InAttribute, const IPCGAttributeAccessorKeys& InKeys, FPCGContext* InOptionalContext)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributePartition);
		check(InAttribute);

		const int32 NumberOfEntries = InKeys.GetNum();

		if (NumberOfEntries <= 0)
		{
			return {};
		}

		// Get all value keys (-1 + 0 - N)
		const int64 MetadataValueKeyCount = InAttribute->GetValueKeyOffsetForChild();

		// For every value key, check if it should be merged with the default value
		TMap<PCGMetadataValueKey, int32> ValueKeyMapping;
		ValueKeyMapping.Reserve(MetadataValueKeyCount);

		int32 NumUniqueValueKeys = 0;

		const bool bUsesValueKeys = InAttribute->UsesValueKeys();

		if (!bUsesValueKeys)
		{
			TArray<PCGMetadataValueKey> UniqueValueKeys;

			for (PCGMetadataValueKey ValueKey = 0; ValueKey < MetadataValueKeyCount; ++ValueKey)
			{
				if (InAttribute->IsEqualToDefaultValue(ValueKey))
				{
					ValueKeyMapping.Add(ValueKey, -1);
					continue;
				}

				// TODO: Might want to upgrade to something better since it can be quadratic and grow quickly.
				int32 UniqueValueKeyIndex = Algo::IndexOfByPredicate(UniqueValueKeys, [ValueKey, InAttribute](const PCGMetadataValueKey& Key)
				{
					return InAttribute->AreValuesEqual(ValueKey, Key);
				});

				if (UniqueValueKeyIndex == INDEX_NONE)
				{
					ValueKeyMapping.Add(ValueKey, UniqueValueKeys.Num());
					UniqueValueKeys.Add(ValueKey);
					NumUniqueValueKeys++;
				}
				else
				{
					ValueKeyMapping.Add(ValueKey, UniqueValueKeyIndex);
				}
			}
		}
		else
		{
			NumUniqueValueKeys = MetadataValueKeyCount;
		}

		TArray<PartitionType> PartitionedData;
		PartitionedData.SetNum(1 + NumUniqueValueKeys);
		if constexpr (IsBitArray<PartitionType>)
		{
			for (PartitionType& Partition : PartitionedData)
			{
				Partition.Init(false, NumberOfEntries);
			}
		}

		constexpr int32 ChunkSize = 256;
		TArray<const PCGMetadataEntryKey*, TInlineAllocator<ChunkSize>> TempEntries;
		TempEntries.SetNum(ChunkSize);

		const int32 NumberOfIterations = (NumberOfEntries + ChunkSize - 1) / ChunkSize;

		for (int32 i = 0; i < NumberOfIterations; ++i)
		{
			const int32 StartIndex = i * ChunkSize;
			const int32 Range = FMath::Min(NumberOfEntries - StartIndex, ChunkSize);
			TArrayView<const PCGMetadataEntryKey*> View(TempEntries.GetData(), Range);
			InKeys.GetKeys(StartIndex, View);

			for (int32 j = 0; j < Range; ++j)
			{
				PCGMetadataValueKey ValueKey = InAttribute->GetValueKey(*TempEntries[j]);
				// Remap if not using value keys
				if (!bUsesValueKeys && ValueKey != PCGDefaultValueKey)
				{
					ValueKey = ValueKeyMapping[ValueKey];
				}

				const int32 PartitionDataIndex = 1 + ValueKey;
				if constexpr (IsBitArray<PartitionType>)
				{
					PartitionedData[PartitionDataIndex][StartIndex + j] = true;
				}
				else
				{
					PartitionedData[PartitionDataIndex].Add(StartIndex + j);
				}
			}
		}

		// Since we partition on the value array, it is not guaranteed that the values appears in the same order than the entries.
		// So sort the final array using the first index as a sort criteria. Empty partitions will be at the beginning too.
		if constexpr (IsBitArray<PartitionType>)
		{
			Algo::Sort(PartitionedData, [](const TBitArray<>& LHS, const TBitArray<>& RHS) -> bool
			{
				const int32 FirstBitSetLHS = LHS.Find(true);
				if (FirstBitSetLHS == INDEX_NONE)
				{
					return true;
				}

				const int32 FirstBitSetRHS = RHS.Find(true);
				if (FirstBitSetRHS == INDEX_NONE)
				{
					return false;
				}

				return  FirstBitSetLHS < FirstBitSetRHS;
			});
		}
		else
		{
			PartitionedData.Sort([](const PartitionType& LHS, const PartitionType& RHS) -> bool
			{
				if (LHS.IsEmpty())
				{
					return true;
				}
				else if (RHS.IsEmpty())
				{
					return false;
				}
				else
				{
					return LHS[0] < RHS[0];
				}
			});
		}

		return PartitionedData;
	}

	/**
	* Partition a given accessor that iterate on all values, find the identical ones,
	* and then for each unique value, list of index in the keys that match for this value.
	*/
	template <typename PartitionType, typename T>
	TArray<PartitionType> ValuePartition(const IPCGAttributeAccessor& InAccessor, const IPCGAttributeAccessorKeys& InKeys, FPCGContext* InOptionalContext)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::ValuePartition);
		TArray<T> UniqueValues;
		TArray<PartitionType> PartitionedData;

		PCGMetadataElementCommon::ApplyOnAccessor<T>(InKeys, InAccessor, [&PartitionedData, &UniqueValues, NumberOfEntries = InKeys.GetNum()](const T& InValue, int32 InIndex)
		{
			// TODO: Might want to upgrade to something better since it can be quadratic and grow quickly.
			int32 UniqueValueIndex = UniqueValues.IndexOfByPredicate([&InValue](const T& OtherValue)
			{
				// For consistency with the attribute part, use MetadataTraits::Equal
				return PCG::Private::MetadataTraits<T>::Equal(InValue, OtherValue);
			});

			if (UniqueValueIndex == INDEX_NONE)
			{
				UniqueValueIndex = UniqueValues.Add(InValue);
				PartitionType& Partition = PartitionedData.Emplace_GetRef();
				if constexpr (IsBitArray<PartitionType>)
				{
					Partition.Init(false, NumberOfEntries);
				}
			}

			if constexpr (IsBitArray<PartitionType>)
			{
				PartitionedData[UniqueValueIndex][InIndex] = true;
			}
			else
			{
				PartitionedData[UniqueValueIndex].Add(InIndex);
			}
		});

		return PartitionedData;
	}

	/**
	* Dispatch the partition according to the data and selector.
	*/
	template <typename PartitionType>
	TArray<PartitionType> AttributeGenericPartition(const UPCGData* InData, const FPCGAttributePropertySelector& InSelector, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributeGenericPartition::SingleSelector);
		if (!InData)
		{
			return {};
		}

		TUniquePtr<const IPCGAttributeAccessorKeys> Keys = PCGAttributeAccessorHelpers::CreateConstKeys(InData, InSelector);
		if (!Keys.IsValid())
		{
			PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidKeys", "Could not create keys for the input data with selector {0}"), InSelector.GetDisplayText()), InOptionalContext);
			return {};
		}

		// Implementation note:
		// We'll use the attribute partition only for compressed types here (+ needs to be basic attribute only)
		// because otherwise we can run into issues where keeping track of the breadth of values is not great.
		bool bUseAttributePartition = false;
		const FPCGMetadataAttributeBase* Attribute = nullptr;

		if (InSelector.IsBasicAttribute())
		{
			const UPCGMetadata* Metadata = InData->ConstMetadata();
			if (!Metadata)
			{
				PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidMetadata", "Input data does not have metadata, while requesting an attribute {0}"), InSelector.GetDisplayText()), InOptionalContext);
				return {};
			}

			Attribute = Metadata->GetConstAttribute(InSelector.GetName());
			if (!Attribute)
			{
				if (!bSilenceMissingAttributeErrors)
				{
					PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidAttribute", "Attribute {0} not found"), InSelector.GetDisplayText()), InOptionalContext);
				}

				return {};
			}

			bUseAttributePartition = Attribute->UsesValueKeys();
		}

		if (bUseAttributePartition)
		{
			check(Attribute);
			return AttributePartition<PartitionType>(Attribute, *Keys, InOptionalContext);
		}
		else
		{
			TUniquePtr<const IPCGAttributeAccessor> Accessor = PCGAttributeAccessorHelpers::CreateConstAccessor(InData, InSelector);
			if (!Accessor.IsValid())
			{
				if (!bSilenceMissingAttributeErrors)
				{
					PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidAccessor", "Attribute {0} not found"), InSelector.GetDisplayText()), InOptionalContext);
				}

				return {};
			}

			auto Operation = [&Accessor, &Keys, &InSelector, InOptionalContext](auto Dummy) -> TArray<PartitionType>
			{
				// Rotators don't have a hash, convert them to Quat
				using AttributeType = std::conditional_t<std::is_same_v<decltype(Dummy), FRotator>, FQuat, decltype(Dummy)>;

				// Can't partition on a transform.
				if constexpr (std::is_same_v<AttributeType, FTransform>)
				{
					PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidType", "Attribute {0} is a transform, partition on transforms is not supported"), InSelector.GetDisplayText()), InOptionalContext);
					return {};
				}
				else
				{
					return ValuePartition<PartitionType, AttributeType>(*Accessor, *Keys, InOptionalContext);
				}
			};

			return PCGMetadataAttribute::CallbackWithRightType(Accessor->GetUnderlyingType(), Operation);
		}
	}

	/**
	* Dispatch the partition according to the data and selector.
	*/
	TArray<TArray<int32>> AttributeGenericPartition(const UPCGData* InData, const FPCGAttributePropertySelector& InSelector, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		return AttributeGenericPartition<TArray<int32>>(InData, InSelector, InOptionalContext, bSilenceMissingAttributeErrors);
	}

	/**
	 * Partition on multiple attributes by first partitioning on the attributes independently. Then take the resultant
	 * partition and convert them to a BitArray representation of each element's partition. Once in BitArray form,
	 * combine the results with a logical AND operation to filter them into final partition groupings.
	 *
	 * Multi-Partition Example:
	 * Pt  A  B  C                         Partition on A->[0,1],[2,3,4]
	 *  0  a  a  a                         Partition on B->[0],[1,2],[3,4]
	 *  1  a  b  a                         Partition on C->[0,1],[2],[3,4]
	 *  2  b  b  b                         Partition on A&B->[0],[1],[2],[3,4]
	 *  3  b  c  c                         Final Partition (A&B&C)->[0],[1],[2],[3,4]
	 *  4  b  c  c
	 */
	TArray<TArray<int32>> AttributeGenericPartition(const UPCGData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArrayView, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributeGenericPartition::MultiSelector);

		// Small optimization to partition on a single attribute
		if (InSelectorArrayView.Num() == 1)
		{
			return AttributeGenericPartition<TArray<int32>>(InData, InSelectorArrayView[0], InOptionalContext, bSilenceMissingAttributeErrors);
		}

		if (!InData || InSelectorArrayView.IsEmpty() || !InData->ConstMetadata())
		{
			return {};
		}

		// Get the element count from the number of keys which should work for spatial points and attribute sets
		const TUniquePtr<const IPCGAttributeAccessorKeys> Keys = PCGAttributeAccessorHelpers::CreateConstKeys(InData, InSelectorArrayView[0]);
		if (!Keys.IsValid())
		{
			PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("InvalidKeys", "Could not create keys for the input data with selector {0}"), InSelectorArrayView[0].GetDisplayText()), InOptionalContext);
			return {};
		}

		const int64 NumElements = Keys->GetNum();
		const int64 NumAttributes = InSelectorArrayView.Num();

		using IndexPartition = TArray<TArray<int32>>;
		using BitPartition = TArray<TBitArray<>>;

		TArray<BitPartition> BitPartitions;
		BitPartitions.SetNum(NumAttributes);

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributeGenericPartition::MultiSelector::PartitionOnBitArray);
			/* TODO: Can be executed in parallel, threadsafe. There is a follow up task to evaluate between
			* option A.) partitioning on all attributes, and then merging and B.) Partitioning on each attribute
			* in succession, further partitioning the grouping results of the previous iteration.
			*/
			// Calculate each partition into a bitfield for simple/efficient intersection processing
			for (int32 I = 0; I < InSelectorArrayView.Num(); ++I)
			{
				BitPartitions[I] = AttributeGenericPartition<TBitArray<>>(InData, InSelectorArrayView[I], InOptionalContext, bSilenceMissingAttributeErrors);
			}
		}

		BitPartition IterativePartition = BitPartitions[0]; // TODO: This can be optimized to filter down in pairs in parallel - O(log N) - instead of serial

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributeGenericPartition::Intersection);
			// Intersect all the BitArray partitions
			for (int32 PartitionIndex = 1; PartitionIndex < BitPartitions.Num(); ++PartitionIndex)
			{
				BitPartition CurrentBitPartition = IterativePartition;
				BitPartition& NextBitPartition = BitPartitions[PartitionIndex];

				IterativePartition.Empty();

				for (const TBitArray<>& FirstBitArray : CurrentBitPartition)
				{
					for (const TBitArray<>& SecondBitArray : NextBitPartition)
					{
						TBitArray<> Result = TBitArray<>::BitwiseAND(FirstBitArray, SecondBitArray, EBitwiseOperatorFlags::MaxSize);
						// Only capture if non-empty. Discard empty BitArrays.
						if (TConstSetBitIterator(Result))
						{
							IterativePartition.Emplace(std::move(Result));
						}
					}
				}
			}
		}

		IndexPartition FinalPartition;

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(PCGMetadataPartitionCommon::AttributeGenericPartition::MultiSelector::ConversionToIndices);
			// Convert back into indices
			for (const TBitArray<>& BitArray : IterativePartition)
			{
				TArray<int>& Indices = FinalPartition.Emplace_GetRef();
				for (TConstSetBitIterator It(BitArray); It; ++It)
				{
					Indices.Emplace(It.GetIndex());
				}
			}
		}

		return FinalPartition;
	}

	/**
	* Do a partition on the given point data for the selector
	*/
	TArray<UPCGData*> AttributePointPartition(const UPCGBasePointData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArrayView, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		TArray<TArray<int32>> Partition = AttributeGenericPartition(InData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		if (Partition.IsEmpty())
		{
			return {};
		}

		TArray<UPCGData*> PartitionedData;
		PartitionedData.Reserve(Partition.Num());

		for (TArray<int32>& Indices : Partition)
		{
			if (Indices.IsEmpty())
			{
				continue;
			}

			UPCGBasePointData* CurrentPointData = FPCGContext::NewPointData_AnyThread(InOptionalContext);
			PartitionedData.Add(CurrentPointData);

			FPCGInitializeFromDataParams InitializeFromDataParams(InData);
			InitializeFromDataParams.bInheritSpatialData = false;

			CurrentPointData->InitializeFromDataWithParams(InitializeFromDataParams);

			UPCGBasePointData::SetPoints(InData, CurrentPointData, Indices, /*bCopyAll=*/false);
		}

		return PartitionedData;
	}

	UPCGData* RemoveDuplicatesPoint(const UPCGBasePointData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArrayView, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		TArray<TArray<int32>> Partition = AttributeGenericPartition(InData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		if (Partition.IsEmpty())
		{
			return nullptr;
		}

		UPCGBasePointData* OutputPointData = nullptr;
		TArray<int32> IndicesToCopy;
		IndicesToCopy.Reserve(Partition.Num());

		for (TArray<int32>& Indices : Partition)
		{
			IndicesToCopy.Add(Indices[0]);
		}

		if (IndicesToCopy.IsEmpty())
		{
			return nullptr;
		}

		OutputPointData = FPCGContext::NewPointData_AnyThread(InOptionalContext);

		FPCGInitializeFromDataParams InitializeFromDataParams(InData);
		InitializeFromDataParams.bInheritSpatialData = false;

		OutputPointData->InitializeFromDataWithParams(InitializeFromDataParams);
		OutputPointData->SetNumPoints(IndicesToCopy.Num());
		
		UPCGBasePointData::SetPoints(InData, OutputPointData, IndicesToCopy, /*bCopyAll=*/false);

		return OutputPointData;
	}

	TArray<UPCGData*> AttributeParamSpatialPartition(const UPCGData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArray, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		if (!InData->IsA<UPCGSpatialData>() && !InData->IsA<UPCGParamData>())
		{
			PCGLog::LogErrorOnGraph(LOCTEXT("InvalidDataType", "Input data is not an attribute set nor a spatial data. Operation not supported."), InOptionalContext);
			return {};
		}

		const TArray<TArray<int32>> Partition = AttributeGenericPartition(InData, InSelectorArray, InOptionalContext, bSilenceMissingAttributeErrors);

		if (Partition.IsEmpty())
		{
			return {};
		}

		const UPCGSpatialData* InSpatialData = Cast<const UPCGSpatialData>(InData);

		TArray<UPCGData*> PartitionedData;
		PartitionedData.Reserve(Partition.Num());

		TArray<FName> AttributeNames;
		TArray<EPCGMetadataTypes> AttributeTypes;
		const UPCGMetadata* OriginalMetadata = InData->ConstMetadata();
		OriginalMetadata->GetAttributes(AttributeNames, AttributeTypes);

		for (const TArray<int32>& Indices : Partition)
		{
			if (Indices.IsEmpty())
			{
				continue;
			}

			UPCGMetadata* NewMetadata = nullptr;

			if (InSpatialData)
			{
				UPCGSpatialData* NewData = FPCGContext::NewObject_AnyThread<UPCGSpatialData>(InOptionalContext, GetTransientPackage(), InSpatialData->GetClass());
				NewData->InitializeFromData(InSpatialData);
				NewMetadata = NewData->Metadata;
				PartitionedData.Add(NewData);
			}
			else
			{
				UPCGParamData* NewData = FPCGContext::NewObject_AnyThread<UPCGParamData>(InOptionalContext);
				NewData->Metadata->AddAttributes(OriginalMetadata);
				NewMetadata = NewData->Metadata;
				PartitionedData.Add(NewData);
			}

			TArray<PCGMetadataEntryKey> EntryKeys;
			EntryKeys.Reserve(Indices.Num());
			for (int32 i = 0; i < Indices.Num(); ++i)
			{
				EntryKeys.Add(NewMetadata->AddEntry());
			}

			for (const FName AttributeName : AttributeNames)
			{
				const FPCGMetadataAttributeBase* OriginalAttribute = OriginalMetadata->GetConstAttribute(AttributeName);
				FPCGMetadataAttributeBase* NewAttribute = NewMetadata->GetMutableAttribute(AttributeName);
				check(OriginalAttribute && NewAttribute);

				for (int32 i = 0; i < Indices.Num(); ++i)
				{
					NewAttribute->SetValue(EntryKeys[i], OriginalAttribute, Indices[i]);
				}
			}
		}

		return PartitionedData;
	}

	UPCGData* RemoveDuplicatesParamSpatial(const UPCGData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArray, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		if (!InData->IsA<UPCGSpatialData>() && !InData->IsA<UPCGParamData>())
		{
			PCGLog::LogErrorOnGraph(LOCTEXT("InvalidDataType", "Input data is not an attribute set nor a spatial data. Operation not supported."), InOptionalContext);
			return nullptr;
		}

		const TArray<TArray<int32>> Partition = AttributeGenericPartition(InData, InSelectorArray, InOptionalContext, bSilenceMissingAttributeErrors);

		if (Partition.IsEmpty())
		{
			return nullptr;
		}

		const UPCGSpatialData* InSpatialData = Cast<const UPCGSpatialData>(InData);

		UPCGData* OutputData = nullptr;
		UPCGMetadata* OutMetadata = nullptr;

		TArray<FName> AttributeNames;
		TArray<EPCGMetadataTypes> AttributeTypes;
		const UPCGMetadata* OriginalMetadata = InData->ConstMetadata();
		OriginalMetadata->GetAttributes(AttributeNames, AttributeTypes);

		for (const TArray<int32>& Indices : Partition)
		{
			if (Indices.IsEmpty())
			{
				continue;
			}

			if (!OutputData)
			{
				if (InSpatialData)
				{
					UPCGSpatialData* NewData = FPCGContext::NewObject_AnyThread<UPCGSpatialData>(InOptionalContext, GetTransientPackage(), InSpatialData->GetClass());
					NewData->InitializeFromData(InSpatialData);
					OutputData = NewData;
					OutMetadata = NewData->Metadata;
				}
				else
				{
					UPCGParamData* NewData = FPCGContext::NewObject_AnyThread<UPCGParamData>(InOptionalContext);
					NewData->Metadata->AddAttributes(OriginalMetadata);
					OutputData = NewData;
					OutMetadata = NewData->Metadata;
				}
			}

			check(OutMetadata);

			const PCGMetadataEntryKey CurrentEntryKey = OutMetadata->AddEntry();

			for (const FName AttributeName : AttributeNames)
			{
				const FPCGMetadataAttributeBase* OriginalAttribute = OriginalMetadata->GetConstAttribute(AttributeName);
				FPCGMetadataAttributeBase* NewAttribute = OutMetadata->GetMutableAttribute(AttributeName);
				check(OriginalAttribute && NewAttribute);

				NewAttribute->SetValue(CurrentEntryKey, OriginalAttribute, Indices[0]);
			}
		}

		return OutputData;
	}

	TArray<UPCGData*> AttributePartition(const UPCGData* InData, const FPCGAttributePropertySelector& InSelector, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		return AttributePartition(InData, TArrayView<const FPCGAttributePropertySelector>(&InSelector, 1), InOptionalContext, bSilenceMissingAttributeErrors);
	}

	TArray<UPCGData*> AttributePartition(const UPCGData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArrayView, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		if (const UPCGBasePointData* InPointData = Cast<UPCGBasePointData>(InData))
		{
			return AttributePointPartition(InPointData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		}
		else
		{
			return AttributeParamSpatialPartition(InData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		}
	}

	UPCGData* RemoveDuplicates(const UPCGData* InData, const FPCGAttributePropertySelector& InSelector, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		return RemoveDuplicates(InData, TArrayView<const FPCGAttributePropertySelector>(&InSelector, 1), InOptionalContext, bSilenceMissingAttributeErrors);
	}

	UPCGData* RemoveDuplicates(const UPCGData* InData, const TArrayView<const FPCGAttributePropertySelector>& InSelectorArrayView, FPCGContext* InOptionalContext, bool bSilenceMissingAttributeErrors)
	{
		if (const UPCGBasePointData* InPointData = Cast<UPCGBasePointData>(InData))
		{
			return RemoveDuplicatesPoint(InPointData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		}
		else
		{
			return RemoveDuplicatesParamSpatial(InData, InSelectorArrayView, InOptionalContext, bSilenceMissingAttributeErrors);
		}
	}
}

#undef LOCTEXT_NAMESPACE