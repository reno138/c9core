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

#ifndef SharedPlayerState_h__
#define SharedPlayerState_h__

#include "Define.h"
#include <cstdint>
#include <string>
#include <vector>

/// Bitmask of which fields changed since last broadcast
enum StateFieldMask : uint8
{
    STATE_FIELD_POSITION  = 0x01,
    STATE_FIELD_HEALTH    = 0x02,
    STATE_FIELD_POWER     = 0x04,
    STATE_FIELD_COMBAT    = 0x08,
    STATE_FIELD_TRANSPORT = 0x10,
    STATE_FIELD_PET       = 0x20,
    STATE_FIELD_AURAS     = 0x40,
    STATE_FIELD_DEATH     = 0x80,
    STATE_FIELD_ALL       = 0xFF,
};

struct CachedAura
{
    uint32 spellId{0};
    int32  duration{-1};   // ms remaining, -1 = permanent
    uint8  stacks{1};
};

struct SharedPlayerState
{
    // Identity
    uint64 guid{0};
    uint32 accountId{0};
    uint8  ownerNodeId{0};     // which node currently "owns" this player
    bool   active{false};       // true on owning node only

    // Position
    uint32 mapId{0};
    uint32 zoneId{0};
    uint32 areaId{0};
    float  posX{0}, posY{0}, posZ{0}, posO{0};

    // Vitals
    uint32 health{0};
    uint32 maxHealth{0};
    uint8  powerType{0};
    uint32 power{0};
    uint32 maxPower{0};
    uint8  level{0};

    // Combat state
    bool   inCombat{false};
    bool   inFlight{false};
    bool   isDead{false};

    // Transport
    uint64 transportGuid{0};
    float  transOffX{0}, transOffY{0}, transOffZ{0}, transOffO{0};

    // Pet
    uint32 petEntry{0};        // 0 = no pet
    uint32 petHealth{0};
    uint32 petMana{0};
    std::string petName;

    // Buffs
    std::vector<CachedAura> auras;

    // Display (for future cross-node visibility)
    uint8  raceId{0};
    uint8  classId{0};
    uint8  gender{0};
    uint32 displayId{0};

    // Timestamps (getMSTime based)
    uint32 lastUpdateMs{0};
    uint32 lastBroadcastMs{0};
};

#endif // SharedPlayerState_h__
