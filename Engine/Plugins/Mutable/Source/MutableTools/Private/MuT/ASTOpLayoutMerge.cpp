// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuT/ASTOpLayoutMerge.h"

#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"
#include "MuR/Mesh.h"
#include "MuR/MeshBufferSet.h"
#include "MuR/ModelPrivate.h"
#include "MuR/MutableTrace.h"
#include "MuR/RefCounted.h"
#include "MuR/System.h"
#include "MuR/Types.h"
#include "MuT/ASTOpConditional.h"
#include "MuT/ASTOpConstantResource.h"
#include "MuT/ASTOpMeshClipMorphPlane.h"
#include "MuT/ASTOpMeshRemoveMask.h"
#include "MuT/ASTOpMeshMorph.h"
#include "MuT/ASTOpSwitch.h"


namespace mu
{


	//-------------------------------------------------------------------------------------------------
	ASTOpLayoutMerge::ASTOpLayoutMerge()
		: Base(this)
		, Added(this)
	{
	}


	ASTOpLayoutMerge::~ASTOpLayoutMerge()
	{
		// Explicit call needed to avoid recursive destruction
		ASTOp::RemoveChildren();
	}


	bool ASTOpLayoutMerge::IsEqual(const ASTOp& otherUntyped) const
	{
		if (otherUntyped.GetOpType()==GetOpType())
		{
			const ASTOpLayoutMerge* other = static_cast<const ASTOpLayoutMerge*>(&otherUntyped);
			return Base == other->Base && Added == other->Added;
		}
		return false;
	}


	uint64 ASTOpLayoutMerge::Hash() const
	{
		uint64 res = std::hash<void*>()(Base.child().get());
		hash_combine(res, Added.child().get());
		return res;
	}


	mu::Ptr<ASTOp> ASTOpLayoutMerge::Clone(MapChildFuncRef mapChild) const
	{
		mu::Ptr<ASTOpLayoutMerge> n = new ASTOpLayoutMerge();
		n->Base = mapChild(Base.child());
		n->Added = mapChild(Added.child());
		return n;
	}


	void ASTOpLayoutMerge::ForEachChild(const TFunctionRef<void(ASTChild&)> f)
	{
		f(Base);
		f(Added);
	}


	void ASTOpLayoutMerge::Link(FProgram& program, FLinkerOptions*)
	{
		// Already linked?
		if (!linkedAddress)
		{
			OP::LayoutMergeArgs Args;
			FMemory::Memzero(Args);

			if (Base) Args.Base = Base->linkedAddress;
			if (Added) Args.Added = Added->linkedAddress;

			linkedAddress = (OP::ADDRESS)program.OpAddress.Num();
			program.OpAddress.Add((uint32_t)program.ByteCode.Num());
			AppendCode(program.ByteCode, GetOpType());
			AppendCode(program.ByteCode, Args);
		}

	}


	void ASTOpLayoutMerge::GetBlockLayoutSize(uint64 BlockId, int32* pBlockX, int32* pBlockY, FBlockLayoutSizeCache* cache)
	{
		if (Base)
		{
			Base->GetBlockLayoutSize(BlockId, pBlockX, pBlockY, cache);
		}

		if (!*pBlockX && Added)
		{
			Added->GetBlockLayoutSize(BlockId, pBlockX, pBlockY, cache);
		}
	}


}
