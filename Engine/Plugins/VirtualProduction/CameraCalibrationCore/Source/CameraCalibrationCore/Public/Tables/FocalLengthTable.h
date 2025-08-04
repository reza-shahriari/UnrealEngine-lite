// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Curves/RichCurve.h"
#include "Tables/BaseLensTable.h"
#include "LensData.h"

#include "FocalLengthTable.generated.h"

#define UE_API CAMERACALIBRATIONCORE_API


/**
 * Focal length associated to a zoom value
 */
USTRUCT(BlueprintType)
struct FFocalLengthZoomPoint
{
	GENERATED_BODY()

public:

	/** Input zoom value for this point */
	UPROPERTY()
	float Zoom = 0.0f;

	/** Value expected to be normalized (unitless) */
	UPROPERTY()
	FFocalLengthInfo FocalLengthInfo;

	/** Whether this focal length was added along calibrated distortion parameters */
	UPROPERTY()
	bool bIsCalibrationPoint = false;
};

/**
 * Contains list of focal length points associated to zoom value
 */
USTRUCT()
struct FFocalLengthFocusPoint : public FBaseFocusPoint
{
	GENERATED_BODY()

	using PointType = FFocalLengthInfo;

public:
	//~ Begin FBaseFocusPoint Interface
	virtual float GetFocus() const override { return Focus; }
	UE_API virtual int32 GetNumPoints() const override;
	UE_API virtual float GetZoom(int32 Index) const override;
	//~ End FBaseFocusPoint Interface

	/** Returns data type copy value for a given float  */
	UE_API bool GetPoint(float InZoom, FFocalLengthInfo& OutData, float InputTolerance = KINDA_SMALL_NUMBER) const;

	/** Adds a new point at InZoom. Updates existing one if tolerance is met */
	UE_API bool AddPoint(float InZoom, const FFocalLengthInfo& InData, float InputTolerance, bool bIsCalibrationPoint);

