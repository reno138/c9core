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

#include "PlayerStateSync.h"
#include "Log.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Little-endian serialization helpers (same pattern as PlayerTransfer.cpp)
// ---------------------------------------------------------------------------

namespace
{
    template <typename T>
    void WriteLE(std::vector<uint8>& buf, T value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const size_t off = buf.size();
        buf.resize(off + sizeof(T));
        std::memcpy(buf.data() + off, &value, sizeof(T));
    }

    void WriteString8(std::vector<uint8>& buf, std::string const& s)
    {
        uint8 slen = static_cast<uint8>(s.size() > 255 ? 255 : s.size());
        WriteLE<uint8>(buf, slen);
        if (slen > 0)
        {
            const size_t off = buf.size();
            buf.resize(off + slen);
            std::memcpy(buf.data() + off, s.data(), slen);
        }
    }

    struct Reader
    {
        uint8 const* data;
        size_t       len;
        size_t       pos = 0;

        bool CanRead(size_t n) const { return pos + n <= len; }

        template <typename T>
        bool ReadLE(T& out)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            if (!CanRead(sizeof(T)))
                return false;
            std::memcpy(&out, data + pos, sizeof(T));
            pos += sizeof(T);
            return true;
        }

        bool ReadString8(std::string& out)
        {
            uint8 slen = 0;
            if (!ReadLE(slen))
                return false;
            if (!CanRead(slen))
                return false;
            out.assign(reinterpret_cast<char const*>(data + pos), slen);
            pos += slen;
            return true;
        }
    };

    // Serialize fields matching the fieldMask into buf
    void WriteDeltaFields(std::vector<uint8>& buf, uint8 fieldMask, SharedPlayerState const& s)
    {
        if (fieldMask & STATE_FIELD_POSITION)
        {
            WriteLE<uint32>(buf, s.mapId);
            WriteLE<uint32>(buf, s.zoneId);
            WriteLE<uint32>(buf, s.areaId);
            WriteLE<float>(buf, s.posX);
            WriteLE<float>(buf, s.posY);
            WriteLE<float>(buf, s.posZ);
            WriteLE<float>(buf, s.posO);
        }

        if (fieldMask & STATE_FIELD_HEALTH)
        {
            WriteLE<uint32>(buf, s.health);
            WriteLE<uint32>(buf, s.maxHealth);
        }

        if (fieldMask & STATE_FIELD_POWER)
        {
            WriteLE<uint8>(buf, s.powerType);
            WriteLE<uint32>(buf, s.power);
            WriteLE<uint32>(buf, s.maxPower);
        }

        if (fieldMask & STATE_FIELD_COMBAT)
        {
            uint8 flags = 0;
            if (s.inCombat) flags |= 0x01;
            if (s.inFlight) flags |= 0x02;
            if (s.isDead)   flags |= 0x04;
            WriteLE<uint8>(buf, flags);
        }

        if (fieldMask & STATE_FIELD_TRANSPORT)
        {
            WriteLE<uint64>(buf, s.transportGuid);
            WriteLE<float>(buf, s.transOffX);
            WriteLE<float>(buf, s.transOffY);
            WriteLE<float>(buf, s.transOffZ);
            WriteLE<float>(buf, s.transOffO);
        }

        if (fieldMask & STATE_FIELD_PET)
        {
            WriteLE<uint32>(buf, s.petEntry);
            WriteLE<uint32>(buf, s.petHealth);
            WriteLE<uint32>(buf, s.petMana);
            WriteString8(buf, s.petName);
        }

        if (fieldMask & STATE_FIELD_AURAS)
        {
            uint8 count = static_cast<uint8>(s.auras.size() > 255 ? 255 : s.auras.size());
            WriteLE<uint8>(buf, count);
            for (uint8 i = 0; i < count; ++i)
            {
                WriteLE<uint32>(buf, s.auras[i].spellId);
                WriteLE<int32>(buf, s.auras[i].duration);
                WriteLE<uint8>(buf, s.auras[i].stacks);
            }
        }

        if (fieldMask & STATE_FIELD_DEATH)
        {
            WriteLE<uint8>(buf, s.isDead ? 1 : 0);
        }
    }

    // Deserialize fields matching the fieldMask from reader into s
    bool ReadDeltaFields(Reader& r, uint8 fieldMask, SharedPlayerState& s)
    {
        if (fieldMask & STATE_FIELD_POSITION)
        {
            if (!r.ReadLE(s.mapId))  return false;
            if (!r.ReadLE(s.zoneId)) return false;
            if (!r.ReadLE(s.areaId)) return false;
            if (!r.ReadLE(s.posX))   return false;
            if (!r.ReadLE(s.posY))   return false;
            if (!r.ReadLE(s.posZ))   return false;
            if (!r.ReadLE(s.posO))   return false;
        }

        if (fieldMask & STATE_FIELD_HEALTH)
        {
            if (!r.ReadLE(s.health))    return false;
            if (!r.ReadLE(s.maxHealth)) return false;
        }

        if (fieldMask & STATE_FIELD_POWER)
        {
            if (!r.ReadLE(s.powerType)) return false;
            if (!r.ReadLE(s.power))     return false;
            if (!r.ReadLE(s.maxPower))  return false;
        }

        if (fieldMask & STATE_FIELD_COMBAT)
        {
            uint8 flags = 0;
            if (!r.ReadLE(flags)) return false;
            s.inCombat = (flags & 0x01) != 0;
            s.inFlight = (flags & 0x02) != 0;
            s.isDead   = (flags & 0x04) != 0;
        }

        if (fieldMask & STATE_FIELD_TRANSPORT)
        {
            if (!r.ReadLE(s.transportGuid)) return false;
            if (!r.ReadLE(s.transOffX))     return false;
            if (!r.ReadLE(s.transOffY))     return false;
            if (!r.ReadLE(s.transOffZ))     return false;
            if (!r.ReadLE(s.transOffO))     return false;
        }

        if (fieldMask & STATE_FIELD_PET)
        {
            if (!r.ReadLE(s.petEntry))  return false;
            if (!r.ReadLE(s.petHealth)) return false;
            if (!r.ReadLE(s.petMana))   return false;
            if (!r.ReadString8(s.petName)) return false;
        }

        if (fieldMask & STATE_FIELD_AURAS)
        {
            uint8 count = 0;
            if (!r.ReadLE(count)) return false;
            s.auras.resize(count);
            for (uint8 i = 0; i < count; ++i)
            {
                if (!r.ReadLE(s.auras[i].spellId))  return false;
                if (!r.ReadLE(s.auras[i].duration)) return false;
                if (!r.ReadLE(s.auras[i].stacks))   return false;
            }
        }

        if (fieldMask & STATE_FIELD_DEATH)
        {
            uint8 dead = 0;
            if (!r.ReadLE(dead)) return false;
            s.isDead = (dead != 0);
        }

        return true;
    }

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::vector<uint8> SerializeStateDelta(uint64 guid, uint8 fieldMask, SharedPlayerState const& state)
{
    std::vector<uint8> buf;
    buf.reserve(64);

    WriteLE<uint64>(buf, guid);
    WriteLE<uint8>(buf, fieldMask);
    WriteDeltaFields(buf, fieldMask, state);

    return buf;
}

