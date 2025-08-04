// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Curves/RichCurve.h"
#include "NiagaraStatelessRange.h"
#include "NiagaraCommon.h"
#include "StructUtils/InstancedStruct.h"

#include "NiagaraStatelessDistribution.generated.h"

UENUM()
enum class ENiagaraDistributionMode
{
	Binding,
	Expression,
	UniformConstant,
	NonUniformConstant,
	UniformRange,
	NonUniformRange,
	UniformCurve,
	NonUniformCurve,
	ColorGradient,
};

enum class ENiagaraDistributionCurveLUTMode
{
	Sample,		// Each sample in the LUT represents the curve evaulation
	Accumulate,	// Each sample in the LUT represents the acculumation of the curve evaluations
};

USTRUCT()
struct FNiagaraDistributionBase
{
	GENERATED_BODY()

	virtual ~FNiagaraDistributionBase() = default;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	ENiagaraDistributionMode Mode = ENiagaraDistributionMode::UniformConstant;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FNiagaraVariableBase ParameterBinding;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FInstancedStruct ParameterExpression;

	bool IsBinding() const { return Mode == ENiagaraDistributionMode::Binding; }
	bool IsExpression() const { return Mode == ENiagaraDistributionMode::Expression; }
	bool IsConstant() const { return Mode == ENiagaraDistributionMode::UniformConstant || Mode == ENiagaraDistributionMode::NonUniformConstant; }
	bool IsUniform() const { return Mode == ENiagaraDistributionMode::UniformConstant || Mode == ENiagaraDistributionMode::UniformRange; }
	bool IsCurve() const { return Mode == ENiagaraDistributionMode::UniformCurve || Mode == ENiagaraDistributionMode::NonUniformCurve; }
	bool IsGradient() const { return Mode == ENiagaraDistributionMode::ColorGradient; }
	bool IsRange() const { return Mode == ENiagaraDistributionMode::UniformRange || Mode == ENiagaraDistributionMode::NonUniformRange; }

#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<float> ChannelConstantsAndRanges;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<FRichCurve> ChannelCurves;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	int32 MaxLutSampleCount = 128;

	NIAGARA_API bool operator==(const FNiagaraDistributionBase& Other) const;

	void ForEachParameterBinding(TFunction<void(const FNiagaraVariableBase&)> Delegate) const;

	virtual bool AllowBinding() const { return true; }
	virtual bool AllowConstant() const { return true; }
	virtual bool AllowCurves() const { return true; }
	virtual bool DisplayAsColor() const { return false; }
	virtual int32 GetBaseNumberOfChannels() const { return 0; }
	virtual void UpdateValuesFromDistribution() { }

	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition(); }

	static void PostEditChangeProperty(UObject* OwnerObject, FPropertyChangedEvent& PropertyChangedEvent);
#endif
};

USTRUCT()
struct FNiagaraDistributionRangeInt
{
	GENERATED_BODY()