	/** Sets an existing point at InZoom. Updates existing one if tolerance is met */
	UE_API bool SetPoint(float InZoom, const FFocalLengthInfo& InData, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Gets whether the point at InZoom is a calibration point. */
	UE_API bool IsCalibrationPoint(float InZoom, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Returns data at the requested index */
	UE_API bool GetValue(int32 Index, FFocalLengthInfo& OutData) const;

	/** Removes a point corresponding to specified zoom */
	UE_API void RemovePoint(float InZoomValue);

	/** Returns true if this point is empty */
	UE_API bool IsEmpty() const;

	/** Gets the curve for the specified parameter, or nullptr if the parameter index is invalid */
	UE_API const FRichCurve* GetCurveForParameter(int32 InParameterIndex) const;

	/** Gets the curve for the specified parameter, or nullptr if the parameter index is invalid */
	UE_API FRichCurve* GetCurveForParameter(int32 InParameterIndex);
	
public:

	/** Input focus for this point */
	UPROPERTY()
	float Focus = 0.0f;

	/** Curves mapping normalized Fx value to Zoom value (Time) */
	UPROPERTY()
	FRichCurve Fx;

	/** Curves mapping normalized Fy value to Zoom value (Time) */
	UPROPERTY()
	FRichCurve Fy;

	/** Used to know points that are locked */
	UPROPERTY()
	TArray<FFocalLengthZoomPoint> ZoomPoints;
};

USTRUCT()
struct FFocalLengthFocusCurve : public FBaseFocusCurve
{
	GENERATED_BODY()

public:
	/** Adds a new point to the focus curve, or updates a matching existing point if one is found */
	UE_API void AddPoint(float InFocus, const FFocalLengthInfo& InData, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Updates an existing point if one is found */
	UE_API void SetPoint(float InFocus, const FFocalLengthInfo& InData, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Removes the point at the specified focus if one is found */
	UE_API void RemovePoint(float InFocus, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Changes the focus value of the point at the specified focus, if one is found */
	UE_API void ChangeFocus(float InExistingFocus, float InNewFocus, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Changes the focus value of the point at the specified focus and optionally replaces any point at the new focus with the old point */
	UE_API void MergeFocus(float InExistingFocus, float InNewFocus, bool bReplaceExisting, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Gets whether the curve is empty */
	UE_API bool IsEmpty() const;

	/** Gets the curve for the specified parameter, or nullptr if the parameter index is invalid */
	UE_API const FRichCurve* GetCurveForParameter(int32 InParameterIndex) const;

	/** Gets the curve for the specified parameter, or nullptr if the parameter index is invalid */
	UE_API FRichCurve* GetCurveForParameter(int32 InParameterIndex);
	
public:
	/** Focus curve for the x parameter of the focal length */
	UPROPERTY()
	FRichCurve Fx;

	/** Focus curve for the y parameter of the focal length */
	UPROPERTY()
	FRichCurve Fy;

	/** The fixed zoom value of the curve */
	UPROPERTY()
	float Zoom = 0.0f;
};

/**
 * Focal Length table containing FxFy values for each focus and zoom input values
 */
USTRUCT()
struct FFocalLengthTable : public FBaseLensTable
{
	GENERATED_BODY()

	using FocusPointType = FFocalLengthFocusPoint;
	using FocusCurveType = FFocalLengthFocusCurve;

	/** Wrapper for indices of specific parameters for the focal length table  */
	struct FParameters
	{
		static constexpr int32 Aggregate = INDEX_NONE;
		static constexpr int32 Fx = 0;
		static constexpr int32 Fy = 1;

		/** Returns if a parameter index is valid (not including the aggregate value) */
		static bool IsValid(int32 InParameterIndex) { return InParameterIndex >= 0 && InParameterIndex < 2; }
		
		/** Returns if a parameter index is valid or the aggregate value */
		static bool IsValidOrAggregate(int32 InParameterIndex) { return IsValid(InParameterIndex) || InParameterIndex == Aggregate; }
	};
	
protected:
	//~ Begin FBaseDataTable Interface
	UE_API virtual TMap<ELensDataCategory, FLinkPointMetadata> GetLinkedCategories() const override;
	UE_API virtual bool DoesFocusPointExists(float InFocus, float InputTolerance = KINDA_SMALL_NUMBER) const override;
	UE_API virtual bool DoesZoomPointExists(float InFocus, float InZoom, float InputTolerance = KINDA_SMALL_NUMBER) const override;
	UE_API virtual const FBaseFocusPoint* GetBaseFocusPoint(int32 InIndex) const override;
	//~ End FBaseDataTable Interface
	
public:
	//~ Begin FBaseDataTable Interface
	UE_API virtual void ForEachPoint(FFocusPointCallback InCallback) const override;
	virtual int32 GetFocusPointNum() const override { return FocusPoints.Num(); }
	UE_API virtual int32 GetTotalPointNum() const override;
	UE_API virtual UScriptStruct* GetScriptStruct() const override;
	UE_API virtual bool BuildParameterCurveAtFocus(float InFocus, int32 ParameterIndex, FRichCurve& OutCurve) const override;
	UE_API virtual bool BuildParameterCurveAtZoom(float InZoom, int32 ParameterIndex, FRichCurve& OutCurve) const override;
	UE_API virtual void SetParameterCurveKeysAtFocus(float InFocus, int32 InParameterIndex, const FRichCurve& InSourceCurve, TArrayView<const FKeyHandle> InKeys) override;
	UE_API virtual void SetParameterCurveKeysAtZoom(float InZoom, int32 InParameterIndex, const FRichCurve& InSourceCurve, TArrayView<const FKeyHandle> InKeys) override;
	virtual bool CanEditCurveKeyPositions(int32 InParameterIndex) const override { return true; }
	virtual bool CanEditCurveKeyAttributes(int32 InParameterIndex) const override { return true; }
	UE_API virtual TRange<double> GetCurveKeyPositionRange(int32 InParameterIndex) const override;
	UE_API virtual FText GetParameterValueLabel(int32 InParameterIndex) const override;
	UE_API virtual FText GetParameterValueUnitLabel(int32 InParameterIndex) const override;
	//~ End FBaseDataTable Interface
	
	/** Returns const point for a given focus */
	UE_API const FFocalLengthFocusPoint* GetFocusPoint(float InFocus, float InputTolerance = KINDA_SMALL_NUMBER) const;
	
	/** Returns point for a given focus */
	UE_API FFocalLengthFocusPoint* GetFocusPoint(float InFocus, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Gets the focus curve for the specified zoom, or nullptr if none were found */
	UE_API const FFocalLengthFocusCurve* GetFocusCurve(float InZoom, float InputTolerance = KINDA_SMALL_NUMBER) const;

	/** Gets the focus curve for the specified zoom, or nullptr if none were found */
	UE_API FFocalLengthFocusCurve* GetFocusCurve(float InZoom, float InputTolerance = KINDA_SMALL_NUMBER);
	
	/** Returns all focus points */
	UE_API TConstArrayView<FFocalLengthFocusPoint> GetFocusPoints() const;

	/** Returns all focus points */
	UE_API TArray<FFocalLengthFocusPoint>& GetFocusPoints();
	
	/** Returns all focus curves */
	UE_API TConstArrayView<FFocalLengthFocusCurve> GetFocusCurves() const;

	/** Returns all focus curves */
	UE_API TArray<FFocalLengthFocusCurve>& GetFocusCurves();
	
	/** Removes a focus point identified as InFocusIdentifier */
	UE_API void RemoveFocusPoint(float InFocus);

	/** Checks to see if there exists a focus point matching the specified focus value */
	UE_API bool HasFocusPoint(float InFocus, float InputTolerance = KINDA_SMALL_NUMBER) const;

	/** Changes the value of a focus point */
	UE_API void ChangeFocusPoint(float InExistingFocus, float InNewFocus, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Merges the points in the specified source focus into the specified destination focus */
	UE_API void MergeFocusPoint(float InSrcFocus, float InDestFocus, bool bReplaceExistingZoomPoints, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Removes a zoom point from a focus point*/
	UE_API void RemoveZoomPoint(float InFocus, float InZoom);

	/** Checks to see if there exists a zoom point matching the specified zoom and focus values */
	UE_API bool HasZoomPoint(float InFocus, float InZoom, float InputTolerance = KINDA_SMALL_NUMBER);

	/** Changes the value of a zoom point */
	UE_API void ChangeZoomPoint(float InFocus, float InExistingZoom, float InNewZoom, float InputTolerance = KINDA_SMALL_NUMBER);
	
	/** Adds a new point in the table */
	UE_API bool AddPoint(float InFocus, float InZoom, const FFocalLengthInfo& InData, float InputTolerance, bool bIsCalibrationPoint);

	/** Get the point from the table */
	UE_API bool GetPoint(const float InFocus, const float InZoom, FFocalLengthInfo& OutData, float InputTolerance = KINDA_SMALL_NUMBER) const;

	/** Set a new point into the table */
	UE_API bool SetPoint(float InFocus, float InZoom, const FFocalLengthInfo& InData, float InputTolerance = KINDA_SMALL_NUMBER);
	
	/** Builds the focus curves to match existing data in the table */
	UE_API void BuildFocusCurves();
	
public:

	/** Lists of focus points */
	UPROPERTY()
	TArray<FFocalLengthFocusPoint> FocusPoints;

	/** A list of curves along the focus axis for each zoom value */
	UPROPERTY()
	TArray<FFocalLengthFocusCurve> FocusCurves;
};

#undef UE_API
