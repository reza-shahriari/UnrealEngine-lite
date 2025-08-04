// Copyright Epic Games, Inc. All Rights Reserved.

#include "Helpers/PCGPropertyHelpers.h"

#include "PCGContext.h"
#include "PCGElement.h"
#include "PCGModule.h"
#include "PCGParamData.h"
#include "Helpers/PCGHelpers.h"
#include "Helpers/PCGSettingsHelpers.h"
#include "Metadata/PCGMetadata.h"

#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Field.h"
#include "UObject/UnrealType.h"

#define LOCTEXT_NAMESPACE "PCGPropertyHelpers"

namespace PCGPropertyHelpers
{
	static constexpr uint64 ExcludePropertyFlags = CPF_DisableEditOnInstance;
	static constexpr uint64 IncludePropertyFlags = CPF_BlueprintVisible;

	/**
	* Expands container locations to their contents when the property passed in is an array or a set.
	* This is useful to allow extraction downstream of properties inside of arrays/sets and also to generate the list of addresses/values to look at
	* when extracting the values to the attribute set.
	* 
	* @param InContainerProperty Property that drives the container expansion.
	* @param InContainers        Container locations to expand
	* @param OutContainers       Expanded container locations. Expected to be a different array than InContainers.
	*/
	template<typename ContainerProperty, typename FirstArrayType, typename SecondArrayType>
	void ExpandContainers(const ContainerProperty* InContainerProperty, const FirstArrayType& InContainers, SecondArrayType& OutContainers)
	{
		check(OutContainers.IsEmpty() && InContainerProperty);

		static_assert(std::is_same_v<ContainerProperty, FArrayProperty> || std::is_same_v<ContainerProperty, FSetProperty>);
		using FScriptContainerHelper = std::conditional_t<std::is_same_v<ContainerProperty, FArrayProperty>, FScriptArrayHelper_InContainer, FScriptSetHelper_InContainer>;

		for (const void* Container : InContainers)
		{
			FScriptContainerHelper Helper(InContainerProperty, Container);
			int32 Offset = OutContainers.Num();
			OutContainers.SetNumUninitialized(OutContainers.Num() + Helper.Num());
			for (int32 DynamicIndex = 0; DynamicIndex < Helper.Num(); ++DynamicIndex)
			{
				OutContainers[Offset + DynamicIndex] = Helper.GetElementPtr(DynamicIndex);
			}
		}
	}

	/**
	* Recursive function to go down the property chain to find the property and its container addresses.
	* @param CurrentClass            Struct/Class for the current container
	* @param CurrentName             Property name to look for in the container class.
	* @param NextNames               List of property names to continue extracting at a deeper level.
	* @param bNeedsToBeVisible       Discard properties that are not visibile in Blueprint
	* @param OutContainers           Raw addresses for the containers. Will be written to at each recursive call.
	* @param OptionalContext         Optional context used for logging.
	* @param OptionalObjectTraversed Optional set to store all object that we traversed, to be able to react to those objects changes.
	* @returns                 The last property of the chain (and its container address is in OutContainer)
	*/
	const FProperty* ExtractPropertyChain(const UStruct* CurrentClass, const FName CurrentName, TArrayView<const FString> NextNames, const bool bNeedsToBeVisible, TArray<const void*>& OutContainers, FPCGContext* OptionalContext, TSet<FSoftObjectPath>* OptionalObjectTraversed, bool bQuiet)
	{
		check(CurrentClass);

		const FProperty* Property = nullptr;
		// Try to get the property. If it is coming from a user struct, we need to iterate on all properties because the property name is mangled.
		// There is also a difference between runtime and editor name, as property name in editor can contain invalid characters (like spaces).
		// So if the current name is invalid, also try with sanitized name.
		if (const UUserDefinedStruct* UserDefinedStruct = Cast<UUserDefinedStruct>(CurrentClass))
		{
			Property = FindPropertyInUserDefinedStruct(UserDefinedStruct, CurrentName);
		}
		else
		{
			Property = FindFProperty<FProperty>(CurrentClass, CurrentName);
		}

		if (!Property)
		{
			if (!bQuiet)
			{
				PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("PropertyDoesNotExist", "Property '{0}' does not exist in {1}."), FText::FromName(CurrentName), FText::FromName(CurrentClass->GetFName())), OptionalContext);
			}

