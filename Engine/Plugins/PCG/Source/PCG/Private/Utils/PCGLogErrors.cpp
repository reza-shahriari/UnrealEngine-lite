// Copyright Epic Games, Inc. All Rights Reserved.

#include "Utils/PCGLogErrors.h"

#define LOCTEXT_NAMESPACE "PCGLog"

namespace PCGLog::InputOutput
{
	namespace Format
	{
		const FText InvalidInputData = LOCTEXT("InvalidInputData", "Invalid input data.");

		const FTextFormat TypedInputNotFound = LOCTEXT("TypedInputNotFound", "Data of type {0} not found on pin '{1}'.");
		const FTextFormat FirstInputOnly = LOCTEXT("FirstInputOnly", "Multiple inputs found on single-input pin '{0}'. Only the first will be selected.");
		const FTextFormat InvalidCardinality = LOCTEXT("InvalidCardinality", "Invalid cardinality among pins '{0}' and '{1}'. They must match 1:1, N:1, or N:N.");
	}

	// Warnings
	void LogTypedDataNotFoundWarning(EPCGDataType DataType, const FName PinLabel, const FPCGContext* InContext)
	{
		const UEnum* PCGDataTypeEnum = StaticEnum<EPCGDataType>();
		const FText TypeText = PCGDataTypeEnum ? PCGDataTypeEnum->GetDisplayNameTextByValue(static_cast<int64>(DataType)) : LOCTEXT("UnknownDataType", "Unknown");
		LogWarningOnGraph(FText::Format(Format::TypedInputNotFound, FText::FromName(PinLabel), TypeText), InContext);
	}

	void LogFirstInputOnlyWarning(const FName PinLabel, const FPCGContext* InContext)
	{
		LogWarningOnGraph(FText::Format(Format::FirstInputOnly, FText::FromName(PinLabel)), InContext);
	}

	// Errors
	void LogInvalidInputDataError(const FPCGContext* InContext)
	{
		LogErrorOnGraph(Format::InvalidInputData, InContext);
	}

	void LogInvalidCardinalityError(const FName SourcePinLabel, const FName TargetPinLabel, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::InvalidCardinality, FText::FromName(SourcePinLabel), FText::FromName(TargetPinLabel)), InContext);
	}
}

namespace PCGLog::Metadata
{
	namespace Format
	{
		const FTextFormat CreateAccessorFailure = LOCTEXT("CreateAccessorFailure", "Could not create accessor. Attribute '{0}' was not found.");
		const FTextFormat CreateAttributeFailure = LOCTEXT("CreateAttributeFailure", "Could not create attribute '{0}' of type '{1}'.");
		const FTextFormat GetAttributeFailure = LOCTEXT("GetAttributeFailure", "Could not retrieve attribute '{0}' value.");
		const FTextFormat GetTypedAttributeFailure = LOCTEXT("GetTypedAttributeFailure", "Could not retrieve attribute '{0}' value. Expected type: {1}, Actual Type: {2}.");
		const FTextFormat GetTypedAttributeFailureNoAccessor = LOCTEXT("GetTypedAttributeFailureNoAccessor", "Could not retrieve attribute '{0}' value of type: '{1}.");
		const FTextFormat SetTypedAttributeFailure = LOCTEXT("SetTypedAttributeFailure", "Could not set value in attribute '{0}'. Attribute type: {1}, Value Type: {2}.");
		const FTextFormat SetTypedAttributeFailureNoAccessor = LOCTEXT("SetTypedAttributeFailureNoAccessor", "Could not set value in attribute '{0}' value of type: '{1}.");
		const FTextFormat IncomparableTypesFailure = LOCTEXT("IncomparableTypesFailure", "Attributes '{0}' and '{1}' are incomparable. Ensure they are either of the same or compatible types.");
		const FTextFormat InvalidMetadataDomain = LOCTEXT("InvalidMetadataDomain", "Metadata domain {0} is invalid for this data.");
		const FText InvalidMetadata = LOCTEXT("InvalidMetadata", "Metadata is unsupported or invalid for this data.");
	}

