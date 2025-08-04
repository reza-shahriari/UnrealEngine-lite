// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#if WITH_VERSE_VM || defined(__INTELLISENSE__)

#include "UObject/Class.h" // For UScriptStruct::ICppStructOps which can not be fwd-declared
#include "VVMCell.h"
#include "VVMEmergentTypeCreator.h"
#include "VVMGlobalHeapPtr.h"
#include <new>

namespace Verse
{
struct VShape;
struct VType;

struct VEmergentType final : VCell
{
	DECLARE_DERIVED_VCPPCLASSINFO(COREUOBJECT_API, VCell);

	TWriteBarrier<VShape> Shape; // This is immutable. If you need to change an object's shape, transition to a new emergent type that points to your new shape instead.
	TWriteBarrier<VType> Type;
	TWriteBarrier<VEmergentType> MeltTransition;
	VCppClassInfo* CppClassInfo = nullptr;

	static VEmergentType* New(FAllocationContext Context, VType* Type, VCppClassInfo* CppClassInfo)
	{
		return new (Context.AllocateEmergentType(sizeof(VEmergentType))) VEmergentType(Context, VEmergentTypeCreator::EmergentTypeForEmergentType.Get(), Type, CppClassInfo);
	}

	static VEmergentType* New(FAllocationContext Context, VShape* InShape, VType* Type, VCppClassInfo* CppClassInfo)
	{
		return new (Context.AllocateEmergentType(sizeof(VEmergentType))) VEmergentType(Context, InShape, VEmergentTypeCreator::EmergentTypeForEmergentType.Get(), Type, CppClassInfo);
	}

	VEmergentType& GetOrCreateMeltTransition(FAllocationContext Context)
	{
		if (VEmergentType* Transition = MeltTransition.Get())
		{
			return *Transition;
		}
		return GetOrCreateMeltTransitionSlow(Context);
	}

	static bool Equals(const VEmergentType& EmergentType, VType* Type, VCppClassInfo* CppClassInfo)
	{
		return EmergentType.Shape.Get() == nullptr && EmergentType.Type.Get() == Type && EmergentType.CppClassInfo == CppClassInfo;
	}

	static bool Equals(const VEmergentType& EmergentType, const VShape* InShape, VType* Type, VCppClassInfo* CppClassInfo)
	{
		return EmergentType.Shape.Get() == InShape && EmergentType.Type.Get() == Type && EmergentType.CppClassInfo == CppClassInfo;
	}

	friend uint32 GetTypeHash(const VEmergentType& EmergentType)
	{
		uint32 Hash = HashCombineFast(::GetTypeHash(EmergentType.Shape.Get()), ::GetTypeHash(EmergentType.Type.Get()));
		Hash = HashCombineFast(Hash, ::GetTypeHash(EmergentType.CppClassInfo));
		return Hash;
	}

	// Emergent types are not serialized; this also prevents them from being recorded.
	static constexpr bool SerializeIdentity = false;

private:
	friend class VEmergentTypeCreator;

	// Need this for the EmergentType of EmergentType.
	static VEmergentType* NewIncomplete(FAllocationContext Context, VCppClassInfo* CppClassInfo)
	{
		return new (Context.AllocateEmergentType(sizeof(VEmergentType))) VEmergentType(Context, CppClassInfo);
	}

	VEmergentType& GetOrCreateMeltTransitionSlow(FAllocationContext);

	void SetEmergentType(FAccessContext Context, VEmergentType* EmergentType)
	{
		VCell::SetEmergentType(Context, EmergentType);
	}

	VEmergentType(FAllocationContext Context, VCppClassInfo* CppClassInfo)
		: VCell()
		, CppClassInfo(CppClassInfo)
	{
	}

	VEmergentType(FAllocationContext Context, VEmergentType* EmergentType, VType* T, VCppClassInfo* CppClassInfo)
		: VCell(Context, EmergentType)
		, Type(Context, T)
		, CppClassInfo(CppClassInfo)
	{
	}

	VEmergentType(FAllocationContext Context, VShape* InShape, VEmergentType* EmergentType, VType* InType, VCppClassInfo* InCppClassInfo)
		: VCell(Context, EmergentType)
		, Shape(Context, InShape)
		, Type(Context, InType)
		, CppClassInfo(InCppClassInfo)
	{
	}
};

}; // namespace Verse

#endif // WITH_VERSE_VM
