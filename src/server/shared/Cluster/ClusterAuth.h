/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef ClusterAuth_h__
#define ClusterAuth_h__

#include "Define.h"
#include <cstddef>
#include <string>
#include <vector>

/**
 * @brief Authenticated framing for the NATS cluster bus.
 *
 * THREAT MODEL
 * ------------
 * The NATS bus carries privileged control messages between worldserver nodes,
 * including MSG_RA_COMMAND (arbitrary server console commands) and
 * MSG_CLUSTER_PLAYER_ONLINE (which kicks a matching local session). Prior to
 * this module the bus was entirely unauthenticated and unencrypted: anyone who
 * could reach the NATS port could execute console commands on every node and
 * disconnect arbitrary players.
 *
 * This module adds an HMAC-SHA256 authentication tag plus replay protection to
 * every cluster frame. It deliberately does NOT add confidentiality — use NATS
 * TLS for that. Authentication is the property that actually gates privilege.
 *
 * WIRE FORMAT
 * -----------
 *   [ver:1][srcNode:1][timestampMs:8 LE][nonce:8][msgType:1][payload:N][tag:32]
 *
 *   tag = HMAC-SHA256(key, ver || srcNode || timestampMs || nonce || msgType || payload)
 *
 * The tag covers srcNode, so a peer cannot forge another node's identity, and it
 * covers msgType, so a frame cannot be re-typed (e.g. a benign chat relay
 * replayed as MSG_RA_COMMAND).
 *
 * REPLAY PROTECTION
 * -----------------
 * A frame is rejected if its timestamp is outside +/- ACCEPT_SKEW_MS of local
 * time, or if its nonce has already been seen inside that window. The seen-set
 * is pruned on insert, so memory is bounded by the frame rate over the window.
 *
 * KEY MANAGEMENT
 * --------------
 * The key comes from ClusterServer.AuthKey (>= 32 bytes of high-entropy hex or
 * base64). If the option is absent or too short, Init() fails and the caller is
 * expected to refuse to start the cluster bus. Failing closed is deliberate:
 * a silently-unauthenticated bus is what this module exists to prevent.
 */
namespace ClusterAuth
{
    /// Minimum accepted shared-key length in bytes. 32 bytes = 256 bits.
    constexpr std::size_t MIN_KEY_BYTES = 32;

    /// Frames older/newer than this (ms) are rejected outright.
    constexpr uint64 ACCEPT_SKEW_MS = 30000;

    /// Byte counts of the fixed frame fields.
    constexpr std::size_t HEADER_BYTES = 1 + 1 + 8 + 8 + 1; ///< ver+src+ts+nonce+msgType
    constexpr std::size_t TAG_BYTES    = 32;
    constexpr std::size_t MIN_FRAME    = HEADER_BYTES + TAG_BYTES;

    /**
     * @brief Install the shared key. Must succeed before Seal/Open are used.
     * @return false if the key is shorter than MIN_KEY_BYTES (caller must fail closed).
     */
    bool Init(std::string const& sharedKey);

    /// True once Init() has succeeded with an acceptable key.
    bool IsInitialised();

    /**
     * @brief Build an authenticated frame carrying @p msgType and @p payload.
     * @return the complete frame, ready to hand to natsConnection_Publish.
     */
    std::vector<uint8> Seal(uint8 srcNodeId, uint8 msgType, uint8 const* payload, std::size_t payloadLen);

    /**
     * @brief Verify and unpack a received frame.
     *
     * Performs, in order: length check, constant-time tag comparison, clock-skew
     * check, replay check. Only on full success are the out-parameters written.
     *
     * @return true if the frame is authentic and fresh.
     */
    bool Open(uint8 const* frame, std::size_t frameLen,
              uint8& outSrcNodeId, uint8& outMsgType, std::vector<uint8>& outPayload);
}

#endif // ClusterAuth_h__
