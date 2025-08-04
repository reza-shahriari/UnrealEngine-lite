// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuT/ASTOpImagePatch.h"

#include "Containers/Map.h"
#include "MuR/ModelPrivate.h"
#include "MuR/RefCounted.h"
#include "MuR/Types.h"


namespace mu
{

	ASTOpImagePatch::ASTOpImagePatch()
		: base(this)
		, patch(this)
	{
	}


	//-------------------------------------------------------------------------------------------------
	ASTOpImagePatch::~ASTOpImagePatch()
	{
		// Explicit call needed to avoid recursive destruction
		ASTOp::RemoveChildren();
	}


	//-------------------------------------------------------------------------------------------------
	bool ASTOpImagePatch::IsEqual(const ASTOp& otherUntyped) const
	{
		if (otherUntyped.GetOpType()==GetOpType())
		{
			const ASTOpImagePatch* other = static_cast<const ASTOpImagePatch*>(&otherUntyped);
			return base == other->base &&
				patch == other->patch &&
				location == other->location;
		}
		return false;
	}


	//-------------------------------------------------------------------------------------------------
	uint64 ASTOpImagePatch::Hash() const
	{
		uint64 res = std::hash<EOpType>()(EOpType::IM_PATCH);
		hash_combine(res, base.child().get());
		hash_combine(res, patch.child().get());
		return res;
	}


	//-------------------------------------------------------------------------------------------------
	mu::Ptr<ASTOp> ASTOpImagePatch::Clone(MapChildFuncRef mapChild) const
	{
		Ptr<ASTOpImagePatch> n = new ASTOpImagePatch();
		n->base = mapChild(base.child());
		n->patch = mapChild(patch.child());
		n->location = location;
		return n;
	}


	//-------------------------------------------------------------------------------------------------
	void ASTOpImagePatch::ForEachChild(const TFunctionRef<void(ASTChild&)> f)
	{
		f(base);
		f(patch);
	}


	//-------------------------------------------------------------------------------------------------
	void ASTOpImagePatch::Link(FProgram& program, FLinkerOptions*)
	{
		// Already linked?
		if (!linkedAddress)
		{
			OP::ImagePatchArgs Args;
			FMemory::Memzero(Args);

			if (base) Args.base = base->linkedAddress;
			if (patch) Args.patch = patch->linkedAddress;
			Args.minX = location[0];
			Args.minY = location[1];

			linkedAddress = (OP::ADDRESS)program.OpAddress.Num();
			program.OpAddress.Add((uint32_t)program.ByteCode.Num());
			AppendCode(program.ByteCode, EOpType::IM_PATCH);
			AppendCode(program.ByteCode, Args);
		}

	}


	//-------------------------------------------------------------------------------------------------
	FImageDesc ASTOpImagePatch::GetImageDesc(bool returnBestOption, FGetImageDescContext* context) const
	{
		FImageDesc res;

		// Local context in case it is necessary
		FGetImageDescContext localContext;
		if (!context)
		{
			context = &localContext;
		}
		else
		{
			// Cached result?
			FImageDesc* PtrValue = context->m_results.Find(this);
			if (PtrValue)
			{
				return *PtrValue;
			}
		}

		// Actual work
		if (base)
		{
			res = base->GetImageDesc(returnBestOption, context);
		}


		// Cache the result
		if (context)
		{
			context->m_results.Add(this, res);
		}

		return res;
	}


	//-------------------------------------------------------------------------------------------------
	mu::Ptr<ImageSizeExpression> ASTOpImagePatch::GetImageSizeExpression() const
	{
		if (base)
		{
			return base->GetImageSizeExpression();
		}

		return nullptr;
	}


	//-------------------------------------------------------------------------------------------------
	void ASTOpImagePatch::GetLayoutBlockSize(int* pBlockX, int* pBlockY)
	{
		// We didn't find any layout yet.
		*pBlockX = 0;
		*pBlockY = 0;

		// Try the source
		if (base)
		{
			base->GetLayoutBlockSize( pBlockX, pBlockY );
		}

		if (patch && *pBlockX == 0 && *pBlockY == 0)
		{
			patch->GetLayoutBlockSize(pBlockX, pBlockY);
		}
	}


	FSourceDataDescriptor ASTOpImagePatch::GetSourceDataDescriptor(FGetSourceDataDescriptorContext* Context) const
	{
		// Cache management
		TUniquePtr<FGetSourceDataDescriptorContext> LocalContext;
		if (!Context)
		{
			LocalContext.Reset(new FGetSourceDataDescriptorContext);
			Context = LocalContext.Get();
		}

		FSourceDataDescriptor* Found = Context->Cache.Find(this);
		if (Found)
		{
			return *Found;
		}

		// Not cached: calculate
		FSourceDataDescriptor Result;

		if (base)
		{
			FSourceDataDescriptor SourceDesc = base->GetSourceDataDescriptor(Context);
			Result.CombineWith(SourceDesc);
		}

		if (patch)
		{
			FSourceDataDescriptor SourceDesc = patch->GetSourceDataDescriptor(Context);
			Result.CombineWith(SourceDesc);
		}

		Context->Cache.Add(this, Result);

		return Result;
	}

}
