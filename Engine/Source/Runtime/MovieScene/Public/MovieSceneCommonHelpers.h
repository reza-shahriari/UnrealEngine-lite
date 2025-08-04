// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/EngineTypes.h"
#include "UObject/UnrealType.h"
#include "UObject/ObjectKey.h"
#include "Curves/KeyHandle.h"
#include "Misc/FrameNumber.h"
#include "UObject/WeakFieldPtr.h"

class AActor;
class UCameraComponent;
class UMovieScene;
class UMovieSceneSection;
class UMovieSceneSubSection;
class UMovieSceneSequence;
class USceneComponent;
class USoundBase;
template <class TClass> class TSubclassOf;
class UMovieSceneCustomBinding;
struct FRichCurve;
enum class EMovieSceneKeyInterpolation : uint8;
struct FMovieSceneSequenceID;
class UMovieSceneCondition;
class UMovieSceneTrack;

namespace UE::MovieScene
{
	struct FSharedPlaybackState;
}

class MovieSceneHelpers
{
public:

	/*
	* Helper struct to cache the package dirty state and then to restore it
	* after this leaves scope. This is for a few minor areas where calling
	* functions on actors dirties them, but Sequencer doesn't actually want
	* the package to be dirty as it causes Sequencer to unnecessairly dirty
	* actors.
	*/
	struct FMovieSceneScopedPackageDirtyGuard
	{
		MOVIESCENE_API FMovieSceneScopedPackageDirtyGuard(class USceneComponent* InComponent);
		MOVIESCENE_API virtual ~FMovieSceneScopedPackageDirtyGuard();

	private:
		class USceneComponent* Component;
		bool bPackageWasDirty;
	};

	/** 
	 * @return Whether the section is keyable (active, on a track that is not muted, etc 
	 */
	static MOVIESCENE_API bool IsSectionKeyable(const UMovieSceneSection*);

	/**
	 * Finds a section that exists at a given time
	 *
	 * @param Time	The time to find a section at
	 * @param RowIndex  Limit the search to a given row index
	 * @return The found section or null
	 */
	static MOVIESCENE_API UMovieSceneSection* FindSectionAtTime( TArrayView<UMovieSceneSection* const> Sections, FFrameNumber Time, int32 RowIndex = INDEX_NONE );

	/**
	 * Finds the nearest section to the given time
	 *
	 * @param Time	The time to find a section at
	 * @param RowIndex  Limit the search to a given row index
	 * @return The found section or null
	 */
	static MOVIESCENE_API UMovieSceneSection* FindNearestSectionAtTime( TArrayView<UMovieSceneSection* const> Sections, FFrameNumber Time, int32 RowIndex = INDEX_NONE );

	/** Find the next section that doesn't overlap - the section that has the next closest start time to the requested start time */
	static MOVIESCENE_API UMovieSceneSection* FindNextSection(TArrayView<UMovieSceneSection* const> Sections, FFrameNumber Time);

	/** Find the previous section that doesn't overlap - the section that has the previous closest start time to the requested start time */
	static MOVIESCENE_API UMovieSceneSection* FindPreviousSection(TArrayView<UMovieSceneSection* const> Sections, FFrameNumber Time);

	/*
	 * Fix up consecutive sections so that there are no gaps
	 * 
	 * @param Sections All the sections
	 * @param Section The section that was modified 
	 * @param bDelete Was this a deletion?
	 * @param bCleanUp Should we cleanup any invalid sections?
	 * @return Whether the list of sections was modified as part of the clean-up
	 */
	static MOVIESCENE_API bool FixupConsecutiveSections(TArray<UMovieSceneSection*>& Sections, UMovieSceneSection& Section, bool bDelete, bool bCleanUp = false);

