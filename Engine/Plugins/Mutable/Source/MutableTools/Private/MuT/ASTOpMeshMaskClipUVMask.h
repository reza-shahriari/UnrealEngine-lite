// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/Platform.h"
#include "MuR/Operations.h"
#include "MuR/Ptr.h"
#include "MuT/AST.h"


namespace mu
{
	struct FProgram;

	/** Generate a mesh mask from an image mask or a layout with blocks. */
	class ASTOpMeshMaskClipUVMask final : public ASTOp
	{
	public:

		ASTChild Source;
		ASTChild UVSource;
		ASTChild MaskImage;
		ASTChild MaskLayout;
		uint8 LayoutIndex = 0;

	public:

		ASTOpMeshMaskClipUVMask();
		ASTOpMeshMaskClipUVMask(const ASTOpMeshMaskClipUVMask&) = delete;
		~ASTOpMeshMaskClipUVMask();

		EOpType GetOpType() const override { return EOpType::ME_MASKCLIPUVMASK; }
		uint64 Hash() const override;
		bool IsEqual(const ASTOp& otherUntyped) const override;
		Ptr<ASTOp> Clone(MapChildFuncRef mapChild) const override;
		void ForEachChild(const TFunctionRef<void(ASTChild&)>) override;
		void Link(FProgram& program, FLinkerOptions* Options) override;
		Ptr<ASTOp> OptimiseSink(const FModelOptimizationOptions&, FOptimizeSinkContext&) const override;
		virtual FSourceDataDescriptor GetSourceDataDescriptor(FGetSourceDataDescriptorContext*) const override;

	};

}

