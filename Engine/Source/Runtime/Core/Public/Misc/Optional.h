// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "Misc/IntrusiveUnsetOptionalState.h"
#include "Misc/OptionalFwd.h"
#include "Templates/MemoryOps.h"
#include "Templates/UnrealTemplate.h"
#include "Serialization/Archive.h"

inline constexpr FNullOpt NullOpt{0};

namespace UE::Core::Private
{
	struct CIntrusiveUnsettable
	{
		template <typename T>
		auto Requires(bool& b) -> decltype(
			b = std::decay_t<typename T::IntrusiveUnsetOptionalStateType>::bHasIntrusiveUnsetOptionalState
		);
	};

	template <size_t N>
	struct TNonIntrusiveOptionalStorage
	{
		uint8 Storage[N];
		bool bIsSet;
	};
}

template <typename T>
constexpr bool HasIntrusiveUnsetOptionalState()
{
	if constexpr (!TModels<UE::Core::Private::CIntrusiveUnsettable, T>::Value)
	{
		return false;
	}
	// Derived types are not guaranteed to have an intrusive state, so ensure IntrusiveUnsetOptionalStateType matches the type in the optional
	else if constexpr (!std::is_same_v<const typename T::IntrusiveUnsetOptionalStateType, const T>)
	{
		return false;
	}
	else
	{
		return T::bHasIntrusiveUnsetOptionalState;
	}
}

/**
 * When we have an optional value IsSet() returns true, and GetValue() is meaningful.
 * Otherwise GetValue() is not meaningful.
 */
template<typename OptionalType>
struct TOptional
{
private:
	static constexpr bool bUsingIntrusiveUnsetState = HasIntrusiveUnsetOptionalState<OptionalType>();

public:
	using ElementType = OptionalType;

	/** Construct an OptionalType with a valid value. */
	[[nodiscard]] TOptional(const OptionalType& InValue)
		: TOptional(InPlace, InValue)
	{
	}
	[[nodiscard]] TOptional(OptionalType&& InValue)
		: TOptional(InPlace, MoveTempIfPossible(InValue))
	{
	}
	template <typename... ArgTypes>
	[[nodiscard]] explicit TOptional(EInPlace, ArgTypes&&... Args)
	{
		// If this fails to compile when trying to call TOptional(EInPlace, ...) with a non-public constructor,
		// do not make TOptional a friend.
		//
		// Instead, prefer this pattern:
		//
		//     class FMyType
		//     {
		//     private:
		//         struct FPrivateToken { explicit FPrivateToken() = default; };
		//
		//     public:
		//         // This has an equivalent access level to a private constructor,
		//         // as only friends of FMyType will have access to FPrivateToken,
		//         // but the TOptional constructor can legally call it since it's public.
		//         explicit FMyType(FPrivateToken, int32 Int, float Real, const TCHAR* String);
		//     };
		//
		//     // Won't compile if the caller doesn't have access to FMyType::FPrivateToken
		//     TOptional<FMyType> Opt(InPlace, FMyType::FPrivateToken{}, 5, 3.14f, TEXT("Banana"));
		//
		::new((void*)&Value) OptionalType(Forward<ArgTypes>(Args)...);

		if constexpr (!bUsingIntrusiveUnsetState)
		{
			Value.bIsSet = true;
		}
		else
		{
			// Ensure that a user doesn't emplace an unset state into the optional
			checkf(IsSet(), TEXT("TOptional::TOptional(EInPlace, ...) - optionals should not be unset by emplacement"));
		}
	}
	
	/** Construct an OptionalType with an invalid value. */
	[[nodiscard]] TOptional(FNullOpt)
		: TOptional()
	{
	}

	/** Construct an OptionalType with no value; i.e. unset */
	[[nodiscard]] TOptional()
	{
		if constexpr (bUsingIntrusiveUnsetState)
		{
			::new ((void*)&Value) OptionalType(FIntrusiveUnsetOptionalState{});
		}
		else
		{
			Value.bIsSet = false;
		}
	}

	~TOptional()
	{
		Reset();
	}