			return nullptr;
		}

		// Make sure the property is visible, if requested
		if (bNeedsToBeVisible && (Property->HasAnyPropertyFlags(ExcludePropertyFlags) || !Property->HasAnyPropertyFlags(IncludePropertyFlags)))
		{
			if (!bQuiet)
			{
				PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("PropertyExistsButNotVisible", "Property '{0}' does exist in {1}, but is not visible."), FText::FromName(CurrentName), FText::FromName(CurrentClass->GetFName())), OptionalContext);
			}

			return nullptr;
		}

		auto ExtractContainers = [&OutContainers](UStruct*& NextClass, const auto* ContainerProperty, const FProperty* InnerProperty) -> bool
		{
			bool bPropertyNotExtractable = false;
			const FObjectProperty* InnerObjectProperty = nullptr;
			
			if (const FStructProperty* InnerStructProperty = CastField<FStructProperty>(InnerProperty))
			{
				NextClass = InnerStructProperty->Struct;
			}
			else if ((InnerObjectProperty = CastField<FObjectProperty>(InnerProperty)) != nullptr)
			{
				NextClass = InnerObjectProperty->PropertyClass;
			}
			else
			{
				bPropertyNotExtractable = true;
			}

			// If contents of the container are extractable, do so now by replacing the container entry (e.g. the array/set) with the pointer to its contents
			if (!bPropertyNotExtractable)
			{
				TArray<const void*> Subcontainers;
				PCGPropertyHelpers::ExpandContainers(ContainerProperty, OutContainers, Subcontainers);

				// If we have an object, we also need to apply the object indirection on the Subcontainers
				if (InnerObjectProperty)
				{
					for (const void*& OutContainer : Subcontainers)
					{
						OutContainer = OutContainer ? InnerObjectProperty->GetObjectPropertyValue_InContainer(OutContainer) : nullptr;
					}
				}

				OutContainers = MoveTemp(Subcontainers);
			}
			
			return bPropertyNotExtractable;
		};

		if (!NextNames.IsEmpty())
		{
			UStruct* NextClass = nullptr;
			bool bPropertyNotExtractable = false;

			if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				NextClass = StructProperty->Struct;
				for (const void*& OutContainer : OutContainers)
				{
					OutContainer = OutContainer ? StructProperty->ContainerPtrToValuePtr<void>(OutContainer) : nullptr;
				}
			}
			else if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property))
			{
				NextClass = ObjectProperty->PropertyClass;
				for (const void*& OutContainer : OutContainers)
				{
					OutContainer = OutContainer ? ObjectProperty->GetObjectPropertyValue_InContainer(OutContainer) : nullptr;
				}
			}
			else if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
			{
				bPropertyNotExtractable = ExtractContainers(NextClass, ArrayProperty, ArrayProperty->Inner);
			}
			else if (const FSetProperty* SetProperty = CastField<FSetProperty>(Property))
			{
				bPropertyNotExtractable = ExtractContainers(NextClass, SetProperty, SetProperty->ElementProp);
			}
			else
			{
				bPropertyNotExtractable = true;
			}
			
			if(bPropertyNotExtractable)
			{
				if (!bQuiet)
				{
					PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("PropertyIsNotExtractable", "Property '{0}' does exist in {1}, but is not extractable."), FText::FromName(CurrentName), FText::FromName(CurrentClass->GetFName())), OptionalContext);
				}

				return nullptr;
			}

			return ExtractPropertyChain(NextClass, FName(NextNames[0]), NextNames.RightChop(1), bNeedsToBeVisible, OutContainers, OptionalContext, OptionalObjectTraversed, bQuiet);
		}
		else
		{
			return Property;
		}
	}

	FExtractorParameters::FExtractorParameters(const void* InContainer, const UStruct* InClass, const FPCGAttributePropertySelector& InPropertySelector, FName InOutputAttributeName, bool bInShouldExtract, bool bInPropertyNeedsToBeVisible)
		: Container(InContainer), Class(InClass), OutputAttributeName(InOutputAttributeName), bShouldExtract(bInShouldExtract), bPropertyNeedsToBeVisible(bInPropertyNeedsToBeVisible)
	{
		PropertySelectors.Add(InPropertySelector);
	}

	FExtractorParameters::FExtractorParameters(const void* InContainer, const UStruct* InClass, const TArray<FPCGAttributePropertySelector>& InPropertySelectors, FName InOutputAttributeName, bool bInShouldExtract, bool bInPropertyNeedsToBeVisible)
		: Container(InContainer), Class(InClass), PropertySelectors(InPropertySelectors), OutputAttributeName(InOutputAttributeName), bShouldExtract(bInShouldExtract), bPropertyNeedsToBeVisible(bInPropertyNeedsToBeVisible)
	{}

	FExtractorParameters::FExtractorParameters(const void* InContainer, const UStruct* InClass, const FString& InPropertySelectorString, FName InOutputAttributeName, bool bInShouldExtract, bool bInPropertyNeedsToBeVisible)
		: Container(InContainer), Class(InClass), OutputAttributeName(InOutputAttributeName), bShouldExtract(bInShouldExtract), bPropertyNeedsToBeVisible(bInPropertyNeedsToBeVisible)
	{
		const TArray<FString> PropertySelectorStrings = PCGHelpers::GetStringArrayFromCommaSeparatedList(InPropertySelectorString);

		PropertySelectors.Reserve(PropertySelectorStrings.Num());
		for (int StringIndex = 0; StringIndex < PropertySelectorStrings.Num(); ++StringIndex)
		{
			PropertySelectors.Add(FPCGAttributePropertySelector::CreateSelectorFromString(PropertySelectorStrings[StringIndex]));
		}
	}
}

