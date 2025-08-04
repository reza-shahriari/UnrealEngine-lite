// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IDetailCustomization.h"
#include "IDetailCustomNodeBuilder.h"
#include "UObject/WeakObjectPtr.h"
#include "EdGraph/RigVMEdGraph.h"
#include "RigVMBlueprint.h"
#include "Editor/RigVMNewEditor.h"
#include "Editor/RigVMLegacyEditor.h"
#include "SGraphPin.h"
#include "Widgets/SRigVMGraphNode.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Editor/RigVMDetailsViewWrapperObject.h"
#include "IDetailPropertyExtensionHandler.h"
#include "EdGraph/RigVMEdGraphSchema.h"
#include "SAdvancedTransformInputBox.h"
#include "Widgets/SRigVMGraphPinNameListValueWidget.h"
#include "Widgets/SRigVMLogWidget.h"
#include "HAL/PlatformApplicationMisc.h"

class IDetailLayoutBuilder;
class FRigVMGraphDetailCustomizationImpl;

class RIGVMEDITOR_API FRigVMFunctionArgumentGroupLayout : public IDetailCustomNodeBuilder, public TSharedFromThis<FRigVMFunctionArgumentGroupLayout>
{
public:
	FRigVMFunctionArgumentGroupLayout(
		const TWeakObjectPtr<URigVMGraph>& InGraph,
		const TWeakInterfacePtr<IRigVMClientHost>& InRigVMClientHost,
		const TWeakPtr<IRigVMEditor>& InEditor,
		bool bInputs);
	virtual ~FRigVMFunctionArgumentGroupLayout();

private:
	/** IDetailCustomNodeBuilder Interface*/
	virtual void SetOnRebuildChildren(FSimpleDelegate InOnRegenerateChildren) override { OnRebuildChildren = InOnRegenerateChildren; }
	virtual void GenerateHeaderRowContent(FDetailWidgetRow& NodeRow) override {}
	virtual void GenerateChildContent(IDetailChildrenBuilder& ChildrenBuilder) override;
	virtual void Tick(float DeltaTime) override {}
	virtual bool RequiresTick() const override { return false; }
	virtual FName GetName() const override { return NAME_None; }
	virtual bool InitiallyCollapsed() const override { return false; }

private:

	void HandleModifiedEvent(ERigVMGraphNotifType InNotifType, URigVMGraph* InGraph, UObject* InSubject);

	TWeakObjectPtr<URigVMGraph> GraphPtr;
	TWeakInterfacePtr<IRigVMClientHost> WeakRigVMClientHost;
	TWeakPtr<IRigVMEditor> RigVMEditorPtr;
	bool bIsInputGroup;
	FSimpleDelegate OnRebuildChildren;
};

class RIGVMEDITOR_API FRigVMFunctionArgumentLayout : public IDetailCustomNodeBuilder, public TSharedFromThis<FRigVMFunctionArgumentLayout>
{
public:

	FRigVMFunctionArgumentLayout(
		const TWeakObjectPtr<URigVMPin>& InPin,
		const TWeakObjectPtr<URigVMGraph>& InGraph, 
		const TWeakInterfacePtr<IRigVMClientHost>& InRigVMClientHost,
		const TWeakPtr<IRigVMEditor>& InEditor)
		: PinPtr(InPin)
		, GraphPtr(InGraph)
		, WeakRigVMClientHost(InRigVMClientHost)
		, RigVMEditorPtr(InEditor)
		, NameValidator(nullptr, InGraph.Get(), InPin->GetFName()) // TODO check if missing Blueprint affects name validation
	{}

private:

	/** IDetailCustomNodeBuilder Interface*/
	virtual void GenerateHeaderRowContent(FDetailWidgetRow& NodeRow) override;
	virtual void GenerateChildContent(IDetailChildrenBuilder& ChildrenBuilder) override;
	virtual void Tick(float DeltaTime) override {}
	virtual bool RequiresTick() const override { return false; }
	virtual FName GetName() const override { return PinPtr.Get()->GetFName(); }
	virtual bool InitiallyCollapsed() const override { return true; }

private:

	/** Determines if this pin should not be editable */
	bool ShouldPinBeReadOnly(bool bIsEditingPinType = false) const;

	/** Determines if editing the pins on the node should be read only */
	bool IsPinEditingReadOnly(bool bIsEditingPinType = false) const;

	/** Determines if an argument can be moved up or down */
	bool CanArgumentBeMoved(bool bMoveUp) const;

