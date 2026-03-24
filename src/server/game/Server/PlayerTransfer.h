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

#ifndef PlayerTransfer_h__
#define PlayerTransfer_h__

#include "Define.h"
#include "ObjectGuid.h"
#include <string>
#include <vector>

class Player;

/// Wire‑format version — bump when struct layout changes
constexpr uint8 PLAYER_TRANSFER_VERSION = 1;
/// Two‑byte magic "PT" (0x50 0x54)
constexpr uint16 PLAYER_TRANSFER_MAGIC = 0x5450; // little‑endian "PT"

/// Aura snapshot transferred over NATS
struct TransferAuraInfo
{
    uint32 spellId      = 0;
    int32  duration     = 0;
    int32  maxDuration  = 0;
    uint8  stackAmount  = 0;
    uint64 casterGuid   = 0; // raw ObjectGuid counter
};

/// Pet snapshot transferred over NATS
struct TransferPetInfo
{
    bool   hasPet       = false;
    uint32 entry        = 0;
    uint32 displayId    = 0;
    uint32 level        = 0;
    uint32 health       = 0;
    uint32 mana         = 0;
    uint32 happiness    = 0;
    uint8  reactState   = 0;
    std::string name;
};

/// Transport snapshot transferred over NATS
struct TransferTransportInfo
{
    bool   onTransport  = false;
    uint32 entry        = 0;
    float  offsetX      = 0.f;
    float  offsetY      = 0.f;
    float  offsetZ      = 0.f;
    float  offsetO      = 0.f;
};

/// Complete player state for cross‑node NATS transfer
struct PlayerTransferData
{
    // identity
    uint64 guid         = 0; // ObjectGuid raw value
    uint32 accountId    = 0;

    // position
    uint32 mapId        = 0;
    uint32 zoneId       = 0;
    uint32 areaId       = 0;
    float  posX         = 0.f;
    float  posY         = 0.f;
    float  posZ         = 0.f;
    float  orientation  = 0.f;

    // vitals
    uint32 health       = 0;
    uint32 maxHealth    = 0;
    uint8  powerType    = 0; // Powers enum
    uint32 power        = 0;
    uint32 maxPower     = 0;
    uint32 level        = 0;

    // transport
    TransferTransportInfo transport;

    // pet
    TransferPetInfo pet;

    // auras (active, non‑passive, non‑hidden)
    std::vector<TransferAuraInfo> auras;

    // timestamp (server game‑time seconds when snapshot was taken)
    uint64 timestamp    = 0;
};

/// Build a PlayerTransferData from a live Player pointer (must be called on the node that owns the player).
PlayerTransferData SnapshotPlayer(Player const* player);

/// Serialize a PlayerTransferData to a binary byte buffer suitable for NATS publish.
/// Wire format: little‑endian, length‑prefixed strings, "PT" magic + version header.
std::vector<uint8> SerializeTransfer(PlayerTransferData const& data);

/// Deserialize a binary NATS message back into PlayerTransferData.
/// Returns false if the buffer is malformed, magic/version mismatch, or truncated.
bool DeserializeTransfer(uint8 const* buf, size_t len, PlayerTransferData& out);

#endif // PlayerTransfer_h__
