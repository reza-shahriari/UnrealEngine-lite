// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	UObjectBaseUtility.cpp: Unreal UObject functions that only depend on UObjectBase
=============================================================================*/

#include "UObject/UObjectBaseUtility.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UObjectHash.h"
#include "Templates/Casts.h"
#include "UObject/Interface.h"
#include "Misc/PackageName.h"
#include "Misc/StringBuilder.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ConfigUtilities.h"
#include "Containers/VersePath.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DelayedAutoRegister.h"
#include "AutoRTFM.h"

/***********************/
/******** Names ********/
/***********************/

/**
 * Returns the fully qualified pathname for this object, in the format:
 * 'Outermost.[Outer:]Name'
 *
 * @param	StopOuter	if specified, indicates that the output string should be relative to this object.  if StopOuter
 *						does not exist in this object's Outer chain, the result would be the same as passing NULL.
 *
 * @note	safe to call on NULL object pointers!
 */
FString UObjectBaseUtility::GetPathName( const UObject* StopOuter/*=NULL*/ ) const
{
	FString Result;
	GetPathName(StopOuter, Result);
	return Result;
}

/**
 * Version of GetPathName() that eliminates unnecessary copies and appends an existing string.
 */
void UObjectBaseUtility::GetPathName(const UObject* StopOuter, FString& ResultString) const
{
	TStringBuilder<256> ResultBuilder;
	GetPathName(StopOuter, ResultBuilder);
	ResultString += FStringView(ResultBuilder);
}

void UObjectBaseUtility::GetPathName(const UObject* StopOuter, FStringBuilderBase& ResultString) const
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if(this != StopOuter && IsThisNotNull(this, "UObjectBaseUtility::GetPathName"))
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	{
		UObject* ObjOuter = GetOuter();
		if (ObjOuter && ObjOuter != StopOuter)
		{
			ObjOuter->GetPathName(StopOuter, ResultString);

			// SUBOBJECT_DELIMITER_CHAR is used to indicate that this object's outer is not a UPackage
			if (ObjOuter->GetClass() != UPackage::StaticClass()
			&& ObjOuter->GetOuter()->GetClass() == UPackage::StaticClass())
			{
				ResultString << SUBOBJECT_DELIMITER_CHAR;
			}
			else
			{
				ResultString << TEXT('.');
			}
		}
		GetFName().AppendString(ResultString);
	}
	else
	{
		ResultString << TEXT("None");
	}
}

/**
 * Returns the fully qualified pathname for this object as well as the name of the class, in the format:
 * 'ClassName Outermost.[Outer:]Name'.
 *
 * @param	StopOuter	if specified, indicates that the output string should be relative to this object.  if StopOuter
 *						does not exist in this object's Outer chain, the result would be the same as passing NULL.
 *
 * @note	safe to call on NULL object pointers!
 */
FString UObjectBaseUtility::GetFullName(const UObject* StopOuter, EObjectFullNameFlags Flags) const
{
	FString Result;
	Result.Empty(128);
	GetFullName(StopOuter, Result, Flags);
	return Result;
}

 /**
  * Version of GetFullName() that eliminates unnecessary copies and appends an existing string.
  */
void UObjectBaseUtility::GetFullName(const UObject* StopOuter, FString& ResultString, EObjectFullNameFlags Flags) const
{
	TStringBuilder<256> StringBuilder;
	GetFullName(StringBuilder, StopOuter, Flags);
	ResultString.Append(StringBuilder);
}

void UObjectBaseUtility::GetFullName(FStringBuilderBase& ResultString, const UObject* StopOuter, EObjectFullNameFlags Flags) const
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	if (IsThisNotNull(this, "UObjectBaseUtility::GetFullName"))
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	{
		if (EnumHasAllFlags(Flags, EObjectFullNameFlags::IncludeClassPackage))
		{
			GetClass()->GetPathName(nullptr, ResultString);
		}
		else
		{
			GetClass()->GetFName().AppendString(ResultString);
		}
		ResultString += TEXT(' ');
		GetPathName(StopOuter, ResultString);
	}
	else
	{
		ResultString += TEXT("None");
	}
}


/**
 * Walks up the chain of packages until it reaches the top level, which it ignores.
 *
 * @param	bStartWithOuter		whether to include this object's name in the returned string
 * @return	string containing the path name for this object, minus the outermost-package's name
 */
