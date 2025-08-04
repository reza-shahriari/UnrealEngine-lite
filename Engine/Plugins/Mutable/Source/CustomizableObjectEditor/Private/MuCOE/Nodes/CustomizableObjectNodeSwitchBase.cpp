// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuCOE/Nodes/CustomizableObjectNodeSwitchBase.h"

#include "MuCO/CustomizableObjectCustomVersion.h"
#include "MuCOE/EdGraphSchema_CustomizableObject.h"
#include "MuCOE/GraphTraversal.h"
#include "MuCOE/Nodes/CustomizableObjectNodeEnumParameter.h"

class UCustomizableObjectNodeRemapPins;

#define LOCTEXT_NAMESPACE "CustomizableObjectEditor"

void UCustomizableObjectNodeSwitchBase::ReloadEnumParam()
{
	ReloadingElementsNames.Empty();

	// Get the names of the enum parameter for the element pins
	if (const UEdGraphPin* EnumPin = SwitchParameter())
	{
		if (const UEdGraphPin* LinkedPin = FollowInputPin(*EnumPin))
		{
			if (UCustomizableObjectNodeEnumParameter* EnumNode = Cast<UCustomizableObjectNodeEnumParameter>(LinkedPin->GetOwningNode()))
			{
				for (int i = 0; i < EnumNode->Values.Num(); ++i)
				{
					ReloadingElementsNames.Add(EnumNode->Values[i].Name);
				}
			}
		}
	}
}


void UCustomizableObjectNodeSwitchBase::ReconstructNode(UCustomizableObjectNodeRemapPins* RemapPins)
{
	ReloadEnumParam();

	Super::ReconstructNode(RemapPins);
}


UEdGraphPin* UCustomizableObjectNodeSwitchBase::OutputPin() const
{
	return OutputPinReference.Get();
}


UEdGraphPin* UCustomizableObjectNodeSwitchBase::SwitchParameter() const
{
	return SwitchParameterPinReference.Get();
}


void UCustomizableObjectNodeSwitchBase::PinConnectionListChanged(UEdGraphPin* Pin)
{
	if (Pin == SwitchParameter())
	{
		LinkPostEditChangePropertyDelegate(*Pin);
	}

	Super::PinConnectionListChanged(Pin);
}


void UCustomizableObjectNodeSwitchBase::AllocateDefaultPins(UCustomizableObjectNodeRemapPins* RemapPins)
{
	const UEdGraphSchema_CustomizableObject* Schema = GetDefault<UEdGraphSchema_CustomizableObject>();

	UEdGraphPin* OutputPin = CustomCreatePin(EGPD_Output, GetCategory(), FName(UEdGraphSchema_CustomizableObject::GetPinCategoryName(GetCategory()).ToString()));
	OutputPin->bDefaultValueIsIgnored = true;
	OutputPin->PinFriendlyName = UEdGraphSchema_CustomizableObject::GetPinCategoryFriendlyName(GetCategory());

	OutputPinReference = FEdGraphPinReference(OutputPin);

	UEdGraphPin* SwitchParameterPin = CustomCreatePin(EGPD_Input, Schema->PC_Enum, FName(TEXT("Switch Parameter")));
	SwitchParameterPin->bDefaultValueIsIgnored = true;
	SwitchParameterPin->SetOwningNode(this);

	SwitchParameterPinReference = FEdGraphPinReference(SwitchParameterPin);
	
	for (int32 LayerIndex = 0; LayerIndex < ReloadingElementsNames.Num(); ++LayerIndex)
	{
		FString PinName = GetPinPrefix(LayerIndex);
		UEdGraphPin* InputPin = CustomCreatePin(EGPD_Input, GetCategory(), FName(*PinName));
		FString PinDisplayName = ReloadingElementsNames[LayerIndex].IsEmpty() ? PinName : ReloadingElementsNames[LayerIndex];
		const FString CategoryFriendlyName = UEdGraphSchema_CustomizableObject::GetPinCategoryFriendlyName(GetCategory()).ToString();
		InputPin->PinFriendlyName = FText::FromString(CategoryFriendlyName + TEXT(" ") + FString::FromInt(LayerIndex));
		InputPin->bDefaultValueIsIgnored = true;
		InputPin->SetOwningNode(this);
	}
}


void UCustomizableObjectNodeSwitchBase::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	if (Ar.CustomVer(FCustomizableObjectCustomVersion::GUID) < FCustomizableObjectCustomVersion::BugPinsSwitch)
	{
		SwitchParameterPinReference = FindPin(TEXT("Switch Parameter"));	
	}
}


