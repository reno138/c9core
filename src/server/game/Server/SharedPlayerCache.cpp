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

#include "SharedPlayerCache.h"
#include "Log.h"

SharedPlayerCache& SharedPlayerCache::Instance()
{
    static SharedPlayerCache instance;
    return instance;
}

void SharedPlayerCache::StoreFullState(SharedPlayerState&& state)
{
    uint64 guid = state.guid;
    std::unique_lock lock(_mutex);
    _cache[guid] = std::move(state);
}

void SharedPlayerCache::ApplyDelta(uint64 guid, uint8 fieldMask, SharedPlayerState const& delta)
{
    std::unique_lock lock(_mutex);
    auto it = _cache.find(guid);
    if (it == _cache.end())
        return;

    auto& s = it->second;

    if (fieldMask & STATE_FIELD_POSITION)
    {
        s.mapId  = delta.mapId;
        s.zoneId = delta.zoneId;
        s.areaId = delta.areaId;
        s.posX   = delta.posX;
        s.posY   = delta.posY;
        s.posZ   = delta.posZ;
        s.posO   = delta.posO;
    }

    if (fieldMask & STATE_FIELD_HEALTH)
    {
        s.health    = delta.health;
        s.maxHealth = delta.maxHealth;
    }

    if (fieldMask & STATE_FIELD_POWER)
    {
        s.powerType = delta.powerType;
        s.power     = delta.power;
        s.maxPower  = delta.maxPower;
    }

    if (fieldMask & STATE_FIELD_COMBAT)
    {
        s.inCombat = delta.inCombat;
        s.inFlight = delta.inFlight;
    }

    if (fieldMask & STATE_FIELD_TRANSPORT)
    {
        s.transportGuid  = delta.transportGuid;
        s.transportEntry = delta.transportEntry;
        s.transOffX      = delta.transOffX;
        s.transOffY      = delta.transOffY;
        s.transOffZ      = delta.transOffZ;
        s.transOffO      = delta.transOffO;
    }

    if (fieldMask & STATE_FIELD_PET)
    {
        s.petEntry  = delta.petEntry;
        s.petHealth = delta.petHealth;
        s.petMana   = delta.petMana;
        s.petName   = delta.petName;
    }

    if (fieldMask & STATE_FIELD_AURAS)
    {
        s.auras = delta.auras;
    }

    if (fieldMask & STATE_FIELD_DEATH)
    {
        s.isDead = delta.isDead;
    }

    s.lastUpdateMs = delta.lastUpdateMs;
}

std::optional<SharedPlayerState> SharedPlayerCache::Get(uint64 guid) const
{
    std::shared_lock lock(_mutex);
    auto it = _cache.find(guid);
    if (it == _cache.end())
        return std::nullopt;
    return it->second;
}

void SharedPlayerCache::Remove(uint64 guid)
{
    std::unique_lock lock(_mutex);
    _cache.erase(guid);
}

std::vector<SharedPlayerState> SharedPlayerCache::GetAll() const
{
    std::shared_lock lock(_mutex);
    std::vector<SharedPlayerState> result;
    result.reserve(_cache.size());
    for (auto const& [_, state] : _cache)
        result.push_back(state);
    return result;
}

std::vector<SharedPlayerState> SharedPlayerCache::GetByMap(uint32 mapId) const
{
    std::shared_lock lock(_mutex);
    std::vector<SharedPlayerState> result;
    for (auto const& [_, state] : _cache)
    {
        if (state.mapId == mapId)
            result.push_back(state);
    }
    return result;
}

void SharedPlayerCache::UpdateFromPlayer(uint64 guid, std::function<void(SharedPlayerState&)> updater)
{
    std::unique_lock lock(_mutex);
    auto it = _cache.find(guid);
    if (it == _cache.end())
        return;
    updater(it->second);
}

bool SharedPlayerCache::Has(uint64 guid) const
{
    std::shared_lock lock(_mutex);
    return _cache.count(guid) > 0;
}

size_t SharedPlayerCache::Size() const
{
    std::shared_lock lock(_mutex);
    return _cache.size();
}
