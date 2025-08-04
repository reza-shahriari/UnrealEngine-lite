// Copyright Epic Games, Inc. All Rights Reserved.


#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "K2Node.h"
#include "UObject/SoftObjectPath.h"
#include "EdGraphSchema_K2.h"
#include "RigVMModel/RigVMGraph.h"
#include "EdGraph/RigVMEdGraphNode.h"
#include "ControlRigGraphNode.generated.h"

class FBlueprintActionDatabaseRegistrar;
class UEdGraph;
struct FSlateIcon;
class UControlRigBlueprint;
#if WITH_RIGVMLEGACYEDITOR
struct FSearchTagDataPair;
#endif

/** Base class for animation ControlRig-related nodes */
UCLASS()
class CONTROLRIGDEVELOPER_API UControlRigGraphNode : public URigVMEdGraphNode
{
	GENERATED_BODY()

	friend class FControlRigGraphNodeDetailsCustomization;
	friend class FControlRigBlueprintCompilerContext;
	friend class UControlRigGraph;
	friend class UControlRigGraphSchema;
	friend class UControlRigBlueprint;
	friend class FControlRigGraphTraverser;
	friend class FControlRigGraphPanelPinFactory;
	friend class SControlRigGraphPinCurveFloat;

public:

	UControlRigGraphNode();

#if WITH_EDITOR
#if WITH_RIGVMLEGACYEDITOR
	void AddPinSearchMetaDataInfo(const UEdGraphPin* Pin, TArray<FSearchTagDataPair>& OutTaggedMetaData) const;
#else
	void AddRigVMPinSearchMetaDataInfo(const UEdGraphPin* Pin, TArray<UBlueprintExtension::FSearchTagDataPair>& OutTaggedMetaData) const;
#endif
#endif

private:

	friend class SRigVMGraphNode;
	friend class FControlRigArgumentLayout;
	friend class FControlRigGraphDetails;
	friend class UControlRigTemplateNodeSpawner;
	friend class UControlRigArrayNodeSpawner;
	friend class UControlRigIfNodeSpawner;
	friend class UControlRigSelectNodeSpawner;
};