FString UObjectBaseUtility::GetFullGroupName( bool bStartWithOuter ) const
{
	const UObjectBaseUtility* Obj = bStartWithOuter ? GetOuter() : this;
	return Obj ? Obj->GetPathName(GetOutermost()) : TEXT("");
}


/***********************/
/*** Outer & Package ***/
/***********************/

bool UObjectBaseUtility::IsPackageExternal() const
{
	return HasAnyFlags(RF_HasExternalPackage);
}

void UObjectBaseUtility::DetachExternalPackage()
{
	ClearFlags(RF_HasExternalPackage);
}

void UObjectBaseUtility::ReattachExternalPackage()
{
	// GetObjectExternalPackageThreadSafe doesn't check for the RF_HasExternalPackage before looking up the external package
	if (!HasAnyFlags(RF_HasExternalPackage) && GetObjectExternalPackageThreadSafe(this))
	{
		SetFlags(RF_HasExternalPackage);
	}
}

/**
 * Walks up the list of outers until it finds the top-level one that isn't a package.
 * Will return null if called on a package
 * @return outermost non-null, non-package Outer.
 */
UObject* UObjectBaseUtility::GetOutermostObject() const
{
	UObject* Top = (UObject*)(this);
	if (Top->IsA<UPackage>())
	{
		return nullptr;
	}
	for (;;)
	{
		UObject* CurrentOuter = Top->GetOuter();
		if (CurrentOuter->IsA<UPackage>())
		{
			return Top;
		}
		Top = CurrentOuter;
	}
}

/**
 * Walks up the list of outers until it finds a package directly associated with the object.
 *
 * @return the package the object is in.
 */
UPackage* UObjectBaseUtility::GetPackage() const
{
	const UObject* Top = static_cast<const UObject*>(this);
	for (;;)
	{
		// GetExternalPackage will return itself if called on a UPackage
		if (UPackage* Package = Top->GetExternalPackage())
		{
			return Package;
		}
		Top = Top->GetOuter();
	}
}

UE::Core::FVersePath UObjectBaseUtility::GetVersePath() const
{
	return FPackageName::GetVersePath(FTopLevelAssetPath(static_cast<const UObject*>(this)));
}

/**
 * Legacy function, has the same behavior as GetPackage
 * use GetPackage instead.
 * @return the package the object is in.
 * @see GetPackage
 */
UPackage* UObjectBaseUtility::GetOutermost() const
{
	return GetPackage();
}

/** 
 * Finds the outermost package and marks it dirty
 */
bool UObjectBaseUtility::MarkPackageDirty() const
{
	UPackage* Package = nullptr;

	// since transient objects will never be saved into a package, there is no need to mark a package dirty
	// if we're transient along the outer chain up to the associated package.
	{
		const UObjectBaseUtility* Object = this;
		while( Object && !Package )
		{
			if( Object->HasAnyFlags(RF_Transient) )
			{
				return true;
			}
			Package = Object->GetExternalPackage();
			Object = Object->GetOuter();
		}
	}

	if( Package )
	{
		// It is against policy to dirty a map or package during load/undo/redo in the Editor, to enforce this policy
		// we explicitly disable the ability to dirty a package or map during load/undo/redo.  Commandlets can still
		// set the dirty state on load.
		if( IsRunningCommandlet() || 
			(!IsInAsyncLoadingThread() && GIsEditor && !UE::GetIsEditorLoadingPackage() && !GIsCookerLoadingPackage && !GIsPlayInEditorWorld && !IsReloadActive()
#if WITH_EDITORONLY_DATA
			&& !GIsTransacting
			&& !Package->bIsCookedForEditor // Cooked packages can't be modified nor marked as dirty
#endif
			))
		{
			const bool bIsDirty = Package->IsDirty();

			// We prevent needless re-dirtying as this can be an expensive operation.
			if( !bIsDirty )
			{
				Package->SetDirtyFlag(true);
			}

			// Always call PackageMarkedDirtyEvent, even when the package is already dirty
			Package->PackageMarkedDirtyEvent.Broadcast(Package, bIsDirty);

			return true;
		}

		// notify the caller that the request to mark the package as dirty was suppressed
		return false;
	}

	return true;
}

/**
* Determines whether this object is a template object
*
* @return	true if this object is a template object (owned by a UClass)
*/
bool UObjectBaseUtility::IsTemplate( EObjectFlags TemplateTypes ) const
{
	for (const UObjectBaseUtility* TestOuter = this; TestOuter; TestOuter = TestOuter->GetOuter() )
	{
		if ( TestOuter->HasAnyFlags(TemplateTypes) )
			return true;
	}

	return false;
}


