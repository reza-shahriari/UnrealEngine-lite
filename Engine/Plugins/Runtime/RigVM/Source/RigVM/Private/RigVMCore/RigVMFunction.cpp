// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigVMCore/RigVMFunction.h"
#include "RigVMCore/RigVMTemplate.h"
#include "RigVMCore/RigVMRegistry.h"
#include "RigVMCore/RigVMExternalVariable.h"
#include "RigVMCore/RigVMStruct.h"
#include "RigVMPropertyUtils.h"
#include "UObject/Package.h"

FRigVMFunction::FRigVMFunction(const TCHAR* InName, FRigVMFunctionPtr InFunctionPtr, UScriptStruct* InStruct, int32 InIndex, const TArray<FRigVMFunctionArgument>& InArguments)
: Name(InName)
, Struct(InStruct)
, Factory(nullptr)
, FunctionPtr(InFunctionPtr)
, Index(InIndex)
, TemplateIndex(INDEX_NONE)
, Arguments(InArguments)
, ArgumentNames()
{
	ArgumentNames.SetNumZeroed(Arguments.Num());
	for(int32 ArgumentIndex = 0; ArgumentIndex < Arguments.Num(); ArgumentIndex++)
	{
		ArgumentNames[ArgumentIndex] = Arguments[ArgumentIndex].Name;
	}
}

FString FRigVMFunction::GetName() const
{
	return Name;
}

FName FRigVMFunction::GetMethodName() const
{
	FString FullName(Name);
	FString Right;
	if (FullName.Split(TEXT("::"), nullptr, &Right))
	{
		return *Right;
	}
	return NAME_None;
}

FString FRigVMFunction::GetModuleName() const
{
#if WITH_EDITOR
	if (Struct)
	{
		if (UPackage* Package = Struct->GetPackage())
		{
			return Package->GetName();
		}
	}
	if (Factory)
	{
		if (UPackage* Package = Factory->GetScriptStruct()->GetPackage())
		{
			return Package->GetName();
		}
	}
#endif
	return FString();
}

FString FRigVMFunction::GetModuleRelativeHeaderPath() const
{
#if WITH_EDITOR
	if (Struct)
	{
		FString ModuleRelativePath;
		if (Struct->GetStringMetaDataHierarchical(TEXT("ModuleRelativePath"), &ModuleRelativePath))
		{
			return ModuleRelativePath;
		}
	}
#endif
	return FString();
}

const TArray<TRigVMTypeIndex>& FRigVMFunction::GetArgumentTypeIndices() const
{
	if(ArgumentTypeIndices.IsEmpty() && !Arguments.IsEmpty())
	{
		const FRigVMRegistryWriteLock _;

		if(Struct)
		{
			for(const FRigVMFunctionArgument& Argument : Arguments)
			{
				if(const FProperty* Property = Struct->FindPropertyByName(Argument.Name))
				{
					FName CPPType = NAME_None;
					UObject* CPPTypeObject = nullptr;
					RigVMPropertyUtils::GetTypeFromProperty(Property, CPPType, CPPTypeObject);

					const FRigVMTemplateArgumentType Type(CPPType, CPPTypeObject);
					ArgumentTypeIndices.Add(FRigVMRegistry_NoLock::GetForWrite().FindOrAddType_NoLock(Type));
				}
			}
		}
		else if(const FRigVMTemplate* Template = GetTemplate_NoLock())
		{
			const int32 PermutationIndex = Template->FindPermutation(this);
			check(PermutationIndex != INDEX_NONE);

			for(const FRigVMFunctionArgument& FunctionArgument : Arguments)
			{
				const FRigVMTemplateArgument* TemplateArgument = Template->FindArgument(FunctionArgument.Name);
				check(TemplateArgument);
				ArgumentTypeIndices.Add(TemplateArgument->GetTypeIndex_NoLock(PermutationIndex));
			}
		}
		else
		{
			// checkNoEntry();
		}
	}
	return ArgumentTypeIndices;
}

const FRigVMTemplate* FRigVMFunction::GetTemplate() const
{
	const FRigVMRegistryReadLock _;
	return GetTemplate_NoLock();
}

const FRigVMTemplate* FRigVMFunction::GetTemplate_NoLock() const
{
	if(TemplateIndex == INDEX_NONE)
	{
		return nullptr;
	}

	const FRigVMTemplate* Template = &FRigVMRegistry_NoLock::GetForRead().GetTemplates_NoLock()[TemplateIndex];
	if(Template->NumPermutations() <= 1)
	{
		return nullptr;
	}

	return Template;
}

const UScriptStruct* FRigVMFunction::GetExecuteContextStruct(bool bLockRegistry) const
{
	if(Factory)
	{
		return Factory->GetExecuteContextStruct();
	}
	if(Struct && Struct->IsChildOf(FRigVMStruct::StaticStruct()))
	{
		FStructOnScope StructOnScope(Struct);
		const FRigVMStruct* StructOnScopeMemory = reinterpret_cast<const FRigVMStruct*>(StructOnScope.GetStructMemory());
		return StructOnScopeMemory->GetExecuteContextStruct();
		
	}
	return FRigVMExecuteContext::StaticStruct();
}

bool FRigVMFunction::SupportsExecuteContextStruct(const UScriptStruct* InExecuteContextStruct) const
{
	return InExecuteContextStruct->IsChildOf(GetExecuteContextStruct());
}

const FName& FRigVMFunction::GetArgumentNameForOperandIndex(int32 InOperandIndex, int32 InTotalOperands) const
{
	if(Factory)
	{
		const TArray<FName>* FactoryArgumentNames = Factory->UpdateArgumentNameCache_NoLock(InTotalOperands);
		check(FactoryArgumentNames);
		check(FactoryArgumentNames->IsValidIndex(InOperandIndex));
		return (*FactoryArgumentNames)[InOperandIndex];
	}

	check(ArgumentNames.IsValidIndex(InOperandIndex));
	return ArgumentNames[InOperandIndex];
}
