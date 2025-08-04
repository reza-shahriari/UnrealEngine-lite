// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EdGraph/EdGraph.h" // IWYU pragma: keep
#include "EdGraph/EdGraphPin.h"
#include "MuCO/CustomizableObject.h"
#include "MuCO/CustomizableObjectPrivate.h"

#include "CustomizableObjectNode.generated.h"

class FArchive;
class FCustomizableObjectGraphEditorToolkit;
class SWidget;
class UCustomizableObjectNodeMacroInstance;
class UCustomizableObjectNodeObject;
class UCustomizableObjectNodeRemapPins;
class UCustomizableObjectNodeRemapPinsByName;
class UCustomizableObjectNodeRemapPinsByPosition;
class IDetailsView;
class ICustomizableObjectEditor;

struct FPropertyChangedEvent;

/** Abstract base class for all pin data. */
UCLASS(Abstract)
class UCustomizableObjectNodePinData : public UObject
{
	GENERATED_BODY()

public:
	UCustomizableObjectNodePinData();

	/** False if inherited types are different. */
	bool operator==(const UCustomizableObjectNodePinData& Other) const;
	
	virtual bool operator!=(const UCustomizableObjectNodePinData& Other) const;

	/** Virtual function used to copy pin data when remapping pins. */
	virtual void Copy(const UCustomizableObjectNodePinData& Other) {}
	
	/** Add backwards compatible code here.
	 * When called, it is guaranteed that all nodes in this graph will have executed the PostLoad function. */
	virtual void BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion) {}

protected:
	/** Virtual function used to perform comparision between different UCustomizableObjectNodePinData inherited types. */
	virtual bool Equals(const UCustomizableObjectNodePinData& Other) const;
};

/** Encapsulation of parameters for the FPostEditChangePropertyDelegate delegate function.
 *
 * The delegate requires the function being a UFUNCTION which requires all parameters to be UCLASS, USTRUC or UENUM.
 */
USTRUCT()
struct FPostEditChangePropertyDelegateParameters
{
	GENERATED_BODY()

	UObject* Node;
	FPropertyChangedEvent* FPropertyChangedEvent;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostEditChangePropertyDelegate, FPostEditChangePropertyDelegateParameters&, Parameters); // Deprecated, use non-dynamic version.
DECLARE_MULTICAST_DELEGATE_TwoParams(FPostEditChangePropertyRegularDelegate, UObject*, FPropertyChangedEvent&);

DECLARE_MULTICAST_DELEGATE(FPostReconstructNodeDelegate);

DECLARE_MULTICAST_DELEGATE(FNodeConnectionListChangedDelegate);

DECLARE_MULTICAST_DELEGATE_OneParam(FPinConnectionListChangedDelegate, UEdGraphPin*);

DECLARE_MULTICAST_DELEGATE(FDestroyNodeDelegate);

using FRemapPinsDelegateParameter = TMap<UEdGraphPin*, UEdGraphPin*>; // Required for the delegate macro.
DECLARE_MULTICAST_DELEGATE_OneParam(FRemapPinsDelegate, const FRemapPinsDelegateParameter&);

/** Base class of all Customizable Object nodes.
 * 
 * The Customizable Object node system is build on top of the following premises. To avoid breaking this system, these premise must be hold!
 * 
 * PREMISES:
 * 1. A node can only be modified when a node is reconstructed (i.e., inside the ReconstructNode, AllocateDefaultPins functions). Exceptionally, a pin can also be created inside the BeginConstruct.
 *    We consider that a node has been modified if one of the following things happen:
 *        - Creation, destruction and modification of pins.
 *        - Modification of any data that would modify any pin.
 */
UCLASS(abstract)
class CUSTOMIZABLEOBJECTEDITOR_API UCustomizableObjectNode : public UEdGraphNode
{
public:
	GENERATED_BODY()

	// UObject interface
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override; // Do work at PostBackwardsCompatibleFixup.

	// UEdGraphNode interface
	virtual void AutowireNewNode(UEdGraphPin* FromPin) override;
	virtual void NodeConnectionListChanged() override;
	virtual void PinConnectionListChanged(UEdGraphPin* Pin) override;
	virtual void DestroyNode() override;
	virtual TSharedPtr<SGraphNode> CreateVisualWidget() override;

	/** Allocates the default pins using the empty remap pins action. Usually called from CreateNode. */
	virtual void AllocateDefaultPins() override final; // Final. Override AllocateDefaultPins(UCustomizableObjectNodeRemapPins* RemapPins) instead