FText UCustomizableObjectNodeSwitchBase::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return FText::Format(LOCTEXT("Switch_Title", "{0} Switch"), UEdGraphSchema_CustomizableObject::GetPinCategoryFriendlyName(GetCategory()));
}


FLinearColor UCustomizableObjectNodeSwitchBase::GetNodeTitleColor() const
{
	return UEdGraphSchema_CustomizableObject::GetPinTypeColor(GetCategory());
}


FText UCustomizableObjectNodeSwitchBase::GetTooltipText() const
{
	return LOCTEXT("Switch_Tooltip", "Change the resulting value depending on what is currently chosen among a predefined amount of sources.");
}


void UCustomizableObjectNodeSwitchBase::EnumParameterPostEditChangeProperty(FPostEditChangePropertyDelegateParameters& Parameters)
{
	if (const UEdGraphPin* SwitchPin = SwitchParameter())
	{
		if (const UEdGraphPin* ConnectedPin = FollowInputPin(*SwitchPin); ConnectedPin && ConnectedPin->GetOwningNode() == Parameters.Node)
		{
			Super::ReconstructNode();
		}
		else if (UCustomizableObjectNode* EnumNode = Cast<UCustomizableObjectNode>(Parameters.Node))
		{
			EnumNode->PostEditChangePropertyDelegate.RemoveDynamic(this, &UCustomizableObjectNodeSwitchBase::EnumParameterPostEditChangeProperty);
		}
	}
 }


int32 UCustomizableObjectNodeSwitchBase::GetNumElements() const
{
	int32 Count = 0;
	for (UEdGraphPin* Pin : GetAllNonOrphanPins())
	{
		if (Pin->GetName().StartsWith(GetPinPrefix()))
		{
			Count++;
		}
	}

	return Count;
}


FString UCustomizableObjectNodeSwitchBase::GetPinPrefix(int32 Index) const
{
	return GetPinPrefix() + FString::FromInt(Index) + TEXT(" ");
}


void UCustomizableObjectNodeSwitchBase::PostPasteNode()
{
	Super::PostPasteNode();

	if (UEdGraphPin* SwitchPin = SwitchParameter())
	{
		LinkPostEditChangePropertyDelegate(*SwitchPin);
	}
}


void UCustomizableObjectNodeSwitchBase::BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion)
{
	Super::BackwardsCompatibleFixup(CustomizableObjectCustomVersion);

	if (CustomizableObjectCustomVersion == FCustomizableObjectCustomVersion::ChangedSwitchNodesInputPinsFriendlyNames)
	{
		const FString CategoryFriendlyName = UEdGraphSchema_CustomizableObject::GetPinCategoryFriendlyName(GetCategory()).ToString();

		for (int32 ElementIndex = 0; ElementIndex < GetNumElements(); ++ElementIndex)
		{
			UEdGraphPin* ElementPin = GetElementPin(ElementIndex);

			if (ElementPin->PinFriendlyName.ToString().StartsWith(TEXT("Material")))
			{
				ElementPin->PinFriendlyName = FText::FromString(CategoryFriendlyName + TEXT(" ") + FString::FromInt(ElementIndex));
			}
		}
	}
}


void UCustomizableObjectNodeSwitchBase::PostBackwardsCompatibleFixup()
{
	Super::PostBackwardsCompatibleFixup();

	if (UEdGraphPin* SwitchPin = SwitchParameter())
	{
		LinkPostEditChangePropertyDelegate(*SwitchPin);
	}
}


void UCustomizableObjectNodeSwitchBase::LinkPostEditChangePropertyDelegate(const UEdGraphPin& Pin)
{
	if (LastNodeEnumParameterConnected.IsValid())
	{
		LastNodeEnumParameterConnected->PostEditChangePropertyDelegate.RemoveDynamic(this, &UCustomizableObjectNodeSwitchBase::EnumParameterPostEditChangeProperty);
	}

	if (const UEdGraphPin* ConnectedPin = FollowInputPin(Pin))
	{
		LastNodeEnumParameterConnected = Cast<UCustomizableObjectNode>(ConnectedPin->GetOwningNode());

		if (LastNodeEnumParameterConnected.IsValid())
		{
			LastNodeEnumParameterConnected->PostEditChangePropertyDelegate.AddUniqueDynamic(this, &UCustomizableObjectNodeSwitchBase::EnumParameterPostEditChangeProperty);
		}
	}

	Super::ReconstructNode();
}


FString UCustomizableObjectNodeSwitchBase::GetOutputPinName() const
{
	return FString();
}


FString UCustomizableObjectNodeSwitchBase::GetPinPrefix() const
{
	return UEdGraphSchema_CustomizableObject::GetPinCategoryName(GetCategory()).ToString() + " ";
}


#undef LOCTEXT_NAMESPACE

