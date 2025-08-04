﻿// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "LinkGenerationDebugFlags.generated.h"

UENUM(meta = (Bitflags, UseEnumValuesAsMaskValuesInEditor = "true"))
enum class ELinkGenerationDebugFlags : uint16
{
	WalkableSurface				    = 1 << 0,
	WalkableBorders				    = 1 << 1,
	SelectedEdge				    = 1 << 2,
	SelectedEdgeTrajectory		    = 1 << 3,
	SelectedEdgeLandingSamples	    = 1 << 4,
	SelectedEdgeCollisions		    = 1 << 5,
	SelectedEdgeCollisionsSamples	= 1 << 6,
	Links						    = 1 << 7,
	FilteredLinks				    = 1 << 8,
};
ENUM_CLASS_FLAGS(ELinkGenerationDebugFlags);