/**
 * Traverses the outer chain searching for the next object of a certain type.  (T must be derived from UObject)
 *
 * @param	Target class to search for
 * @return	a pointer to the first object in this object's Outer chain which is of the correct type.
 */
UObject* UObjectBaseUtility::GetTypedOuter(UClass* Target) const
{
	ensureMsgf(Target != UPackage::StaticClass(), TEXT("Calling GetTypedOuter to retrieve a package is now invalid, you should use GetPackage() instead."));

	UObject* Result = NULL;
	for ( UObject* NextOuter = GetOuter(); Result == NULL && NextOuter != NULL; NextOuter = NextOuter->GetOuter() )
	{
		if ( NextOuter->IsA(Target) )
		{
			Result = NextOuter;
		}
	}
	return Result;
}

UObjectBaseUtility* UObjectBaseUtility::GetImplementingOuterObject(const UClass* InInterfaceClass) const
{
	for (UObject* NextOuter = GetOuter(); NextOuter != nullptr; NextOuter = NextOuter->GetOuter())
	{
		UClass* OuterClass = NextOuter->GetClass();
		if (OuterClass && OuterClass->ImplementsInterface(InInterfaceClass))
		{
			return NextOuter;
		}
	}

	return nullptr;
}

/*-----------------------------------------------------------------------------
	UObject accessors that depend on UClass.
-----------------------------------------------------------------------------*/

/**
 * @return	true if the specified object appears somewhere in this object's outer chain.
 */
bool UObjectBaseUtility::IsIn( const UObject* SomeOuter ) const
{
	if (SomeOuter->IsA<UPackage>())
	{
		return IsInPackage(static_cast<const UPackage*>(SomeOuter));
	}
	return IsInOuter(SomeOuter);
}

/** Overload to determine if an object is in the specified package which can now be different than its outer chain. */
bool UObjectBaseUtility::IsIn(const UPackage* SomePackage) const
{
	// uncomment the ensure to more easily find where IsIn should be changed to IsInPackage
	//ensure(0);
	return IsInPackage(SomePackage);
}

bool UObjectBaseUtility::IsInOuter(const UObject* SomeOuter) const
{
	for (UObject* It = GetOuter(); It; It = It->GetOuter())
	{
		if (It == SomeOuter)
		{
			return true;
		}
	}
	return SomeOuter == nullptr;
}

/**
 * @return	true if the object is contained in the specified package.
 */
bool UObjectBaseUtility::IsInPackage(const UPackage* SomePackage) const
{
	return SomePackage != this && GetPackage() == SomePackage;
}

/**
 * Find out if this object is inside (has an outer) that is of the specified class
 * @param SomeBaseClass	The base class to compare against
 * @return True if this object is in an object of the given type.
 */
bool UObjectBaseUtility::IsInA( const UClass* SomeBaseClass ) const
{
	for (const UObjectBaseUtility* It=this; It; It = It->GetOuter())
	{
		if (It->IsA(SomeBaseClass))
		{
			return true;
		}
	}
	return SomeBaseClass == NULL;
}

/**
* Checks whether this object's top-most package has any of the specified flags
*
* @param	CheckFlagMask	a bitmask of EPackageFlags values to check for
*
* @return	true if the PackageFlags member of this object's top-package has any bits from the mask set.
*/
bool UObjectBaseUtility::RootPackageHasAnyFlags( uint32 CheckFlagMask ) const
{
	return GetOutermost()->HasAnyPackageFlags(CheckFlagMask);
}

/***********************/
/******** Class ********/
/***********************/

/**
 * Finds the most-derived class which is a parent of both TestClass and this object's class.
 *
 * @param	TestClass	the class to find the common base for
 */
