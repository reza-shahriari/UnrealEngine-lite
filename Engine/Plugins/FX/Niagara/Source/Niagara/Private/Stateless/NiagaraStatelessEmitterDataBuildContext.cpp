// Copyright Epic Games, Inc. All Rights Reserved.

#include "Stateless/NiagaraStatelessEmitterDataBuildContext.h"
#include "Stateless/NiagaraStatelessExpression.h"
#include "Stateless/NiagaraStatelessParticleSimExecData.h"

#include "NiagaraDataSetCompiledData.h"
#include "NiagaraParameterBinding.h"
#include "NiagaraParameterStore.h"
#include "NiagaraParameterStore.h"

void FNiagaraStatelessEmitterDataBuildContext::PreModuleBuild(int32 InShaderParameterOffset)
{
	ModuleBuiltDataOffset = BuiltData.Num();
	ShaderParameterOffset = InShaderParameterOffset;
	++RandomSeedOffest;
}

uint32 FNiagaraStatelessEmitterDataBuildContext::AddStaticData(TConstArrayView<float> FloatData) const
{
	for (int32 i=0; i <= StaticFloatData.Num() - FloatData.Num(); ++i)
	{
		if (FMemory::Memcmp(StaticFloatData.GetData() + i, FloatData.GetData(), FloatData.Num() * sizeof(float)) == 0)
		{
			return i;
		}
	}

	// Add new
	const uint32 OutIndex = StaticFloatData.AddUninitialized(FloatData.Num());
	FMemory::Memcpy(StaticFloatData.GetData() + OutIndex, FloatData.GetData(), FloatData.Num() * sizeof(float));
	return OutIndex;
}

uint32 FNiagaraStatelessEmitterDataBuildContext::AddStaticData(TConstArrayView<FVector2f> FloatData) const
{
	return AddStaticData(MakeArrayView(reinterpret_cast<const float*>(FloatData.GetData()), FloatData.Num() * 2));
}

uint32 FNiagaraStatelessEmitterDataBuildContext::AddStaticData(TConstArrayView<FVector3f> FloatData) const
{
	return AddStaticData(MakeArrayView(reinterpret_cast<const float*>(FloatData.GetData()), FloatData.Num() * 3));
}

uint32 FNiagaraStatelessEmitterDataBuildContext::AddStaticData(TConstArrayView<FVector4f> FloatData) const
{
	return AddStaticData(MakeArrayView(reinterpret_cast<const float*>(FloatData.GetData()), FloatData.Num() * 4));
}

uint32 FNiagaraStatelessEmitterDataBuildContext::AddStaticData(TConstArrayView<FLinearColor> FloatData) const
{
	return AddStaticData(MakeArrayView(reinterpret_cast<const float*>(FloatData.GetData()), FloatData.Num() * 4));
}

int32 FNiagaraStatelessEmitterDataBuildContext::AddRendererBinding(const FNiagaraVariableBase& Variable) const
{
	int32 DataOffset = INDEX_NONE;
	if (Variable.IsValid())
	{
		FNiagaraVariable Var(Variable);
		RendererBindings.AddParameter(Var, false, false, &DataOffset);
		DataOffset /= sizeof(uint32);
	}

	return DataOffset;
}

int32 FNiagaraStatelessEmitterDataBuildContext::AddRendererBinding(const FNiagaraParameterBinding& Binding) const
{
	int32 DataOffset = INDEX_NONE;
	if (Binding.ResolvedParameter.IsValid())
	{
		FNiagaraVariable Var(Binding.ResolvedParameter);
		RendererBindings.AddParameter(Var, false, false, &DataOffset);
		DataOffset /= sizeof(uint32);
	}

	return DataOffset;
}

int32 FNiagaraStatelessEmitterDataBuildContext::AddRendererBinding(const FNiagaraParameterBindingWithValue& Binding) const
{
	int32 DataOffset = INDEX_NONE;
	if (Binding.ResolvedParameter.IsValid())
	{
		FNiagaraVariable Var(Binding.ResolvedParameter);
		RendererBindings.AddParameter(Var, false, false, &DataOffset);

		TConstArrayView<uint8> DefaultValue = Binding.GetDefaultValueArray();
		if (DefaultValue.Num() > 0)
		{
			check(DataOffset != INDEX_NONE);
			check(DefaultValue.Num() == Var.GetSizeInBytes());
			RendererBindings.SetParameterData(DefaultValue.GetData(), DataOffset, DefaultValue.Num());
		}

		DataOffset /= sizeof(uint32);
	}

	return DataOffset;
}


int32 FNiagaraStatelessEmitterDataBuildContext::AddExpression(const FInstancedStruct& ExpressionStruct) const
{
	int32 DataOffset = INDEX_NONE;
	if (const FNiagaraStatelessExpression* Expression = ExpressionStruct.GetPtr<FNiagaraStatelessExpression>())
	{
		const FName ExpressionName("__StatelessInternal__.Expression", Expressions.Num());
		const FNiagaraVariable ExpressionVariable(Expression->GetOutputTypeDef(), ExpressionName);
		RendererBindings.AddParameter(ExpressionVariable, false, false, &DataOffset);

		Expressions.Emplace(DataOffset, Expression->Build(*this));
		DataOffset /= sizeof(uint32);
	}
	return DataOffset;
}

void FNiagaraStatelessEmitterDataBuildContext::AddParticleSimulationExecSimulate(TFunction<void(const NiagaraStateless::FParticleSimulationContext&)> Func) const
{
	if (!ParticleExecData)
	{
		return;
	}

	ParticleExecData->SimulateFunctions.Emplace(MoveTemp(Func), ModuleBuiltDataOffset, ShaderParameterOffset, RandomSeedOffest);
}

int32 FNiagaraStatelessEmitterDataBuildContext::FindParticleVariableIndex(const FNiagaraVariableBase& Variable) const
{
	return ParticleDataSet.Variables.IndexOfByKey(Variable);
}
