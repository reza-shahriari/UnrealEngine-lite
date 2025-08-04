// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "epic_rtc/common/defines.h"
#include "epic_rtc/containers/epic_rtc_string.h"
#include "epic_rtc/core/ref_count.h"
#include "connection.h"

#pragma pack(push, 8)

/**
 * Represents a Room participant with its id, tracks, and media connection.
 */
class EpicRtcParticipantInterface : public EpicRtcRefCountInterface
{
public:
    /**
     * Gets the participant Id.
     */
    virtual EPICRTC_API EpicRtcStringView GetId() = 0;

    /**
     * Blocks the participant.
     */
    virtual EPICRTC_API void Block() = 0;

    /**
     * Kicks the participant out of the Room.
     */
    virtual EPICRTC_API void Kick() = 0;

    /**
     * Mutes all participant's tracks.
     * @param bIsMuted True to mute, false to unmute.
     */
    virtual EPICRTC_API void MuteAll(EpicRtcBool isMuted) = 0;

    /**
     * Subscribes to all participant's tracks
     */
    virtual EPICRTC_API void SubscribeAll() = 0;

    /**
     * Unsubscribes from all participant's tracks.
     */
    virtual EPICRTC_API void UnsubscribeAll() = 0;

    /**
     * Get P2P EpicRtcConnectionInterface associated with this participant
     * @return P2P Connection
     */
    virtual EPICRTC_API EpicRtcConnectionInterface* GetConnection() = 0;

    // Prevent copying
    EpicRtcParticipantInterface(const EpicRtcParticipantInterface&) = delete;
    EpicRtcParticipantInterface& operator=(const EpicRtcParticipantInterface&) = delete;

protected:
    // Only class Implementation can be constructed or destroyed
    EPICRTC_API EpicRtcParticipantInterface() = default;
    virtual EPICRTC_API ~EpicRtcParticipantInterface() = default;
};

#pragma pack(pop)