	/** Callbacks for all the functionality for modifying arguments */
	void OnRemoveClicked();
	FReply OnArgMoveUp();
	FReply OnArgMoveDown();

	FText OnGetArgNameText() const;
	FText OnGetArgToolTipText() const;
	void OnArgNameTextCommitted(const FText& NewText, ETextCommit::Type InTextCommit);

	FEdGraphPinType OnGetPinInfo() const;
	void PinInfoChanged(const FEdGraphPinType& PinType);
	void OnPrePinInfoChange(const FEdGraphPinType& PinType);

private:

	/** The argument pin that this layout reflects */
	TWeakObjectPtr<URigVMPin> PinPtr;
	
	/** The target graph that this argument is on */
	TWeakObjectPtr<URigVMGraph> GraphPtr;

	/** The asset host we are editing */
	TWeakInterfacePtr<IRigVMClientHost> WeakRigVMClientHost;

	/** The editor we are editing */
	TWeakPtr<IRigVMEditor> RigVMEditorPtr;

	/** Holds a weak pointer to the argument name widget, used for error notifications */
	TWeakPtr<SEditableTextBox> ArgumentNameWidget;

	/** The validator to check if a name for an argument is valid */
	FRigVMLocalVariableNameValidator NameValidator;
};

class RIGVMEDITOR_API FRigVMFunctionArgumentDefaultNode : public IDetailCustomNodeBuilder, public TSharedFromThis<FRigVMFunctionArgumentDefaultNode>
{
public:
	FRigVMFunctionArgumentDefaultNode(
		const TWeakObjectPtr<URigVMGraph>& InGraph,
		const TWeakInterfacePtr<IRigVMClientHost>& InClientHost
	);
	virtual ~FRigVMFunctionArgumentDefaultNode();

private:
	/** IDetailCustomNodeBuilder Interface*/
	virtual void SetOnRebuildChildren(FSimpleDelegate InOnRegenerateChildren) override { OnRebuildChildren = InOnRegenerateChildren; }
	virtual void GenerateHeaderRowContent(FDetailWidgetRow& NodeRow) override {}
	virtual void GenerateChildContent(IDetailChildrenBuilder& ChildrenBuilder) override;
	virtual void Tick(float DeltaTime) override {}
	virtual bool RequiresTick() const override { return false; }
	virtual FName GetName() const override { return NAME_None; }
	virtual bool InitiallyCollapsed() const override { return false; }

private:

	void OnGraphChanged(const FEdGraphEditAction& InAction);
	void HandleModifiedEvent(ERigVMGraphNotifType InNotifType, URigVMGraph* InGraph, UObject* InSubject);

	TWeakObjectPtr<URigVMGraph> GraphPtr;
	TWeakObjectPtr<URigVMEdGraph> EdGraphOuterPtr;
	TWeakInterfacePtr<IRigVMClientHost> WeakRigVMClientHost;
	FSimpleDelegate OnRebuildChildren;
	TSharedPtr<SRigVMGraphNode> OwnedNodeWidget;
	FDelegateHandle GraphChangedDelegateHandle;
};

/** Customization for editing rig vm graphs */
class RIGVMEDITOR_API FRigVMGraphDetailCustomization : public IDetailCustomization
{
public:
	/** Makes a new instance of this detail layout class for a specific detail view requesting it */
	static TSharedRef<IDetailCustomization> MakeInstance(TSharedPtr<IRigVMEditor> InBlueprintEditor, const UClass* InExpectedBlueprintClass);
	FRigVMGraphDetailCustomization(TSharedPtr<IRigVMEditor> RigVMigEditor, URigVMBlueprint* RigVMBlueprint);
#if WITH_RIGVMLEGACYEDITOR
	static TSharedPtr<IDetailCustomization> MakeLegacyInstance(TSharedPtr<IBlueprintEditor> InBlueprintEditor, const UClass* InExpectedBlueprintClass);
	FRigVMGraphDetailCustomization(TSharedPtr<IBlueprintEditor> RigVMigEditor, URigVMBlueprint* RigVMBlueprint);
#endif


	// IDetailCustomization interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;

private:
	/** The editor we are embedded in */
	TWeakPtr<IRigVMEditor> RigVMEditorPtr;

	/** The graph we are editing */
	TWeakObjectPtr<URigVMEdGraph> GraphPtr;

	/** The blueprint we are editing */
	TWeakObjectPtr<URigVMBlueprint> RigVMBlueprintPtr;