const UClass* UObjectBaseUtility::FindNearestCommonBaseClass( const UClass* TestClass ) const
{
	const UClass* Result = NULL;

	if ( TestClass != NULL )
	{
		const UClass* CurrentClass = GetClass();

		// early out if it's the same class or one is the parent of the other
		// (the check for TestClass->IsChildOf(CurrentClass) returns true if TestClass == CurrentClass
		if ( TestClass->IsChildOf(CurrentClass) )
		{
			Result = CurrentClass;
		}
		else if ( CurrentClass->IsChildOf(TestClass) )
		{
			Result = TestClass;
		}
		else
		{
			// find the nearest parent of TestClass which is also a parent of CurrentClass
			for ( UClass* Cls = TestClass->GetSuperClass(); Cls; Cls = Cls->GetSuperClass() )
			{
				if ( CurrentClass->IsChildOf(Cls) )
				{
					Result = Cls;
					break;
				}
			}
		}
	}

	// at this point, Result should only be NULL if TestClass is NULL
	checkfSlow(Result != NULL || TestClass == NULL, TEXT("No common base class found for object '%s' with TestClass '%s'"), *GetFullName(), *TestClass->GetFullName());
	return Result;
}


/**
 * Returns a pointer to this object safely converted to a pointer to the specified interface class.
 *
 * @param	InterfaceClass	the interface class to use for the returned type
 *
 * @return	a pointer that can be assigned to a variable of the interface type specified, or NULL if this object's
 *			class doesn't implement the interface indicated.  Will be the same value as 'this' if the interface class
 *			isn't native.
 */
void* UObjectBaseUtility::GetInterfaceAddress( UClass* InterfaceClass )
{
	void* Result = NULL;

	if ( InterfaceClass != NULL && InterfaceClass->HasAnyClassFlags(CLASS_Interface) && InterfaceClass != UInterface::StaticClass() )
	{
		// Script interface
		if ( !InterfaceClass->HasAnyClassFlags(CLASS_Native) )
		{
			if ( GetClass()->ImplementsInterface(InterfaceClass) )
			{
				// if it isn't a native interface, the address won't be different
				Result = this;
			}
		}
		// Native interface
		else
		{
			for( UClass* CurrentClass=GetClass(); Result == NULL && CurrentClass != NULL; CurrentClass = CurrentClass->GetSuperClass() )
			{
				for (TArray<FImplementedInterface>::TIterator It(CurrentClass->Interfaces); It; ++It)
				{
					// See if this is the implementation we are looking for, and it was done natively, not in K2
					FImplementedInterface& ImplInterface = *It;
					if ( !ImplInterface.bImplementedByK2 && ImplInterface.Class->IsChildOf(InterfaceClass) )
					{
						Result = (uint8*)this + It->PointerOffset;
						break;
					}
				}
			}
		}
	}

	return Result;
}

void* UObjectBaseUtility::GetNativeInterfaceAddress(UClass* InterfaceClass)
{
	check(InterfaceClass != NULL);
	check(InterfaceClass->HasAllClassFlags(CLASS_Interface | CLASS_Native));
	check(InterfaceClass != UInterface::StaticClass());

	for( UClass* CurrentClass=GetClass(); CurrentClass; CurrentClass = CurrentClass->GetSuperClass() )
	{
		for (auto It = CurrentClass->Interfaces.CreateConstIterator(); It; ++It)
		{
			// See if this is the implementation we are looking for, and it was done natively, not in K2
			auto& ImplInterface = *It;
			if ( !ImplInterface.bImplementedByK2 && ImplInterface.Class->IsChildOf(InterfaceClass) )
			{
				if ( It->PointerOffset )
				{
					return (uint8*)this + It->PointerOffset;
				}
			}
		}
	}

	return NULL;
}

bool UObjectBaseUtility::IsTemplateForSubobjects(EObjectFlags TemplateTypes) const
{
	// This includes archetype objects that are inside CDOs or inheritable component templates, but not the CDO itself
	return HasAnyFlags(RF_ArchetypeObject) && !HasAnyFlags(RF_ClassDefaultObject) && IsTemplate(TemplateTypes);
}

bool UObjectBaseUtility::IsDefaultSubobject() const
{
	// For historical reasons this behavior does not match the RF_DefaultSubObject flag.
	// It will return true for any object instanced using a non-CDO archetype, 
	// but it will return false for indirectly nested subobjects of a CDO that can be used as an archetype.
	
	return !HasAnyFlags(RF_ClassDefaultObject) && GetOuter() &&
		(GetOuter()->HasAnyFlags(RF_ClassDefaultObject) || ((UObject*)this)->GetArchetype() != GetClass()->GetDefaultObject(false));
}

