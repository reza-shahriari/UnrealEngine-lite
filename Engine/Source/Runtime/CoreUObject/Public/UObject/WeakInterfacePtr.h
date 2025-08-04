// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"
#include "Templates/Casts.h"
#include "ScriptInterface.h"

#include <type_traits>

/**
 * An alternative to TWeakObjectPtr that makes it easier to work through an interface.
 */
template<class T>
struct TWeakInterfacePtr
{
	using ElementType = T;
	using UObjectType = TCopyQualifiersFromTo_T<T, UObject>;

	FORCEINLINE TWeakInterfacePtr() = default;
	FORCEINLINE TWeakInterfacePtr(const TWeakInterfacePtr& Other) = default;
	FORCEINLINE TWeakInterfacePtr(TWeakInterfacePtr&& Other) = default;
	FORCEINLINE ~TWeakInterfacePtr() = default;
	FORCEINLINE TWeakInterfacePtr& operator=(const TWeakInterfacePtr& Other) = default;
	FORCEINLINE TWeakInterfacePtr& operator=(TWeakInterfacePtr&& Other) = default;

	/**
	 * Construct from an object pointer
	 * @param Object The object to create a weak pointer to. This object must implement interface T.
	 */
	template<
		typename U
		UE_REQUIRES(std::is_convertible_v<U, TCopyQualifiersFromTo_T<U, UObject>*>)
	>
	TWeakInterfacePtr(U&& Object)
	{
		InterfaceInstance = Cast<T>(ImplicitConv<TCopyQualifiersFromTo_T<U, UObject>*>(Object));
		if (InterfaceInstance != nullptr)
		{
			ObjectInstance = Object;
		}
	}

	/**
	 * Construct from an interface pointer
	 * @param Interface The interface pointer to create a weak pointer to. There must be a UObject behind the interface.
	 */
	TWeakInterfacePtr(T* Interface)
	{
		ObjectInstance = Cast<UObject>(Interface);
		if (ObjectInstance != nullptr)
		{
			InterfaceInstance = Interface;
		}
	}

	/**
	 * Construct from a TScriptInterface of the same interface type
	 * @param ScriptInterface 	The TScriptInterface to copy from.
	 * 							No validation is done here; passing an invalid TScriptInterface in will result in an invalid TWeakInterfacePtr.
	 */
	TWeakInterfacePtr(const TScriptInterface<T>& ScriptInterface)
	{
		ObjectInstance = ScriptInterface.GetObject();
		InterfaceInstance = ScriptInterface.GetInterface();
	}

	/**
	 * Reset the weak pointer back to the null state.
	 */
	FORCEINLINE void Reset()
	{
		InterfaceInstance = nullptr;
		ObjectInstance.Reset();
	}

	/**
	 * Test if this points to a live object. Parameters are passed to the underlying TWeakObjectPtr.
	 */
	FORCEINLINE bool IsValid(bool bEvenIfPendingKill, bool bThreadsafeTest = false) const
	{
		return InterfaceInstance != nullptr && ObjectInstance.IsValid(bEvenIfPendingKill, bThreadsafeTest);
	}

	/**
	 * Test if this points to a live object. Calls the underlying TWeakObjectPtr's parameterless IsValid method.
	 */
	FORCEINLINE bool IsValid() const
	{
		return InterfaceInstance != nullptr && ObjectInstance.IsValid();
	}

	/**
	 * Test if this pointer is stale. Parameters are passed to the underlying TWeakObjectPtr.
	 */
	FORCEINLINE bool IsStale(bool bEvenIfPendingKill = false, bool bThreadsafeTest = false) const
	{
		return InterfaceInstance != nullptr && ObjectInstance.IsStale(bEvenIfPendingKill, bThreadsafeTest);
	}

	/**
	 * Dereference the weak pointer into an interface pointer.
	 */
	FORCEINLINE T* Get() const
	{
		return IsValid() ? InterfaceInstance : nullptr;
	}

	/**
	 * Dereference the weak pointer into a UObject pointer.
	 */
	FORCEINLINE UObjectType* GetObject() const
	{
		return ObjectInstance.Get();
	}

	/**
	 * Dereference the weak pointer.
	 */
	FORCEINLINE T& operator*() const
	{
		check(IsValid());
		return *InterfaceInstance;
	}

	/**
	 * Dereference the weak pointer.
	 */
	FORCEINLINE T* operator->() const
	{
		check(IsValid());
		return InterfaceInstance;
	}

	/**
	 * Assign from an interface pointer.
	 */
	FORCEINLINE TWeakInterfacePtr<T>& operator=(T* Other)
	{
		*this = TWeakInterfacePtr<T>(Other);
		return *this;
	}

	/**
	 * Assign from a script interface.
	 */
	FORCEINLINE TWeakInterfacePtr<T>& operator=(const TScriptInterface<T>& Other)
	{
		ObjectInstance = Other.GetObject();
		InterfaceInstance = (T*)Other.GetInterface();
		return *this;
	}

	FORCEINLINE bool operator==(const TWeakInterfacePtr<T>& Other) const
	{
		return InterfaceInstance == Other.InterfaceInstance;
	}

	FORCEINLINE bool operator!=(const TWeakInterfacePtr<T>& Other) const
	{
		return InterfaceInstance != Other.InterfaceInstance;
	}

	FORCEINLINE TScriptInterface<T> ToScriptInterface() const
	{
		UObjectType* Object = ObjectInstance.Get();
		if (Object)
		{
			return TScriptInterface<T>(Object);
		}

		return TScriptInterface<T>();
	}

	FORCEINLINE TWeakObjectPtr<UObjectType> GetWeakObjectPtr() const
	{
		return ObjectInstance;
	}

private:
	TWeakObjectPtr<UObjectType> ObjectInstance;
	T* InterfaceInstance = nullptr;
};
