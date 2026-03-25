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
#include "Item.h"
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
// SnapshotPlayerFull — capture FULL state (v2) into PlayerTransferData
// ---------------------------------------------------------------------------

PlayerTransferData SnapshotPlayerFull(Player const* player)
{
    PlayerTransferData d = SnapshotPlayer(player);
    if (!player)
        return d;

    d.activeSpec = player->GetActiveSpec();

    // Equipment (19 slots)
    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (Item* item = const_cast<Player*>(player)->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            TransferEquipItem ei;
            ei.slot = slot;
            ei.entry = item->GetEntry();
            ei.randomProp = item->GetItemRandomPropertyId();
            ei.enchants[0] = item->GetEnchantmentId(PERM_ENCHANTMENT_SLOT);
            ei.enchants[1] = item->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT);
            ei.enchants[2] = item->GetEnchantmentId(SOCK_ENCHANTMENT_SLOT);
            d.equipment.push_back(ei);
        }
    }

    // Known spells
    for (auto const& [spellId, spell] : player->GetSpellMap())
    {
        if (spell->State == PLAYERSPELL_REMOVED)
            continue;
        TransferSpellInfo si;
        si.spellId = spellId;
        si.active = spell->Active;
        si.specMask = spell->specMask;
        d.spells.push_back(si);
    }

    // Talents
    for (auto const& [talentId, talent] : player->GetTalentMap())
    {
        if (talent->State == PLAYERSPELL_REMOVED)
            continue;
        TransferTalentInfo ti;
        ti.talentId = talent->talentID;
        ti.spellId = talentId;
        ti.specMask = talent->specMask;
        d.talents.push_back(ti);
    }

    // Action buttons
    for (auto const& [button, ab] : player->GetActionButtons())
    {
        if (ab.uState == ACTIONBUTTON_DELETED)
            continue;
        TransferActionButton tb;
        tb.button = button;
        tb.action = ab.GetAction();
        tb.type = static_cast<uint8>(ab.GetType());
        d.actionButtons.push_back(tb);
    }

    // Skills
    for (auto const& [skillId, status] : player->GetSkillStatusMap())
    {
        if (status.uState == SKILL_DELETED)
            continue;
        TransferSkillInfo si;
        si.skillId = static_cast<uint16>(skillId);
        uint16 field = static_cast<uint16>(status.pos);
        si.value    = player->GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(field)) & 0xFFFF;
        si.maxValue = (player->GetUInt32Value(PLAYER_SKILL_VALUE_INDEX(field)) >> 16) & 0xFFFF;
        si.bonusTemp = player->GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(field)) & 0xFFFF;
        si.bonusPerm = (player->GetUInt32Value(PLAYER_SKILL_BONUS_INDEX(field)) >> 16) & 0xFFFF;
        d.skills.push_back(si);
    }

    // Quest log (active quests)
    for (auto const& [questId, qStatus] : player->getQuestStatusMap())
    {
        if (qStatus.Status == QUEST_STATUS_NONE)
            continue;
        TransferQuestInfo qi;
        qi.questId = questId;
        qi.status = static_cast<uint8>(qStatus.Status);
        qi.explored = qStatus.Explored;
        qi.timer = qStatus.Timer;
        for (int i = 0; i < 4; ++i)
            qi.creatureOrGOCount[i] = qStatus.CreatureOrGOCount[i];
        for (int i = 0; i < 6; ++i)
            qi.itemCount[i] = qStatus.ItemCount[i];
        qi.playerCount = qStatus.PlayerCount;
        d.quests.push_back(qi);
    }

    // Cooldowns
    uint32 now = GameTime::GetGameTimeMS().count();
    for (auto const& [spellId, cd] : player->GetSpellCooldownMap())
    {
        if (cd.end <= now)
            continue;
        TransferCooldownInfo ci;
        ci.spellId = spellId;
        ci.endTimeMs = cd.end - now;
        ci.categoryId = cd.category;
        ci.itemId = cd.itemid;
        d.cooldowns.push_back(ci);
    }

    // Rewarded quests
    for (uint32 qid : player->getRewardedQuests())
        d.rewardedQuests.push_back(qid);

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

    // --- v2 extended fields ---
    WriteLE<uint8>(buf, d.activeSpec);

    WriteLE<uint16>(buf, static_cast<uint16>(d.equipment.size()));
    for (auto const& e : d.equipment)
    {
        WriteLE<uint8>(buf, e.slot);
        WriteLE<uint32>(buf, e.entry);
        WriteLE<int32>(buf, e.randomProp);
        WriteLE<uint32>(buf, e.enchants[0]);
        WriteLE<uint32>(buf, e.enchants[1]);
        WriteLE<uint32>(buf, e.enchants[2]);
    }

    WriteLE<uint32>(buf, static_cast<uint32>(d.spells.size()));
    for (auto const& s : d.spells)
    {
        WriteLE<uint32>(buf, s.spellId);
        WriteLE<uint8>(buf, s.active ? 1 : 0);
        WriteLE<uint8>(buf, s.specMask);
    }

    WriteLE<uint16>(buf, static_cast<uint16>(d.talents.size()));
    for (auto const& t : d.talents)
    {
        WriteLE<uint32>(buf, t.talentId);
        WriteLE<uint32>(buf, t.spellId);
        WriteLE<uint8>(buf, t.specMask);
    }

    WriteLE<uint16>(buf, static_cast<uint16>(d.actionButtons.size()));
    for (auto const& ab : d.actionButtons)
    {
        WriteLE<uint8>(buf, ab.button);
        WriteLE<uint32>(buf, ab.action);
        WriteLE<uint8>(buf, ab.type);
    }

    WriteLE<uint16>(buf, static_cast<uint16>(d.skills.size()));
    for (auto const& sk : d.skills)
    {
        WriteLE<uint16>(buf, sk.skillId);
        WriteLE<uint16>(buf, sk.value);
        WriteLE<uint16>(buf, sk.maxValue);
        WriteLE<uint16>(buf, sk.bonusTemp);
        WriteLE<uint16>(buf, sk.bonusPerm);
    }

    WriteLE<uint16>(buf, static_cast<uint16>(d.quests.size()));
    for (auto const& q : d.quests)
    {
        WriteLE<uint32>(buf, q.questId);
        WriteLE<uint8>(buf, q.status);
        WriteLE<uint8>(buf, q.explored ? 1 : 0);
        WriteLE<uint32>(buf, q.timer);
        for (int i = 0; i < 4; ++i) WriteLE<uint16>(buf, q.creatureOrGOCount[i]);
        for (int i = 0; i < 6; ++i) WriteLE<uint16>(buf, q.itemCount[i]);
        WriteLE<uint16>(buf, q.playerCount);
    }

    WriteLE<uint16>(buf, static_cast<uint16>(d.cooldowns.size()));
    for (auto const& cd : d.cooldowns)
    {
        WriteLE<uint32>(buf, cd.spellId);
        WriteLE<uint32>(buf, cd.endTimeMs);
        WriteLE<uint16>(buf, cd.categoryId);
        WriteLE<uint32>(buf, cd.itemId);
    }

    WriteLE<uint32>(buf, static_cast<uint32>(d.rewardedQuests.size()));
    for (uint32 qid : d.rewardedQuests)
        WriteLE<uint32>(buf, qid);

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
    if (!r.ReadLE(version) || version < 1 || version > PLAYER_TRANSFER_VERSION)
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

    // --- v2 extended fields (optional — v1 payloads end here) ---
    if (version >= 2 && r.CanRead(1))
    {
        if (!r.ReadLE(d.activeSpec)) return false;

        uint16 equipCount = 0;
        if (!r.ReadLE(equipCount)) return false;
        d.equipment.resize(equipCount);
        for (uint16 i = 0; i < equipCount; ++i)
        {
            auto& e = d.equipment[i];
            if (!r.ReadLE(e.slot))       return false;
            if (!r.ReadLE(e.entry))      return false;
            if (!r.ReadLE(e.randomProp)) return false;
            if (!r.ReadLE(e.enchants[0])) return false;
            if (!r.ReadLE(e.enchants[1])) return false;
            if (!r.ReadLE(e.enchants[2])) return false;
        }

        uint32 spellCount = 0;
        if (!r.ReadLE(spellCount)) return false;
        if (spellCount > 10000) return false;
        d.spells.resize(spellCount);
        for (uint32 i = 0; i < spellCount; ++i)
        {
            auto& s = d.spells[i];
            if (!r.ReadLE(s.spellId)) return false;
            uint8 activeByte = 0;
            if (!r.ReadLE(activeByte)) return false;
            s.active = (activeByte != 0);
            if (!r.ReadLE(s.specMask)) return false;
        }

        uint16 talentCount = 0;
        if (!r.ReadLE(talentCount)) return false;
        d.talents.resize(talentCount);
        for (uint16 i = 0; i < talentCount; ++i)
        {
            auto& t = d.talents[i];
            if (!r.ReadLE(t.talentId)) return false;
            if (!r.ReadLE(t.spellId))  return false;
            if (!r.ReadLE(t.specMask)) return false;
        }

        uint16 abCount = 0;
        if (!r.ReadLE(abCount)) return false;
        d.actionButtons.resize(abCount);
        for (uint16 i = 0; i < abCount; ++i)
        {
            auto& ab = d.actionButtons[i];
            if (!r.ReadLE(ab.button)) return false;
            if (!r.ReadLE(ab.action)) return false;
            if (!r.ReadLE(ab.type))   return false;
        }

        uint16 skillCount = 0;
        if (!r.ReadLE(skillCount)) return false;
        d.skills.resize(skillCount);
        for (uint16 i = 0; i < skillCount; ++i)
        {
            auto& sk = d.skills[i];
            if (!r.ReadLE(sk.skillId))   return false;
            if (!r.ReadLE(sk.value))     return false;
            if (!r.ReadLE(sk.maxValue))  return false;
            if (!r.ReadLE(sk.bonusTemp)) return false;
            if (!r.ReadLE(sk.bonusPerm)) return false;
        }

        uint16 questCount = 0;
        if (!r.ReadLE(questCount)) return false;
        d.quests.resize(questCount);
        for (uint16 i = 0; i < questCount; ++i)
        {
            auto& q = d.quests[i];
            if (!r.ReadLE(q.questId)) return false;
            if (!r.ReadLE(q.status))  return false;
            uint8 exploredByte = 0;
            if (!r.ReadLE(exploredByte)) return false;
            q.explored = (exploredByte != 0);
            if (!r.ReadLE(q.timer)) return false;
            for (int j = 0; j < 4; ++j)
                if (!r.ReadLE(q.creatureOrGOCount[j])) return false;
            for (int j = 0; j < 6; ++j)
                if (!r.ReadLE(q.itemCount[j])) return false;
            if (!r.ReadLE(q.playerCount)) return false;
        }

        uint16 cdCount = 0;
        if (!r.ReadLE(cdCount)) return false;
        d.cooldowns.resize(cdCount);
        for (uint16 i = 0; i < cdCount; ++i)
        {
            auto& cd = d.cooldowns[i];
            if (!r.ReadLE(cd.spellId))    return false;
            if (!r.ReadLE(cd.endTimeMs))  return false;
            if (!r.ReadLE(cd.categoryId)) return false;
            if (!r.ReadLE(cd.itemId))     return false;
        }

        uint32 rwdCount = 0;
        if (!r.ReadLE(rwdCount)) return false;
        if (rwdCount > 10000) return false;
        d.rewardedQuests.resize(rwdCount);
        for (uint32 i = 0; i < rwdCount; ++i)
            if (!r.ReadLE(d.rewardedQuests[i])) return false;
    }

    return true;
}