bool DeserializeStateDelta(uint8 const* buf, int len, uint64& outGuid, uint8& outMask, SharedPlayerState& outState)
{
    if (!buf || len < 9) // minimum: guid(8) + mask(1)
        return false;

    Reader r{buf, static_cast<size_t>(len), 0};

    if (!r.ReadLE(outGuid)) return false;
    if (!r.ReadLE(outMask)) return false;

    outState.guid = outGuid;

    if (!ReadDeltaFields(r, outMask, outState))
    {
        LOG_INFO("server.worldserver", "PlayerStateSync: delta deserialize failed for guid {}", outGuid);
        return false;
    }

    return true;
}

std::vector<uint8> SerializeFullState(SharedPlayerState const& state)
{
    std::vector<uint8> buf;
    buf.reserve(128);

    // Identity header (prepended before the delta portion)
    WriteLE<uint64>(buf, state.guid);
    WriteLE<uint32>(buf, state.accountId);
    WriteLE<uint8>(buf, state.ownerNodeId);
    WriteLE<uint8>(buf, state.level);
    WriteLE<uint8>(buf, state.raceId);
    WriteLE<uint8>(buf, state.classId);
    WriteLE<uint8>(buf, state.gender);
    WriteLE<uint32>(buf, state.displayId);
    WriteLE<uint8>(buf, state.active ? 1 : 0);

    // Full delta (all fields)
    uint8 fieldMask = STATE_FIELD_ALL;
    WriteLE<uint8>(buf, fieldMask);
    WriteDeltaFields(buf, fieldMask, state);

    return buf;
}

bool DeserializeFullState(uint8 const* buf, int len, SharedPlayerState& outState)
{
    if (!buf || len < 20) // identity header minimum
        return false;

    Reader r{buf, static_cast<size_t>(len), 0};

    // Identity header
    if (!r.ReadLE(outState.guid))        return false;
    if (!r.ReadLE(outState.accountId))   return false;
    if (!r.ReadLE(outState.ownerNodeId)) return false;
    if (!r.ReadLE(outState.level))       return false;
    if (!r.ReadLE(outState.raceId))      return false;
    if (!r.ReadLE(outState.classId))     return false;
    if (!r.ReadLE(outState.gender))      return false;
    if (!r.ReadLE(outState.displayId))   return false;

    uint8 activeByte = 0;
    if (!r.ReadLE(activeByte)) return false;
    outState.active = (activeByte != 0);

    // Field mask + delta fields
    uint8 fieldMask = 0;
    if (!r.ReadLE(fieldMask)) return false;

    if (!ReadDeltaFields(r, fieldMask, outState))
    {
        LOG_INFO("server.worldserver", "PlayerStateSync: full state deserialize failed for guid {}", outState.guid);
        return false;
    }

    return true;
}