	/** Reconstructs the node using its default remap pins action. */
	virtual void ReconstructNode() override final; // Final. Override ReconstructNode(UCustomizableObjectNodeRemapPins* RemapPins) instead
	virtual void PostInitProperties() override;

	// Own interface
	/** Called at the beginning of the node lifecycle.*/
	virtual void BeginConstruct() {};

	/** Add backwards compatible code here.
	 * When called, it is guaranteed that all nodes in this graph will have executed the PostLoad function. */
	virtual void BackwardsCompatibleFixup(int32 CustomizableObjectCustomVersion);

	/** Reconstructs the node pins using the provided AllocateDefaultPins function.
	 * Safe to call in BackwardsCompatibleFixup.
	 * Notice that this function will not call the node AllocateDefaultPins function nor the ReconstructNode overrides.
	 *
	 * Since this function is used BackwardsCompatibleFixup, it should not have major changes (or they will break existing fixups).
	 * If changes are needed, duplicate this function so old fixups remain unchanged. */
	void FixupReconstructPins(UCustomizableObjectNodeRemapPins* RemapPinsAction, TFunction<void(UCustomizableObjectNode*, UCustomizableObjectNodeRemapPins*)> AllocateDefaultPins);

	/** Add post load work here.
	 * When called, it is guaranteed that all nodes in this graph will have executed the BackwardsCompatibleFixup function.
	 * Notice that no compatibility code should not go here. Use BackwardsCompatibleFixup instead. */
	virtual void PostBackwardsCompatibleFixup() {}

	/** Virtual implementation of RemovePin. Allows to do work before removing a pin.
	 * Use this function instead of RemovePin. RemovePin does not removes possible attached PinData. */
	virtual bool CustomRemovePin(UEdGraphPin& Pin);

	/**
	 * Subclasses should override this to return true and set OutCategory if this node should be
	 * auto-added to the right-click context menu in the graph editor.
	 *
	 * Some nodes are added manually in the graph editor code and don't need to do this.
	 */
	virtual bool ShouldAddToContextMenu(FText& OutCategory) const;

	void GetInputPins(TArray<class UEdGraphPin*>& OutInputPins) const;
	void GetOutputPins(TArray<class UEdGraphPin*>& OutOutputPins) const;
	UEdGraphPin* GetOutputPin(int32 OutputIndex) const;

	virtual bool IsNodeOutDatedAndNeedsRefresh() { return false; }
	virtual FString GetRefreshMessage() const { return "Refresh Node."; }
	void SetRefreshNodeWarning();
	void RemoveWarnings();

	// Used to replace Ids of referenced nodes by their new ids after duplicating the CustomizableObject.
	virtual void UpdateReferencedNodeId(const FGuid& NewGuid) {};

	TSharedPtr<FCustomizableObjectGraphEditorToolkit> GetGraphEditor() const;

	virtual bool ProvidesCustomPinRelevancyTest() const { return false; } // Override and return true if IsPinRelevant is overridden.
	virtual bool IsPinRelevant(const UEdGraphPin*) const { return true; } // Override if default pins allocated with AllocateDefaultPins do not provide enough information.

	struct FAttachedErrorDataView
	{
		TArrayView<const float> UnassignedUVs;
	};

	/**
	 * Check if two pins can be connected.
	 *
	 * @param	InOwnedInputPin	Input pin which belongs to this node.
	 * @param	InOutputPin		Output pin which belongs to another node. If the node connects to iself, it could belong to this node.
	 * @param	bOutIsOtherNodeBlocklisted		Is the other node of a type we are not allowed to connect?
	 * @param	bOutArePinsCompatible		Does InOutputPin pin share the same type as the InOwnedInputPin?
	 * @return	True if the pin types match and if the other node is not one of the Blocklisted types.
	 */
	virtual bool CanConnect(const UEdGraphPin* InOwnedInputPin, const UEdGraphPin* InOutputPin, bool& bOutIsOtherNodeBlocklisted, bool& bOutArePinsCompatible) const;

	// Used during compilation process to cache the node for all LODs, or generate it specifically for each of them.
	virtual bool IsAffectedByLOD() const;

	/** Return an array of tags that this node will enable and apply to its data. This tags can come from the node details or string nodes linked to the "Enable tags" pin. */
	virtual TArray<FString> GetEnableTags(TArray<const UCustomizableObjectNodeMacroInstance*>* MacroContext = nullptr);

	/** Return an array with all tags defined in the node's details panel. Null if none, or of it doesn't apply. */
	virtual TArray<FString>* GetEnableTagsArray();