	void LogFailToCreateAccessorError(const FPCGAttributePropertySelector& Selector, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::CreateAccessorFailure, Selector.GetDisplayText()), InContext);
	}

	void LogInvalidMetadata(const FPCGContext* InContext)
	{
		LogErrorOnGraph(Format::InvalidMetadata, InContext);
	}

	void LogInvalidMetadataDomain(const FPCGAttributePropertySelector& Selector, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::InvalidMetadataDomain, FText::FromName(Selector.GetDomainName())), InContext);
	}

	void LogFailToGetAttributeError(const FText& AttributeName, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::GetAttributeFailure, AttributeName), InContext);
	}

	void LogFailToGetAttributeError(FName AttributeName, const FPCGContext* InContext)
	{
		LogFailToGetAttributeError(FText::FromName(AttributeName), InContext);
	}

	void LogFailToGetAttributeError(const FPCGAttributePropertySelector& Selector, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::GetAttributeFailure, Selector.GetDisplayText()), InContext);
	}

	void LogIncomparableAttributesError(const FPCGAttributePropertySelector& FirstSelector, const FPCGAttributePropertySelector& SecondSelector, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::IncomparableTypesFailure, FirstSelector.GetDisplayText(), SecondSelector.GetDisplayText()), InContext);
	}
}

namespace PCGLog::Parsing
{
	namespace Format
	{
		const FText EmptyExpression = LOCTEXT("EmptyExpression", "Empty expression in parsed string.");

		const FTextFormat InvalidCharacter = LOCTEXT("InvalidCharacter", "Invalid character in parsed string: '{0}'.");
		const FTextFormat InvalidExpression = LOCTEXT("InvalidExpression", "Invalid expression in parsed string: '{0}'.");
	}

	// Warnings
	void LogEmptyExpressionWarning(const FPCGContext* InContext)
	{
		LogWarningOnGraph(Format::EmptyExpression, InContext);
	}

	// Errors
	void LogInvalidCharacterInParsedStringError(const FStringView& ParsedString, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::InvalidCharacter, FText::FromStringView(ParsedString)), InContext);
	}

	void LogInvalidExpressionInParsedStringError(const FStringView& ParsedString, const FPCGContext* InContext)
	{
		LogErrorOnGraph(FText::Format(Format::InvalidExpression, FText::FromStringView(ParsedString)), InContext);
	}
}

namespace PCGLog::Component
{
	namespace Format
	{
		const FText ComponentAttachmentFailed = LOCTEXT("ComponentAttachmentFailed", "Failed to attach the component, check the logs.");
	}
	
	void LogComponentAttachmentFailedWarning(const FPCGContext* InContext)
	{
		LogWarningOnGraph(Format::ComponentAttachmentFailed, InContext);
	}
}

namespace PCGLog::Settings
{
	namespace Format
	{
		const FTextFormat InvalidPreconfiguration = LOCTEXT("InvalidPreconfiguration", "Invalid preconfiguration index '{0}' for node settings '{1}'. Default settings will be used.");
		const FTextFormat InvalidConversion = LOCTEXT("InvalidConversion", "Invalid conversion for preconfiguration index '{0}' for node settings '{1}'. Reason: {2}");
	}

	// Warnings
	void LogInvalidPreconfigurationWarning(const int32 PreconfigurationIndex, const FText& NodeTitle)
	{
		LogWarningOnGraph(FText::Format(Format::InvalidPreconfiguration, FText::AsNumber(PreconfigurationIndex), NodeTitle));
	}

	// Errors
	void LogInvalidConversionError(const int32 PreconfigurationIndex, const FText& NodeTitle, const FText& Reason)
	{
		LogErrorOnGraph(FText::Format(Format::InvalidConversion, FText::AsNumber(PreconfigurationIndex), NodeTitle, Reason));
	}
}

namespace PCGLog::Landscape
{
	namespace Format
	{
		const FText LandscapeCacheNotAvailable = LOCTEXT("LandscapeCacheNotAvailableInPIEOrCookedBuilds", "PCG Landscape cache (on the PCG World Actor) is not set to be serialized and will not work in non-editor modes.");
	}

	void LogLandscapeCacheNotAvailableError(const FPCGContext* InContext)
	{
		LogErrorOnGraph(Format::LandscapeCacheNotAvailable, InContext);
	}
}

#undef LOCTEXT_NAMESPACE