	TSharedPtr<FRigVMGraphDetailCustomizationImpl> RigVMGraphDetailCustomizationImpl;
};

class RIGVMEDITOR_API FRigVMGraphDetailCustomizationImpl : public TSharedFromThis<FRigVMGraphDetailCustomizationImpl>
{
public:
	void CustomizeDetails(IDetailLayoutBuilder& DetailLayout,
		URigVMGraph* Model,
		URigVMController* Controller,
		IRigVMClientHost* InRigVMClientHost,
		TWeakPtr<IRigVMEditor> InEditor);

private:

	bool IsAddNewInputOutputEnabled() const;
	EVisibility GetAddNewInputOutputVisibility() const;
	FReply OnAddNewInputClicked();
	FReply OnAddNewOutputClicked();
	FText GetNodeCategory() const;
	void SetNodeCategory(const FText& InNewText, ETextCommit::Type InCommitType);
	FText GetNodeKeywords() const;
	void SetNodeKeywords(const FText& InNewText, ETextCommit::Type InCommitType);
	FText GetNodeDescription() const;
	void SetNodeDescription(const FText& InNewText, ETextCommit::Type InCommitType);
	FLinearColor GetNodeColor() const;
	void SetNodeColor(FLinearColor InColor, bool bSetupUndoRedo);
	void OnNodeColorBegin();
	void OnNodeColorEnd();
	void OnNodeColorCancelled(FLinearColor OriginalColor);
	FReply OnNodeColorClicked();
	FText GetCurrentAccessSpecifierName() const;
	void OnAccessSpecifierSelected( TSharedPtr<FRigVMStringWithTag> SpecifierName, ESelectInfo::Type SelectInfo );
	TSharedRef<ITableRow> HandleGenerateRowAccessSpecifier( TSharedPtr<FRigVMStringWithTag> SpecifierName, const TSharedRef<STableViewBase>& OwnerTable );
	bool IsValidFunction() const;
	FRigVMVariant GetVariant() const;
	FRigVMVariantRef GetSubjectVariantRef() const;
	TArray<FRigVMVariantRef> GetVariantRefs() const;

private:

	void OnVariantChanged(const FRigVMVariant& InVariant);
	void OnBrowseVariantRef(const FRigVMVariantRef& InVariantRef);
	TArray<FRigVMTag> OnGetAssignedTags() const;
	void OnAddAssignedTag(const FName& InTagName);
	void OnRemoveAssignedTag(const FName& InTagName);

	URigVMLibraryNode* GetLibraryNode() const;
	URigVMNode* GetNodeForLayout() const;
	const FRigVMNodeLayout* GetNodeLayout() const;
	TArray<FString> GetUncategorizedPins() const;
	TArray<FRigVMPinCategory> GetPinCategories() const;
	FString GetPinCategory(FString InPinPath) const;
	int32 GetPinIndexInCategory(FString InPinPath) const;
	FString GetPinLabel(FString InPinPath) const;
	FLinearColor GetPinColor(FString InPinPath) const;
	const FSlateBrush* GetPinIcon(FString InPinPath) const;
	void HandleCategoryAdded(FString InCategory);
	void HandleCategoryRemoved(FString InCategory);
	void HandleCategoryRenamed(FString InOldCategory, FString InNewCategory);
	void HandlePinCategoryChanged(FString InPinPath, FString InCategory);
	void HandlePinLabelChanged(FString InPinPath, FString InNewLabel);
	void HandlePinIndexInCategoryChanged(FString InPinPath, int32 InIndexInCategory);
	static bool ValidateName(FString InNewName, FText& OutErrorMessage);
	bool HandleValidateCategoryName(FString InCategoryPath, FString InNewName, FText& OutErrorMessage);
	bool HandleValidatePinDisplayName(FString InPinPath, FString InNewName, FText& OutErrorMessage);

	uint32 GetNodeLayoutHash() const;

	/** The graph we are editing */
	TWeakObjectPtr<URigVMGraph> WeakModel;

	/** The graph controller we are editing */
	TWeakObjectPtr<URigVMController> WeakController;

	/** The editor we are embedded in */
	TWeakPtr<IRigVMEditor> RigVMEditorPtr;

	/** The asset host we are editing */
	TWeakInterfacePtr<IRigVMClientHost> RigVMClientHost;

	/** The color block widget */
	TSharedPtr<SColorBlock> ColorBlock;

	/** Set to true if the UI is currently picking a color */
	bool bIsPickingColor;

