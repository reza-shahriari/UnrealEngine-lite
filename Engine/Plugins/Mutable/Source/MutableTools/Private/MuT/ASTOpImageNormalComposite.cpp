// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuT/ASTOpImageNormalComposite.h"

#include "Containers/Map.h"
#include "HAL/PlatformMath.h"
#include "HAL/UnrealMemory.h"
#include "MuR/ModelPrivate.h"
#include "MuR/RefCounted.h"
#include "MuR/Types.h"


namespace mu
{


	ASTOpImageNormalComposite::ASTOpImageNormalComposite()
		: Base(this)
		, Normal(this)
		, Mode(ECompositeImageMode::CIM_Disabled)
		, Power(1.0f)
	{
	}


	ASTOpImageNormalComposite::~ASTOpImageNormalComposite()
	{
		// Explicit call needed to avoid recursive destruction
		ASTOp::RemoveChildren();
	}


	bool ASTOpImageNormalComposite::IsEqual(const ASTOp& OtherUntyped) const
	{
		if (OtherUntyped.GetOpType()==GetOpType())
		{
			const ASTOpImageNormalComposite* Other = static_cast<const ASTOpImageNormalComposite*>(&OtherUntyped);
			return Base == Other->Base 
				&& Normal == Other->Normal 
				&& Power == Other->Power 
				&& Mode == Other->Mode;
		}

		return false;
	}


	uint64 ASTOpImageNormalComposite::Hash() const
	{
		uint64 Res = std::hash<void*>()(Base.child().get());
		hash_combine(Res, Normal.child().get());

		return Res;
	}


	mu::Ptr<ASTOp> ASTOpImageNormalComposite::Clone(MapChildFuncRef mapChild) const
	{
		Ptr<ASTOpImageNormalComposite> N = new ASTOpImageNormalComposite();
		N->Base = mapChild(Base.child());
		N->Normal = mapChild(Normal.child());
		N->Mode = Mode;
		N->Power = Power;

		return N;
	}


	void ASTOpImageNormalComposite::ForEachChild(const TFunctionRef<void(ASTChild&)> f)
	{
		f(Base);
		f(Normal);
	}


	void ASTOpImageNormalComposite::Link(FProgram& program, FLinkerOptions*)
	{
		// Already linked?
		if (!linkedAddress)
		{
			OP::ImageNormalCompositeArgs Args;
			FMemory::Memzero(Args);

			if (Base)
			{
				Args.base = Base->linkedAddress;
			}

			if (Normal)
			{
				Args.normal = Normal->linkedAddress;
			}

			Args.power = Power;
			Args.mode = Mode;

			linkedAddress = (OP::ADDRESS)program.OpAddress.Num();
			program.OpAddress.Add((uint32_t)program.ByteCode.Num());
			AppendCode(program.ByteCode, EOpType::IM_NORMALCOMPOSITE);
			AppendCode(program.ByteCode, Args);
		}

	}


	FImageDesc ASTOpImageNormalComposite::GetImageDesc(bool returnBestOption, FGetImageDescContext* context) const
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

		if (Base)
		{
			res = Base->GetImageDesc(returnBestOption, context);
		}

		if (context)
		{
			context->m_results.Add(this, res);
		}

		return res;
	}

	void ASTOpImageNormalComposite::GetLayoutBlockSize(int* pBlockX, int* pBlockY)
	{
		if (Base)
		{
			Base->GetLayoutBlockSize(pBlockX, pBlockY);
		}
	}

	mu::Ptr<ImageSizeExpression> ASTOpImageNormalComposite::GetImageSizeExpression() const
	{
		if (Base)
		{
			return Base->GetImageSizeExpression();
		}

		return nullptr;
	}


	FSourceDataDescriptor ASTOpImageNormalComposite::GetSourceDataDescriptor(FGetSourceDataDescriptorContext* Context) const
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

		if (Base)
		{
			FSourceDataDescriptor SourceDesc = Base->GetSourceDataDescriptor(Context);
			Result.CombineWith(SourceDesc);
		}

		if (Normal)
		{
			FSourceDataDescriptor SourceDesc = Normal->GetSourceDataDescriptor(Context);
			Result.CombineWith(SourceDesc);
		}

		Context->Cache.Add(this, Result);

		return Result;
	}

}
