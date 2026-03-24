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

#include "PlayerTransfer.h"
#include "GameTime.h"
#include "Player.h"
#include "Pet.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "Transport.h"

#include <cstring>

// ---------------------------------------------------------------------------
// Little-endian serialization helpers
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

    void WriteString(std::vector<uint8>& buf, std::string const& s)
    {
        WriteLE<uint16>(buf, static_cast<uint16>(s.size()));
        if (!s.empty())
        {
            const size_t off = buf.size();
            buf.resize(off + s.size());
            std::memcpy(buf.data() + off, s.data(), s.size());
        }
    }

    // Reader cursor
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

        bool ReadString(std::string& out)
        {
            uint16 slen = 0;
            if (!ReadLE(slen))
                return false;
            if (!CanRead(slen))
                return false;
            out.assign(reinterpret_cast<char const*>(data + pos), slen);
            pos += slen;
            return true;
        }
    };
} // namespace

// ---------------------------------------------------------------------------
// SnapshotPlayer — capture live state into PlayerTransferData
// ---------------------------------------------------------------------------

PlayerTransferData SnapshotPlayer(Player const* player)
{
    PlayerTransferData d{};
    if (!player)
        return d;

    // identity
    d.guid      = player->GetGUID().GetRawValue();
    d.accountId = player->GetSession() ? player->GetSession()->GetAccountId() : 0;

    // position
    d.mapId       = player->GetMapId();
    d.zoneId      = player->GetZoneId();
    d.areaId      = player->GetAreaId();
    d.posX        = player->GetPositionX();
    d.posY        = player->GetPositionY();
    d.posZ        = player->GetPositionZ();
    d.orientation = player->GetOrientation();

    // vitals
    d.health   = player->GetHealth();
    d.maxHealth = player->GetMaxHealth();
    Powers ptype = player->getPowerType();
    d.powerType = static_cast<uint8>(ptype);
    d.power     = player->GetPower(ptype);
    d.maxPower  = player->GetMaxPower(ptype);
    d.level     = player->GetLevel();

    // transport
    if (Transport* transport = player->GetTransport())
    {
        d.transport.onTransport = true;
        d.transport.entry   = transport->GetEntry();
        d.transport.offsetX = player->GetTransOffsetX();
        d.transport.offsetY = player->GetTransOffsetY();
        d.transport.offsetZ = player->GetTransOffsetZ();
        d.transport.offsetO = player->GetTransOffsetO();
    }

    // pet
    if (Pet* pet = player->GetPet())
    {
        d.pet.hasPet     = true;
        d.pet.entry      = pet->GetEntry();
        d.pet.displayId  = pet->GetDisplayId();
        d.pet.level      = pet->GetLevel();
        d.pet.health     = pet->GetHealth();
        d.pet.mana       = pet->GetPower(POWER_MANA);
        d.pet.happiness  = pet->GetPower(POWER_HAPPINESS);
        d.pet.reactState = static_cast<uint8>(pet->GetReactState());
        d.pet.name       = pet->GetName();
    }

    // auras — only active, non-passive, visible auras
    for (auto const& pair : player->GetAppliedAuras())
    {
        Aura* aura = pair.second->GetBase();
        if (!aura)
            continue;
        if (aura->IsPassive())
            continue;

        SpellInfo const* info = aura->GetSpellInfo();
        if (!info)
            continue;
        if (info->HasAttribute(SPELL_ATTR0_DO_NOT_DISPLAY))
            continue;

        TransferAuraInfo ai;
        ai.spellId     = aura->GetId();
        ai.duration    = aura->GetDuration();
        ai.maxDuration = aura->GetMaxDuration();
        ai.stackAmount = aura->GetStackAmount();
        ai.casterGuid  = aura->GetCasterGUID().GetRawValue();
        d.auras.push_back(ai);
    }

    // timestamp
    d.timestamp = static_cast<uint64>(GameTime::GetGameTime().count());

    return d;
}

// ---------------------------------------------------------------------------
// SerializeTransfer — PlayerTransferData → binary buffer
// ---------------------------------------------------------------------------

