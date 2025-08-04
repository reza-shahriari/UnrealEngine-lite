// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuCOE/Nodes/CustomizableObjectNodePassThroughTexture.h"

#include "MuCO/CustomizableObjectCustomVersion.h"
#include "MuCOE/EdGraphSchema_CustomizableObject.h"

#define LOCTEXT_NAMESPACE "CustomizableObjectEditor"


void UCustomizableObjectNodePassThroughTexture::BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion)
{
	Super::BackwardsCompatibleFixup(CustomizableObjectCustomVersion);
	
	if (CustomizableObjectCustomVersion == FCustomizableObjectCustomVersion::AddedAnyTextureTypeToPassThroughTextures)
	{
		if (Texture_DEPRECATED)
		{
			if (!PassThroughTexture)
			{
				PassThroughTexture = Texture_DEPRECATED;
			}

			Texture_DEPRECATED = nullptr;
		}
	}
	else if (CustomizableObjectCustomVersion == FCustomizableObjectCustomVersion::UpdatedNodesPinName3)
	{
		if (UEdGraphPin* FoundPin = FindPin(TEXT("Texture")))
		{
			FoundPin->PinName = TEXT("PassThrough Texture");
			FoundPin->PinFriendlyName = LOCTEXT("PassThrough_Image_Pin_Category", "PassThrough Texture");
		}
	}
}


void UCustomizableObjectNodePassThroughTexture::AllocateDefaultPins(UCustomizableObjectNodeRemapPins* RemapPins)
{
	UEdGraphPin* PinImagePin = CustomCreatePinSimple(EGPD_Output, UEdGraphSchema_CustomizableObject::PC_PassThroughImage);
	PinImagePin->bDefaultValueIsIgnored = true;
}


FText UCustomizableObjectNodePassThroughTexture::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	if (PassThroughTexture)
	{
		FFormatNamedArguments Args;
		Args.Add(TEXT("TextureName"), FText::FromString(PassThroughTexture->GetName()));

		return FText::Format(LOCTEXT("Passthrough Texture_Title", "{TextureName}\nPassthrough Texture"), Args);
	}
	else
	{
		return LOCTEXT("Passthrough Texture", "Passthrough Texture");
	}
}


FLinearColor UCustomizableObjectNodePassThroughTexture::GetNodeTitleColor() const
{
	return UEdGraphSchema_CustomizableObject::GetPinTypeColor(UEdGraphSchema_CustomizableObject::PC_PassThroughImage);
}


FText UCustomizableObjectNodePassThroughTexture::GetTooltipText() const
{
	return LOCTEXT("PassThrough_Texture_Tooltip", "Defines a pass-through texture. It will not be modified by Mutable in any way, just referenced as a UE asset. It's much cheaper than a Mutable texture, but you cannot make any operations on it, just switch it.");
}


TObjectPtr<UTexture> UCustomizableObjectNodePassThroughTexture::GetTexture()
{
	return PassThroughTexture;
}


#undef LOCTEXT_NAMESPACE