	/**
	 * Fix up consecutive sections so that there are no gaps, but there can be overlaps, in which case the sections
	 * blend together.
	 *
	 * @param Sections All the sections
	 * @param Section The section that was modified 
	 * @param bDelete Was this a deletion?
	 * @param bCleanUp Should we cleanup any invalid sections?
	 * @return Whether the list of sections was modified as part of the clean-up
	 */
	static MOVIESCENE_API bool FixupConsecutiveBlendingSections(TArray<UMovieSceneSection*>& Sections, UMovieSceneSection& Section, bool bDelete, bool bCleanUp = false);

	/*
 	 * Sort consecutive sections so that they are in order based on start time
 	 */
	static MOVIESCENE_API void SortConsecutiveSections(TArray<UMovieSceneSection*>& Sections);

	/*
	 * Gather up descendant movie scenes from the incoming sequence
	 */
	static MOVIESCENE_API void GetDescendantMovieScenes(UMovieSceneSequence* InSequence, TArray<UMovieScene*> & InMovieScenes);

	/*
	 * Gather up descendant movie scene sub-sections from the incoming movie scene
	 */
	static MOVIESCENE_API void GetDescendantSubSections(const UMovieScene* InMovieScene, TArray<UMovieSceneSubSection*>& InSubSections);
	
	/**
	 * Get the scene component from the runtime object
	 *
	 * @param Object The object to get the scene component for
	 * @return The found scene component
	 */	
	static MOVIESCENE_API USceneComponent* SceneComponentFromRuntimeObject(UObject* Object);
	static MOVIESCENE_API UObject* ResolveSceneComponentBoundObject(UObject* Object);

	/**
	 * Get the active camera component from the actor 
	 *
	 * @param InActor The actor to look for the camera component on
	 * @return The active camera component
	 */
	static MOVIESCENE_API UCameraComponent* CameraComponentFromActor(const AActor* InActor);

	/**
	 * Find and return camera component from the runtime object
	 *
	 * @param Object The object to get the camera component for
	 * @return The found camera component
	 */	
	static MOVIESCENE_API UCameraComponent* CameraComponentFromRuntimeObject(UObject* RuntimeObject);

	/**
	 * Set the runtime object movable
	 *
	 * @param Object The object to set the mobility for
	 * @param Mobility The mobility of the runtime object
	 */
	static MOVIESCENE_API void SetRuntimeObjectMobility(UObject* Object, EComponentMobility::Type ComponentMobility = EComponentMobility::Movable);

	/*
	 * Get the duration for the given sound

	 * @param Sound The sound to get the duration for
	 * @return The duration in seconds
	 */
	static MOVIESCENE_API float GetSoundDuration(USoundBase* Sound);

	/**
	 * Sort predicate that sorts lower bounds of a range
	 */
	static bool SortLowerBounds(TRangeBound<FFrameNumber> A, TRangeBound<FFrameNumber> B)
	{
		return TRangeBound<FFrameNumber>::MinLower(A, B) == A && A != B;
	}

	/**
	 * Sort predicate that sorts upper bounds of a range
	 */
	static bool SortUpperBounds(TRangeBound<FFrameNumber> A, TRangeBound<FFrameNumber> B)
	{
		return TRangeBound<FFrameNumber>::MinUpper(A, B) == A && A != B;
	}

	/**
	 * Sort predicate that sorts overlapping sections by row primarily, then by overlap priority
	 */
	static MOVIESCENE_API bool SortOverlappingSections(const UMovieSceneSection* A, const UMovieSceneSection* B);

	/*
	* Get weight needed to modify the global difference in order to correctly key this section due to it possibly being blended by other sections.
	* @param Section The Section who's weight we are calculating.
	* @param  Time we are at.
	* @return Returns the weight that needs to be applied to the global difference to correctly key this section.
	*/
	static MOVIESCENE_API float CalculateWeightForBlending(UMovieSceneSection* SectionToKey, FFrameNumber Time);

	/*
	 * Return a name unique to the binding names in the given movie scene
	 * @param InMovieScene The movie scene to look for existing possessables.
	 * @param InName The requested name to make unique.
	 * @return The unique name
	 */
	static MOVIESCENE_API FString MakeUniqueBindingName(UMovieScene* InMovieScene, const FString& InName);