void UObjectBaseUtility::ReloadObjectsFromModifiedConfigSections(const FConfigModificationTracker* ChangeTracker)
{	
	TArray<const UClass*> ClassesToReload;
	TArray<UObject*> ObjectsToReload;
	
	// Reload configs so objects get the changes
	for (const TPair<FName, TSet<FString>>& Pair : ChangeTracker->ModifiedSectionsPerBranch)
	{
		for (const FString& SectionName : Pair.Value)
		{
			UE_LOG(LogConfig, Verbose, TEXT("Found modified section: %s"), *SectionName);
			
			// @todo: This entire overarching process is very similar in its goals as that of UOnlineHotfixManager::HotfixIniFile.
			// Could consider a combined refactor of the hotfix manager, the base config cache system, etc. to expose an easier way to support this pattern
			
			// INI files might be handling per-object config items, so need to handle them specifically
			const int32 PerObjConfigDelimIdx = SectionName.Find(" ");
			if (PerObjConfigDelimIdx != INDEX_NONE)
			{
				const FString ObjectName = SectionName.Left(PerObjConfigDelimIdx);
				const FString ClassName = SectionName.Mid(PerObjConfigDelimIdx + 1);
				
				// TryFindTypeSlow will throw a warning/callstack for short pathnames, so use the function it calls internally
				UClass* ObjClass = (UClass*)StaticFindFirstObject(UClass::StaticClass(), *ClassName, EFindFirstObjectOptions::EnsureIfAmbiguous | EFindFirstObjectOptions::NativeFirst, ELogVerbosity::Error, TEXT("UGameFeatureData::ReloadConfigs"));
				if (ObjClass && ObjClass->HasAnyClassFlags(CLASS_Config))
				{
					// Now try to actually find the object it's referencing specifically and update it
					// @note: Choosing not to warn on not finding it for now, as Fortnite has transient uses instantiated at run-time (might not be constructed yet)
					UObject* PerObjConfigObj = StaticFindFirstObject(ObjClass, *ObjectName, EFindFirstObjectOptions::ExactClass, ELogVerbosity::Warning, TEXT("UObjectBaseUtility::ReloadObjectsFromModifiedConfigSections"));
					if (IsValid(PerObjConfigObj))
					{
						ObjectsToReload.Add(PerObjConfigObj);
					}
				}
				else
				{
					UE_LOG(LogCore, Warning, TEXT("[%s]: Couldn't find PerObjectConfig class %s for %s, config changes won't be reloaded."), *Pair.Key.ToString(), *ClassName, *ObjectName);
				}
			}
			// Standard INI section case
			else
			{
				const FString NativeClassPrefix(TEXT("/Script/"));
				const FString BPClassPrefix(TEXT("/Game/"));
				
				UClass* ObjClass = nullptr;
				// handle /Script and /Game (/Game is for BP classes)
				if (SectionName.StartsWith(NativeClassPrefix))
				{
					ObjClass = FindFirstObject<UClass>(*SectionName, EFindFirstObjectOptions::ExactClass | EFindFirstObjectOptions::EnsureIfAmbiguous | EFindFirstObjectOptions::NativeFirst);
				}
				else if (SectionName.StartsWith(BPClassPrefix))
				{
					ObjClass = LoadObject<UClass>(nullptr, *SectionName);
				}
				
				// Find the affected class and push updates to all instances of it, including children
				// @note:	Intentionally not using the propagation flags inherent in ReloadConfig to handle this, as it utilizes a naive complete object iterator
				//			and tanks performance pretty badly
				if (ObjClass && ObjClass->HasAnyClassFlags(CLASS_Config))
				{
					ClassesToReload.Add(ObjClass);
				}
			}
		}
		
		// @todo: this seems like a convenient place for this, but it's out of the scope of the function name
		// for each modified branch, broadcast the changes
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(Pair.Key.ToString(), Pair.Value);
	}
	
	
	int32 NumObjectsReloaded = 0;
	const double StartTime = FPlatformTime::Seconds();

	auto ReloadObjectImpl = [&NumObjectsReloaded](UObject* ReloadObject)
	{
		UE_LOG(LogConfig, Verbose, TEXT("Reloading %s"), *ReloadObject->GetPathName());
		// Intentionally using LoadConfig instead of ReloadConfig, since we do not want to call modify/preeditchange/posteditchange on the objects changed when GIsEditor
		ReloadObject->LoadConfig(nullptr, nullptr, UE::LCPF_ReloadingConfigData | UE::LCPF_ReadParentSections, nullptr);
//		ReloadObject->ReloadConfig();
	   
	   NumObjectsReloaded++;
	};
	
	// Now that we have a list of classes to update, we can iterate objects and reload
	for (const UClass* ClassToReload : ClassesToReload)
	{
		bool bReloadInstances = true;
		bool bReloadSubclasses = true;

		if (ChangeTracker->ClassesToSkipSubclasses.Num() || ChangeTracker->ClassesToSkipInstances.Num())
		{
			FString ClassPath = ClassToReload->GetPathName();
			bReloadInstances = !ChangeTracker->ClassesToSkipInstances.Contains(ClassPath);
			bReloadSubclasses = !ChangeTracker->ClassesToSkipSubclasses.Contains(ClassPath);
		}

		TArray<UObject*> ClassObjectsToReload;
		if (bReloadInstances)
		{
			GetObjectsOfClass(ClassToReload, ClassObjectsToReload, bReloadSubclasses, RF_NoFlags);
			for (UObject* ReloadObject : ClassObjectsToReload)
			{
				if (IsValid(ReloadObject))
				{
					ReloadObjectImpl(ReloadObject);
				}
			}
		}
		else
		{
			// if not the instances, then just reload the CDO of the class
			ReloadObjectImpl(ClassToReload->GetDefaultObject());
			if (bReloadSubclasses)
			{
				TArray<UClass*> Results;
				GetDerivedClasses(ClassToReload, Results, true);
				for (UClass* Subclass : Results)
				{
					ReloadObjectImpl(Subclass->GetDefaultObject());
				}
			}
		}
	}

	// Reload any PerObjectConfig objects that were affected
	for (auto ReloadObject : ObjectsToReload)
	{
		ReloadObjectImpl(ReloadObject);
	}
	
	UE_LOG(LogCore, Log, TEXT("Updating config from took %f seconds and reloaded %d objects"),
		FPlatformTime::Seconds() - StartTime, NumObjectsReloaded);
}