std::vector<uint8> SerializeTransfer(PlayerTransferData const& d)
{
    std::vector<uint8> buf;
    buf.reserve(256); // typical size hint

    // header: magic (2 bytes) + version (1 byte)
    WriteLE<uint16>(buf, PLAYER_TRANSFER_MAGIC);
    WriteLE<uint8>(buf, PLAYER_TRANSFER_VERSION);

    // identity
    WriteLE<uint64>(buf, d.guid);
    WriteLE<uint32>(buf, d.accountId);

    // position
    WriteLE<uint32>(buf, d.mapId);
    WriteLE<uint32>(buf, d.zoneId);
    WriteLE<uint32>(buf, d.areaId);
    WriteLE<float>(buf, d.posX);
    WriteLE<float>(buf, d.posY);
    WriteLE<float>(buf, d.posZ);
    WriteLE<float>(buf, d.orientation);

    // vitals
    WriteLE<uint32>(buf, d.health);
    WriteLE<uint32>(buf, d.maxHealth);
    WriteLE<uint8>(buf, d.powerType);
    WriteLE<uint32>(buf, d.power);
    WriteLE<uint32>(buf, d.maxPower);
    WriteLE<uint32>(buf, d.level);

    // transport
    WriteLE<uint8>(buf, d.transport.onTransport ? 1 : 0);
    if (d.transport.onTransport)
    {
        WriteLE<uint32>(buf, d.transport.entry);
        WriteLE<float>(buf, d.transport.offsetX);
        WriteLE<float>(buf, d.transport.offsetY);
        WriteLE<float>(buf, d.transport.offsetZ);
        WriteLE<float>(buf, d.transport.offsetO);
    }

    // pet
    WriteLE<uint8>(buf, d.pet.hasPet ? 1 : 0);
    if (d.pet.hasPet)
    {
        WriteLE<uint32>(buf, d.pet.entry);
        WriteLE<uint32>(buf, d.pet.displayId);
        WriteLE<uint32>(buf, d.pet.level);
        WriteLE<uint32>(buf, d.pet.health);
        WriteLE<uint32>(buf, d.pet.mana);
        WriteLE<uint32>(buf, d.pet.happiness);
        WriteLE<uint8>(buf, d.pet.reactState);
        WriteString(buf, d.pet.name);
    }

    // auras
    WriteLE<uint32>(buf, static_cast<uint32>(d.auras.size()));
    for (auto const& a : d.auras)
    {
        WriteLE<uint32>(buf, a.spellId);
        WriteLE<int32>(buf, a.duration);
        WriteLE<int32>(buf, a.maxDuration);
        WriteLE<uint8>(buf, a.stackAmount);
        WriteLE<uint64>(buf, a.casterGuid);
    }

    // timestamp
    WriteLE<uint64>(buf, d.timestamp);

    return buf;
}

// ---------------------------------------------------------------------------
// DeserializeTransfer — binary buffer → PlayerTransferData
// ---------------------------------------------------------------------------

bool DeserializeTransfer(uint8 const* data, size_t len, PlayerTransferData& d)
{
    Reader r{data, len};

    // header
    uint16 magic = 0;
    uint8  version = 0;
    if (!r.ReadLE(magic) || magic != PLAYER_TRANSFER_MAGIC)
        return false;
    if (!r.ReadLE(version) || version != PLAYER_TRANSFER_VERSION)
        return false;

    // identity
    if (!r.ReadLE(d.guid))      return false;
    if (!r.ReadLE(d.accountId)) return false;

    // position
    if (!r.ReadLE(d.mapId))       return false;
    if (!r.ReadLE(d.zoneId))      return false;
    if (!r.ReadLE(d.areaId))      return false;
    if (!r.ReadLE(d.posX))        return false;
    if (!r.ReadLE(d.posY))        return false;
    if (!r.ReadLE(d.posZ))        return false;
    if (!r.ReadLE(d.orientation)) return false;

    // vitals
    if (!r.ReadLE(d.health))    return false;
    if (!r.ReadLE(d.maxHealth)) return false;
    if (!r.ReadLE(d.powerType)) return false;
    if (!r.ReadLE(d.power))     return false;
    if (!r.ReadLE(d.maxPower))  return false;
    if (!r.ReadLE(d.level))     return false;

    // transport
    uint8 hasTransport = 0;
    if (!r.ReadLE(hasTransport)) return false;
    d.transport.onTransport = (hasTransport != 0);
    if (d.transport.onTransport)
    {
        if (!r.ReadLE(d.transport.entry))   return false;
        if (!r.ReadLE(d.transport.offsetX)) return false;
        if (!r.ReadLE(d.transport.offsetY)) return false;
        if (!r.ReadLE(d.transport.offsetZ)) return false;
        if (!r.ReadLE(d.transport.offsetO)) return false;
    }

    // pet
    uint8 hasPet = 0;
    if (!r.ReadLE(hasPet)) return false;
    d.pet.hasPet = (hasPet != 0);
    if (d.pet.hasPet)
    {
        if (!r.ReadLE(d.pet.entry))      return false;
        if (!r.ReadLE(d.pet.displayId))  return false;
        if (!r.ReadLE(d.pet.level))      return false;
        if (!r.ReadLE(d.pet.health))     return false;
        if (!r.ReadLE(d.pet.mana))       return false;
        if (!r.ReadLE(d.pet.happiness))  return false;
        if (!r.ReadLE(d.pet.reactState)) return false;
        if (!r.ReadString(d.pet.name))   return false;
    }

    // auras
    uint32 auraCount = 0;
    if (!r.ReadLE(auraCount)) return false;
    // sanity cap — no player should have more than 255 transferable auras
    if (auraCount > 255)
        return false;
    d.auras.resize(auraCount);
    for (uint32 i = 0; i < auraCount; ++i)
    {
        if (!r.ReadLE(d.auras[i].spellId))     return false;
        if (!r.ReadLE(d.auras[i].duration))    return false;
        if (!r.ReadLE(d.auras[i].maxDuration)) return false;
        if (!r.ReadLE(d.auras[i].stackAmount)) return false;
        if (!r.ReadLE(d.auras[i].casterGuid))  return false;
    }

    // timestamp
    if (!r.ReadLE(d.timestamp)) return false;

    return true;
}
