// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "HAL/PlatformMath.h"
#include "MuR/Image.h"
#include "MuR/MeshBufferSet.h"
#include "MuR/Parameters.h"
#include "MuR/ParametersPrivate.h"

// This header file provides additional information about data types defined in the runtime, that
// can be useful only for the tools. For instance, it provides strings for some enumeration values.

namespace mu
{

	class MUTABLETOOLS_API TypeInfo
	{
	public:

		static const char* s_imageFormatName[size_t(EImageFormat::Count)];

		static const char* s_meshBufferSemanticName[uint32(EMeshBufferSemantic::Count)];

		static const char* s_meshBufferFormatName[uint32(EMeshBufferFormat::Count) ];

		static const char* s_blendModeName[uint32(EBlendType::_BT_COUNT) ];

		static const char* s_projectorTypeName [ static_cast<uint32>(mu::EProjectorType::Count) ];

	};

}
