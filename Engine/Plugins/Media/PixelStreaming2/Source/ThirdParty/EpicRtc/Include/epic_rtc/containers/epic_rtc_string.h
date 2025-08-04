// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include <cstdint>

#include "epic_rtc/common/defines.h"
#include "epic_rtc/core/ref_count.h"

#pragma pack(push, 8)

/**
 * A reference counted String container that allows strings to be copied over ABI and DLL boundary.
 * Normally a std::string may not be valid when passed between DLLs and memory allocation may be different.
 */
class EpicRtcStringInterface : public EpicRtcRefCountInterface
{
public:
    /**
     * Get the pointer to the string.
     * @return The string.
     */
    virtual EPICRTC_API const char* Get() const = 0;

    /**
     * Get the length  of the string in number of characters.
     * @return the length of the string.
     */
    virtual EPICRTC_API uint64_t Length() const = 0;

    // Prevent copying
    EpicRtcStringInterface(const EpicRtcStringInterface&) = delete;
    EpicRtcStringInterface& operator=(const EpicRtcStringInterface&) = delete;

protected:
    // Only class Implementation can be constructed or destroyed
    EPICRTC_API EpicRtcStringInterface() = default;
    virtual EPICRTC_API ~EpicRtcStringInterface() = default;
};

/**
 * A reference counted pair of Strings. Memory owning, destructor must release the underlying EpicRtcStringInterface implementation.
 */
class EpicRtcParameterPairInterface : public EpicRtcRefCountInterface
{
public:
    /**
     * Get the key.
     * @return A reference counted String container.
     */
    virtual EPICRTC_API EpicRtcStringInterface* GetKey() = 0;

    /**
     * Get the value.
     * @return A reference counted String container.
     */
    virtual EPICRTC_API EpicRtcStringInterface* GetValue() = 0;

    // Prevent copying
    EpicRtcParameterPairInterface(const EpicRtcParameterPairInterface&) = delete;
    EpicRtcParameterPairInterface& operator=(const EpicRtcParameterPairInterface&) = delete;

protected:
    // Only class Implementation can be constructed or destroyed
    EPICRTC_API EpicRtcParameterPairInterface() = default;
    virtual EPICRTC_API ~EpicRtcParameterPairInterface() = default;
};

#pragma pack(pop)
