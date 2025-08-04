// Copyright Epic Games, Inc. All Rights Reserved.

#include "NNERuntimeRDGUnsqueeze.h"

#include "NNEHlslShadersLog.h"
#include "NNERuntimeRDGTensor.h"
#include "NNETypes.h"
#include "RenderGraphUtils.h"

namespace UE::NNERuntimeRDG::Private::Hlsl
{
	DECLARE_GPU_STAT_NAMED(FNNEOperatorUnsqueeze, TEXT("NNE.Operator.Hlsl.Unsqueeze"));

	/**
	 * Unsqueeze operator implementation
	 */
	class FUnsqueeze : public FOperatorHlsl
	{
	public:

		FUnsqueeze() {}
		virtual ~FUnsqueeze() = default;

		TArray<int32> Axes;

	public:

		virtual int PrepareOutputs(TConstArrayView<FTensorRef> InputTensors, TArrayView<FTensorRef> OutputTensors) override
		{
			check(InputTensors.Num() == 1);
			check(OutputTensors.Num() == 1);

			const FTensor& X = *InputTensors[0];
			TArray<uint32> OutputShapeData(X.GetShape().GetData());

			for (int32 Axe : Axes)
			{
				OutputShapeData.Insert(1, Axe);
			}
			
			NNE::FTensorShape OutputShape = NNE::FTensorShape::Make(OutputShapeData);
			
			OutputTensors[0]->SetShape(OutputShape);
			if (X.HasPreparedData())
			{
				OutputTensors[0]->SetPreparedData<uint8>(X.GetPreparedData<uint8>());
			}
			
			return 0;
		};

		virtual bool Initialize(TConstArrayView<NNE::FTensorDesc> InputTensorDescs, TConstArrayView<NNE::FTensorDesc> OutputTensorDescs, const NNERuntimeRDGData::Internal::FAttributeMap& Attributes) override
		{
			check(InputTensorDescs.Num() == 1);
			check(OutputTensorDescs.Num() == 1);

			Axes = Attributes.GetValue<TArray<int32>>(TEXT("axes"));
			for (int32 Axe : Axes)
			{
				if (Axe < 0)
				{
					UE_LOG(LogNNERuntimeRDGHlsl, Warning, TEXT("Unsqueeze: Does not support negative axes"));
					return false;
				}
				if (Axe >= InputTensorDescs[0].GetShape().Rank() + Axes.Num())
				{
					UE_LOG(LogNNERuntimeRDGHlsl, Warning, TEXT("Unsqueeze: Does not support axes greater than the number of dimensions of the resulting tensor shape"));
					return false;
				}
			}

			Axes.Sort();
			
			return true;
		}

		virtual void Dispatch(FRDGBuilder& GraphBuilder, TConstArrayView<FTensorRDGRef> InputTensors, TConstArrayView<FTensorRDGRef> OutputTensors) override
		{
			check(InputTensors.Num() == 1);
			check(OutputTensors.Num() == 1);
			check(InputTensors[0] != nullptr);
			check(OutputTensors[0] != nullptr);

			const FTensorRDG& Data = *InputTensors[0];
			const FTensorRDG& Output = *OutputTensors[0];

			RDG_EVENT_SCOPE_STAT(GraphBuilder, FNNEOperatorUnsqueeze, "NNE.Operator.Hlsl.Unsqueeze");
			RDG_GPU_STAT_SCOPE(GraphBuilder, FNNEOperatorUnsqueeze);

			AddCopyBufferPass(GraphBuilder, Output.GetBuffer(), Data.GetBuffer());
		}
	};

	bool ValidateUnsqueezeOperator(const NNERuntimeRDGData::Internal::FAttributeMap& AttributeMap, TConstArrayView<ENNETensorDataType> InputTypes, TConstArrayView<NNE::FSymbolicTensorShape> InputUnsqueezes)
	{
		bool bIsValid = true;

		FAttributeValidator AttributeValidator;
		AttributeValidator.AddRequired(TEXT("axes"), ENNERuntimeRDGDataAttributeDataType::Int32Array);
		bIsValid &= AttributeValidator.Validate(AttributeMap);

		FInputValidator InputValidator;
		InputValidator.AddSupportedType(ENNETensorDataType::Half);
		InputValidator.AddSupportedType(ENNETensorDataType::Float);
		InputValidator.AddSupportedType(ENNETensorDataType::Double);
		InputValidator.AddSupportedType(ENNETensorDataType::Int8);
		InputValidator.AddSupportedType(ENNETensorDataType::Int16);
		InputValidator.AddSupportedType(ENNETensorDataType::Int32);
		InputValidator.AddSupportedType(ENNETensorDataType::Int64);
		InputValidator.AddSupportedType(ENNETensorDataType::UInt8);
		InputValidator.AddSupportedType(ENNETensorDataType::UInt16);
		InputValidator.AddSupportedType(ENNETensorDataType::UInt32);
		InputValidator.AddSupportedType(ENNETensorDataType::UInt64);
		InputValidator.AddRequired();

		bIsValid &= InputValidator.Validate(InputTypes);

		return bIsValid;
	}

	FOperatorHlsl* CreateUnsqueezeOperator()
	{
		return new FUnsqueeze();
	}

	bool RegisterUnsqueezeOperator(FOperatorRegistryHlsl& Registry)
	{
		// Note: support of a particular version is partial with respect to tensor data types (only the most typical ones are usually supported).
		Registry.OpAdd({{TEXT("Unsqueeze"), TEXT("Onnx")}, 1}, CreateUnsqueezeOperator, ValidateUnsqueezeOperator);
		return true;
	}
} // UE::NNERuntimeRDG::Private::Hlsl
