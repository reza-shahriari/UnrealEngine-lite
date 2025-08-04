/* SPDX-License-Identifier: MIT */
/**
	@file		ntv2version.h
	@brief		Defines for the NTV2 SDK version number, used by `ajantv2/includes/ntv2enums.h`.
	See the `ajantv2/includes/ntv2version.h.in` template when building with with CMake.
	@copyright	(C) 2013-2022 AJA Video Systems, Inc.  All rights reserved.
**/
#ifndef _NTV2VERSION_H_
#define _NTV2VERSION_H_

#include "ajaexport.h"

#define AJA_NTV2_SDK_VERSION_MAJOR		17		///< @brief The SDK major version number, an unsigned decimal integer.
#define AJA_NTV2_SDK_VERSION_MINOR		0		///< @brief The SDK minor version number, an unsigned decimal integer.
#define AJA_NTV2_SDK_VERSION_POINT		0		///< @brief The SDK "point" release version, an unsigned decimal integer.
#define AJA_NTV2_SDK_BUILD_NUMBER		12			///< @brief The SDK build number, an unsigned decimal integer.
#define AJA_NTV2_SDK_BUILD_DATETIME		"2023-11-17T04:34:41Z"		///< @brief The date and time the SDK was built, in ISO-8601 format
#define AJA_NTV2_SDK_BUILD_TYPE			""			///< @brief The SDK build type, where "a"=alpha, "b"=beta, "d"=development, ""=release.

#define AJA_NTV2_SDK_VERSION	((AJA_NTV2_SDK_VERSION_MAJOR << 24) | (AJA_NTV2_SDK_VERSION_MINOR << 16) | (AJA_NTV2_SDK_VERSION_POINT << 8) | (AJA_NTV2_SDK_BUILD_NUMBER))
#define AJA_NTV2_SDK_VERSION_AT_LEAST(__a__,__b__)		(AJA_NTV2_SDK_VERSION >= (((__a__) << 24) | ((__b__) << 16)))
#define AJA_NTV2_SDK_VERSION_BEFORE(__a__,__b__)		(AJA_NTV2_SDK_VERSION < (((__a__) << 24) | ((__b__) << 16)))

#if !defined(NTV2_BUILDING_DRIVER)
	#include <string>
	AJAExport std::string NTV2Version (const bool inDetailed = false);	///< @returns a string containing SDK version information
	AJAExport const std::string & NTV2GitHash (void);		///< @returns the 40-character ID of the last commit for this SDK build
	AJAExport const std::string & NTV2GitHashShort (void);	///< @returns the 10-character ID of the last commit for this SDK build
#endif

#endif	//	_NTV2VERSION_H_