	/*
	 * Return a name unique to the spawnable names in the given movie scene
	 * @param InMovieScene The movie scene to look for existing spawnables.
	 * @param InName The requested name to make unique.
	 * @return The unique name
	 */
	static MOVIESCENE_API FString MakeUniqueSpawnableName(UMovieScene* InMovieScene, const FString& InName);

	/**
	 * Return a copy of the source object, suitable for use as a spawnable template.
	 * @param InSourceObject The source object to convert into a spawnable template
	 * @param InMovieScene The movie scene the spawnable template will be associated with
	 * @param InName The name to use for the spawnable template
	 * @return The spawnable template
	 */
	static MOVIESCENE_API UObject* MakeSpawnableTemplateFromInstance(UObject& InSourceObject, UMovieScene* InMovieScene, FName InName);

	/*
	* Returns whether the given ObjectId is valid and is currently bound to at least 1 spawnable give the current context.
	* More specifically, if a FMovieSceneSpawnable exists with this ObjectId, true will be returned.
	* If a Level Sequence binding reference exists with a Custom Binding implementing MovieSceneSpawnableBindingBase, true will be returned.
	* If a Level Sequence binding reference exists with a Custom Binding implementing MovieSceneReplaceableBindingBase and the Context is an editor world, then true will be returned.
	* Otherwise, false will be returned.
	*/
	static MOVIESCENE_API bool IsBoundToAnySpawnable(UMovieSceneSequence* Sequence, const FGuid& ObjectId, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState);

	/*
	* Returns whether the given ObjectId is valid and is the given bindingindex is currently bound to a spawnable give the current context.
	* More specifically, if a FMovieSceneSpawnable exists with this ObjectId, true will be returned.
	* If a Level Sequence binding reference for this guid with the given BindingIndex exists with a Custom Binding implementing MovieSceneSpawnableBindingBase, true will be returned.
	* If a Level Sequence binding reference for this guid with the given BindingIndex exists with a Custom Binding implementing MovieSceneReplaceableBindingBase and the Context is an editor world, then true will be returned.
	* Otherwise, false will be returned.
	*/
	static MOVIESCENE_API bool IsBoundToSpawnable(UMovieSceneSequence* Sequence, const FGuid& ObjectId, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);

	/*
	* Attempts to create a new custom spawnable binding for the passed in UObject*. 
	* Where possible, it is preferred to call FSequencerUtilities::CreateOrReplaceBinding as it handles more cases. This should only be called in cases where there is no editor or sequencer context.
	* FactoryCreatedActor may be passed in as an alternative option for creating the binding in the case an actor factory was able to create an actor from this object.
	*/

	static MOVIESCENE_API FGuid TryCreateCustomSpawnableBinding(UMovieSceneSequence* Sequence, UObject* CustomBindingObject);
	

	/*
	* Returns the single bound object currently bound to the given objectid and binding index (optional).
	*/
	static MOVIESCENE_API UObject* GetSingleBoundObject(UMovieSceneSequence* Sequence, const FGuid& ObjectId, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);

	/*
	* If the binding for the given ObjectId supports object templates, returns the template, otherwise returns nullptr
	*/
	static MOVIESCENE_API UObject* GetObjectTemplate(UMovieSceneSequence* Sequence, const FGuid& ObjectId, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);

	/*
	* If the binding for the given ObjectId supports object templates, sets the template and returns true, otherwise returns false
	*/
	static MOVIESCENE_API bool SetObjectTemplate(UMovieSceneSequence* Sequence, const FGuid& ObjectId, UObject* InSourceObject, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);

	/*
	* Returns whether the binding for the given ObjectId supports object templates
	*/
	static MOVIESCENE_API bool SupportsObjectTemplate(UMovieSceneSequence* Sequence, const FGuid& ObjectId, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);