	/** Return the unique internal tag that can identify this node across all objects.
	* Internal tags are automatically added to some nodes that can be then referred to from other nodes.
	* They are usually shown differently in the UI, to represent them as tags that will be defined in a single node.
	*/
	FString GetInternalTag() const;

	/** Return a valid GUID if a tag represents an internal tag. */
	static FGuid GetInternalTagNodeId(const FString& Tag);
	static bool IsInternalTag(const FString& Tag);

	/** Find the object and node that a given tag refers to, if it belongs to the same CO hierarchy that this node. */
	bool FindNodeForInternalTag(const FString& Tag, UCustomizableObjectNode*& OutNode, UCustomizableObject*& OutObject);

	/** Return a non-unique, non-persistent UI-ready text to represent the interna tag of this node. It includes some node information for the user to identify the tag. */
	virtual FString GetInternalTagDisplayName();

	/** Return a UI-ready text to represent the given tag. For most cases this is just the tag, but if it is an internal tag
	* of any node related to this one (in the same CO hierarchy) a more descriptive (non-persitent) name is build with node information.
	*/
	FString GetTagDisplayName(const FString& Tag);

	// Get the CustomizableObject graph that owns this node
	class UCustomizableObjectGraph* GetCustomizableObjectGraph() const;

	/** If returns true, only a single link can go out of every output pin. False by default. */
	virtual bool IsSingleOutputNode() const;

	/** Wrapper of the pin creation method. Will use the PinName and PinFriendlyname returned by the "UEdGraphSchema_CustomizableObject" */
	UEdGraphPin* CustomCreatePinSimple( EEdGraphPinDirection Direction, const FName& Category, bool bIsArray=false );
	
	// Wrapper of the pin creation method, to cope with multiple Unreal Engine versions
	UEdGraphPin* CustomCreatePin( EEdGraphPinDirection Direction, const FName& Type, const FName& Name, bool bIsArray=false );

	/** Create pin with attached data.
	 *
     * @param PinData Pin data to be saved. */
	UEdGraphPin* CustomCreatePin(EEdGraphPinDirection Direction, const FName& Type, const FName& Name, UCustomizableObjectNodePinData* PinData);

	
	virtual void AddAttachedErrorData(const FAttachedErrorDataView& ) {};
	virtual void ResetAttachedErrorData() {};

	/** 
	 * When creating a new connection, break all previous connections. This method can be overridden.
	 * 
	 * @return false by default.
	 */
	virtual bool ShouldBreakExistingConnections(const UEdGraphPin* InputPin, const UEdGraphPin* OutputPin) const;

	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

	/** Custom post duplicate function. Called at the beginning of duplication, before the nodes have their Guid updated. */
	virtual void BeginPostDuplicate(bool bDuplicateForPIE) {};
	
	/** Gets all non orphan pins this node owns. */
	TArray<UEdGraphPin*> GetAllNonOrphanPins() const;

	/** Gets all orphan pins this node owns. */
	TArray<UEdGraphPin*> GetAllOrphanPins() const;

	/**
	 * Specialization of ReconstructNode UEdGraphNode function.
	 *
	 * @param RemapPinsMode pointer to a remap pins action. Remap pins action which will be used once the node has been reconstructed. Can be null.
	 */
	virtual void ReconstructNode(UCustomizableObjectNodeRemapPins* RemapPinsMode);

	/** Return the LOD which this node belongs to. Return -1 if it is not connected (directly or indirectly) to a LOD pin. */
	int32 GetLOD() const;
	
	/** Set a pin to be hidden or not. */
	void SetPinHidden(UEdGraphPin& Pin, bool bHidden);

	/** Set an array of pins to be hidden or not. 
	 *
	 * @param Pins Input pins. All pins must be non null.
	 */
	void SetPinHidden(const TArray<UEdGraphPin*>& Pins, bool bHidden);

	/** Return true if a pin can be hidden. Override it to have a custom behaviour. */
	virtual bool CanPinBeHidden(const UEdGraphPin& Pin) const;

	/** @return true if the details should spawn a Pin Viewer. */
	virtual bool HasPinViewer() const;

	/** Returns pin custom details. Override if required. */
	virtual TSharedPtr<IDetailsView> CustomizePinDetails(const UEdGraphPin& Pin) const;

	/** Given the pin data, returns its associated pin. */
	UEdGraphPin* GetPin(const UCustomizableObjectNodePinData& PinData);
	
	/** See GetPinData(const FEdGraphPinReference&). */
	UCustomizableObjectNodePinData* GetPinData(const UEdGraphPin &Pin) const;
	