	static TArray<TSharedPtr<FRigVMStringWithTag>> AccessSpecifierStrings;
	mutable TOptional<FRigVMNodeLayout> CachedNodeLayout;
};

/** Customization for editing a rig vm node */
class RIGVMEDITOR_API FRigVMWrappedNodeDetailCustomization : public IDetailCustomization
{
public:
	
	FRigVMWrappedNodeDetailCustomization();

	/** Makes a new instance of this detail layout class for a specific detail view requesting it */
	static TSharedRef<IDetailCustomization> MakeInstance();

	// IDetailCustomization interface
	virtual void CustomizeDetails(IDetailLayoutBuilder& DetailLayout) override;

	TSharedRef<SWidget> MakeNameListItemWidget(TSharedPtr<FRigVMStringWithTag> InItem);
	FText GetNameListText(const FNameProperty* InProperty) const;
	TSharedPtr<FRigVMStringWithTag> GetCurrentlySelectedItem(const FNameProperty* InProperty, const TArray<TSharedPtr<FRigVMStringWithTag>>* InNameList) const;
	void SetNameListText(const FText& NewTypeInValue, ETextCommit::Type, const FNameProperty* InProperty, TSharedRef<IPropertyUtilities> PropertyUtilities);
	void OnNameListChanged(TSharedPtr<FRigVMStringWithTag> NewSelection, ESelectInfo::Type SelectInfo, const FNameProperty* InProperty, TSharedRef<IPropertyUtilities> PropertyUtilities);
	void OnNameListComboBox(const FNameProperty* InProperty, const TArray<TSharedPtr<FRigVMStringWithTag>>* InNameList);
	void CustomizeLiveValues(IDetailLayoutBuilder& DetailLayout);

	URigVMBlueprint* BlueprintBeingCustomized;
	TArray<TWeakObjectPtr<URigVMDetailsViewWrapperObject>> ObjectsBeingCustomized;
	TArray<TWeakObjectPtr<URigVMNode>> NodesBeingCustomized;
	TMap<FName, TSharedPtr<SRigVMGraphPinNameListValueWidget>> NameListWidgets;
};

/** Customization for editing a rig vm integer control enum class */
class RIGVMEDITOR_API FRigVMGraphEnumDetailCustomization : public IPropertyTypeCustomization
{
public:

	FRigVMGraphEnumDetailCustomization();
	
	static TSharedRef<IPropertyTypeCustomization> MakeInstance()
	{
		return MakeShareable(new FRigVMGraphEnumDetailCustomization);
	}

