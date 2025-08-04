// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MuR/System.h"

#include "MuR/Settings.h"
#include "MuR/Operations.h"
#include "MuR/Image.h"
#include "MuR/MutableString.h"
#include "MuR/ModelPrivate.h"
#include "MuR/Mesh.h"
#include "MuR/Instance.h"
#include "MuR/ParametersPrivate.h"
#include "MuR/MutableTrace.h"

#include "Templates/UnrealTypeTraits.h"

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#include "HAL/Thread.h"
#include "HAL/PlatformTLS.h"
#endif

#ifndef MUTABLE_DEBUG_CODERUNNER_TASK_SCHEDULE_CALLSTACK
	#define MUTABLE_DEBUG_CODERUNNER_TASK_SCHEDULE_CALLSTACK 0	
#endif

namespace mu::MemoryCounters
{
	struct FInternalMemoryCounter
	{
		MUTABLERUNTIME_API static std::atomic<SSIZE_T>& Get()
		{
			static std::atomic<SSIZE_T> Counter{0};
			return Counter;
		}
	};
}

namespace mu
{
	inline uint32 GetResourceIDRoot(FResourceID Id)
	{
		return uint32(Id >> 32);
	}

	class FExtensionDataStreamer;

	// Call the tick of the LLM system (we do this to simulate a frame since the LLM system is not entirelly designed to run over a program)
	inline void UpdateLLMStats()
	{
		// This code will only be compiled (and ran) if the global definition to enable LLM tracking is set to 1 for the host program
		// Ex : 			GlobalDefinitions.Add("LLM_ENABLED_IN_CONFIG=1");
#if ENABLE_LOW_LEVEL_MEM_TRACKER && IS_PROGRAM
		FLowLevelMemTracker& MemTracker = FLowLevelMemTracker::Get();
		if (MemTracker.IsEnabled())
		{
			MemTracker.UpdateStatsPerFrame();
		}
#endif
	}

	constexpr uint64 AllParametersMask = TNumericLimits<uint64>::Max();
 
	/** ExecutinIndex stores the location inside all ranges of the execution of a specific
	* operation. The first integer on each pair is the dimension/range index in the program
	* array of ranges, and the second integer is the value inside that range.
	* The vector order is undefined.
	*/
	class ExecutionIndex : public  TArray<TPair<int32, int32>>
	{
	public:
		//! Set or add a value to the index
		void SetFromModelRangeIndex(uint16 FRangeIndex, int32 RangeValue)
		{
			SizeType Index = IndexOfByPredicate([=](const ElementType& v) { return v.Key >= FRangeIndex; });
			if (Index != INDEX_NONE && (*this)[Index].Key == FRangeIndex)
			{
				// Update
				(*this)[Index].Value = RangeValue;
			}
			else
			{
				// Add new
				Push(ElementType(FRangeIndex, RangeValue));
			}
		}

		//! Get the value of the index from the range index in the model.
		int32 GetFromModelRangeIndex(int32 ModelRangeIndex) const
		{
			for (const TPair<int32, int32>& e : *this)
			{
				if (e.Key == ModelRangeIndex)
				{
					return e.Value;
				}
			}
			return 0;
		}
	};


	/** This structure stores the data about an ongoing mutable operation that needs to be executed. */
	struct FScheduledOp
	{
		inline FScheduledOp()
		{
			Stage = 0;
			Type = EType::Full;
		}

		inline FScheduledOp(OP::ADDRESS InAt, const FScheduledOp& InOpTemplate, uint8 InStage = 0, uint32 InCustomState = 0)
		{
			check(InStage < 120);
			At = InAt;
			ExecutionOptions = InOpTemplate.ExecutionOptions;
			ExecutionIndex = InOpTemplate.ExecutionIndex;
			Stage = InStage;
			CustomState = InCustomState;
			Type = InOpTemplate.Type;
		}

		static inline FScheduledOp FromOpAndOptions(OP::ADDRESS InAt, const FScheduledOp& InOpTemplate, uint8 InExecutionOptions)
		{
			FScheduledOp r;
			r.At = InAt;
			r.ExecutionOptions = InExecutionOptions;
			r.ExecutionIndex = InOpTemplate.ExecutionIndex;
			r.Stage = 0;
			r.CustomState = InOpTemplate.CustomState;
			r.Type = InOpTemplate.Type;
			return r;
		}

		//! Address of the operation
		OP::ADDRESS At = 0;

		//! Additional custom state data that the operation can store. This is usually used to pass information
		//! between execution stages of an operation.
		uint32 CustomState = 0;

		//! Index of the operation execution: This is used for iteration of different ranges.
		//! It is an index into the CodeRunner::GetMemory()::m_rangeIndex vector.
		//! executionIndex 0 is always used for empty ExecutionIndex, which is the most common
		//! one.
		uint16 ExecutionIndex = 0;

		//! Additional execution options. Set externally to this op, it usually alters the result.
		//! For example, this is used to keep track of the mipmaps to skip in image operations.
		uint8 ExecutionOptions = 0;

		//! Internal stage of the operation.
		//! Stage 0 is usually scheduling of children, and 1 is execution. Some instructions
		//! may have more steges to schedule children that are optional for execution, etc.
		uint8 Stage : 7;

		//! Type of calculation we are requesting for this operation.
		enum class EType : uint8
		{
			//! Execute the operation to calculate the full result
			Full,

			//! Execute the operation to obtain the descriptor of an image.
			ImageDesc
		};
		EType Type : 1;

#if MUTABLE_DEBUG_CODERUNNER_TASK_SCHEDULE_CALLSTACK
		static constexpr uint64 CallstackMaxDepth = 16;
		uint64 StackDepth = 0; 
		uint64 ScheduleCallstack[CallstackMaxDepth];
#endif
	};

	inline uint32 GetTypeHash(const FScheduledOp& Op)
	{
		return HashCombineFast(::GetTypeHash(Op.At), HashCombineFast((uint32)Op.Stage), (uint32)Op.ExecutionIndex);
	}

	/** A cache address is the operation plus the context of execution(iteration indices, etc...). */
	struct FCacheAddress
	{
		/** The meaning of all these fields is the same than the FScheduledOp struct. */
		OP::ADDRESS At = 0;
		uint16 ExecutionIndex = 0;
		uint8 ExecutionOptions = 0;
		FScheduledOp::EType Type = FScheduledOp::EType::Full;

		FCacheAddress() 
		{
		}

		FCacheAddress(OP::ADDRESS InAt, uint16 InExecutionIndex, uint8 InExecutionOptions, FScheduledOp::EType InType = FScheduledOp::EType::Full)
			: At(InAt), ExecutionIndex(InExecutionIndex), ExecutionOptions(InExecutionOptions), Type(InType)
		{
		}

