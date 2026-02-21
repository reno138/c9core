/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef ClusterMgr_h__
#define ClusterMgr_h__

#include "Define.h"
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Cached information about a player on a different worldserver node.
 *
 * Populated by push events from the proxy (MSG_CLUSTER_PLAYER_ONLINE) and
 * cleared by MSG_CLUSTER_PLAYER_OFFLINE.  Used for:
 *  - Cross-node name resolution (whispers, group invites)
 *  - /who query aggregation
 *  - Friend status lookups
 */
struct ClusterPlayerInfo
{
    uint64      guid{ 0 };
    std::string name;        ///< lowercase, original case unavailable cross-node
    uint32      zoneId{ 0 };
    uint8       level{ 0 };
    uint8       classId{ 0 };
    uint8       raceId{ 0 };
    uint8       teamId{ 0 }; ///< 0 = Alliance, 1 = Horde
    uint8       nodeId{ 0 }; ///< Proxy-assigned cluster node ID (1–5)
};

/**
 * @brief Worldserver-side cache of players on other cluster nodes.
 *
 * All mutating methods are called from ProxyClient (io_context thread).
 * Read methods (Find*) may be called from the game update loop — protected
 * by a shared_mutex for concurrent reads.
 */
class ClusterMgr
{
public:
    static ClusterMgr& Instance()
    {
        static ClusterMgr instance;
        return instance;
    }

    // ── Cache updates (called by ProxyClient on io_context thread) ────────────

    /// A player on another node just logged in.
    void OnRemotePlayerOnline(uint64 guid, std::string name,
                              uint32 zoneId, uint8 level, uint8 classId, uint8 raceId, uint8 teamId, uint8 nodeId);

    /// A player on another node just logged out.
    void OnRemotePlayerOffline(uint64 guid);

    // ── Lookups (called from game update loop) ────────────────────────────────

    /// Find a remote player by lowercase name.  Returns nullptr if not found.
    ClusterPlayerInfo const* FindRemotePlayer(std::string const& lowerName) const;

    /// Find a remote player by GUID.  Returns nullptr if not found.
    ClusterPlayerInfo const* FindRemotePlayerByGuid(uint64 guid) const;

    /// Copy all remote players (for /who aggregation).
    std::vector<ClusterPlayerInfo> GetAllRemotePlayers() const;

    /// True if any remote players are tracked (cluster is active).
    bool HasRemotePlayers() const;

    // ── Local map set (zone-based routing) ───────────────────────────────────

    /// Parse ClusterServer.Maps / ClusterServer.InstanceServer from config.
    ///   ClusterServer.InstanceServer = 1  -> handle all instanceable maps (dungeons/raids/BGs/arenas)
    ///   ClusterServer.Maps = "-1"         -> handle all maps (no rerouting)
    ///   ClusterServer.Maps = "0,530,571"  -> explicit comma-separated list
    ///   ClusterServer.Maps = ""           -> all maps local (standalone/dev mode)
    void LoadLocalMaps();

    /// Returns true if mapId is handled locally by this node.
    bool IsMapLocal(uint32 mapId) const;
    std::unordered_set<uint32> GetLocalMaps() const;

    // ── Cross-node group invite state ─────────────────────────────────────────

    struct CrossNodeInvite
    {
        uint64      inviterGuid{ 0 };
        std::string inviterName;
    };

    struct CrossNodeGroupMember
    {
        uint64 guid;
        uint8  subgroup;
        uint8  roleFlags;
        uint8  nodeId;
    };

    struct CrossNodeInviteResult
    {
        uint64 inviteeGuid;
        uint8  result;   ///< 0 = declined, 1 = accepted
    };

    /// Store a pending cross-node group invite (called from io_context thread).
    void SetPendingCrossNodeInvite(uint64 inviteeGuid, uint64 inviterGuid, std::string const& inviterName);

    /// Retrieve and clear a pending invite for an invitee (called from game thread).
    bool GetAndClearPendingCrossNodeInvite(uint64 inviteeGuid, CrossNodeInvite& out);

    /// Queue an invite result for processing on the game update thread.
    void QueueCrossNodeInviteResult(uint64 inviteeGuid, uint8 result);

    /// Drain invite result queue (called from World update loop).
    std::vector<CrossNodeInviteResult> DrainInviteResults();

    /// Update group membership state from a GROUP_UPDATE broadcast.
    void OnGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> const& memberData);

    /// Remove group state on disband.
    void OnGroupDisband(uint64 groupGuid);

    /// Get cross-node members of a group (for chat routing).
    std::vector<CrossNodeGroupMember> GetGroupRemoteMembers(uint64 groupGuid) const;

    // ── Cross-node unit state (health/power/auras for party frames) ───────────

    struct ClusterUnitState
    {
        uint64 guid{ 0 };
        uint16 status{ 0 };     ///< MEMBER_STATUS_* flags
        uint32 health{ 0 };
        uint32 maxHealth{ 0 };
        uint8  powerType{ 0 };
        uint16 power{ 0 };
        uint16 maxPower{ 0 };
        uint16 level{ 0 };
        uint16 zoneId{ 0 };
        uint64 auraMask{ 0 };

        struct AuraEntry
        {
            uint32 spellId{ 0 };
            uint8  flags{ 0 };
        };
        std::vector<AuraEntry> auras; ///< one entry per set bit in auraMask
    };

    /// Update (or insert) the cached unit state for a remote player.
    void UpdateUnitState(ClusterUnitState state);

    /// Retrieve cached unit state.  Returns false if guid not found.
    bool GetUnitState(uint64 guid, ClusterUnitState& out) const;

private:
    ClusterMgr() = default;

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64, ClusterPlayerInfo> _byGuid;
    std::unordered_map<std::string, uint64>       _byName; ///< lowercase name → guid

    // ── Local map set ──────────────────────────────────────────────────────
    mutable std::mutex _localMapsMutex;
    std::unordered_set<uint32> _localMaps; ///< non-empty explicit map list
    bool _instanceServerMode{false}; ///< true = handle all instanceable maps
    bool _allMapsMode{false};        ///< true = handle all maps (no rerouting)

    // ── Cross-node group state ─────────────────────────────────────────────
    mutable std::mutex _groupMutex;
    std::unordered_map<uint64, std::vector<CrossNodeGroupMember>> _groupMembers; ///< group_guid → members

    // ── Cross-node unit states ────────────────────────────────────────────
    mutable std::mutex _unitStateMutex;
    std::unordered_map<uint64, ClusterUnitState> _unitStates;

    // ── Pending invites & results (io_context → game thread) ──────────────
    std::mutex _inviteMutex;
    std::unordered_map<uint64, CrossNodeInvite>   _pendingInvites;   ///< invitee_guid → invite
    std::vector<CrossNodeInviteResult>             _inviteResults;    ///< queued results
};

#define sClusterMgr ClusterMgr::Instance()

#endif // ClusterMgr_h__