	/*
	* If the binding for the given ObjectId supports object templates, copies the object template into the binding and returns true, otherwise returns false
	*/
	static MOVIESCENE_API bool CopyObjectTemplate(UMovieSceneSequence* Sequence, const FGuid& ObjectId, UObject* InSourceObject, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState, int32 BindingIndex = 0);
#if WITH_EDITORONLY_DATA
	/*
	* Returns the bound object class for the binding for the given ObjectId.
	*/
	static MOVIESCENE_API const UClass* GetBoundObjectClass(UMovieSceneSequence* Sequence, const FGuid& ObjectId, int32 BindingIndex = 0);
#endif

	/* Returns a sorted list of all custom binding type classes currently known. Slow, may desire to cache result*/
	static void MOVIESCENE_API GetPrioritySortedCustomBindingTypes(TArray<const TSubclassOf<UMovieSceneCustomBinding>>& OutCustomBindingTypes);

	/* For cases where the user does not have a IMovieScenePlayer with a shared playback state, creates a transient one. Use sparingly. */
	static TSharedRef<UE::MovieScene::FSharedPlaybackState> MOVIESCENE_API CreateTransientSharedPlaybackState(UObject* WorldContext, UMovieSceneSequence* Sequence);

	/* Finds the resolution context to use to resolve the given guid. */
	static MOVIESCENE_API UObject* GetResolutionContext(UMovieSceneSequence* Sequence, const FGuid& ObjectId, const FMovieSceneSequenceID& SequenceID, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState);

	/* Given a movie scene track and an optional section inside it, returns an optional single condition that needs to be evaluated.
	* If multiple conditions exist in the given scope (for example a track condition, a track row condition for the row the section is on, and a section),
	* a UMovieSceneGroupCondition will be generated, and the caller is responsible for holding a reference to this new UObject.
	* If bFromCompilation is true, then any generated conditions will be stored on the movie scene.
	*/
	static MOVIESCENE_API const UMovieSceneCondition* GetSequenceCondition(const UMovieSceneTrack* Track, const UMovieSceneSection* Section, bool bFromCompilation=false);
	
	/* Helper function for evaluating a condition in a movie scene, taking advantage of any cacheing that may apply. */
	static MOVIESCENE_API bool EvaluateSequenceCondition(const FGuid& BindingID, const FMovieSceneSequenceID& SequenceID, const UMovieSceneCondition* Condition, UObject* ConditionOwnerObject, TSharedRef<const UE::MovieScene::FSharedPlaybackState> SharedPlaybackState);
};

/**
 * Manages bindings to keyed properties for a track instance. 
 * Calls UFunctions to set the value on runtime objects
 */
class FTrackInstancePropertyBindings
{
public:

	MOVIESCENE_API FTrackInstancePropertyBindings(FName InPropertyName, const FString& InPropertyPath);

	/**
	 * Calls the setter function for a specific runtime object or if the setter function does not exist, the property is set directly
	 *
	 * @param InRuntimeObject The runtime object whose function to call
	 * @param PropertyValue The new value to assign to the property
	 */
	template <typename ValueType>
	void CallFunction(UObject& InRuntimeObject, typename TCallTraits<ValueType>::ParamType PropertyValue)
	{
		FResolvedPropertyAndFunction PropAndFunction = FindOrAdd(InRuntimeObject);

		FProperty* Property = PropAndFunction.ResolvedProperty.GetValidProperty();
		if (Property && Property->HasSetter())
		{
			Property->CallSetter(&InRuntimeObject, &PropertyValue);
		}
		else if (UFunction* SetterFunction = PropAndFunction.SetterFunction.Get())
		{
			InvokeSetterFunction(&InRuntimeObject, SetterFunction, PropertyValue);
		}
		else if (ValueType* Val = PropAndFunction.GetPropertyAddress<ValueType>())
		{
			*Val = MoveTempIfPossible(PropertyValue);
		}

		if (UFunction* NotifyFunction = PropAndFunction.NotifyFunction.Get())
		{
			InRuntimeObject.ProcessEvent(NotifyFunction, nullptr);
		}
	}