	/** IPropertyTypeCustomization interface */
	virtual void CustomizeHeader(TSharedRef<class IPropertyHandle> InPropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<class IPropertyHandle> InPropertyHandle, class IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

protected:

	TArray<uint8*> GetMemoryBeingCustomized()
	{
		TArray<uint8*> MemoryPtr;
		MemoryPtr.Reserve(ObjectsBeingCustomized.Num() + StructsBeingCustomized.Num());

		for(const TWeakObjectPtr<UObject>& Object : ObjectsBeingCustomized)
		{
			if(Object.IsValid())
			{
				MemoryPtr.Add((uint8*)Object.Get());
			}
		}

		for(const TSharedPtr<FStructOnScope>& StructPtr: StructsBeingCustomized)
		{
			if(StructPtr.IsValid())
			{
				MemoryPtr.Add(StructPtr->GetStructMemory());
			}
		}

		return MemoryPtr;
	}
	
	bool GetPropertyChain(TSharedRef<class IPropertyHandle> InPropertyHandle, FEditPropertyChain& OutPropertyChain, TArray<int32> &OutPropertyArrayIndices, bool& bOutEnabled)
	{
		if (!InPropertyHandle->IsValidHandle())
		{
			return false;
		}
		
		OutPropertyChain.Empty();
		OutPropertyArrayIndices.Reset();
		bOutEnabled = false;

		const bool bHasObject = !ObjectsBeingCustomized.IsEmpty() && ObjectsBeingCustomized[0].Get();
		const bool bHasStruct = !StructsBeingCustomized.IsEmpty() && StructsBeingCustomized[0].Get();
		
		if (bHasStruct || bHasObject)
		{
			TSharedPtr<class IPropertyHandle> ChainHandle = InPropertyHandle;
			while (ChainHandle.IsValid() && ChainHandle->GetProperty() != nullptr)
			{
				OutPropertyChain.AddHead(ChainHandle->GetProperty());
				OutPropertyArrayIndices.Insert(ChainHandle->GetIndexInArray(), 0);
				ChainHandle = ChainHandle->GetParentHandle();
			}

			if (OutPropertyChain.GetHead() != nullptr)
			{
				OutPropertyChain.SetActiveMemberPropertyNode(OutPropertyChain.GetTail()->GetValue());
				bOutEnabled = !OutPropertyChain.GetHead()->GetValue()->HasAnyPropertyFlags(CPF_EditConst);
				return true;
			}
		}
		return false;
	}

	// extracts the value for a nested property from an outer owner
	static UEnum** ContainerMemoryBlockToEnumPtr(uint8* InMemoryBlock, FEditPropertyChain& InPropertyChain, TArray<int32> &InPropertyArrayIndices)
	{
		if (InPropertyChain.GetHead() == nullptr)
		{
			return nullptr;
		}
		
		FEditPropertyChain::TDoubleLinkedListNode* PropertyNode = InPropertyChain.GetHead();
		uint8* MemoryPtr = InMemoryBlock;
		int32 ChainIndex = 0;
		do
		{
			const FProperty* Property = PropertyNode->GetValue();
			MemoryPtr = Property->ContainerPtrToValuePtr<uint8>(MemoryPtr);

			PropertyNode = PropertyNode->GetNextNode();
			ChainIndex++;
			
			if(InPropertyArrayIndices.IsValidIndex(ChainIndex))
			{
				const int32 ArrayIndex = InPropertyArrayIndices[ChainIndex];
				if(ArrayIndex != INDEX_NONE)
				{
					const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property->GetOwnerProperty());
					check(ArrayProperty);
					
					FScriptArrayHelper ArrayHelper(ArrayProperty, MemoryPtr);
					if(!ArrayHelper.IsValidIndex(ArrayIndex))
					{
						return nullptr;
					}
					MemoryPtr = ArrayHelper.GetRawPtr(ArrayIndex);

					// skip to the next property node already
					PropertyNode = PropertyNode->GetNextNode();
					ChainIndex++;
				}
			}
		}
		while (PropertyNode);

		return (UEnum**)MemoryPtr;
	}

	void HandleControlEnumChanged(TSharedPtr<FString> InEnumPath, ESelectInfo::Type InSelectType, TSharedRef<IPropertyHandle> InPropertyHandle);

	URigVMBlueprint* BlueprintBeingCustomized;
	URigVMGraph* GraphBeingCustomized;
	TArray<TWeakObjectPtr<UObject>> ObjectsBeingCustomized;
	TArray<TSharedPtr<FStructOnScope>> StructsBeingCustomized;
};

/** Customization for editing a rig vm node */
class RIGVMEDITOR_API FRigVMGraphMathTypeDetailCustomization : public IPropertyTypeCustomization
{
public:

	FRigVMGraphMathTypeDetailCustomization();
	
	static TSharedRef<IPropertyTypeCustomization> MakeInstance()
	{
		return MakeShareable(new FRigVMGraphMathTypeDetailCustomization);
	}