// register a callback so non-uobject-knowing config code can still trigger object reloads
static FDelayedAutoRegisterHelper GSetupReload(EDelayedRegisterRunPhase::ObjectSystemReady, []
	{
		UE::DynamicConfig::ReloadObjects.AddStatic(&UObjectBaseUtility::ReloadObjectsFromModifiedConfigSections);
	});


UClass* GetParentNativeClass(UClass* Class)
{
	while (Class && !Class->IsNative())
	{
		Class = Class->GetSuperClass();
	}

	return Class;
}

UClass* GetTopMostNonNativeClass(UClass* Class, bool bAllowReturnNativeClass)
{
	while (Class && !Class->IsNative())
	{
		UClass* SuperClass = Class->GetSuperClass();
		// is the parent class native?
		if (SuperClass && SuperClass->IsNative())
		{
			// return the current class instead (Top most non-native class)
			return Class;
		}

		// otherwise keep going up the hierarchy
		Class = SuperClass;
	}

	// If the input given class is Native, return it only when bFallBackToNative is true, otherwise return nullptr
	return bAllowReturnNativeClass ? Class : nullptr;
}

#if !STATS && !ENABLE_STATNAMEDEVENTS && USE_LIGHTWEIGHT_STATS_FOR_HITCH_DETECTION && USE_HITCH_DETECTION && USE_LIGHTWEIGHT_UOBJECT_STATS_FOR_HITCH_DETECTION
#include "HAL/ThreadHeartBeat.h"
#include "HAL/ThreadManager.h"

UE_AUTORTFM_ALWAYS_OPEN
void FScopeCycleCounterUObject::ReportHitch()
{
	float Delta = float(FGameThreadHitchHeartBeat::Get().GetCurrentTime() - FGameThreadHitchHeartBeat::Get().GetFrameStartTime()) * 1000.0f;
	const uint32 CurrentThreadId = FPlatformTLS::GetCurrentThreadId();
	const FString& ThreadString = FThreadManager::GetThreadName(CurrentThreadId);
	FString StackString;
	if (CurrentThreadId == GGameThreadId)
	{
		if (StatObject->IsValidLowLevel() && StatObject->IsValidLowLevelFast())
		{
			StackString = GetFullNameSafe(StatObject);
		}
		else
		{
			StackString = FString(TEXT("[UObject was invalid]"));
		}
	}
	else
	{
		StackString = FString(TEXT("[Not grabbing UObject name from other threads]"));
	}
	UE_LOG(LogCore, Error, TEXT("Leaving UObject scope on hitch (+%8.2fms) [%s] %s"), Delta, *ThreadString, *StackString);
}
#endif