	/**
	 * Calls the setter function for a specific runtime object or if the setter function does not exist, the property is set directly
	 *
	 * @param InRuntimeObject The runtime object whose function to call
	 * @param PropertyValue The new value to assign to the property
	 */
	MOVIESCENE_API void CallFunctionForEnum( UObject& InRuntimeObject, int64 PropertyValue );

	/**
	 * Rebuilds the property and function mappings for a single runtime object, and adds them to the cache
	 *
	 * @param InRuntimeObject	The object to cache mappings for
	 */
	MOVIESCENE_API void CacheBinding(const UObject& InRuntimeObject);

	/**
	 * Gets the FProperty that is bound to the track instance.
	 *
	 * @param Object	The Object that owns the property
	 * @return			The property on the object if it exists
	 */
	MOVIESCENE_API FProperty* GetProperty(const UObject& Object);

	/**
	 * Returns whether this binding is valid for the given object.
	 *
	 * @param Object	The Object that owns the property
	 * @return          Whether the property binding is valid
	 */
	MOVIESCENE_API bool HasValidBinding(const UObject& Object);

	/**
	 * Gets the structure type of the bound property if it is a property of that type.
	 *
	 * @param Object	The Object that owns the property
	 * @return          The structure type
	 */
	MOVIESCENE_API const UStruct* GetPropertyStruct(const UObject& Object);

	/**
	 * Gets the current value of a property on an object
	 *
	 * @param Object	The object to get the property from
	 * @return ValueType	The current value
	 */
	template <typename ValueType>
	ValueType GetCurrentValue(const UObject& Object)
	{
		ValueType Value{};

		FResolvedPropertyAndFunction PropAndFunction = FindOrAdd(Object);
		TryGetPropertyValue<ValueType>(PropAndFunction.ResolvedProperty, Value);

		return Value;
	}

	/**
	 * Optionally gets the current value of a property on an object
	 *
	 * @param Object	The object to get the property from
	 * @return (Optional) The current value of the property on the object
	 */
	template <typename ValueType>
	TOptional<ValueType> GetOptionalValue(const UObject& Object)
	{
		ValueType Value{};

		FResolvedPropertyAndFunction PropAndFunction = FindOrAdd(Object);
		if (TryGetPropertyValue<ValueType>(PropAndFunction.ResolvedProperty, Value))
		{
			return Value;
		}

		return TOptional<ValueType>();
	}

	/**
	 * Static function for accessing a property value on an object without caching its address
	 *
	 * @param Object			The object to get the property from
	 * @param InPropertyPath	The path to the property to retrieve
	 * @return (Optional) The current value of the property on the object
	 */
	template <typename ValueType>
	static TOptional<ValueType> StaticValue(const UObject* Object, const FString& InPropertyPath)
	{
		checkf(Object, TEXT("No object specified"));

		FTrackInstancePropertyBindings Temp(NAME_None, InPropertyPath);
		FResolvedProperty ResolvedProperty = ResolveProperty(Temp, *Object);

		ValueType Value;
		if (TryGetPropertyValue<ValueType>(ResolvedProperty, Value))
		{
			return Value;
		}

		return TOptional<ValueType>();
	}

	/**
	 * Gets the current value of a property on an object
	 *
	 * @param Object	The object to get the property from
	 * @return ValueType	The current value
	 */
	MOVIESCENE_API int64 GetCurrentValueForEnum(const UObject& Object);

	/**
	 * Sets the current value of a property on an object
	 *
	 * @param Object	The object to set the property on
	 * @param InValue   The value to set
	 */
	template <typename ValueType>
	void SetCurrentValue(UObject& Object, typename TCallTraits<ValueType>::ParamType InValue)
	{
		FResolvedPropertyAndFunction PropAndFunction = FindOrAdd(Object);

		if (ValueType* Val = PropAndFunction.GetPropertyAddress<ValueType>())
		{
			*Val = InValue;

			if (UFunction* NotifyFunction = PropAndFunction.NotifyFunction.Get())
			{
				Object.ProcessEvent(NotifyFunction, nullptr);
			}
		}
	}