	/** IPropertyTypeCustomization interface */
	virtual void CustomizeHeader(TSharedRef<class IPropertyHandle> InPropertyHandle, class FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;
	virtual void CustomizeChildren(TSharedRef<class IPropertyHandle> InPropertyHandle, class IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils) override;

protected:

	bool GetPropertyChain(TSharedRef<class IPropertyHandle> InPropertyHandle, FEditPropertyChain& OutPropertyChain, TArray<int32> &OutPropertyArrayIndices, bool& bOutEnabled)
	{
		if (!InPropertyHandle->IsValidHandle())
		{
			return false;
		}
		
		OutPropertyChain.Empty();
		OutPropertyArrayIndices.Reset();
		bOutEnabled = false;

		if (InPropertyHandle->GetNumPerObjectValues() > 0)
		{
			TSharedPtr<class IPropertyHandle> ChainHandle = InPropertyHandle;
			while (ChainHandle.IsValid() && ChainHandle->GetProperty() != nullptr)
			{
				OutPropertyChain.AddHead(ChainHandle->GetProperty());
				OutPropertyArrayIndices.Insert(ChainHandle->GetIndexInArray(), 0);
				ChainHandle = ChainHandle->GetParentHandle();					
			}

			if (OutPropertyChain.GetHead() != nullptr)
			{
				OutPropertyChain.SetActiveMemberPropertyNode(OutPropertyChain.GetTail()->GetValue());
				bOutEnabled = !OutPropertyChain.GetHead()->GetValue()->HasAnyPropertyFlags(CPF_EditConst);
				return true;
			}
		}
		return false;
	}

	// extracts the value for a nested property (for Example Settings.WorldTransform) from an outer owner
	template<typename ValueType>
	static ValueType& ContainerMemoryBlockToValueRef(uint8* InMemoryBlock, ValueType& InDefault, FEditPropertyChain& InPropertyChain, TArray<int32> &InPropertyArrayIndices)
	{
		if (InPropertyChain.GetHead() == nullptr)
		{
			return InDefault;
		}
		
		FEditPropertyChain::TDoubleLinkedListNode* PropertyNode = InPropertyChain.GetHead();
		uint8* MemoryPtr = InMemoryBlock;
		int32 ChainIndex = 0;
		do
		{
			const FProperty* Property = PropertyNode->GetValue();
			MemoryPtr = Property->ContainerPtrToValuePtr<uint8>(MemoryPtr);

			PropertyNode = PropertyNode->GetNextNode();
			ChainIndex++;
			
			if(InPropertyArrayIndices.IsValidIndex(ChainIndex))
			{
				const int32 ArrayIndex = InPropertyArrayIndices[ChainIndex];
				if(ArrayIndex != INDEX_NONE)
				{
					const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property->GetOwnerProperty());
					check(ArrayProperty);
					
					FScriptArrayHelper ArrayHelper(ArrayProperty, MemoryPtr);
					if(!ArrayHelper.IsValidIndex(ArrayIndex))
					{
						return InDefault;
					}
					MemoryPtr = ArrayHelper.GetRawPtr(ArrayIndex);

					// skip to the next property node already
					PropertyNode = PropertyNode->GetNextNode();
					ChainIndex++;
				}
			}
		}
		while (PropertyNode);

		return *(ValueType*)MemoryPtr;
	}

	// specializations for FEulerTransform and FRotator at the end of this file
	template<typename ValueType>
	static bool IsQuaternionBasedRotation() { return true; }

	// returns the numeric value of a vector component (or empty optional for varying values)
	template<typename VectorType, typename NumericType>
	TOptional<NumericType> GetVectorComponent(TSharedRef<class IPropertyHandle> InPropertyHandle, int32 InComponent) 
	{
		TOptional<NumericType> Result;
		FEditPropertyChain PropertyChain;
		TArray<int32> PropertyArrayIndices;
		bool bEnabled;
		if (!GetPropertyChain(InPropertyHandle, PropertyChain, PropertyArrayIndices, bEnabled))
		{
			return Result;
		}

		if (TSharedPtr<IPropertyHandle> ChildHandle = InPropertyHandle->GetChildHandle(InComponent))
		{
			if (ChildHandle->IsValidHandle())
			{
				NumericType Value;
				if (ChildHandle->GetValue(Value) == FPropertyAccess::Success) // note that this will fail if multiple values
				{
					Result = Value;
				}
			}
		}

		return Result;
	};

	// called when a numeric value of a vector component is changed
	template<typename VectorType, typename NumericType>
	void OnVectorComponentChanged(TSharedRef<class IPropertyHandle> InPropertyHandle, int32 InComponent, NumericType InValue, bool bIsCommit, ETextCommit::Type InCommitType = ETextCommit::Default)
	{
		URigVMController* Controller = nullptr;
		if (TStrongObjectPtr<URigVMBlueprint> Blueprint = BlueprintBeingCustomized.Pin(); GraphBeingCustomized.IsValid())
		{
			Controller = BlueprintBeingCustomized->GetController(GraphBeingCustomized.Get());
			if (bIsCommit)
			{
				Controller->OpenUndoBracket(FString::Printf(TEXT("Set %s"), *InPropertyHandle->GetProperty()->GetName()));
			}
		}

		if (TSharedPtr<IPropertyHandle> ChildHandle = InPropertyHandle->GetChildHandle(InComponent))
		{
			if (ChildHandle->IsValidHandle())
			{
				ChildHandle->SetValue(InValue);
			}
		}

		if (Controller && bIsCommit)
		{
			Controller->CloseUndoBracket();
		}
	};

	// specializations for FVector and FVector4 at the end of this file
	template<typename VectorType>
	void ExtendVectorArgs(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr) {}

	template<typename VectorType, int32 NumberOfComponents>
	void MakeVectorHeaderRow(TSharedRef<class IPropertyHandle> InPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils);