	/** Copy/Move construction */
	[[nodiscard]] TOptional(const TOptional& Other)
	{
		bool bLocalIsSet = Other.IsSet();
		if constexpr (!bUsingIntrusiveUnsetState)
		{
			Value.bIsSet = bLocalIsSet;
		}
		if (bLocalIsSet)
		{
			::new((void*)&Value) OptionalType(*(const OptionalType*)&Other.Value);
		}
		else
		{
			if constexpr (bUsingIntrusiveUnsetState)
			{
				::new ((void*)&Value) OptionalType(FIntrusiveUnsetOptionalState{});
			}
		}
	}
	[[nodiscard]] TOptional(TOptional&& Other)
	{
		bool bLocalIsSet = Other.IsSet();
		if constexpr (!bUsingIntrusiveUnsetState)
		{
			Value.bIsSet = bLocalIsSet;
		}
		if (bLocalIsSet)
		{
			::new((void*)&Value) OptionalType(MoveTempIfPossible(*(OptionalType*)&Other.Value));
		}
		else
		{
			if constexpr (bUsingIntrusiveUnsetState)
			{
				::new ((void*)&Value) OptionalType(FIntrusiveUnsetOptionalState{});
			}
		}
	}

	TOptional& operator=(const TOptional& Other)
	{
		if (&Other != this)
		{
			if (Other.IsSet())
			{
				Emplace(Other.GetValue());
			}
			else
			{
				Reset();
			}
		}
		return *this;
	}
	TOptional& operator=(TOptional&& Other)
	{
		if (&Other != this)
		{
			if(Other.IsSet())
			{
				Emplace(MoveTempIfPossible(Other.GetValue()));
			}
			else
			{
				Reset();
			}
		}
		return *this;
	}

	TOptional& operator=(const OptionalType& InValue)
	{
		if (&InValue != (const OptionalType*)&Value)
		{
			Emplace(InValue);
		}
		return *this;
	}
	TOptional& operator=(OptionalType&& InValue)
	{
		if (&InValue != (const OptionalType*)&Value)
		{
			Emplace(MoveTempIfPossible(InValue));
		}
		return *this;
	}

	void Reset()
	{
		if (IsSet())
		{
			DestroyValue();
			if constexpr (bUsingIntrusiveUnsetState)
			{
				::new((void*)&Value) OptionalType(FIntrusiveUnsetOptionalState{});
			}
			else
			{
				Value.bIsSet = false;
			}
		}
	}

	template <typename... ArgsType>
	OptionalType& Emplace(ArgsType&&... Args)
	{
		// Destroy the member in-place before replacing it - a bit nasty, but it'll work since we don't support exceptions
		if constexpr (bUsingIntrusiveUnsetState)
		{
			DestroyValue();
		}
		else
		{
			if (IsSet())
			{
				DestroyValue();
			}
		}

		// If this fails to compile when trying to call Emplace with a non-public constructor,
		// do not make TOptional a friend.
		//
		// Instead, prefer this pattern:
		//
		//     class FMyType
		//     {
		//     private:
		//         struct FPrivateToken { explicit FPrivateToken() = default; };
		//
		//     public:
		//         // This has an equivalent access level to a private constructor,
		//         // as only friends of FMyType will have access to FPrivateToken,
		//         // but Emplace can legally call it since it's public.
		//         explicit FMyType(FPrivateToken, int32 Int, float Real, const TCHAR* String);
		//     };
		//
		//     TOptional<FMyType> Opt:
		//
		//     // Won't compile if the caller doesn't have access to FMyType::FPrivateToken
		//     Opt.Emplace(FMyType::FPrivateToken{}, 5, 3.14f, TEXT("Banana"));
		//
		OptionalType* Result = ::new((void*)&Value) OptionalType(Forward<ArgsType>(Args)...);

		if constexpr (!bUsingIntrusiveUnsetState)
		{
			Value.bIsSet = true;
		}
		else
		{
			// Ensure that a user doesn't emplace an unset state into the optional
			checkf(IsSet(), TEXT("TOptional::Emplace(...) - optionals should not be unset by an emplacement"));
		}

		return *Result;
	}

	[[nodiscard]] friend bool operator==(const TOptional& Lhs, const TOptional& Rhs)
	{
		bool bIsLhsSet = Lhs.IsSet();
		bool bIsRhsSet = Rhs.IsSet();

		if (bIsLhsSet != bIsRhsSet)
		{
			return false;
		}
		if (!bIsLhsSet) // both unset
		{
			return true;
		}

		return (*(const OptionalType*)&Lhs.Value) == (*(const OptionalType*)&Rhs.Value);
	}

	[[nodiscard]] friend bool operator!=(const TOptional& Lhs, const TOptional& Rhs)
	{
		return !(Lhs == Rhs);
	}

