// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Elements/Interfaces/TypedElementDataStorageInterface.h"
#include "Features/IModularFeature.h"
#include "Templates/Function.h"
#include "UObject/Interface.h"
#include "UObject/ObjectKey.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ObjectPtr.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/WeakObjectPtrTemplates.h"

namespace UE::Editor::DataStorage
{
/**
 * Interface to provide compatibility with existing systems that don't directly
 * support the data storage.
 */
class ICompatibilityProvider : public IModularFeature
{
public:
	using ObjectRegistrationFilter = TFunction<bool(const ICompatibilityProvider&, const UObject*)>;
	using ObjectToRowDealiaser = TFunction<RowHandle(const ICompatibilityProvider&, const UObject*)>;
	
	/**
	 * @section Type-agnostic functions
	 * These allow compatibility with any type. These do eventually fall back to the explicit versions.
	 * Any references given are non-owning so it's up to the caller to deregister the object after it's no longer
	 * available.
	 */

	/** 
	 * Adds a reference to an existing object to the data storage. The data storage does NOT take ownership of the object and
	 * the caller is responsible for managing the life cycle of the object. The address is only used for associating the object
	 * with a row and to setup the initial row data.
	 */
	template<typename ObjectType>
	RowHandle AddCompatibleObject(ObjectType&& Object);
	
	/** Removes a previously registered object from the data storage. */
	template<typename ObjectType>
	void RemoveCompatibleObject(ObjectType&& Object);

	template<typename ObjectType>
	RowHandle FindRowWithCompatibleObject(ObjectType&& Object) const;

	/**
	 * @section Callback registration
	 * Functions to register callbacks with the compatibility layer to help refine its operations.
	 */
	 
	/**
	 * Objects like actors are registered through the compatibility layer in bulk. This can lead to objects being added that cause
	 * conflicts with other data in the data storage. This callback offers the opportunity to inspect the objects that are being
	 * added and if they include an object that shouldn't be store it can filter them out.
	 */
	virtual void RegisterRegistrationFilter(ObjectRegistrationFilter Filter) = 0;
	/**
	 * Notifications and request can be made to the compatibility layer for objects that are stored but don't directly map to a row.
	 * An example is a UObject represented by a column. If the UObject gets updated there's no direct mapping to the row the column is
	 * stored in but the row still needs to be updated. For cases like this it's possible to store information to find the row that's
	 * being aliased.
	 */
	virtual void RegisterDealiaserCallback(ObjectToRowDealiaser Dealiaser) = 0;
	/**
	 * Allows a specific type to be associated with a table. Whenever a compatible object is added, the type information of that object
	 * will be used to find the closest match in the registered types and use the associated table. E.g. actors derive from uobjects so
	 * if the type information of an actor is registered the actor table will be used instead of the uobject table.
	 */
	virtual void RegisterTypeTableAssociation(TWeakObjectPtr<UStruct> TypeInfo, TableHandle Table) = 0;

	/**
	 * @section Explicit functions
	 * These are functions that work on specific types.
	 */

	/** Adds a UObject to the data storage. */
	virtual RowHandle AddCompatibleObjectExplicit(UObject* Object) = 0;
	/** Adds an FStruct to the data storage. */
	virtual RowHandle AddCompatibleObjectExplicit(void* Object, TWeakObjectPtr<const UScriptStruct> TypeInfo) = 0;
	
	/** Removes a UObject from the data storage. */
	virtual void RemoveCompatibleObjectExplicit(UObject* Object) = 0;
	/** Removes an FStruct from the data storage. */
	virtual void RemoveCompatibleObjectExplicit(void* Object) = 0;

	/** Finds a previously stored UObject. If not found an invalid row handle will be returned. */
	virtual RowHandle FindRowWithCompatibleObjectExplicit(const UObject* Object) const = 0;
	/** Finds a previously stored FStruct. If not found an invalid row handle will be returned. */
	virtual RowHandle FindRowWithCompatibleObjectExplicit(const void* Object) const = 0;

	/**
	 * @section Miscellaneous functions
	 */