	template<typename RotationType>
	void MakeRotationHeaderRow(TSharedRef<class IPropertyHandle> InPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils);

	template<typename TransformType>
	void ConfigureTransformWidgetArgs(TSharedRef<class IPropertyHandle> InPropertyHandle, typename SAdvancedTransformInputBox<TransformType>::FArguments& WidgetArgs, TConstArrayView<FName> ComponentNames);

	template<typename TransformType>
	void MakeTransformHeaderRow(TSharedRef<class IPropertyHandle> InPropertyHandle, FDetailWidgetRow& HeaderRow, IPropertyTypeCustomizationUtils& StructCustomizationUtils, TConstArrayView<FName> ComponentNames);

	template<typename TransformType>
	void MakeTransformChildren(TSharedRef<class IPropertyHandle> InPropertyHandle, IDetailChildrenBuilder& StructBuilder, IPropertyTypeCustomizationUtils& StructCustomizationUtils, TConstArrayView<FName> ComponentNames);

	// returns the rotation for rotator or quaternions (or empty optional for varying values)
	template<typename RotationType>
	TOptional<RotationType> GetRotation(TSharedRef<class IPropertyHandle> InPropertyHandle)
	{
		TOptional<RotationType> Result;
		FEditPropertyChain PropertyChain;
		TArray<int32> PropertyArrayIndices;
		bool bEnabled;
		if (!GetPropertyChain(InPropertyHandle, PropertyChain, PropertyArrayIndices, bEnabled))
		{
			return Result;
		}

		if (InPropertyHandle->IsValidHandle())
		{
			RotationType Value;
			if (InPropertyHandle->GetValue(Value) == FPropertyAccess::Success) // note that this will fail if multiple values
			{
				Result = Value;
			}
		}

		return Result;
	};

	// called when a rotation value is changed / committed
	template<typename RotationType>
	void OnRotationChanged(TSharedRef<class IPropertyHandle> InPropertyHandle, RotationType InValue, bool bIsCommit, ETextCommit::Type InCommitType = ETextCommit::Default)
	{
		
		FEditPropertyChain PropertyChain;
        TArray<int32> PropertyArrayIndices;
        bool bEnabled;
        if (!GetPropertyChain(InPropertyHandle, PropertyChain, PropertyArrayIndices, bEnabled))
        {
        	return;
        }
	
		URigVMController* Controller = nullptr;
		if (TStrongObjectPtr<URigVMBlueprint> Blueprint = BlueprintBeingCustomized.Pin(); GraphBeingCustomized.IsValid())
		{
			Controller = BlueprintBeingCustomized->GetController(GraphBeingCustomized.Get());
			if(bIsCommit)
			{
				Controller->OpenUndoBracket(FString::Printf(TEXT("Set %s"), *InPropertyHandle->GetProperty()->GetName()));
			}
		}

		if (InPropertyHandle->IsValidHandle())
		{
			InPropertyHandle->SetValue(InValue);
		}

		if(Controller && bIsCommit)
		{
			Controller->CloseUndoBracket();
		}
	};

	// specializations for FRotator and FQuat at the end of this file
	template<typename RotationType>
	void ExtendRotationArgs(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr) {}

	template<typename TransformType>
	static FName GetTranslationPropertyName()
	{
		return TEXT("Translation");
	}

	template<typename TransformType>
	static FName GetRotationPropertyName()
	{
		return TEXT("Rotation");
	}

	template<typename TransformType>
	static FName GetScalePropertyName()
	{
		return TEXT("Scale3D");
	}

	TWeakObjectPtr<URigVMBlueprint> BlueprintBeingCustomized;
	TWeakObjectPtr<URigVMGraph> GraphBeingCustomized;
};

template<>
inline bool FRigVMGraphMathTypeDetailCustomization::IsQuaternionBasedRotation<FEulerTransform>() { return false; }

template<>
inline bool FRigVMGraphMathTypeDetailCustomization::IsQuaternionBasedRotation<FRotator>() { return false; }