		FCacheAddress(OP::ADDRESS InAt, const FScheduledOp& Item)
			: At(InAt), ExecutionIndex(Item.ExecutionIndex), ExecutionOptions(Item.ExecutionOptions), Type(Item.Type)
		{
		}

		FCacheAddress(const FScheduledOp& Item)
			: At(Item.At), ExecutionIndex(Item.ExecutionIndex), ExecutionOptions(Item.ExecutionOptions), Type(Item.Type)
		{
		}
	};

	inline uint32 GetTypeHash(const FCacheAddress& Address)
	{
		return HashCombineFast(::GetTypeHash(Address.At), (uint32)Address.ExecutionIndex);
	}


	inline uint32 GetTypeHash(const Ptr<const FResource>& V)
	{
		return ::GetTypeHash(V.get());
	}


	//! Container that stores data per executable code operation (indexed by address and execution
	//! index).
	template<class DATA>
	class CodeContainer
	{
		using MemoryCounter = MemoryCounters::FInternalMemoryCounter;
		using ArrayDataContainerType = TArray<DATA, FDefaultMemoryTrackingAllocator<MemoryCounter>>;
		using MapDataContainerType = TMap<FCacheAddress, DATA, FDefaultMemoryTrackingSetAllocator<MemoryCounter>>;

	public:
		void resize(size_t s)
		{
			m_index0.SetNumZeroed(s);
		}

		uint32 size_code() const
		{
			return uint32(m_index0.Num());
		}

		void clear()
		{
			m_index0.Empty();
			m_otherIndex.Empty();
		}

		inline void erase(const FCacheAddress& at)
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				m_index0[at.At] = nullptr;
			}
			else
			{
				m_otherIndex.Remove(at);
			}
		}

		inline DATA get(const FCacheAddress& at) const
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				if (at.At < uint32(m_index0.Num()))
				{
					return m_index0[at.At];
				}
				else
				{
					return 0;
				}
			}
			else
			{
				const DATA* it = m_otherIndex.Find(at);
				if (it)
				{
					return *it;
				}
			}
			return 0;
		}

		inline DATA* get_ptr(const FCacheAddress& at)
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				if (at.At < uint32(m_index0.Num()))
				{
					return &m_index0[at.At];
				}
				else
				{
					return nullptr;
				}
			}
			else
			{
				DATA* it = m_otherIndex.Find(at);
				if (it)
				{
					return it;
				}
			}
			return nullptr;
		}

		inline const DATA* get_ptr(const FCacheAddress& at) const
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				if (at.At < uint32(m_index0.Num()))
				{
					return &m_index0[at.At];
				}
				else
				{
					return nullptr;
				}
			}
			else
			{
				const DATA* it = m_otherIndex.Find(at);
				if (it)
				{
					return it;
				}
			}
			return nullptr;
		}

		inline DATA& operator[](const FCacheAddress& at)
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				return m_index0[at.At];
			}
			else
			{
				return m_otherIndex.FindOrAdd(at);
			}
		}

		inline const DATA& operator[](const FCacheAddress& at) const
		{
			if (at.ExecutionIndex == 0 && at.ExecutionOptions == 0)
			{
				return m_index0[at.At];
			}
			else
			{
				return m_otherIndex[at];
			}
		}

		struct iterator
		{
		private:
			friend class CodeContainer<DATA>;

			const CodeContainer<DATA>* container;
			typename CodeContainer<DATA>::ArrayDataContainerType::TIterator it0;
			typename CodeContainer<DATA>::MapDataContainerType::TIterator it1;

			iterator(CodeContainer<DATA>* InContainer)
				: container(InContainer)
				, it0(InContainer->m_index0.CreateIterator())
				, it1(InContainer->m_otherIndex.CreateIterator())
			{
			}

		public:

			inline void operator++(int)
			{
				if (it0)
				{
					++it0;
				}
				else
				{
					++it1;
				}
			}

			iterator operator++()
			{
				if (it0)
				{
					++it0;
				}
				else
				{
					++it1;
				}
				return *this;
			}

			inline bool operator!=(const iterator& o) const
			{
				return it0 != o.it0 || it1 != o.it1;
			}

			inline DATA& operator*()
			{
				if (it0)
				{
					return *it0;
				}
				else
				{
					return it1->Value;
				}
			}

			inline FCacheAddress get_address() const
			{
				if (it0)
				{
					return { uint32_t(it0.GetIndex()), 0, 0};
				}
				else
				{
					return it1->Key;
				}
			}

			inline bool IsValid() const
			{
				return bool(it0) || bool(it1);
			}
		};

		inline iterator begin()
		{
			iterator it(this);
			return it;
		}

		inline int32 GetAllocatedSize() const
		{
			return m_index0.GetAllocatedSize()
				+ m_otherIndex.GetAllocatedSize();
		}
	private:
		// For index 0
		ArrayDataContainerType m_index0;

		// For index>0
		MapDataContainerType m_otherIndex;
	};


	/** Interface for storage of data while Mutable code is being executed. */
	class FProgramCache
	{
	public:

		using AllocType = FDefaultMemoryTrackingAllocator<MemoryCounters::FInternalMemoryCounter>;

		template<class Type, class Alloc = AllocType >
		using TMemoryTrackedArray = TArray<Type, Alloc>;
		
		TMemoryTrackedArray<ExecutionIndex, TInlineAllocator<4, AllocType>> m_usedRangeIndices;

		/** Runtime data for each program op. */
		struct FOpExecutionData
		{
			uint16 OpHitCount;

			/** Enabled if the op descriptor has been calculated. */
			uint8 IsDescCacheValid : 1;
			uint8 IsValueValid : 1;
			uint8 IsCacheLocked : 1;

			/** The operation DATA_TYPE. */
			EDataType DataType;

			/** The position in the type-specific array where the result data is stored. 
			* 0 means index not valid.
			* For small types like bool, int and float, the actual value is the index, instead of having their own array.
			*/
			union
			{
				int32 DataTypeIndex;
				float ScalarResult;
			};
		};

		/** Cached resources while the program is executing. 
		* first value of the pair:
		* 0 : value not valid (not set).
		* 1 : valid, not worth freeing for memory
		* 2 : valid, worth freeing
		*/
		CodeContainer<FOpExecutionData> OpExecutionData;

		template<class ResourceType>
		struct TResourceResult
		{
			static_assert(TIsDerivedFrom<ResourceType, FResource>::Value);

			FCacheAddress OpAddress;
			TSharedPtr<const ResourceType> Value = nullptr;
		};
		static_assert(sizeof(TResourceResult<FImage>) == 24);
		static_assert(sizeof(TResourceResult<FMesh>) == 24);

		/** */

		TMemoryTrackedArray<FVector4f> ColorResults;
		TMemoryTrackedArray<TResourceResult<FImage>> ImageResults;
		TMemoryTrackedArray<TResourceResult<FMesh>> MeshResults;
		TMemoryTrackedArray<TSharedPtr<const FLayout>> LayoutResults;
		TMemoryTrackedArray<TSharedPtr<const FInstance>> InstanceResults;
		TMemoryTrackedArray<FProjector> ProjectorResults;
		TMemoryTrackedArray<TSharedPtr<const String>> StringResults;
		TMemoryTrackedArray<TSharedPtr<const FExtensionData>> ExtensionDataResults;
		TMemoryTrackedArray<FMatrix44f> MatrixResults;

		/** */
		inline const ExecutionIndex& GetRangeIndex(uint32_t i)
		{
			// Make sure we have the default element.
			if (m_usedRangeIndices.IsEmpty())
			{
				m_usedRangeIndices.Push(ExecutionIndex());
			}

			check(i < uint32_t(m_usedRangeIndices.Num()));
			return m_usedRangeIndices[i];
		}

		//!
		inline uint32 GetRangeIndexIndex(const ExecutionIndex& rangeIndex)
		{
			if (rangeIndex.IsEmpty())
			{
				return 0;
			}

			// Make sure we have the default element.
			if (m_usedRangeIndices.IsEmpty())
			{
				m_usedRangeIndices.Push(ExecutionIndex());
			}

			// Look for or add the new element.
			int32 ElemIndex = m_usedRangeIndices.Find(rangeIndex);
			if (ElemIndex != INDEX_NONE)
			{
				return ElemIndex;
			}

			m_usedRangeIndices.Push(rangeIndex);
			return uint32_t(m_usedRangeIndices.Num()) - 1;
		}

		void Init(uint32 Size)
		{
			// This clear would prevent live update cache reusal
			//OpExecutionData.clear();

			OpExecutionData.resize(Size);

			if (ColorResults.IsEmpty())
			{
				// Insert dafault/null values
				ColorResults.Add(FVector4f());
				ImageResults.Emplace();
				LayoutResults.Add(nullptr);
				MeshResults.Emplace();
				InstanceResults.Add(nullptr);
				ProjectorResults.Add(FProjector());
				StringResults.Add(nullptr);
				MatrixResults.Add(FMatrix44f());
				ExtensionDataResults.Add(nullptr);
			}
		}

		void SetUnused(FOpExecutionData& Data)
		{
			// Only clear datatypes that have results that use relevant amounts of memory			
			uint32 DataTypeIndex = Data.DataTypeIndex;
			Data.IsValueValid = false;

			if (DataTypeIndex)
			{
				check(uint8(Data.DataType) < uint8(EDataType::Count));
				switch (Data.DataType)
				{
				case EDataType::Image:					
					ImageResults[DataTypeIndex].Value = nullptr;
					break;

				case EDataType::Mesh:
					MeshResults[DataTypeIndex].Value = nullptr;
					break;

				case EDataType::Instance:
					InstanceResults[DataTypeIndex] = nullptr;
					break;

				case EDataType::ExtensionData:
					ExtensionDataResults[DataTypeIndex] = nullptr;
					break;

				default:
					break;
				}
			}
		}


		bool IsValid(FCacheAddress at) const
		{
			if (at.At == 0 || at.At>=OpExecutionData.size_code() )
			{
				return false;
			}

			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data)
			{
				return false;
			}

			// Is it a desc data query?
			if (at.Type == FScheduledOp::EType::ImageDesc)
			{
				return Data->IsDescCacheValid;
			}

			// It's a full data query.
			return Data->IsValueValid != 0;
		}


		/** */
		void CheckHitCountsCleared()
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			//MUTABLE_CPUPROFILER_SCOPE(CheckHitCountsCleared);

			//int32 IncorrectCount = 0;
			//CodeContainer<int>::iterator it = m_opHitCount.begin();
			//for (; it.IsValid(); ++it)
			//{
			//	int32 Count = *it;
			//	if (Count>0 && Count < UE_MUTABLE_CACHE_COUNT_LIMIT)
			//	{
			//		// We don't manage the hitcounts of small types that don't use much memory
			//		if (m_resources[it.get_address()].Key==2)
			//		{
			//			// Op hitcount should have reached 0, otherwise, it means we requested an operation but never read 
			//			// the result.
			//			++IncorrectCount;
			//		}
			//	}
			//}

			//if (IncorrectCount > 0)
			//{
			//	UE_LOG(LogMutableCore, Log, TEXT("The op-hit-count didn't hit 0 for %5d operations. This may mean that too much memory is cached."), IncorrectCount);
			//	//check(false);
			//}
