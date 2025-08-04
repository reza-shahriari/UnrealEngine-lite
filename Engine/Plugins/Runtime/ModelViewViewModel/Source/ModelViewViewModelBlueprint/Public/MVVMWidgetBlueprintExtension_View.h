// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "WidgetBlueprintExtension.h"
#include "MVVMDeveloperProjectSettings.h"

#include "MVVMWidgetBlueprintExtension_View.generated.h"

class UMVVMBlueprintView;
class UMVVMBlueprintViewExtension;
class FWidgetBlueprintCompilerContext;
class UWidgetBlueprintGeneratedClass;

namespace UE::MVVM
{
	class FClipboardExtension;
}

namespace UE::MVVM::Private
{
	struct FMVVMViewBlueprintCompiler;
} //namespace

USTRUCT()
struct FMVVMExtensionItem
{
	GENERATED_BODY()

	UPROPERTY()
	FName WidgetName = NAME_None;

	UPROPERTY()
	FGuid ViewmodelId = FGuid();

	UPROPERTY()
	TObjectPtr<UMVVMBlueprintViewExtension> ExtensionObj;

	bool operator==(const FMVVMExtensionItem& OtherExt) const
	{
		return OtherExt.WidgetName == WidgetName 
			&&	OtherExt.ExtensionObj == ExtensionObj
			&&	OtherExt.ViewmodelId == ViewmodelId;
	}
};

/**
 *
 */
UCLASS()
class MODELVIEWVIEWMODELBLUEPRINT_API UMVVMWidgetBlueprintExtension_View : public UWidgetBlueprintExtension
{
	GENERATED_BODY()

public:
	void CreateBlueprintViewInstance();
	void DestroyBlueprintViewInstance();

	UMVVMBlueprintView* GetBlueprintView()
	{
		return BlueprintView;
	}

	const UMVVMBlueprintView* GetBlueprintView() const
	{
		return BlueprintView;
	}

	FSimpleMulticastDelegate& OnBlueprintViewChangedDelegate()
	{
		return BlueprintViewChangedDelegate;
	}

	const TArrayView<const FName> GetGeneratedFunctions() const
	{
		return GeneratedFunctions;
	}

public:
	//~ Begin UObject interface
	virtual void PostLoad() override;
#if WITH_EDITORONLY_DATA
	virtual void PostInitProperties() override;
#endif
	//~ End UObject interface

	//~ Begin UWidgetBlueprintExtension interface
	virtual void HandlePreloadObjectsForCompilation(UBlueprint* OwningBlueprint) override;
	virtual void HandleBeginCompilation(FWidgetBlueprintCompilerContext& InCreationContext) override;
	virtual void HandleCleanAndSanitizeClass(UWidgetBlueprintGeneratedClass* ClassToClean, UObject* OldCDO) override;
	virtual void HandlePopulateGeneratedVariables(const FWidgetBlueprintCompilerContext::FPopulateGeneratedVariablesContext& Context) override;
	virtual void HandleCreateClassVariablesFromBlueprint(const FWidgetBlueprintCompilerContext::FCreateVariableContext& Context) override;
	virtual void HandleCreateFunctionList(const FWidgetBlueprintCompilerContext::FCreateFunctionContext& Context) override;
	virtual void HandleFinishCompilingClass(UWidgetBlueprintGeneratedClass* Class) override;
	virtual void HandleEndCompilation() override;
	virtual FSearchData HandleGatherSearchData(const UBlueprint* OwningBlueprint) const override;
	//~ End UWidgetBlueprintExtension interface

	void VerifyWidgetExtensions();
	void OnFieldRenamed(UClass* FieldOwnerClass, FName OldName, FName NewName);
	void RenameWidgetExtensions(FName OldWidgetName, FName NewWidgetName);
	UMVVMBlueprintViewExtension* CreateBlueprintWidgetExtension(TSubclassOf<UMVVMBlueprintViewExtension> ExtensionClass, FName WidgetName);
	void RemoveBlueprintWidgetExtension(UMVVMBlueprintViewExtension* ExtensionToRemove, FName WidgetName);
	TArray<UMVVMBlueprintViewExtension*> GetBlueprintExtensionsForWidget(FName WidgetName) const;
	TArray<UMVVMBlueprintViewExtension*> GetAllBlueprintExtensions() const;

	void SetFilterSettings(FMVVMViewBindingFilterSettings InFilterSettings);
	FMVVMViewBindingFilterSettings GetFilterSettings() const
	{
		return FilterSettings;
	}

	UPROPERTY(Transient)
	TMap<FGuid, TWeakObjectPtr<UObject>> TemporaryViewModelInstances;

private:
	UPROPERTY(Instanced)
	TObjectPtr<UMVVMBlueprintView> BlueprintView;

	UPROPERTY(Transient)
	FMVVMViewBindingFilterSettings FilterSettings;
	
	UPROPERTY(Transient)
	TArray<FName> GeneratedFunctions;

	FSimpleMulticastDelegate BlueprintViewChangedDelegate;
	TPimplPtr<UE::MVVM::Private::FMVVMViewBlueprintCompiler> CurrentCompilerContext;

	UPROPERTY()
	TArray<FMVVMExtensionItem> BlueprintExtensions;

	friend UE::MVVM::Private::FMVVMViewBlueprintCompiler;
	friend UE::MVVM::FClipboardExtension;
};