	/** @return the property path that this binding was initialized from */
	const FString& GetPropertyPath() const
	{
		return PropertyPath;
	}

	/** @return the property name that this binding was initialized from */
	const FName& GetPropertyName() const
	{
		return PropertyName;
	}

	static MOVIESCENE_API FProperty* FindProperty(const UObject* Object, const FString& InPropertyPath);

private:

	/**
	 * Wrapper for UObject::ProcessEvent that attempts to pass the new property value directly to the function as a parameter,
	 * but handles cases where multiple parameters or a return value exists. The setter parameter must be the first in the list,
	 * any other parameters will be default constructed.
	 */
	template<typename T>
	static void InvokeSetterFunction(UObject* InRuntimeObject, UFunction* Setter, T&& InPropertyValue);

	struct FResolvedProperty
	{
		TWeakFieldPtr<FProperty> Property;

		void* ContainerAddress;

		int32 ArrayIndex = INDEX_NONE;

		FProperty* GetValidProperty() const
		{
			FProperty* PropertyPtr = Property.Get();
			if (PropertyPtr && ContainerAddress && !PropertyPtr->HasAnyFlags(RF_BeginDestroyed | RF_FinishDestroyed))
			{
				return PropertyPtr;
			}
			return nullptr;
		}

		template<typename ValueType>
		ValueType* GetPropertyAddress() const
		{
			if (FProperty* PropertyPtr = GetValidProperty())
			{
				if (ArrayIndex == INDEX_NONE)
				{
					return PropertyPtr->ContainerPtrToValuePtr<ValueType>(ContainerAddress);
				}
				else if (FArrayProperty* ArrayProp = CastFieldChecked<FArrayProperty>(PropertyPtr))
				{
					FScriptArrayHelper ArrayHelper(ArrayProp, ArrayProp->ContainerPtrToValuePtr<void>(ContainerAddress));
					check(ArrayHelper.IsValidIndex(ArrayIndex));
					return (ValueType*)ArrayHelper.GetRawPtr(ArrayIndex);
				}
			}
			return nullptr;
		}

		FResolvedProperty()
			: Property(nullptr)
			, ContainerAddress(nullptr)
		{}
	};

	struct FResolvedPropertyAndFunction
	{
		FResolvedProperty ResolvedProperty;
		TWeakObjectPtr<UFunction> SetterFunction;
		TWeakObjectPtr<UFunction> NotifyFunction;

		template<typename ValueType>
		ValueType* GetPropertyAddress() const
		{
			return ResolvedProperty.GetPropertyAddress<ValueType>();
		}

		FResolvedPropertyAndFunction()
			: ResolvedProperty()
			, SetterFunction( nullptr )
			, NotifyFunction( nullptr )
		{}
	};

	template <typename ValueType>
	static bool TryGetPropertyValue(const FResolvedProperty& ResolvedProperty, ValueType& OutValue)
	{
		if (const ValueType* Value = ResolvedProperty.GetPropertyAddress<ValueType>())
		{
			OutValue = *Value;
			return true;
		}
		return false;
	}

	static MOVIESCENE_API FResolvedProperty ResolvePropertyRecursive(FTrackInstancePropertyBindings& Bindings, void* BasePointer, UStruct* InStruct, TArray<FString>& InPropertyNames, uint32 Index);
	static MOVIESCENE_API FResolvedProperty ResolveProperty(FTrackInstancePropertyBindings& Bindings, const UObject& Object);

	static MOVIESCENE_API void FindProperty(FTrackInstancePropertyBindings& Bindings, void* BasePointer, UStruct* InStruct, const FString& InPropertyName, FResolvedProperty& OutResolvedProperty);
	static MOVIESCENE_API FResolvedProperty FindPropertyAndArrayIndex(FTrackInstancePropertyBindings& Bindings, void* BasePointer, UStruct* InStruct, const FString& PropertyName);