	FNiagaraDistributionRangeInt() = default;
	explicit FNiagaraDistributionRangeInt(int32 ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	ENiagaraDistributionMode Mode = ENiagaraDistributionMode::UniformConstant;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FNiagaraVariableBase ParameterBinding;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FInstancedStruct ParameterExpression;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	int32 Min = 0;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	int32 Max = 0;

	NIAGARA_API void InitConstant(int32 Value);
	NIAGARA_API FNiagaraStatelessRangeInt CalculateRange(const int32 Default = 0) const;

	bool IsBinding() const { return Mode == ENiagaraDistributionMode::Binding; }
	bool IsExpression() const { return Mode == ENiagaraDistributionMode::Expression; }
	bool IsConstant() const { return Mode == ENiagaraDistributionMode::UniformConstant || Mode == ENiagaraDistributionMode::NonUniformConstant; }
	bool IsUniform() const { return Mode == ENiagaraDistributionMode::UniformConstant || Mode == ENiagaraDistributionMode::UniformRange; }
	bool IsCurve() const { return Mode == ENiagaraDistributionMode::UniformCurve || Mode == ENiagaraDistributionMode::NonUniformCurve; }
	bool IsGradient() const { return Mode == ENiagaraDistributionMode::ColorGradient; }
	bool IsRange() const { return Mode == ENiagaraDistributionMode::UniformRange || Mode == ENiagaraDistributionMode::NonUniformRange; }

#if WITH_EDITORONLY_DATA
	FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetIntDef(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionRangeFloat : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionRangeFloat() = default;
	explicit FNiagaraDistributionRangeFloat(float ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionRangeFloat(float MinValue, float MaxValue) { InitRange(MinValue, MaxValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	float Min = 0.0f;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	float Max = 0.0f;

	NIAGARA_API void InitConstant(float Value);
	NIAGARA_API void InitRange(float MinValue, float MaxValue);
	NIAGARA_API FNiagaraStatelessRangeFloat CalculateRange(const float Default = 0.0f) const;

#if WITH_EDITORONLY_DATA
	virtual bool AllowCurves() const override { return false; }
	virtual int32 GetBaseNumberOfChannels() const override { return 1; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetFloatDef(); }
#endif
	bool SerializeFromMismatchedTag(const struct FPropertyTag& Tag, FStructuredArchive::FSlot Slot);
};

USTRUCT()
struct FNiagaraDistributionRangeVector2 : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionRangeVector2() = default;
	explicit FNiagaraDistributionRangeVector2(const FVector2f& ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f Min = FVector2f::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f Max = FVector2f::ZeroVector;

	NIAGARA_API void InitConstant(const FVector2f& Value);
	NIAGARA_API FNiagaraStatelessRangeVector2 CalculateRange(const FVector2f& Default = FVector2f::ZeroVector) const;

#if WITH_EDITORONLY_DATA
	virtual bool AllowCurves() const override { return false; }
	virtual int32 GetBaseNumberOfChannels() const override { return 2; }
	virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetVec2Def(); }
#endif
	bool SerializeFromMismatchedTag(const struct FPropertyTag& Tag, FStructuredArchive::FSlot Slot);
};

USTRUCT()
struct FNiagaraDistributionRangeVector3 : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionRangeVector3() = default;
	explicit FNiagaraDistributionRangeVector3(const FVector3f& ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector3f Min = FVector3f::ZeroVector;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector3f Max = FVector3f::ZeroVector;

	NIAGARA_API void InitConstant(const FVector3f& Value);
	NIAGARA_API FNiagaraStatelessRangeVector3 CalculateRange(const FVector3f& Default = FVector3f::ZeroVector) const;

#if WITH_EDITORONLY_DATA
	virtual bool AllowCurves() const override { return false; }
	virtual int32 GetBaseNumberOfChannels() const override { return 3; }
	virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetVec3Def(); }
#endif
	bool SerializeFromMismatchedTag(const struct FPropertyTag& Tag, FStructuredArchive::FSlot Slot);
};

USTRUCT()
struct FNiagaraDistributionRangeColor : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionRangeColor() = default;
	explicit FNiagaraDistributionRangeColor(const FLinearColor& ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FLinearColor Min = FLinearColor::White;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FLinearColor Max = FLinearColor::White;

	NIAGARA_API void InitConstant(const FLinearColor& Value);
	NIAGARA_API FNiagaraStatelessRangeColor CalculateRange(const FLinearColor& Default = FLinearColor::White) const;

#if WITH_EDITORONLY_DATA
	virtual bool AllowCurves() const override { return false; }
	virtual bool DisplayAsColor() const { return true; }
	virtual int32 GetBaseNumberOfChannels() const override { return 4; }
	virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetColorDef(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionFloat : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionFloat() = default;
	explicit FNiagaraDistributionFloat(float ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionFloat(std::initializer_list<float> CurvePoints) { InitCurve(CurvePoints); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<float> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

	NIAGARA_API void InitConstant(float Value);
	NIAGARA_API void InitCurve(std::initializer_list<float> CurvePoints);
#if WITH_EDITORONLY_DATA
	NIAGARA_API void InitCurve(const TArray<FRichCurveKey>& CurveKeys);
#endif
	NIAGARA_API FNiagaraStatelessRangeFloat CalculateRange(const float Default = 0.0f) const;

#if WITH_EDITORONLY_DATA
	bool operator==(const FNiagaraDistributionFloat& Other) const
	{
		return (FNiagaraDistributionBase)*this == (FNiagaraDistributionBase)Other && Values == Other.Values;
	}
	virtual int32 GetBaseNumberOfChannels() const override { return 1; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetFloatDef(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionVector2 : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionVector2() = default;
	explicit FNiagaraDistributionVector2(const float ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionVector2(const FVector2f& ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<FVector2f> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

	NIAGARA_API void InitConstant(const float Value);
	NIAGARA_API void InitConstant(const FVector2f& Value);
	NIAGARA_API FNiagaraStatelessRangeVector2 CalculateRange(const FVector2f& Default = FVector2f::ZeroVector) const;

#if WITH_EDITORONLY_DATA
	virtual int32 GetBaseNumberOfChannels() const override { return 2; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetVec2Def(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionVector3 : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionVector3() = default;
	explicit FNiagaraDistributionVector3(const float ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionVector3(const FVector3f& ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionVector3(std::initializer_list<float> CurvePoints) { InitCurve(CurvePoints); }
	explicit FNiagaraDistributionVector3(std::initializer_list<FVector3f> CurvePoints) { InitCurve(CurvePoints); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<FVector3f> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

	NIAGARA_API void InitConstant(const float Value);
	NIAGARA_API void InitConstant(const FVector3f& Value);
	NIAGARA_API void InitCurve(std::initializer_list<float> CurvePoints);
	NIAGARA_API void InitCurve(std::initializer_list<FVector3f> CurvePoints);
	NIAGARA_API FNiagaraStatelessRangeVector3 CalculateRange(const FVector3f& Default = FVector3f::ZeroVector) const;

#if WITH_EDITORONLY_DATA
	virtual int32 GetBaseNumberOfChannels() const override { return 3; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetVec3Def(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionPosition : public FNiagaraDistributionVector3
{
	GENERATED_BODY()

	FNiagaraDistributionPosition() = default;
	explicit FNiagaraDistributionPosition(const float ConstantValue) { InitConstant(ConstantValue); }
	explicit FNiagaraDistributionPosition(const FVector3f& ConstantValue) { InitConstant(ConstantValue); }

#if WITH_EDITORONLY_DATA
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetPositionDef(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionColor : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	FNiagaraDistributionColor() = default;
	explicit FNiagaraDistributionColor(const FLinearColor& ConstantValue) { InitConstant(ConstantValue); }

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<FLinearColor> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

	NIAGARA_API void InitConstant(const FLinearColor& Value);
	NIAGARA_API FNiagaraStatelessRangeColor CalculateRange(const FLinearColor& Default = FLinearColor::White) const;

#if WITH_EDITORONLY_DATA
	virtual bool DisplayAsColor() const override { return true; }
	virtual int32 GetBaseNumberOfChannels() const override { return 4; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;
	virtual FNiagaraTypeDefinition GetBindingTypeDef() const { return FNiagaraTypeDefinition::GetColorDef(); }
#endif
};

USTRUCT()
struct FNiagaraDistributionCurveFloat : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	NIAGARA_API FNiagaraDistributionCurveFloat();
	NIAGARA_API explicit FNiagaraDistributionCurveFloat(ENiagaraDistributionCurveLUTMode InLUTMode);

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<float> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

#if WITH_EDITORONLY_DATA
	bool operator==(const FNiagaraDistributionCurveFloat& Other) const
	{
		return (FNiagaraDistributionBase)*this == (FNiagaraDistributionBase)Other && Values == Other.Values;
	}

	virtual bool AllowBinding() const override { return false; }
	virtual bool AllowConstant() const override { return false; }
	virtual int32 GetBaseNumberOfChannels() const override { return 1; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;

private:
	ENiagaraDistributionCurveLUTMode LUTMode = ENiagaraDistributionCurveLUTMode::Sample;
#endif
};

USTRUCT()
struct FNiagaraDistributionCurveVector3 : public FNiagaraDistributionBase
{
	GENERATED_BODY()

	NIAGARA_API FNiagaraDistributionCurveVector3();
	NIAGARA_API explicit FNiagaraDistributionCurveVector3(ENiagaraDistributionCurveLUTMode InLUTMode);

	UPROPERTY(EditAnywhere, Category = "Parameters")
	TArray<FVector3f> Values;

	UPROPERTY(EditAnywhere, Category = "Parameters")
	FVector2f ValuesTimeRange = FVector2f(0.0f, 1.0f);

#if WITH_EDITORONLY_DATA
	bool operator==(const FNiagaraDistributionCurveVector3& Other) const
	{
		return (FNiagaraDistributionBase)*this == (FNiagaraDistributionBase)Other && Values == Other.Values;
	}

	virtual bool AllowBinding() const override { return false; }
	virtual bool AllowConstant() const override { return false; }
	virtual int32 GetBaseNumberOfChannels() const override { return 3; }
	NIAGARA_API virtual void UpdateValuesFromDistribution() override;

private:
	ENiagaraDistributionCurveLUTMode LUTMode = ENiagaraDistributionCurveLUTMode::Sample;
#endif
};

template<>
struct TStructOpsTypeTraits<FNiagaraDistributionRangeFloat> : public TStructOpsTypeTraitsBase2<FNiagaraDistributionRangeFloat>
{
	enum
	{
		WithStructuredSerializeFromMismatchedTag = true,
	};
};

template<>
struct TStructOpsTypeTraits<FNiagaraDistributionRangeVector2> : public TStructOpsTypeTraitsBase2<FNiagaraDistributionRangeVector2>
{
	enum
	{
		WithStructuredSerializeFromMismatchedTag = true,
	};
};

template<>
struct TStructOpsTypeTraits<FNiagaraDistributionRangeVector3> : public TStructOpsTypeTraitsBase2<FNiagaraDistributionRangeVector3>
{
	enum
	{
		WithStructuredSerializeFromMismatchedTag = true,
	};
};