#endif
		}


		void Clear()
		{
			MUTABLE_CPUPROFILER_SCOPE(ProgramCacheClear);

			uint32 CodeSize = OpExecutionData.size_code();
			OpExecutionData.clear();
			OpExecutionData.resize(CodeSize);
		}


		void ClearDescCache()
		{
			MUTABLE_CPUPROFILER_SCOPE(ProgramDescCacheClear);

			CodeContainer<FProgramCache::FOpExecutionData>::iterator it = OpExecutionData.begin();
			for (; it.IsValid(); ++it)
			{
				(*it).IsDescCacheValid = false;
			}
		}


		bool GetBool(FCacheAddress at)
		{
			if (!at.At) return false;
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return false;

			check(Data->DataType==EDataType::Bool);
			return Data->DataTypeIndex!=0;
		}

		float GetScalar(FCacheAddress at)
		{
			if (!at.At) return 0.0f;
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return 0.0f;

			check(Data->DataType == EDataType::Scalar);
			return Data->ScalarResult;
		}

		int32 GetInt(FCacheAddress at)
		{
			if (!at.At) return 0;
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return 0;

			check(Data->DataType == EDataType::Int);
			return Data->DataTypeIndex;
		}

		FVector4f GetColour(FCacheAddress at)
		{
			if (!at.At) return FVector4f();
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return FVector4f();

			check(Data->DataType == EDataType::Color);
			return ColorResults[Data->DataTypeIndex];
		}

		FMatrix44f GetMatrix(FCacheAddress at)
		{
			if (!at.At) return FMatrix44f::Identity;
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return FMatrix44f::Identity;

			check(Data->DataType == EDataType::Matrix);
			return MatrixResults[Data->DataTypeIndex];
		}

		FProjector GetProjector(FCacheAddress at)
		{
			if (!at.At) return FProjector();
			const FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return FProjector();

			check(Data->DataType == EDataType::Projector);
			return ProjectorResults[Data->DataTypeIndex];
		}

		TSharedPtr<const FInstance> GetInstance(FCacheAddress at)
		{
			if (!at.At) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::Instance);
			TSharedPtr<const FInstance> Result = InstanceResults[Data->DataTypeIndex];

			// We need to decrease the hit-count even if the result is null.
			check(Data->OpHitCount > 0);

			--Data->OpHitCount;
			if (Data->OpHitCount == 0 && !Data->IsCacheLocked)
			{
				SetUnused(*Data);
			}

			return Result;
		}


		TSharedPtr<const FImage> GetImage(FCacheAddress at, bool& bIsLastReference)
		{
			bIsLastReference = false;

			if (!at.At) return nullptr;
			if (at.At >= OpExecutionData.size_code()) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::Image);
			TSharedPtr<const FImage> Result = ImageResults[Data->DataTypeIndex].Value;

			// We need to decrease the hit-count even if the result is null.
			check(Data->OpHitCount > 0);

			--Data->OpHitCount;
			if (Data->OpHitCount == 0 && !Data->IsCacheLocked)
			{
				SetUnused(*Data);
				bIsLastReference = true;
			}

			return Result;
		}


		TSharedPtr<const FMesh> GetMesh(FCacheAddress at, bool& bIsLastReference)
		{
			bIsLastReference = false;

			if (!at.At) return nullptr;
			if (at.At >= OpExecutionData.size_code()) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::Mesh);
			TSharedPtr<const FMesh> Result = MeshResults[Data->DataTypeIndex].Value;

			// We need to decrease the hit-count even if the result is null.
			check(Data->OpHitCount > 0);

			--Data->OpHitCount;
			if (Data->OpHitCount == 0 && !Data->IsCacheLocked)
			{
				SetUnused(*Data);
				bIsLastReference = true;
			}

			return Result;
		}


		TSharedPtr<const FLayout> GetLayout(FCacheAddress at)
		{
			if (!at.At) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::Layout);
			return LayoutResults[Data->DataTypeIndex];
		}


		TSharedPtr<const String> GetString(FCacheAddress at)
		{
			if (!at.At) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::String);
			return StringResults[Data->DataTypeIndex];
		}


		TSharedPtr<const FExtensionData> GetExtensionData(FCacheAddress at)
		{
			if (!at.At) return nullptr;
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			if (!Data) return nullptr;

			check(Data->DataType == EDataType::ExtensionData);
			return ExtensionDataResults[Data->DataTypeIndex];
		}

		void SetValidDesc(FCacheAddress at)
		{
			check(at.Type == FScheduledOp::EType::ImageDesc);
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			Data->IsDescCacheValid = true;
		}

		void SetBool(FCacheAddress at, bool v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Bool || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Bool;
			Data->DataTypeIndex = v;
			Data->IsValueValid = true;
		}

		void SetInt(FCacheAddress at, int32 v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Int || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Int;
			Data->DataTypeIndex = v;
			Data->IsValueValid = true;
		}

		void SetScalar(FCacheAddress at, float v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Scalar || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Scalar;
			Data->ScalarResult = v;
			Data->IsValueValid = true;
		}

		void SetColour(FCacheAddress at, const FVector4f& v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Color || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Color;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = ColorResults.Num();
				ColorResults.Add(v);
			}
			else
			{
				ColorResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);
		}

		void SetMatrix(FCacheAddress at, const FMatrix44f& v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Matrix || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Matrix;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = MatrixResults.Num();
				MatrixResults.Add(v);
			}
			else
			{
				MatrixResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);
		}
		
		void SetProjector(FCacheAddress at, const FProjector& v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Projector || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Projector;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = ProjectorResults.Num();
				ProjectorResults.Add(v);
			}
			else
			{
				ProjectorResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);
		}

		void SetInstance(FCacheAddress at, TSharedPtr<const FInstance> v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Instance || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Instance;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = InstanceResults.Num();
				InstanceResults.Add(v);
			}
			else
			{
				InstanceResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);
		}

		void SetExtensionData(FCacheAddress at, TSharedPtr<const FExtensionData> v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::ExtensionData || Data->DataType == EDataType::None);
			Data->DataType = EDataType::ExtensionData;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = ExtensionDataResults.Num();
				ExtensionDataResults.Add(v);
			}
			else
			{
				ExtensionDataResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);
		}

		void SetImage(FCacheAddress At, TSharedPtr<const FImage> Value)
		{
			check(At.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(At);
			check(Data->DataType == EDataType::Image || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Image;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = ImageResults.Num();
				ImageResults.Add(TResourceResult<FImage>{At, Value});
			}
			else
			{
				ImageResults[Data->DataTypeIndex].Value = Value;
			}
			check(Data->DataTypeIndex != 0);

			mu::UpdateLLMStats();
		}

		void SetMesh(FCacheAddress At, TSharedPtr<const FMesh> Value)
		{
			check(At.At < OpExecutionData.size_code());

			FOpExecutionData* Data = OpExecutionData.get_ptr(At);

			check(Data->DataType == EDataType::Mesh || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Mesh;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = MeshResults.Num();
				MeshResults.Add(TResourceResult<FMesh>{At, Value});
			}
			else
			{
				MeshResults[Data->DataTypeIndex].Value = Value;
			}
			check(Data->DataTypeIndex != 0);

			mu::UpdateLLMStats();
		}

		void SetLayout(FCacheAddress at, TSharedPtr<const FLayout> v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::Layout || Data->DataType == EDataType::None);
			Data->DataType = EDataType::Layout;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = LayoutResults.Num();
				LayoutResults.Add(v);
			}
			else
			{
				LayoutResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex != 0);

			mu::UpdateLLMStats();
		}

		void SetString(FCacheAddress at, TSharedPtr<const String> v)
		{
			check(at.At < OpExecutionData.size_code());
			FOpExecutionData* Data = OpExecutionData.get_ptr(at);
			check(Data->DataType == EDataType::String || Data->DataType == EDataType::None);
			Data->DataType = EDataType::String;
			Data->IsValueValid = true;

			if (!Data->DataTypeIndex)
			{
				Data->DataTypeIndex = StringResults.Num();
				StringResults.Add(v);
			}
			else
			{
				StringResults[Data->DataTypeIndex] = v;
			}
			check(Data->DataTypeIndex!=0);
		}


		inline void IncreaseHitCount(FCacheAddress at)
		{
			// Don't count hits for instruction 0, which is always null. It is usually already
			// check that At is not 0, and then it is not requested, generating a stray non-zero count
			// at its position.
			if (at.At)
			{
				check(at.At < OpExecutionData.size_code());
				FOpExecutionData& Data = OpExecutionData[at];
				Data.OpHitCount++;
			}
		}

		inline void SetForceCached(OP::ADDRESS at)
		{
			// \TODO: It only locks at,0,0
			if (at)
			{
				check(at < OpExecutionData.size_code());
				FOpExecutionData& Data = OpExecutionData[FCacheAddress(at, 0, 0)];
				Data.IsCacheLocked = true;
			}
		}
	};


	inline bool operator==(const FCacheAddress& a, const FCacheAddress& b)
	{
		return a.At == b.At
			&&
			a.ExecutionIndex == b.ExecutionIndex
			&&
			a.ExecutionOptions == b.ExecutionOptions
			&&
			a.Type == b.Type;
	}

	inline bool operator<(const FCacheAddress& a, const FCacheAddress& b)
	{
		if (a.At < b.At) return true;
		if (a.At > b.At) return false;
		if (a.ExecutionIndex < b.ExecutionIndex) return true;
		if (a.ExecutionIndex > b.ExecutionIndex) return false;
		if (a.ExecutionOptions < b.ExecutionOptions) return true;
		if (a.ExecutionOptions > b.ExecutionOptions) return false;
		return a.Type < b.Type;
	}

	/** Data for an instance that is currently being processed in the mutable system. This means it is
	* between a BeginUpdate and EndUpdate, or during an "atomic" operation (like generate a single resource).
	*/
	struct FLiveInstance
	{
		FInstance::FID InstanceID;
		int32 State = 0;
		TSharedPtr<const FInstance> Instance;
		TSharedPtr<const FModel> Model;

		TSharedPtr<FParameters> OldParameters;

		/** Mask of the parameters that have changed since the last update.
		* Every bit represents a state parameter.
		*/
		uint64 UpdatedParameters = 0;

		/** Cached data for the generation of this instance. */
		TSharedPtr<FProgramCache> Cache;

		~FLiveInstance()
		{
			// Manually done to trace mem deallocations
			MUTABLE_CPUPROFILER_SCOPE(LiveInstanceDestructor);
			Cache = nullptr;
			OldParameters = nullptr;
			Instance = nullptr;
			Model = nullptr;
		}
	};


    /** Struct to manage all the memory allocated for resources used during mutable operation. */
    struct FWorkingMemoryManager
    {	
		using MemoryCounter = MemoryCounters::FInternalMemoryCounter;

		template<class Type>
		using TMemoryTrackedArray = TArray<Type, FDefaultMemoryTrackingAllocator<MemoryCounter>>;

		template<class KeyType, class ValueType>
		using TMemoryTrackedMap = TMap<KeyType, ValueType, FDefaultMemoryTrackingSetAllocator<MemoryCounter>>;

		/** Cached traking for streamed model data for one model. */
        struct FModelCacheEntry
        {
            /** Model who's data is being tracked. */
            TWeakPtr<const FModel> Model;

            /** For each model rom, the last time its streamed data was used. */
            TMemoryTrackedArray<TPair<uint64, uint64>> RomWeights;

			/** Count of pending operations for every rom index. */
			TMemoryTrackedArray<uint16> PendingOpsPerRom;
        };

		//! Management of generated resources
		//! @{

		//! This is used to uniquely identify a generated resource like meshes or images.
		struct FGeneratedResourceData
		{
			/** Model for this resource. */
			TWeakPtr<const FModel> Model;

			//! The id assigned to the generated resource.
			FResourceID Id;

			//! The last request operation for this resource
			uint32 LastRequestId;

			//! An opaque blob with the values of the relevant parameters
			TMemoryTrackedArray<uint8> ParameterValuesBlob;
		};

		//! The last id generated for a resource
		uint32 LastResourceKeyId = 0;

		//! The last id generated for a resource request. This is used to check the
		//! relevancy of the resources when flushing the cache
		uint32 LastResourceResquestId = 0;

		//! Cached ids for returned assets
		//! This is non-persistent runtime data
		TMemoryTrackedArray<FGeneratedResourceData> GeneratedResources;

		/** */
		FResourceID GetResourceKey(const TSharedPtr<const FModel>&, const FParameters*, uint32 ParamListIndex, OP::ADDRESS RootAt);

		//! @}

		/** Maximum working memory that mutable should be using. */
		int64 BudgetBytes = 0;

		/** Maximum excess memory reached suring the current operation. */
		int64 BudgetExcessBytes = 0;

		/** Maximum number of resource keys that will be stored for resource reusal. */
		int32 MaxGeneratedResourceCacheSize = 1024;


		/** This value is used to track the order of loading of roms. */
        uint64 RomTick = 0;

		/** Control info for the per-model cache of streamed data. */
        TMemoryTrackedArray<FModelCacheEntry> CachePerModel;

		/** Data for each mutable instance that is being updated. */
		TMemoryTrackedArray<FLiveInstance> LiveInstances;

		/** Temporary reference to the memory of the current instance being updated. Only valid during a mutable "atomic" operation, like a BeginUpdate or a GetImage. */
		TSharedPtr<FProgramCache> CurrentInstanceCache;

		/** Resources that have been used in the past, but haven't been deallocated because they still fitted the memory budget and they could be reused. */
		TMemoryTrackedArray<TSharedPtr<FImage>> PooledImages;

		/** List of intermediate resources that are not soterd anywhere yet. They are still locally referenced by code. */
		TMemoryTrackedArray<TSharedPtr<const FImage>> TempImages;
		TMemoryTrackedArray<TSharedPtr<const FMesh>> TempMeshes;

		/** List of resources that are currently in any cache position, and the number of positions they are in. */
		TMemoryTrackedMap<TSharedPtr<const FResource>, int32> CacheResources;

		/** Given a mutable model, find or create its rom cache. */
		FModelCacheEntry* FindModelCache(const FModel*);
		FModelCacheEntry& FindOrAddModelCache(const TSharedPtr<const FModel>&);

        /** Make sure the working memory is below the internal budget, even counting with the passed additional memory. 
		* An optional function can be passed to "block" the unload of certain roms of data.
		* Return true if it succeeded, false otherwise.
		*/
		bool EnsureBudgetBelow(uint64 AdditionalMemory);
		
		/** Return true if the memory budget is 90% full. */
		bool IsMemoryBudgetFull() const;

		/** Calculate the current usage of memory as used to calculate the budget. */
		int64 GetCurrentMemoryBytes() const;

        /** Register that a specific rom has been requested and update the heuristics to keep it in memory. */
        void MarkRomUsed( int32 RomIndex, const TSharedPtr<const FModel>& );

		/** */
		[[nodiscard]] TSharedPtr<FImage> CreateImage(uint32 SizeX, uint32 SizeY, uint32 Lods, EImageFormat Format, EInitializationType Init)
		{
			CheckRunnerThread();

			uint32 DataSize = FImage::CalculateDataSize(SizeX, SizeY, Lods, Format);

			// Look for an unused image in the pool that can be reused
			int32 PooledImageCount = PooledImages.Num();
			for (int Index = 0; DataSize>0 && Index<PooledImageCount; ++Index)
			{
				TSharedPtr<FImage>& Candidate = PooledImages[Index];
				if (Candidate->GetFormat() == Format
					&& Candidate->GetSizeX() == SizeX
					&& Candidate->GetSizeY() == SizeY
					&& Candidate->GetLODCount() == Lods
					)
				{
					TSharedPtr<FImage> Result = Candidate;
					PooledImages.RemoveAtSwap(Index);
					
					if (Init == EInitializationType::Black)
					{
						Result->InitToBlack();
					}
					else
					{
						Result->Flags = 0;
						Result->RelevancyMinY = 0;
						Result->RelevancyMaxY = 0;
					}
					return Result;
				}
			}

			// Make room in the budget
			EnsureBudgetBelow(DataSize);

			// Create it
			TSharedPtr<FImage> Result = MakeShared<FImage>(SizeX, SizeY, Lods, Format, Init);

			TempImages.Add(Result);
			return Result;
		}

		/** Ref will be nulled and relesed in any case. */
		[[nodiscard]] TSharedPtr<FImage> CloneOrTakeOver(TSharedPtr<const FImage>& Resource)
		{
			CheckRunnerThread();

			TempImages.RemoveSingle(Resource);
			
			check(!TempImages.Contains(Resource));
			check(!PooledImages.Contains(Resource));

			TSharedPtr<FImage> Result;
			if (!Resource.IsUnique())
			{
				// TODO: try to grab from the pool

				uint32 DataSize = Resource->GetDataSize();
				EnsureBudgetBelow(DataSize);

				Result = Resource->Clone();
				Release(Resource);
			}
			else
			{
				Result = ConstCastSharedPtr<FImage>(Resource);
				Resource = nullptr;
			}

			return Result;
		}

		/** */
		void Release(TSharedPtr<const FImage>& Resource)
		{
			CheckRunnerThread();

			if (!Resource)
			{
				return;
			}

			const int32 ResourceDataSize = Resource->GetDataSize();
			TempImages.RemoveSingle(Resource);

			check(!TempImages.Contains(Resource));
			check(!PooledImages.Contains(Resource));

			if (IsBudgetTemp(Resource))
			{
				// Check if we are exceeding the budget
				bool bInBudget = EnsureBudgetBelow(ResourceDataSize);
				if (bInBudget)
				{
					PooledImages.Add(ConstCastSharedPtr<FImage>(Resource));
				}
			}
			else
			{
				// Check if we are exceeding the budget
				EnsureBudgetBelow(0);
			}

			Resource = nullptr;
		}

		/** */
		void Release(TSharedPtr<FImage>& Resource)
		{
			CheckRunnerThread();

			if (!Resource)
			{
				return;
			}

			const int32 ResourceDataSize = Resource->GetDataSize();
			TempImages.RemoveSingle(Resource);

			check(!TempImages.Contains(Resource));
			check(!PooledImages.Contains(Resource));

			if (IsBudgetTemp(Resource))
			{
				// Check if we are exceeding the budget
				bool bInBudget = EnsureBudgetBelow(ResourceDataSize);
				if (bInBudget)
				{
					PooledImages.Add(Resource);
				}
			}
			else
			{
				// Check if we are exceeding the budget
				EnsureBudgetBelow(0);
			}

			Resource = nullptr;
		}

		[[nodiscard]] TSharedPtr<FMesh> CreateMesh(int32 BudgetReserveSize)
		{
			CheckRunnerThread();

			EnsureBudgetBelow(BudgetReserveSize);

			TSharedPtr<FMesh> Result = MakeShared<FMesh>();

			TempMeshes.Add(Result);

			return Result;
		}

		[[nodiscard]] TSharedPtr<FMesh> CloneOrTakeOver(TSharedPtr<const FMesh>& Resource)
		{
			CheckRunnerThread();

			const int32 ResourceDataSize = Resource->GetDataSize();
			TempMeshes.RemoveSingle(Resource);

			TSharedPtr<FMesh> Result;
			if (!Resource.IsUnique())
			{
				Result = CreateMesh(ResourceDataSize); 
				Result->CopyFrom(*Resource);
				Release(Resource);
			}
			else
			{
				Result = TSharedPtr<FMesh>(ConstCastSharedPtr<FMesh>(Resource));
				Resource = nullptr;
			}

			return Result;
		}

		void Release(TSharedPtr<const FMesh>& Resource)
		{
			CheckRunnerThread();

			if (!Resource)
			{
				return;
			}

			TempMeshes.RemoveSingle(Resource);
			check(!TempMeshes.Contains(Resource));

			EnsureBudgetBelow(0);

			Resource = nullptr;
		}

		void Release(TSharedPtr<FMesh>& Resource)
		{
			TSharedPtr<const FMesh> ConstPtr = Resource;

			Release(ConstPtr);

			Resource = nullptr;
		}

		/** */
		[[nodiscard]] TSharedPtr<const FMesh> LoadMesh(const FCacheAddress& From, bool bTakeOwnership = false)
		{
			bool bIsLastReference = false;
			TSharedPtr<const FMesh> Result = CurrentInstanceCache->GetMesh(From, bIsLastReference);
			if (!Result)
			{
				return nullptr;
			}

			// If we retrieved the last reference to this resource in "From" cache position (it could still be in other cache positions as well)
			if (bIsLastReference)
			{
				int32* CountPtr = CacheResources.Find(Result);
				check(CountPtr);
				*CountPtr = (*CountPtr) - 1;
				if (!*CountPtr)
				{
					CacheResources.FindAndRemoveChecked(Result);
				}
			}

			if (!bTakeOwnership && Result.IsUnique())
			{
				TempMeshes.Add(Result);
			}

			return Result;
		}

		/** */
		[[nodiscard]] TSharedPtr<const FImage> LoadImage(const FCacheAddress& From, bool bTakeOwnership = false)
		{
			bool bIsLastReference = false;
			TSharedPtr<const FImage> Result = CurrentInstanceCache->GetImage(From, bIsLastReference);
			if (!Result)
			{
				return nullptr;
			}

			// If we retrieved the last reference to this resource in "From" cache position (it could still be in other cache positions as well)
			if (bIsLastReference)
			{
				int32* CountPtr = CacheResources.Find(Result);
				check(CountPtr);
				*CountPtr = (*CountPtr) - 1;
				if (!*CountPtr)
				{
					CacheResources.FindAndRemoveChecked(Result);
				}
			}


			if (!bTakeOwnership && Result.IsUnique())
			{
				TempImages.Add(Result);
			}

			return Result;
		}

		/** */
		void StoreImage(const FCacheAddress& To, TSharedPtr<const FImage> Resource)
		{
			if (Resource)
			{
				int32 ResourceDataSize = Resource->GetDataSize();
				TempImages.RemoveSingle(Resource);

				check(!TempImages.Contains(Resource));

				int32& Count = CacheResources.FindOrAdd(Resource, 0);
				++Count;
			}

			CurrentInstanceCache->SetImage(To, Resource);
		}

		void StoreMesh(const FCacheAddress& To, TSharedPtr<const FMesh> Resource)
		{
			if (Resource)
			{
				//Resource->CheckIntegrity();

				TempMeshes.RemoveSingle(Resource);

				int32& Count = CacheResources.FindOrAdd(Resource, 0);
				++Count;
			}

			CurrentInstanceCache->SetMesh(To, Resource);
		}

		/** Return true if the resource is not in any cache (0,1,rom). */
		bool IsBudgetTemp(const TSharedPtr<const FResource>& Resource)
		{
			if (!Resource)
			{
				return false;
			}

			bool bIsTemp = Resource.IsUnique();
			return bIsTemp;
		}

		int32 GetPooledBytes() const
		{
			int32 Result = 0;
			for (const TSharedPtr<FImage>& Value : PooledImages)
			{
				Result += Value->GetDataSize();
			}

			return Result;
		}

		int32 GetTempBytes() const
		{
			int32 Result = 0;
			for (const TSharedPtr<const FImage>& Value : TempImages)
			{
				Result += Value->GetDataSize();
			}

			for (const TSharedPtr<const FMesh>& Value : TempMeshes)
			{
				Result += Value->GetDataSize();
			}

			return Result;
		}


		int32 GetRomBytes() const
		{
			int32 Result = 0;

			TArray<const FModel*> Models;
			for (const FLiveInstance& Instance : LiveInstances)
			{
				Models.AddUnique(Instance.Model.Get());
			}

			// Data stored per-model, but related to instance construction
			for (const FModel* Model : Models)
			{
				// Count streamable and currently-loaded resources
				const FProgram& Program = Model->GetPrivate()->Program;
				for (const TPair<uint32,TSharedPtr<const FImage>>& ImageLOD : Program.ConstantImageLODsStreamed)
				{
					if (ImageLOD.Value)
					{
						Result += ImageLOD.Value->GetDataSize();
					}
				}
				for (const TPair<uint32, TSharedPtr<const FMesh>>& Rom : Program.ConstantMeshesStreamed)
				{
					if (Rom.Value)
					{
						Result += Rom.Value->GetDataSize();
					}
				}
			}

			return Result;
		}


		int32 GetTrackedCacheBytes() const
		{
			int32 Result = 0;
			for (const TTuple<TSharedPtr<const FResource>,int32>& It : CacheResources)
			{
				Result += It.Key->GetDataSize();
			}

			return Result;
		}

		/** Calculate the amount of bytes in data cached in the level 0 and 1 cache in all live instances. */
		int32 GetCacheBytes() const
		{
			int32 Result = 0;
			TSet<const FResource*> Cache0Unique;
			TSet<const FResource*> Cache1Unique;

			for (const FLiveInstance& Instance : LiveInstances)
			{
				CodeContainer<FProgramCache::FOpExecutionData>::iterator it = Instance.Cache->OpExecutionData.begin();
				for (; it.IsValid(); ++it)
				{
					if (!(*it).DataTypeIndex)
					{
						continue;
					}

					TSet<const FResource*>* TargetSet = &Cache0Unique;
					if ((*it).IsCacheLocked)
					{
						TargetSet = &Cache1Unique;
					}

					const FResource* Value = nullptr;
					switch ((*it).DataType)
					{
					case EDataType::Image:
						Value = Instance.Cache->ImageResults[(*it).DataTypeIndex].Value.Get();
						if (Value)
						{
							TargetSet->Add(Value);
						}
						break;

					case EDataType::Mesh:
						Value = Instance.Cache->MeshResults[(*it).DataTypeIndex].Value.Get();
						if (Value)
						{
							TargetSet->Add(Value);
						}
						break;

					default:
						break;
					}
				}
			}

			// Merge
			Cache0Unique.Append(Cache1Unique);

			// Count
			for (const FResource* Value : Cache0Unique)
			{
				Result += Value->GetDataSize();
			}

			return Result;
		}

		/** Remove all intermediate data (big and small) from the memory except for the one that has been explicitely
		* marked as state cache.
		*/
		void ClearCacheLayer0()
		{
			check(CurrentInstanceCache);

			MUTABLE_CPUPROFILER_SCOPE(ClearLayer0);

			CodeContainer<FProgramCache::FOpExecutionData>::iterator it = CurrentInstanceCache->OpExecutionData.begin();
			for (; it.IsValid(); ++it)
			{
				FProgramCache::FOpExecutionData& Data = *it;
				if (!Data.DataTypeIndex
					||
					Data.IsCacheLocked)
				{
					continue;
				}

				switch (Data.DataType)
				{
				case EDataType::Image:
				{
					TSharedPtr<const FImage> Value = CurrentInstanceCache->ImageResults[Data.DataTypeIndex].Value;
					if (Value)
					{
						CacheResources.Remove(Value);
						CurrentInstanceCache->ImageResults[Data.DataTypeIndex].Value = nullptr;
					}
					break;
				}

				case EDataType::Mesh:
				{
					TSharedPtr<const FMesh> Value = CurrentInstanceCache->MeshResults[Data.DataTypeIndex].Value;
					if (Value)
					{
						CacheResources.Remove(Value);
						CurrentInstanceCache->MeshResults[Data.DataTypeIndex].Value = nullptr;
					}
					break;
				}

				case EDataType::Layout:
				{
					TSharedPtr<const FLayout> Value = CurrentInstanceCache->LayoutResults[Data.DataTypeIndex];
					CurrentInstanceCache->LayoutResults[Data.DataTypeIndex] = nullptr;
					break;
				}

				case EDataType::Instance:
				{
					TSharedPtr<const FInstance> Value = CurrentInstanceCache->InstanceResults[Data.DataTypeIndex];
					CurrentInstanceCache->InstanceResults[Data.DataTypeIndex] = nullptr;
					break;
				}

				default:
					break;
				}

				Data.OpHitCount = 0;
				Data.IsValueValid = false;
			}
		}


		/** Remove all intermediate data (big and small) from the memory including the one that has been explicitely
		* marked as state cache.
		*/
		void ClearCacheLayer1()
		{
			MUTABLE_CPUPROFILER_SCOPE(ClearLayer1);

			CodeContainer<FProgramCache::FOpExecutionData>::iterator it = CurrentInstanceCache->OpExecutionData.begin();
			for (; it.IsValid(); ++it)
			{
				FProgramCache::FOpExecutionData& Data = *it;
				if (!Data.DataTypeIndex)
				{
					continue;
				}

				switch (Data.DataType)
				{
				case EDataType::Image:
				{
					TSharedPtr<const FImage> Value = CurrentInstanceCache->ImageResults[Data.DataTypeIndex].Value;
					CacheResources.Remove(Value);
					CurrentInstanceCache->ImageResults[Data.DataTypeIndex].Value = nullptr;
					break;
				}

				case EDataType::Mesh:
				{
					TSharedPtr<const FMesh> Value = CurrentInstanceCache->MeshResults[Data.DataTypeIndex].Value;
					CacheResources.Remove(Value);
					CurrentInstanceCache->MeshResults[Data.DataTypeIndex].Value = nullptr;
					break;
				}

				case EDataType::Layout:
				{
					TSharedPtr<const FLayout> Value = CurrentInstanceCache->LayoutResults[Data.DataTypeIndex];
					CurrentInstanceCache->LayoutResults[Data.DataTypeIndex] = nullptr;
					break;
				}

				case EDataType::Instance:
				{
					TSharedPtr<const FInstance> Value = CurrentInstanceCache->InstanceResults[Data.DataTypeIndex];
					CurrentInstanceCache->InstanceResults[Data.DataTypeIndex] = nullptr;
					break;
				}

				default:
					break;
				}

				Data.OpHitCount = 0;
				Data.IsValueValid = false;
			}
		}


		void LogWorkingMemory(const class CodeRunner* CurrentRunner) const;


#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		static constexpr uint64 InvalidRunnerId = 0;
		std::atomic<uint64> DebugCurrentRunnerId {1};
		
		/** Temp variable with the ID of the thread running code, for debugging. */
		uint32 DebugRunnerThreadID = FThread::InvalidThreadId;
		uint64 DebugRunnerID = InvalidRunnerId;
#endif

		/** This is a development-only check to make sure calls to resource management happen in the correct thread. */
		FORCEINLINE void BeginRunnerThread()
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// If this check fails it means have not set up correctly all the paths to debug threading for resource management.
			check(DebugRunnerThreadID == FThread::InvalidThreadId);
			check(DebugRunnerID == InvalidRunnerId)

			DebugRunnerThreadID = FPlatformTLS::GetCurrentThreadId();
			DebugRunnerID = ++DebugCurrentRunnerId;
#endif
		}

		/** This is a development-only check to make sure calls to resource management happen in the correct thread. */
		FORCEINLINE void ResetRunnerThread()
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// If this check fails it means have not set up correctly all the paths to debug threading for resource management.
			check(DebugRunnerThreadID == FThread::InvalidThreadId);
			check(DebugRunnerID != InvalidRunnerId)

			DebugRunnerThreadID = FPlatformTLS::GetCurrentThreadId();
