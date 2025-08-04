﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AvaType.h"
#include "IAvaMaterialHandle.h"
#include "IAvaObjectHandle.h"
#include "MaterialTypes.h"
#include "Misc/TVariant.h"
#include "StructUtils/StructView.h"

class UMaterialInstance;
class UMaterialInstanceDynamic;
class UMaterialInterface;
struct FAvaMaskMaterialReference;

class FAvaMaterialInstanceHandle
	: public IAvaMaterialHandle
{
public:
	UE_AVA_INHERITS_WITH_SUPER(FAvaMaterialInstanceHandle, IAvaMaterialHandle);
	
	explicit FAvaMaterialInstanceHandle(const TWeakObjectPtr<UMaterialInterface>& InWeakParentMaterial);
	virtual ~FAvaMaterialInstanceHandle() override = default;

	// ~Begin IAvaMaterialHandle
	virtual FString GetMaterialName() override;
	virtual UMaterialInterface* GetMaterial() override;
	virtual void CopyParametersFrom(UMaterialInstance* InSourceMaterial) override;
	// ~End IAvaMaterialHandle

	// ~Begin IAvaObjectHandle
	virtual bool IsValid() const override;
	// ~End IAvaObjectHandle

	static bool IsSupported(const FAvaMaskMaterialReference& InInstance, FName InTag = NAME_None);

protected:
	virtual UMaterialInterface* GetParentMaterial() const;
	virtual UMaterialInstanceDynamic* GetMaterialInstance();
	virtual UMaterialInstanceDynamic* GetOrCreateMaterialInstance();
	
protected:
	TWeakObjectPtr<UMaterialInterface> WeakParentMaterial;
	TWeakObjectPtr<UMaterialInstanceDynamic> WeakMaterialInstance;
};
