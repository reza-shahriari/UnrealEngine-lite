// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include <cstdint>

#include "epic_rtc/common/defines.h"
#include "epic_rtc/containers/epic_rtc_array.h"
#include "epic_rtc/core/ref_count.h"

#pragma pack(push, 8)

class EpicRtcGenericFrameInfoInterface : public EpicRtcRefCountInterface
{
public:
    /**
     * Gets the spatial layer id.
     * @return Id.
     */
    virtual EPICRTC_API int32_t GetSpatialLayerId() = 0;

    /**
     * Gets the temporal layer id.
     * @return Id.
     */
    virtual EPICRTC_API int32_t GetTemporalLayerId() = 0;

    /**
     * Gets the decode target indications.
     * @return DecodeTargetIndications.
     */
    virtual EPICRTC_API EpicRtcDecodeTargetIndicationArrayInterface* GetDecodeTargetIndications() = 0;

    /**
     * Gets the frame diffs.
     * @return FrameDiffs.
     */
    virtual EPICRTC_API EpicRtcInt32ArrayInterface* GetFrameDiffs() = 0;

    /**
     * Gets the chain diffs.
     * @return ChainDiffs.
     */
    virtual EPICRTC_API EpicRtcInt32ArrayInterface* GetChainDiffs() = 0;

    /**
     * Gets the encoder buffer usage.
     * @return EncoderBufferUsages.
     */
    virtual EPICRTC_API EpicRtcCodecBufferUsageArrayInterface* GetEncoderBufferUsages() = 0;

    /**
     * Gets the part of chain.
     * @return PartOfChain.
     */
    virtual EPICRTC_API EpicRtcBoolArrayInterface* GetPartOfChain() = 0;

    /**
     * Gets the active decode targets.
     * @return ActiveDecodeTargets.
     */
    virtual EPICRTC_API EpicRtcBoolArrayInterface* GetActiveDecodeTargets() = 0;

    EpicRtcGenericFrameInfoInterface(const EpicRtcGenericFrameInfoInterface&) = delete;
    EpicRtcGenericFrameInfoInterface& operator=(const EpicRtcGenericFrameInfoInterface&) = delete;

protected:
    EpicRtcGenericFrameInfoInterface() = default;
    virtual ~EpicRtcGenericFrameInfoInterface() = default;
};

#pragma pack(pop)