#endif
		}

		/** This is a development-only check to make sure calls to resource management happen in the correct thread. */
		FORCEINLINE void InvalidateRunnerThread()
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// If this check fails it means have not set up correctly all the paths to debug threading for resource management.
			check(DebugRunnerThreadID != FThread::InvalidThreadId);
			check(DebugRunnerID != InvalidRunnerId)

			DebugRunnerThreadID = FThread::InvalidThreadId;
#endif
		}

		/** This is a development-only check to make sure calls to resource management happen in the correct thread. */
		FORCEINLINE void CheckRunnerThread()
		{
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// If this check fails it means have not set up correctly all the paths to debug threading for resource management.
			check(DebugRunnerThreadID != FThread::InvalidThreadId);
			check(DebugRunnerID != InvalidRunnerId);

			// If this check fails it means we are doing resource management from a thread that is not the CodeRunner::RunCode
			// thread, and this is not allowed.
			check(DebugRunnerThreadID == FPlatformTLS::GetCurrentThreadId());
			check(DebugRunnerID == DebugCurrentRunnerId);
#endif
		}


		/** This is a development-only check to make sure calls to resource management happen in the correct thread. */
		FORCEINLINE void EndRunnerThread()
		{
			CurrentInstanceCache->CheckHitCountsCleared();

			// If this check fails it means some operation is not correctly handling resource management and didn't release
			// a resource it created.
			// This should be reported and reviewed, but it is not fatal. Some unnecessary memory may be used temporarily.
			ensure(TempImages.Num() == 0);
			ensure(TempMeshes.Num() == 0);

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
			// If this check fails it means have not set up correctly all the paths to debug threading for resource management.
			check(DebugRunnerThreadID != FThread::InvalidThreadId);
			check(DebugRunnerID != InvalidRunnerId);

			DebugRunnerThreadID = FThread::InvalidThreadId;
			DebugRunnerID = InvalidRunnerId;
#endif
		}

	};


	/** */
    class FSystem::Private
    {
		friend class System;
    public:

        Private(const FSettings&);
        ~Private();

        //-----------------------------------------------------------------------------------------
        //! Own interface
        //-----------------------------------------------------------------------------------------

		/** This method can be used to internally prepare for code execution. */
		MUTABLERUNTIME_API void BeginBuild(const TSharedPtr<const FModel>&);
		MUTABLERUNTIME_API void EndBuild();

		MUTABLERUNTIME_API bool BuildBool(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
		MUTABLERUNTIME_API int32 BuildInt(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
		MUTABLERUNTIME_API float BuildScalar(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
		MUTABLERUNTIME_API FVector4f BuildColour(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
		MUTABLERUNTIME_API TSharedPtr<const String> BuildString(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
		MUTABLERUNTIME_API TSharedPtr<const FImage> BuildImage(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS, int32 MipsToSkip, int32 LOD);
		MUTABLERUNTIME_API TSharedPtr<const FMesh> BuildMesh(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS, EMeshContentFlags MeshContentFilter);
		MUTABLERUNTIME_API TSharedPtr<const FInstance> BuildInstance(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS);
		MUTABLERUNTIME_API TSharedPtr<const FLayout> BuildLayout(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
    	MUTABLERUNTIME_API FProjector BuildProjector(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS) ;
    	
        //!
        FSettings Settings;

        //! Data streaming interface, if any.
		TSharedPtr<FModelReader> StreamInterface;

		TSharedPtr<FExternalResourceProvider> ExternalResourceProvider;

		/** Counter used to generate unique IDs for every new instance created in the system. */
        FInstance::FID LastInstanceID = 0;

		/** This flag is turned on when a streaming error or similar happens. Results are not usable.
		* This should only happen in-editor.
		*/
		bool bUnrecoverableError = false;

		/** If this is set, it will be tried first instead of the internal formatting function. */
		FImageOperator::FImagePixelFormatFunc ImagePixelFormatOverride;

		/** */
		FWorkingMemoryManager WorkingMemoryManager;

		/** The pointer returned by this function is only valid for the duration of the current mutable operation. */
		inline FLiveInstance* FindLiveInstance(FInstance::FID);

        //!
        bool CheckUpdatedParameters( const FLiveInstance*, const TSharedPtr<const FParameters>&, uint64& OutUpdatedParameters );


		void RunCode(const TSharedPtr<const FModel>&, const FParameters*, OP::ADDRESS, uint32 LODs = FSystem::AllLODs, uint8 ExecutionOptions = 0, int32 LOD = 0);

		//!
		void PrepareCache(const FModel*, int32 State);

		//! Update some mutable core unreal stats.
		void UpdateStats();
    };

}
