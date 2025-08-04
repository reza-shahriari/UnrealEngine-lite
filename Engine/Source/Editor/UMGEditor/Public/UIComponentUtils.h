// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ClassViewerFilter.h"
#include "UObject/ObjectMacros.h"

class UUIComponent;
class UWidgetBlueprint;
class FWidgetBlueprintEditor;
/**  */

class UMGEDITOR_API FUIComponentUtils
{
public:
	class FUIComponentClassFilter : public IClassViewerFilter
	{
	public:
		/** All children of these classes will be included unless filtered out by another setting. */
		TSet <const UClass*> AllowedChildrenOfClasses;
		TSet <const UClass*> ExcludedChildrenOfClasses;

		/** Disallowed class flags. */
		EClassFlags DisallowedClassFlags;

		virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override;

		virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override;
	};

	static FClassViewerInitializationOptions CreateClassViewerInitializationOptions();

	static void OnWidgetRenamed(const TSharedRef<FWidgetBlueprintEditor>& BlueprintEditor, UWidgetBlueprint* WidgetBlueprint, const FName& OldVarName, const FName& NewVarName);
	static void AddComponent(const TSharedRef<FWidgetBlueprintEditor>& BlueprintEditor, const UClass* ComponentClass, const FName WidgetName);	
	static void RemoveComponent(const TSharedRef<FWidgetBlueprintEditor>& BlueprintEditor, const UClass* ComponentClass, const FName WidgetName);
	
};