// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Containers/Array.h"
#include "HAL/Platform.h"
#include "MuR/Operations.h"
#include "MuR/Ptr.h"
#include "MuR/Skeleton.h"
#include "MuT/AST.h"


namespace mu
{
	struct FProgram;

	//---------------------------------------------------------------------------------------------
	//!
	//---------------------------------------------------------------------------------------------
	class ASTOpMeshBindShape final : public ASTOp
	{
	public:

		ASTChild Mesh;
		ASTChild Shape;

		TArray<FBoneName> BonesToDeform;
		TArray<FBoneName> PhysicsToDeform;

		uint32 BindingMethod = 0;
		
		uint32 bRecomputeNormals	  : 1;
		uint32 bReshapeSkeleton	      : 1;
		uint32 bReshapePhysicsVolumes : 1;
		uint32 bReshapeVertices       : 1;
		uint32 bApplyLaplacian        : 1;

		EVertexColorUsage RChannelUsage = EVertexColorUsage::None;
		EVertexColorUsage GChannelUsage = EVertexColorUsage::None;
		EVertexColorUsage BChannelUsage = EVertexColorUsage::None;
		EVertexColorUsage AChannelUsage = EVertexColorUsage::None;
	public:

		ASTOpMeshBindShape();
		ASTOpMeshBindShape(const ASTOpMeshBindShape&) = delete;
		~ASTOpMeshBindShape();

		EOpType GetOpType() const override { return EOpType::ME_BINDSHAPE; }
		uint64 Hash() const override;
		bool IsEqual(const ASTOp& otherUntyped) const override;
		Ptr<ASTOp> Clone(MapChildFuncRef mapChild) const override;
		void ForEachChild(const TFunctionRef<void(ASTChild&)>) override;
		void Link(FProgram& program, FLinkerOptions* Options) override;
		Ptr<ASTOp> OptimiseSink(const FModelOptimizationOptions&, FOptimizeSinkContext&) const override;
		virtual FSourceDataDescriptor GetSourceDataDescriptor(FGetSourceDataDescriptorContext*) const override;

	};

}