	/** Given a pin which has attached data, get the typed pin data.
	 * @param Pin Pin which has attached data.
	 * @return Pin data already typed. */
	template<typename T>
	T& GetPinData(const UEdGraphPin& Pin) const
	{
		return *CastChecked<T>(PinsDataId[Pin.PinId]);
	}

	/** returs true if this node can be added to a COMacro Graph */
	virtual bool IsNodeSupportedInMacros() const;

	/** Returns true if this node is inside a Macro Graph */
	bool IsInMacro() const;

	/** If a node has a static string pin, this function determines if it should construct an input text widget next to the pin or not. */
	virtual bool CreateStaticStringPinWidget() const;
	
	UPROPERTY()
	FPostEditChangePropertyDelegate PostEditChangePropertyDelegate;

	FPostEditChangePropertyRegularDelegate PostEditChangePropertyRegularDelegate;

	FPostReconstructNodeDelegate PostReconstructNodeDelegate;

	FNodeConnectionListChangedDelegate NodeConnectionListChangedDelegate;

	FPinConnectionListChangedDelegate PinConnectionListChangedDelegate;

	FDestroyNodeDelegate DestroyNodeDelegate;

	FRemapPinsDelegate RemapPinsDelegate;

protected:
	/**
	 * Specialization of AllocateDefaultPins UEdGraphNode function. Override.
	 * 
	 * Given the node context (this), create all the required pins.
	 *
	 * @param RemapPins remap pins action. Use this object to save any information required to later perfrom the remap pins action. Can be null.
	 */
	virtual void AllocateDefaultPins(UCustomizableObjectNodeRemapPins* RemapPins) {};

	/**
	 * Creates and returns the node default node remap pins action.
	 * 
	 * By default all nodes remap by name. Override if a node requires a diferent default remap pins action.
	 */
	virtual UCustomizableObjectNodeRemapPins* CreateRemapPinsDefault() const;

	/**
	 * Creates and returns a remap pins by name action. 
	 * 
	 * Override a if a node requires an specialized remap pins by name.
	 */
	virtual UCustomizableObjectNodeRemapPinsByName* CreateRemapPinsByName() const;

	/**
	 * Creates and returns a remap pins by position action.
	 *
	 * Override a if a node requires an specialized remap pins by position.
	 */
	virtual UCustomizableObjectNodeRemapPinsByPosition* CreateRemapPinsByPosition() const;
	
	/** Allows to perform work when remapping a pin.
	  * Copies pin data from old pin to new pin. Keeps the id of the new pin. */
	virtual void RemapPin(UEdGraphPin& NewPin, const UEdGraphPin& OldPin);

	/** Allows to perform work when remapping a pin. */
	virtual void RemapPins(const TMap<UEdGraphPin*, UEdGraphPin*>& PinsToRemap);

	/*** Allows to perform work when remapping the pin data. */
	virtual void RemapPinsData(const TMap<UEdGraphPin*, UEdGraphPin*>& PinsToRemap);
	
	/** Attach data to a given pin. */
	void AddPinData(const UEdGraphPin& Pin, UCustomizableObjectNodePinData& PinData);

public:
	/** Return true to mark this node as experimental. */
	virtual bool IsExperimental() const;
	
private:
	/** Data attached to a given pin. Not all pins contain data.
	 * Data must be eliminated once the pins is removed. This is done automatically on CustomRemovePin.
	 * Do not use RemovePin since it will not remove the data. */
	UPROPERTY()
	TMap<FGuid, TObjectPtr<UCustomizableObjectNodePinData>> PinsDataId;

	// Deprecated properties in favour of PinsDataId
	UPROPERTY()
	TMap<FEdGraphPinReference, TObjectPtr<UCustomizableObjectNodePinData>> PinsData_DEPRECATED;
};


/** Return the specified node located in another Customizable Object graph.
 *
 * @param Customizable Object object to look for the node. If nulltpr, it returns nullptr.
 * @param NodeGuid node guid. If invalid, it returns nullptr.
 * 
 * Return nullptr if not found.
 */
template<class FNodeType>
FNodeType* GetCustomizableObjectExternalNode(UCustomizableObject* Object, const FGuid& NodeGuid)
{
	FNodeType* Result = nullptr;

	if (Object && Object->GetPrivate()->GetSource() && NodeGuid.IsValid())
	{
		TArray<FNodeType*> GroupNodes;
		Object->GetPrivate()->GetSource()->GetNodesOfClass<FNodeType>(GroupNodes);

		for (FNodeType* GroupNode : GroupNodes)
		{
			if (NodeGuid == GroupNode->NodeGuid)
			{
				Result = GroupNode;
				break;
			}
		}
	}

	return Result;
}