	/** Find or add the FResolvedPropertyAndFunction for the specified object */
	MOVIESCENE_API FResolvedPropertyAndFunction FindOrAdd(const UObject& InObject);

private:

	/** Mapping of objects to bound functions that will be called to update data on the track */
	TMap< FObjectKey, FResolvedPropertyAndFunction > RuntimeObjectToFunctionMap;

	/** Path to the property we are bound to */
	FString PropertyPath;

	/** Name of the function to call to set values */
	FName FunctionName;

	/** Name of a function to call when a value has been set */
	FName NotifyFunctionName;

	/** Actual name of the property we are bound to */
	FName PropertyName;
};

/** Explicit specializations for bools */
template<> MOVIESCENE_API void FTrackInstancePropertyBindings::CallFunction<bool>(UObject& InRuntimeObject, TCallTraits<bool>::ParamType PropertyValue);
template<> MOVIESCENE_API bool FTrackInstancePropertyBindings::TryGetPropertyValue<bool>(const FResolvedProperty& ResolvedProperty, bool& OutValue);
template<> MOVIESCENE_API void FTrackInstancePropertyBindings::SetCurrentValue<bool>(UObject& Object, TCallTraits<bool>::ParamType InValue);

/** Explicit specializations for object pointers */
template<> MOVIESCENE_API void FTrackInstancePropertyBindings::CallFunction<UObject*>(UObject& InRuntimeObject, UObject* PropertyValue);
template<> MOVIESCENE_API bool FTrackInstancePropertyBindings::TryGetPropertyValue<UObject*>(const FResolvedProperty& ResolvedProperty, UObject*& OutValue);
template<> MOVIESCENE_API void FTrackInstancePropertyBindings::SetCurrentValue<UObject*>(UObject& Object, UObject* InValue);

template<typename T>
void FTrackInstancePropertyBindings::InvokeSetterFunction(UObject* InRuntimeObject, UFunction* Setter, T&& InPropertyValue)
{
	// CacheBinding already guarantees that the function has >= 1 parameters
	const int32 ParmsSize = Setter->ParmsSize;

	// This should all be const really, but ProcessEvent only takes a non-const void*
	void* InputParameter = const_cast<typename TDecay<T>::Type*>(&InPropertyValue);

	// By default we try and use the existing stack value
	uint8* Params = reinterpret_cast<uint8*>(InputParameter);

	check(InRuntimeObject && Setter);
	if (Setter->ReturnValueOffset != MAX_uint16 || Setter->NumParms > 0)
	{
		// Function has a return value or multiple parameters, we need to initialize memory for the entire parameter pack
		// We use alloca here (as in UObject::ProcessEvent) to avoid a heap allocation. Alloca memory survives the current function's stack frame.
		Params = reinterpret_cast<uint8*>(FMemory_Alloca(ParmsSize));

		bool bFirstProperty = true;
		for (FProperty* Property = Setter->PropertyLink; Property; Property = Property->PropertyLinkNext)
		{
			// Initialize the parameter pack with any param properties that reside in the container
			if (Property->IsInContainer(ParmsSize))
			{
				Property->InitializeValue_InContainer(Params);

				// The first encountered property is assumed to be the input value so initialize this with the user-specified value from InPropertyValue
				if (Property->HasAnyPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_ReturnParm) && bFirstProperty)
				{
					const bool bIsValid = ensureMsgf(sizeof(T) == Property->GetElementSize(), TEXT("Property type does not match for Sequencer setter function %s::%s (%" SIZE_T_FMT "bytes != %ibytes"), *InRuntimeObject->GetName(), *Setter->GetName(), sizeof(T), Property->GetElementSize());
					if (bIsValid)
					{
						Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Params), &InPropertyValue);
					}
					else
					{
						return;
					}
				}
				bFirstProperty = false;
			}
		}
	}

	// Now we have the parameters set up correctly, call the function
	InRuntimeObject->ProcessEvent(Setter, Params);
}

