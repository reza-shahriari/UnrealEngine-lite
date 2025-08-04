﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "MuT/ASTOpConstantMatrix.h"

mu::ASTOpConstantMatrix::ASTOpConstantMatrix(FMatrix44f InitValue) : value(InitValue)
{
}

uint64 mu::ASTOpConstantMatrix::Hash() const
{
	uint64 res = std::hash<uint64>()(uint64(EOpType::MA_CONSTANT));
	hash_combine(res, value.ComputeHash());
	return res;
}

bool mu::ASTOpConstantMatrix::IsEqual(const ASTOp& otherUntyped) const
{
	if (otherUntyped.GetOpType() == GetOpType())
	{
		const ASTOpConstantMatrix* other = static_cast<const ASTOpConstantMatrix*>(&otherUntyped);
		return value == other->value;
	}
	
	return false;
}

mu::Ptr<mu::ASTOp> mu::ASTOpConstantMatrix::Clone(MapChildFuncRef mapChild) const
{
	Ptr<ASTOpConstantMatrix> n = new ASTOpConstantMatrix();
	n->value = value;
	return n;
}

void mu::ASTOpConstantMatrix::Link(FProgram& program, FLinkerOptions* Options)
{
	if (!linkedAddress)
	{
		OP::MatrixConstantArgs Args;
		FMemory::Memzero(Args);
		Args.value = program.AddConstant(value);

		linkedAddress = static_cast<OP::ADDRESS>(program.OpAddress.Num());
		program.OpAddress.Add(static_cast<uint32_t>(program.ByteCode.Num()));
		AppendCode(program.ByteCode, EOpType::MA_CONSTANT);
		AppendCode(program.ByteCode, Args);
	}
}
