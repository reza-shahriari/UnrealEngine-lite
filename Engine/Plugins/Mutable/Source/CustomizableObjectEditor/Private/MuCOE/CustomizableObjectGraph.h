// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EdGraph/EdGraph.h"

#include "CustomizableObjectGraph.generated.h"

class UObject;

UCLASS()
class CUSTOMIZABLEOBJECTEDITOR_API UCustomizableObjectGraph : public UEdGraph
{
public:
	GENERATED_BODY()

	UCustomizableObjectGraph();

	// UObject Interface
	virtual void PostLoad() override;
	void PostRename(UObject * OldOuter, const FName OldName) override;

	// Own Interface
	void NotifyNodeIdChanged(const FGuid& OldGuid, const FGuid& NewGuid);

	FGuid RequestNotificationForNodeIdChange(const FGuid& OldGuid, const FGuid& NodeToNotifyGuid);

	void PostDuplicate(bool bDuplicateForPIE) override;

	/** Adds the necessary nodes for a CO to work */
	void AddEssentialGraphNodes();

	void BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion);

	void PostBackwardsCompatibleFixup();

	bool IsMacro() const;

private:

	// Request Node Id Update Map
	TMap<FGuid, TSet<FGuid>> NodesToNotifyMap;

	// Guid map with the key beeing the old Guid and the Value the new one, filled after duplicating COs
	TMap<FGuid, FGuid> NotifiedNodeIdsMap;
};

