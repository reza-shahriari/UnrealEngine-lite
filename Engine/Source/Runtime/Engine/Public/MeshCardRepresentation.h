// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MeshCardRepresentation.h
=============================================================================*/

#pragma once

#include "HAL/Platform.h"
#include "Math/Box.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"

class FMeshCardsBuildData;

namespace MeshCardRepresentation
{
	// Generation config
	ENGINE_API float GetMinDensity();
	ENGINE_API float GetNormalTreshold();

	// Debugging
	ENGINE_API bool IsDebugMode();
	ENGINE_API int32 GetDebugSurfelDirection();

	// Util
	ENGINE_API FVector3f GetAxisAlignedDirection(uint32 AxisAlignedDirectionIndex);
	ENGINE_API void SetCardsFromBounds(FMeshCardsBuildData& CardData, bool bCardCoversHalfBounds = false);
};

template<typename T>
class TLumenCardOBB
{
public:
	UE::Math::TVector<T> Origin;
	UE::Math::TVector<T> AxisX;
	UE::Math::TVector<T> AxisY;
	UE::Math::TVector<T> AxisZ;
	UE::Math::TVector<T> Extent;

	/** Default constructor (no initialization). */
	TLumenCardOBB() = default;

	/**
	 * Creates and initializes a new OBB with zeros
	 *
	 * Use enum value EForceInit::ForceInit to force OBB initialization.
	 */
	explicit TLumenCardOBB(EForceInit)
	{
		Reset();
	}

	// Conversion from other type.
	template<typename FArg UE_REQUIRES(!std::is_same_v<T, FArg>)>
	explicit TLumenCardOBB(const TLumenCardOBB<FArg>& From)
	{
		Origin = UE::Math::TVector<T>(From.Origin);
		AxisX = UE::Math::TVector<T>(From.AxisX);
		AxisY = UE::Math::TVector<T>(From.AxisY);
		AxisZ = UE::Math::TVector<T>(From.AxisZ);
		Extent = UE::Math::TVector<T>(From.Extent);
	}

	void Reset()
	{
		Origin = UE::Math::TVector<T>::ZeroVector;
		AxisX = UE::Math::TVector<T>::ZeroVector;
		AxisY = UE::Math::TVector<T>::ZeroVector;
		AxisZ = UE::Math::TVector<T>::ZeroVector;
		Extent = UE::Math::TVector<T>::ZeroVector;
	}

	UE::Math::TVector<T> GetDirection() const
	{
		return AxisZ;
	}

	UE::Math::TMatrix<T> GetCardToLocal() const
	{
		UE::Math::TMatrix<T> CardToLocal;
		CardToLocal.SetIdentity();
		CardToLocal.SetAxes(&AxisX, &AxisY, &AxisZ, &Origin);
		return CardToLocal;
	}

	inline UE::Math::TVector<T> RotateCardToLocal(UE::Math::TVector<T> Vector3) const
	{
		return Vector3.X * AxisX + Vector3.Y * AxisY + Vector3.Z * AxisZ;
	}

	inline UE::Math::TVector<T> RotateLocalToCard(UE::Math::TVector<T> Vector3) const
	{
		return UE::Math::TVector<T>(Vector3 | AxisX, Vector3 | AxisY, Vector3 | AxisZ);
	}

	inline UE::Math::TVector<T> TransformLocalToCard(UE::Math::TVector<T> LocalPosition) const
	{
		UE::Math::TVector<T> Offset = LocalPosition - Origin;
		return UE::Math::TVector<T>(Offset | AxisX, Offset | AxisY, Offset | AxisZ);
	}

	inline UE::Math::TVector<T> TransformCardToLocal(UE::Math::TVector<T> CardPosition) const
	{
		return Origin + CardPosition.X * AxisX + CardPosition.Y * AxisY + CardPosition.Z * AxisZ;
	}

	T ComputeSquaredDistanceToPoint(UE::Math::TVector<T> WorldPosition) const
	{
		UE::Math::TVector<T> CardPositon = TransformLocalToCard(WorldPosition);
		return ::ComputeSquaredDistanceFromBoxToPoint(-Extent, Extent, CardPositon);
	}

	TLumenCardOBB<T> Transform(UE::Math::TMatrix<T> LocalToWorld, bool* bAxisXFlippedPtr = nullptr) const
	{
		TLumenCardOBB<T> WorldOBB;
		WorldOBB.Origin = LocalToWorld.TransformPosition(Origin);

		const UE::Math::TVector<T> ScaledXAxis = LocalToWorld.TransformVector(AxisX);
		const UE::Math::TVector<T> ScaledYAxis = LocalToWorld.TransformVector(AxisY);
		const UE::Math::TVector<T> ScaledZAxis = LocalToWorld.TransformVector(AxisZ);
		const T XAxisLength = ScaledXAxis.Size();
		const T YAxisLength = ScaledYAxis.Size();
		const T ZAxisLength = ScaledZAxis.Size();

		// #lumen_todo: fix axisX flip cascading into entire card code
		WorldOBB.AxisY = ScaledYAxis / FMath::Max(YAxisLength, UE_DELTA);
		WorldOBB.AxisZ = ScaledZAxis / FMath::Max(ZAxisLength, UE_DELTA);
		WorldOBB.AxisX = UE::Math::TVector<T>::CrossProduct(WorldOBB.AxisZ, WorldOBB.AxisY);
		UE::Math::TVector<T>::CreateOrthonormalBasis(WorldOBB.AxisX, WorldOBB.AxisY, WorldOBB.AxisZ);

		if (bAxisXFlippedPtr)
		{
			*bAxisXFlippedPtr = UE::Math::TVector<T>::DotProduct(ScaledXAxis, WorldOBB.AxisX) < T(0);
		}

		WorldOBB.Extent = Extent * UE::Math::TVector<T>(XAxisLength, YAxisLength, ZAxisLength);
		WorldOBB.Extent.Z = FMath::Max(WorldOBB.Extent.Z, 1.0f);

		return WorldOBB;
	}

	UE::Math::TBox<T> GetBox() const
	{
		UE::Math::TVector<T> BoxMin(AxisX.GetAbs() * -Extent.X + AxisY.GetAbs() * -Extent.Y + AxisZ.GetAbs() * -Extent.Z + Origin);
		UE::Math::TVector<T> BoxMax(AxisX.GetAbs() * +Extent.X + AxisY.GetAbs() * +Extent.Y + AxisZ.GetAbs() * +Extent.Z + Origin);
		return UE::Math::TBox<T>(BoxMin, BoxMax);
	}

	bool ContainsNaN() const
	{
		return Origin.ContainsNaN() || AxisX.ContainsNaN() || AxisY.ContainsNaN() || AxisZ.ContainsNaN() || Extent.ContainsNaN();
	}

	friend FArchive& operator<<(FArchive& Ar, TLumenCardOBB<T>& Data)
	{
		Ar << Data.AxisX;
		Ar << Data.AxisY;
		Ar << Data.AxisZ;
		Ar << Data.Origin;
		Ar << Data.Extent;
		return Ar;
	}
};

using FLumenCardOBBf = TLumenCardOBB<float>;
using FLumenCardOBBd = TLumenCardOBB<double>;