	/** Check if a custom extension is supported. This can be used to check for in-development features, custom extensions, etc. */
	virtual bool SupportsExtension(FName Extension) const = 0;
	/** Provides a list of all extensions that are enabled. */
	virtual void ListExtensions(TFunctionRef<void(FName)> Callback) const = 0;
};

template<typename Type> Type* GetRawPointer(const TWeakObjectPtr<Type> Object)	{ return Object.Get(); }
template<typename Type> Type* GetRawPointer(const TObjectPtr<Type> Object)		{ return Object.Get(); }
template<typename Type> Type* GetRawPointer(const TStrongObjectPtr<Type> Object){ return Object.Get(); }
template<typename Type> Type* GetRawPointer(const TObjectKey<Type> Object)		{ return Object.ResolveObjectPtr(); }
template<typename Type> Type* GetRawPointer(const TUniquePtr<Type> Object)		{ return Object.Get(); }
template<typename Type> Type* GetRawPointer(const TSharedPtr<Type> Object)		{ return Object.Get(); }
#if UE_ENABLE_NOTNULL_WRAPPER
template<typename Type> Type* GetRawPointer(TNotNull<Type*> Object)				{ return Object; }
#endif
template<typename Type> Type* GetRawPointer(Type* Object)						{ return Object; }
template<typename Type> Type* GetRawPointer(Type& Object)						{ return &Object; }

template<typename ObjectType>
RowHandle ICompatibilityProvider::AddCompatibleObject(ObjectType&& Object)
{
	auto RawPointer = GetRawPointer(Forward<ObjectType>(Object));
	using BaseType = std::remove_cv_t<std::remove_pointer_t<decltype(RawPointer)>>;

	if constexpr (std::is_base_of_v<UObject, BaseType>)
	{
		return AddCompatibleObjectExplicit(RawPointer);
	}
	else
	{
		return AddCompatibleObjectExplicit(RawPointer, BaseType::StaticStruct());
	}
}

template<typename ObjectType>
void ICompatibilityProvider::RemoveCompatibleObject(ObjectType&& Object)
{
	RemoveCompatibleObjectExplicit(GetRawPointer(Forward<ObjectType>(Object)));
}

template<typename ObjectType>
RowHandle ICompatibilityProvider::FindRowWithCompatibleObject(ObjectType&& Object) const
{
	return FindRowWithCompatibleObjectExplicit(GetRawPointer(Forward<ObjectType>(Object)));
}

template<typename Subsystem>
struct TTypedElementSubsystemTraits final
{
	template<typename T, typename = void>
	struct HasRequiresGameThreadVariable 
	{ 
		static constexpr bool bAvailable = false; 
	};
	template<typename T>
	struct HasRequiresGameThreadVariable <T, decltype((void)T::bRequiresGameThread)>
	{ 
		static constexpr bool bAvailable = true; 
	};

	template<typename T, typename = void>
	struct HasIsHotReloadableVariable
	{ 
		static constexpr bool bAvailable = false;
	};
	template<typename T>
	struct HasIsHotReloadableVariable <T, decltype((void)T::bIsHotReloadable)>
	{ 
		static constexpr bool bAvailable = true;
	};

	static constexpr bool RequiresGameThread()
	{
		if constexpr (HasRequiresGameThreadVariable<Subsystem>::bAvailable)
		{
			return Subsystem::bRequiresGameThread;
		}
		else
		{
			static_assert(HasRequiresGameThreadVariable<Subsystem>::bAvailable, "Subsystem provided to the Typed Elements did not "
				"have a 'static constexpr bool bRequiresGameThread = true|false` declared or have a specialization for "
				"TTypedElementSubsystemTraits.");
			return true;
		}
	}

	static constexpr bool IsHotReloadable()
	{
		if constexpr (HasIsHotReloadableVariable<Subsystem>::bAvailable)
		{
			return Subsystem::bIsHotReloadable;
		}
		else
		{
			static_assert(HasIsHotReloadableVariable<Subsystem>::bAvailable, "Subsystem provided to the Typed Elements did not "
				"have a 'static constexpr bool bIsHotReloadable = true|false` declared or have a specialization for "
				"TTypedElementSubsystemTraits.");
			return false;
		}
	}
};
} // namespace UE::Editor::DataStorage