EPCGMetadataTypes PCGPropertyHelpers::GetMetadataTypeFromProperty(const FProperty* InProperty)
{
	if (!InProperty)
	{
		return EPCGMetadataTypes::Unknown;
	}

	TUniquePtr<IPCGAttributeAccessor> PropertyAccessor = PCGAttributeAccessorHelpers::CreatePropertyAccessor(InProperty);

	return PropertyAccessor.IsValid() ? EPCGMetadataTypes(PropertyAccessor->GetUnderlyingType()) : EPCGMetadataTypes::Unknown;
}

FPropertyBagPropertyDesc PCGPropertyHelpers::CreatePropertyBagDescWithMetadataType(const FName InPropertyName, const EPCGMetadataTypes Type)
{
	EPropertyBagPropertyType ValueType = EPropertyBagPropertyType::Struct;
	const UObject* ValueTypeObject = nullptr;

	switch (Type)
	{
		// Simple Types
		case EPCGMetadataTypes::Float:
			ValueType = EPropertyBagPropertyType::Float;
			break;
		case EPCGMetadataTypes::Double:
			ValueType = EPropertyBagPropertyType::Double;
			break;
		case EPCGMetadataTypes::Integer32:
			ValueType = EPropertyBagPropertyType::Int32;
			break;
		case EPCGMetadataTypes::Integer64:
			ValueType = EPropertyBagPropertyType::Int64;
			break;
		case EPCGMetadataTypes::String:
			ValueType = EPropertyBagPropertyType::String;
			break;
		case EPCGMetadataTypes::Boolean:
			ValueType = EPropertyBagPropertyType::Bool;
			break;
		case EPCGMetadataTypes::Name:
			ValueType = EPropertyBagPropertyType::Name;
			break;

		// Value Type Objects - PropertyType is Struct by default.
		case EPCGMetadataTypes::SoftObjectPath:
			ValueTypeObject = TBaseStructure<FSoftObjectPath>::Get();
			break;
		case EPCGMetadataTypes::SoftClassPath:
			ValueTypeObject = TBaseStructure<FSoftClassPath>::Get();
			break;
		case EPCGMetadataTypes::Vector:
			ValueTypeObject = TBaseStructure<FVector>::Get();
			break;
		case EPCGMetadataTypes::Vector2:
			ValueTypeObject = TBaseStructure<FVector2d>::Get();
			break;
		case EPCGMetadataTypes::Vector4:
			ValueTypeObject = TBaseStructure<FVector4>::Get();
			break;
		case EPCGMetadataTypes::Transform:
			ValueTypeObject = TBaseStructure<FTransform>::Get();
			break;
		case EPCGMetadataTypes::Quaternion:
			ValueTypeObject = TBaseStructure<FQuat>::Get();
			break;
		case EPCGMetadataTypes::Rotator:
			ValueTypeObject = TBaseStructure<FRotator>::Get();
			break;

		default:
			return {};
	}

	return FPropertyBagPropertyDesc(InPropertyName, ValueType, ValueTypeObject);
}