	void Serialize(FArchive& Ar)
	{
		bool bOptionalIsSet = IsSet();
		if (Ar.IsLoading())
		{
			bool bOptionalWasSaved = false;
			Ar << bOptionalWasSaved;
			if (bOptionalWasSaved)
			{
				if (!bOptionalIsSet)
				{
					Emplace();
				}
				Ar << GetValue();
			}
			else
			{
				Reset();
			}
		}
		else
		{
			Ar << bOptionalIsSet;
			if (bOptionalIsSet)
			{
				Ar << GetValue();
			}
		}
	}

	/** @return true when the value is meaningful; false if calling GetValue() is undefined. */
	[[nodiscard]] bool IsSet() const
	{
		if constexpr (bUsingIntrusiveUnsetState)
		{
			return !(*(const OptionalType*)&Value == FIntrusiveUnsetOptionalState{});
		}
		else
		{
			return Value.bIsSet;
		}
	}
	[[nodiscard]] FORCEINLINE explicit operator bool() const
	{
		return IsSet();
	}

	/** @return The optional value; undefined when IsSet() returns false. */
	[[nodiscard]] OptionalType& GetValue()
	{
		checkf(IsSet(), TEXT("It is an error to call GetValue() on an unset TOptional. Please either check IsSet() or use Get(DefaultValue) instead."));
		return *(OptionalType*)&Value;
	}
	[[nodiscard]] FORCEINLINE const OptionalType& GetValue() const
	{
		return const_cast<TOptional*>(this)->GetValue();
	}

	[[nodiscard]] OptionalType* operator->()
	{
		return &GetValue();
	}
	[[nodiscard]] FORCEINLINE const OptionalType* operator->() const
	{
		return const_cast<TOptional*>(this)->operator->();
	}

	[[nodiscard]] OptionalType& operator*()
	{
		return GetValue();
	}
	[[nodiscard]] FORCEINLINE const OptionalType& operator*() const
	{
		return const_cast<TOptional*>(this)->operator*();
	}

	/** @return The optional value when set; DefaultValue otherwise. */
	[[nodiscard]] const OptionalType& Get(const OptionalType& DefaultValue UE_LIFETIMEBOUND) const UE_LIFETIMEBOUND
	{
		return IsSet() ? *(const OptionalType*)&Value : DefaultValue;
	}

	/** @return A pointer to the optional value when set, nullptr otherwise. */
	[[nodiscard]] OptionalType* GetPtrOrNull()
	{
		return IsSet() ? (OptionalType*)&Value : nullptr;
	}
	[[nodiscard]] FORCEINLINE const OptionalType* GetPtrOrNull() const
	{
		return const_cast<TOptional*>(this)->GetPtrOrNull();
	}

private:
	/** 
	 * Destroys the value, must only be called if the value is set, and callers must then mark the value unset or construct a new value in place. 
	 */
	FORCEINLINE void DestroyValue()
	{
		if constexpr (bUsingIntrusiveUnsetState)
		{
			DestructItem((OptionalType*)&Value);
		}
		else
		{
			DestructItem((OptionalType*)Value.Storage);
		}
	}

	using ValueStorageType = std::conditional_t<bUsingIntrusiveUnsetState, uint8[sizeof(OptionalType)], UE::Core::Private::TNonIntrusiveOptionalStorage<sizeof(OptionalType)>>;
	alignas(OptionalType) ValueStorageType Value;
};

template<typename OptionalType>
FArchive& operator<<(FArchive& Ar, TOptional<OptionalType>& Optional)
{
	Optional.Serialize(Ar);
	return Ar;
}

template<typename OptionalType>
[[nodiscard]] inline auto GetTypeHash(const TOptional<OptionalType>& Optional) -> decltype(GetTypeHash(*Optional))
{
	return Optional.IsSet() ? GetTypeHash(*Optional) : 0;
}

/**
 * Trait which determines whether or not a type is a TOptional.
 */
template <typename T> static constexpr bool TIsTOptional_V                              = false;
template <typename T> static constexpr bool TIsTOptional_V<               TOptional<T>> = true;
template <typename T> static constexpr bool TIsTOptional_V<const          TOptional<T>> = true;
template <typename T> static constexpr bool TIsTOptional_V<      volatile TOptional<T>> = true;
template <typename T> static constexpr bool TIsTOptional_V<const volatile TOptional<T>> = true;