template<>
inline void FRigVMGraphMathTypeDetailCustomization::ExtendVectorArgs<FVector>(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr)
{
	using VectorType = FVector;
	typedef typename VectorType::FReal NumericType;
	typedef SNumericVectorInputBox<NumericType, VectorType, 3> SLocalVectorInputBox;

	typename SLocalVectorInputBox::FArguments& Args = *(typename SLocalVectorInputBox::FArguments*)ArgumentsPtr; 
	Args
	.Z_Lambda([this, InPropertyHandle]()
	{
		return GetVectorComponent<VectorType, NumericType>(InPropertyHandle, 2);
	})
	.OnZChanged_Lambda([this, InPropertyHandle](NumericType Value)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 2, Value, false);
	})
	.OnZCommitted_Lambda([this, InPropertyHandle](NumericType Value, ETextCommit::Type CommitType)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 2, Value, true, CommitType);
	});
}

template<>
inline void FRigVMGraphMathTypeDetailCustomization::ExtendVectorArgs<FVector4>(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr)
{
	using VectorType = FVector4;
	typedef typename VectorType::FReal NumericType;
	typedef SNumericVectorInputBox<NumericType, VectorType, 4> SLocalVectorInputBox;

	typename SLocalVectorInputBox::FArguments& Args = *(typename SLocalVectorInputBox::FArguments*)ArgumentsPtr; 
	Args
	.Z_Lambda([this, InPropertyHandle]()
	{
		return GetVectorComponent<VectorType, NumericType>(InPropertyHandle, 2);
	})
	.OnZChanged_Lambda([this, InPropertyHandle](NumericType Value)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 2, Value, false);
	})
	.OnZCommitted_Lambda([this, InPropertyHandle](NumericType Value, ETextCommit::Type CommitType)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 2, Value, true, CommitType);
	})
	.W_Lambda([this, InPropertyHandle]()
	{
		return GetVectorComponent<VectorType, NumericType>(InPropertyHandle, 3);
	})
	.OnWChanged_Lambda([this, InPropertyHandle](NumericType Value)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 3, Value, false);
	})
	.OnWCommitted_Lambda([this, InPropertyHandle](NumericType Value, ETextCommit::Type CommitType)
	{
		OnVectorComponentChanged<VectorType, NumericType>(InPropertyHandle, 3, Value, true, CommitType);
	});
}

template<>
inline void FRigVMGraphMathTypeDetailCustomization::ExtendRotationArgs<FQuat>(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr)
{
	using RotationType = FQuat;
	typedef typename RotationType::FReal NumericType;
	typedef SAdvancedRotationInputBox<NumericType> SLocalRotationInputBox;
	typename SLocalRotationInputBox::FArguments& Args = *(typename SLocalRotationInputBox::FArguments*)ArgumentsPtr; 

	Args.Quaternion_Lambda([this, InPropertyHandle]() -> TOptional<RotationType>
	{
		return GetRotation<RotationType>(InPropertyHandle);
	});

	Args.OnQuaternionChanged_Lambda([this, InPropertyHandle](RotationType InValue)
	{
		OnRotationChanged<RotationType>(InPropertyHandle, InValue, false);
	});

	Args.OnQuaternionCommitted_Lambda([this, InPropertyHandle](RotationType InValue, ETextCommit::Type InCommitType)
	{
		OnRotationChanged<RotationType>(InPropertyHandle, InValue, true, InCommitType);
	});
}

template<>
inline void FRigVMGraphMathTypeDetailCustomization::ExtendRotationArgs<FRotator>(TSharedRef<class IPropertyHandle> InPropertyHandle, void* ArgumentsPtr)
{
	using RotationType = FRotator;
	typedef typename RotationType::FReal NumericType;
	typedef SAdvancedRotationInputBox<NumericType> SLocalRotationInputBox;
	typename SLocalRotationInputBox::FArguments& Args = *(typename SLocalRotationInputBox::FArguments*)ArgumentsPtr; 

	Args.Rotator_Lambda([this, InPropertyHandle]() -> TOptional<RotationType>
	{
		return GetRotation<RotationType>(InPropertyHandle);
	});

	Args.OnRotatorChanged_Lambda([this, InPropertyHandle](RotationType InValue)
	{
		OnRotationChanged<RotationType>(InPropertyHandle, InValue, false);
	});

	Args.OnRotatorCommitted_Lambda([this, InPropertyHandle](RotationType InValue, ETextCommit::Type InCommitType)
	{
		OnRotationChanged<RotationType>(InPropertyHandle, InValue, true, InCommitType);
	});
}

template<>
inline FName FRigVMGraphMathTypeDetailCustomization::GetTranslationPropertyName<FEulerTransform>()
{
	return TEXT("Location");
}

template<>
inline FName FRigVMGraphMathTypeDetailCustomization::GetScalePropertyName<FEulerTransform>()
{
	return TEXT("Scale");
}
