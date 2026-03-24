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

#ifndef SharedPlayerCache_h__
#define SharedPlayerCache_h__

#include "SharedPlayerState.h"
#include <functional>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

/// Thread-safe singleton cache of all online player state across the cluster.
/// Game thread reads (Get, GetAll, GetByMap) use shared locks.
/// NATS callback thread writes (StoreFullState, ApplyDelta, Remove) use exclusive locks.
class SharedPlayerCache
{
public:
    static SharedPlayerCache& Instance();

    /// Insert or replace a full player state snapshot.
    void StoreFullState(SharedPlayerState&& state);

    /// Update only the fields indicated by fieldMask from delta.
    void ApplyDelta(uint64 guid, uint8 fieldMask, SharedPlayerState const& delta);

    /// Return a snapshot copy of the cached state for a player.
    std::optional<SharedPlayerState> Get(uint64 guid) const;

    /// Remove a player from the cache (logged off).
    void Remove(uint64 guid);

    /// Return snapshot copies of all cached player states (for /who).
    std::vector<SharedPlayerState> GetAll() const;

    /// Return snapshot copies of players on a specific map (for cross-node visibility).
    std::vector<SharedPlayerState> GetByMap(uint32 mapId) const;

    /// Apply a lambda updater to a cached player state under exclusive lock.
    void UpdateFromPlayer(uint64 guid, std::function<void(SharedPlayerState&)> updater);

    /// Check if a player exists in the cache.
    bool Has(uint64 guid) const;

    /// Return number of cached player states.
    size_t Size() const;

private:
    SharedPlayerCache() = default;
    ~SharedPlayerCache() = default;
    SharedPlayerCache(SharedPlayerCache const&) = delete;
    SharedPlayerCache& operator=(SharedPlayerCache const&) = delete;

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64, SharedPlayerState> _cache;
};

#define sSharedPlayerCache SharedPlayerCache::Instance()

#endif // SharedPlayerCache_h__