UPCGParamData* PCGPropertyHelpers::ExtractPropertyAsAttributeSet(const PCGPropertyHelpers::FExtractorParameters& Parameters, FPCGContext* OptionalContext, TSet<FSoftObjectPath>* OptionalObjectTraversed, bool bQuiet)
{
	check(Parameters.Container && Parameters.Class);

	// TODO: Remove this after we've removed the deprecated PropertySelector.
	TArrayView<const FPCGAttributePropertySelector> PropertySelectors;

	if (Parameters.PropertySelectors.Num() > 0)
	{
		PropertySelectors = TArrayView<const FPCGAttributePropertySelector>(Parameters.PropertySelectors);
	}
	else
	{
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		PropertySelectors = TArrayView<const FPCGAttributePropertySelector>(&Parameters.PropertySelector, 1);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
	}

	UPCGParamData* ParamData = nullptr;
	UPCGMetadata* Metadata = nullptr;
	TArray<PCGMetadataEntryKey> Entries;
	bool bValidOperation = true;
	bool bIgnoreOutputAttributeName = (PropertySelectors.Num() > 1);

	for(const FPCGAttributePropertySelector& PropertySelector : PropertySelectors)
	{
		TArray<const void*> Containers = { Parameters.Container };
		const FProperty* Property = nullptr;
		const FName PropertyName = PropertySelector.GetName();
		const bool ExtractRoot = (PropertyName == NAME_None);
		// If Name is none, extract the container as-is, using Parameters.Class, otherwise, extract the chain.
		if (!ExtractRoot)
		{
			Property = ExtractPropertyChain(Parameters.Class, PropertyName, PropertySelector.GetExtraNames(), Parameters.bPropertyNeedsToBeVisible, Containers, OptionalContext, OptionalObjectTraversed, bQuiet);
			if (!Property)
			{
				return nullptr;
			}
		}

		const FProperty* OriginalProperty = Property;

		// If the property is an array/set, we will work on the underlying property, and extract each element as an entry in the param data
		const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property);
		const FSetProperty* SetProperty = CastField<FSetProperty>(Property);
		if (ArrayProperty)
		{
			Property = ArrayProperty->Inner;
		}
		else if (SetProperty)
		{
			Property = SetProperty->ElementProp;
		}

		using ExtractablePropertyTuple = TTuple<FString, const FProperty*>;
		TArray<ExtractablePropertyTuple> ExtractableProperties;

		using GetAddressFunc = TFunction<const void* (const void*)>;
		GetAddressFunc AddressFunc;

		// Force extraction if the property is not supported by accessors.
		const bool bShouldExtract = Parameters.bShouldExtract || !PCGAttributeAccessorHelpers::IsPropertyAccessorSupported(Property);

		// Keep track if the extracted property is an object or not
		const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(Property);

		// Special case where the property is a struct/object, that is not supported by our metadata, we will try to break it down to multiple attributes in the resulting param data, if asked.
		if (ExtractRoot || ((Property->IsA<FStructProperty>() || Property->IsA<FObjectProperty>()) && bShouldExtract))
		{
			const UStruct* UnderlyingClass = nullptr;

			if (ExtractRoot)
			{
				UnderlyingClass = Parameters.Class;
				// Identity
				AddressFunc = [](const void* InAddress) { return InAddress; };
			}
			else if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
			{
				UnderlyingClass = StructProperty->Struct;
				AddressFunc = [StructProperty](const void* InAddress) { return InAddress ? StructProperty->ContainerPtrToValuePtr<void>(InAddress) : nullptr; };
			}
			else if (ObjectProperty)
			{
				UnderlyingClass = ObjectProperty->PropertyClass;
				AddressFunc = [ObjectProperty](const void* InAddress) { return InAddress ? ObjectProperty->GetObjectPropertyValue_InContainer(InAddress) : nullptr; };
			}

			check(UnderlyingClass);
			check(!!AddressFunc);

			// Re-use code from overridable params
			// Limit ourselves to not recurse into more structs.
			PCGSettingsHelpers::FPCGGetAllOverridableParamsConfig Config;
			Config.bUseSeed = true;
			Config.bExcludeSuperProperties = true;
			Config.MaxStructDepth = 0;
			// Can only get exposed properties and visible if requested
			if (Parameters.bPropertyNeedsToBeVisible)
			{
				Config.ExcludePropertyFlags = ExcludePropertyFlags;
				Config.IncludePropertyFlags = IncludePropertyFlags;
			}
			TArray<FPCGSettingsOverridableParam> AllChildProperties = PCGSettingsHelpers::GetAllOverridableParams(UnderlyingClass, Config);

			for (const FPCGSettingsOverridableParam& Param : AllChildProperties)
			{
				if (ensure(!Param.PropertiesNames.IsEmpty()))
				{
					const FName ChildPropertyName = Param.PropertiesNames[0];
					if (const FProperty* ChildProperty = UnderlyingClass->FindPropertyByName(ChildPropertyName))
					{
						// We use authored name as attribute name to avoid issue with noisy property names, like in UUserDefinedStructs, where some random number is appended to the property name.
						// By default, it will just return the property name anyway.
						FString AuthoredName = UnderlyingClass->GetAuthoredNameForField(ChildProperty);
						ExtractableProperties.Emplace(std::move(AuthoredName), ChildProperty);
					}
				}
			}
		}
		else
		{
			// For non struct/object, there is just a single property to extract with no shenanigans for address indirection.
			const bool bIsSourceName = Parameters.OutputAttributeName == PCGMetadataAttributeConstants::SourceNameAttributeName || Parameters.OutputAttributeName == PCGMetadataAttributeConstants::SourceAttributeName;
			FString AttributeName;
			if (bIgnoreOutputAttributeName || bIsSourceName)
			{
				// Make sure that the name is the authored name
				if (const UStruct* StructOwner = Property->GetOwnerStruct())
				{
					AttributeName = StructOwner->GetAuthoredNameForField(OriginalProperty);
				}

				if (AttributeName.IsEmpty())
				{
					AttributeName = Property->GetName();
				}
			}
			else
			{
				AttributeName = Parameters.OutputAttributeName.ToString();
			}

			ExtractableProperties.Emplace(std::move(AttributeName), Property);

			// Identity
			AddressFunc = [](const void* InAddress) { return InAddress; };
		}

		if (ExtractableProperties.IsEmpty())
		{
			if (!bQuiet)
			{
				PCGLog::LogErrorOnGraph(LOCTEXT("NoPropertiesFound", "No properties found to extract"), OptionalContext);
			}

			return nullptr;
		}

		// Before we need to compute all the addresses for each entry in our array/set (or just a single entry if there is no array/set)
		TArray<const void*, TInlineAllocator<16>> ExpandedContainers;
		TArrayView<const void*> ElementAddresses;
		if (SetProperty)
		{
			ExpandContainers(SetProperty, Containers, ExpandedContainers);
			ElementAddresses = MakeArrayView(ExpandedContainers);
		}
		else if (ArrayProperty)
		{
			ExpandContainers(ArrayProperty, Containers, ExpandedContainers);
			ElementAddresses = MakeArrayView(ExpandedContainers);
		}
		else
		{
			ElementAddresses = MakeArrayView(Containers);
		}

		if (!ParamData)
		{
			// From there, we should be able to create the data.
			ParamData = NewObject<UPCGParamData>();
			check(!Metadata);
			Metadata = ParamData->MutableMetadata();
			check(Metadata);

			// Allocate entries
			TArray<PCGMetadataEntryKey, TInlineAllocator<16>> ParentEntries;
			ParentEntries.Init(PCGInvalidEntryKey, ElementAddresses.Num());
			Entries = Metadata->AddEntries(ParentEntries);
		}
		else // Pre-existing data, we're expecting the same cardinality
		{
			if (Entries.Num() != ElementAddresses.Num())
			{
				if (!bQuiet)
				{
					PCGLog::LogErrorOnGraph(LOCTEXT("InvalidCardinality", "Unable to extract because some properties are of mismatched sizes"), OptionalContext);
				}

				return nullptr;
			}
		}

		bool bHasWarnNullContainerPtr = false;

		for(int32 ElementAddressIndex = 0; ElementAddressIndex < ElementAddresses.Num(); ++ElementAddressIndex)
		{
			const void* ElementAddress = ElementAddresses[ElementAddressIndex];
			PCGMetadataEntryKey EntryKey = Entries[ElementAddressIndex];

			// Offset the address if needed
			const void* ContainerPtr = AddressFunc(ElementAddress);

			if (!ContainerPtr)
			{
				if (!bHasWarnNullContainerPtr)
				{
					bHasWarnNullContainerPtr = true;
					PCGLog::LogErrorOnGraph(LOCTEXT("NullPtrContainer", "Some resolved objects were not assigned (null pointers), some results are discarded"), OptionalContext);
				}
				
				continue;
			}

			for (ExtractablePropertyTuple& ExtractableProperty : ExtractableProperties)
			{
				FString AttributeNameStr = ExtractableProperty.Get<0>();
				FName AttributeName = NAME_None;
				const FProperty* FinalProperty = ExtractableProperty.Get<1>();

				// Make sure the Attribute name is sanitized, to prevent cases where property names have unsupported characters.
				if (Parameters.bStrictSanitizeOutputAttributeNames)
				{
					AttributeName = MakeObjectNameFromDisplayLabel(AttributeNameStr, NAME_None);
				}
				else
				{
					FPCGMetadataAttributeBase::SanitizeName(AttributeNameStr);
					AttributeName = *AttributeNameStr;
				}

				if (!Metadata->SetAttributeFromDataProperty(AttributeName, EntryKey, ContainerPtr, FinalProperty, /*bCreate=*/ true))
				{
					if (!bQuiet)
					{
						PCGLog::LogErrorOnGraph(FText::Format(LOCTEXT("ErrorCreatingAttribute", "Error while creating an attribute for property '{0}'. Either the property type is not supported by PCG or attribute creation failed."), FText::FromString(FinalProperty->GetName())), OptionalContext);
					}

					bValidOperation = false;
					break;
				}
			}

			if (!bValidOperation)
			{
				break;
			}

			if (bShouldExtract && OptionalObjectTraversed && ObjectProperty)
			{
				const UObject* Object = ObjectProperty->GetPropertyValue_InContainer(ElementAddress);
				if (IsValid(Object))
				{
					OptionalObjectTraversed->Add(Object);
				}
			}
		}
	}

	return bValidOperation ? ParamData : nullptr;
}

const FProperty* PCGPropertyHelpers::FindPropertyInUserDefinedStruct(const UUserDefinedStruct* InStruct, const FName InName)
{
	if (!InStruct)
	{
		return nullptr;
	}

	TArray<FName, TInlineAllocator<2>> NamesToLookFor = { InName };

	if (!InName.IsValidXName())
	{
		NamesToLookFor.Add(MakeObjectNameFromDisplayLabel(InName.ToString(), NAME_None));
	}

	for (TFieldIterator<const FProperty> PropIt(InStruct, EFieldIterationFlags::IncludeSuper); PropIt; ++PropIt)
	{
		const FString PropertyNameStr = InStruct->GetAuthoredNameForField(*PropIt);
		const FName PropertyName = *PropertyNameStr;
		if (NamesToLookFor.Contains(PropertyName)
			|| (!PropertyName.IsValidXName() && NamesToLookFor.Contains(MakeObjectNameFromDisplayLabel(PropertyNameStr, NAME_None))))
		{
			return *PropIt;
		}
	}

	return nullptr;
}

#undef LOCTEXT_NAMESPACE