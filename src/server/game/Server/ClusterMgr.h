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

#ifndef ClusterMgr_h__
#define ClusterMgr_h__

#include "Define.h"
#include "PlayerTransfer.h"
#include <atomic>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Network identity and map ownership of a peer cluster node.
 *
 * Populated from cluster.announce broadcasts on NATS startup.
 * Used by ProxyClient to route targeted messages directly to the correct
 * node without involving the proxy, and by ProxyClient::SendReroute
 * to obtain the destination IP:port for cross-node player redirection.
 */
struct ClusterNodeInfo
{
    uint8       nodeId{ 0 };
    std::string address;            ///< LAN IP (e.g. "192.0.2.71")
    uint16      port{ 0 };          ///< WoW game port (typically 8086)
    std::string externalAddress;    ///< Public/NAT IP for external clients (empty = no NAT)
    uint16      externalPort{ 0 };  ///< Public port for external clients (0 = same as port)
    uint8       type{ 0 };          ///< 0 = regular worldserver, 1 = instance server
    std::unordered_set<uint32> maps; ///< mapIds served by this node
    std::unordered_set<uint32> zones; ///< zoneIds served (empty = all zones on owned maps)
    uint32      lastSeenMs{ 0 };    ///< getMSTime() when last MSG_NODE_STATUS received; 0 = never seen
    uint32      playerCount{ 0 };   ///< player count from most recent MSG_NODE_STATUS
    bool        dead{ false };      ///< true after dead-node detection; cleared on re-announce
};

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
    bool IsEnabled() const;

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

    // ── Peer node routing table ───────────────────────────────────────────────

    /// Register or update a peer node (called from ProxyClient on NATS announce).
    /// @return true if this was a revival of a previously-dead node.
    bool RegisterRemoteNode(ClusterNodeInfo info);

    /// Get routing info for a specific node by ID.
    std::optional<ClusterNodeInfo> GetNodeInfo(uint8 nodeId) const;

    /// Get routing info for whichever node owns the given mapId.
    /// Returns nullopt if no peer has claimed that map (or it's local).
    std::optional<ClusterNodeInfo> GetNodeForMap(uint32 mapId) const;

    /// Return the nodeId of the registered instance server (type == 1), or 0 if none.
    uint8 GetInstanceNodeId() const;

    /// Update a peer node's heartbeat timestamp and player count (from MSG_NODE_STATUS).
    /// @return true if the node was previously dead and this heartbeat revived it.
    bool UpdateNodeStatus(uint8 nodeId, uint32 playerCount, uint32 nowMs);

    /// Return nodeIds of peers whose lastSeenMs is older than deadThresholdMs ago.
    /// Only returns nodes that have sent at least one heartbeat and are not already dead.
    std::vector<uint8> GetStaleNodeIds(uint32 deadThresholdMs, uint32 nowMs) const;

    /// Mark a node dead: set dead=true and remove its maps from the routing table.
    /// @return the set of mapIds that were orphaned (for logging).
    std::unordered_set<uint32> MarkNodeDead(uint8 nodeId);

    /// Return the lowest nodeId among alive (non-dead) non-instance peer nodes, or 0 if none.
    uint8 GetLowestAliveNonInstanceNodeId() const;

    // ── Local map set (zone-based routing) ───────────────────────────────────

    /// Parse ClusterServer.Maps / ClusterServer.InstanceServer from config.
    ///   ClusterServer.InstanceServer = 1  -> handle all instanceable maps (dungeons/raids/BGs/arenas)
    ///   ClusterServer.Maps = "-1"         -> handle all maps (no rerouting)
    ///   ClusterServer.Maps = "0,530,571"  -> explicit comma-separated list
    ///   ClusterServer.Maps = ""           -> all maps local (standalone/dev mode)
    void LoadLocalMaps();

    /// Returns true if mapId is handled locally by this node.
    bool IsMapLocal(uint32 mapId) const;

    /// Returns true if zoneId is handled locally (or zone-based routing is disabled).
    bool IsZoneLocal(uint32 zoneId) const;


    // Player ownership: GUID -> nodeId of current owner
    void SetPlayerOwner(uint64 guid, uint8 nodeId);
    uint8 GetPlayerOwner(uint64 guid) const;
    bool IsPlayerOwnedLocally(uint64 guid) const;

    /// Get the node that owns a zone. Returns nullopt if local or no zone routing.
    std::optional<ClusterNodeInfo> GetNodeForZone(uint32 zoneId) const;
    bool IsInstanceServerMode() const { return _instanceServerMode; }
    std::unordered_set<uint32> GetLocalMaps() const;
    std::unordered_set<uint32> GetLocalZones() const;

    /// Dynamically add a map to this node (e.g. failover from dead node).
    /// Triggers a re-announce so the proxy updates its routing table.
    void AddLocalMap(uint32 mapId);

    /// Dynamically remove a map from this node.
    /// Players on this map should be rerouted before calling this.
    void RemoveLocalMap(uint32 mapId);

    /// Claim all maps from a dead node (failover).
    void ClaimOrphanedMaps(uint8 deadNodeId);

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

    // ── Pending player transfers (NATS callback → login handler) ─────────────

    /// Store a player transfer snapshot received from another node.
    void StorePendingTransfer(uint64 guid, PlayerTransferData&& data);

    /// Retrieve and remove a pending transfer (one-shot consumption).
    std::optional<PlayerTransferData> TakePendingTransfer(uint64 guid);

    // ── Client redirect (proxy-less transfers) ──────────────────────────────

    /// Pending redirect token from a source node, validated when client reconnects.
    struct PendingRedirect
    {
        uint32 accountId{ 0 };
        uint32 token{ 0 };
        uint64 playerGuid{ 0 };
        uint8  sourceNodeId{ 0 };
        uint32 timestampMs{ 0 };   ///< getMSTime() for expiry (30s)
    };

    void StorePendingRedirect(uint32 accountId, PendingRedirect&& redirect);
    std::optional<PendingRedirect> TakePendingRedirect(uint32 accountId);

    /// NAT-aware address resolution for SMSG_REDIRECT_CLIENT.
    /// Returns the correct (ip, port) pair based on whether the client is local or external.
    std::pair<std::string, uint16> GetRedirectAddressForNode(uint8 destNodeId, std::string const& clientIp) const;

    /// This node's config
    std::string GetGameAddress() const;
    uint16 GetGamePort() const;
    std::string GetExternalAddress() const;
    uint16 GetExternalPort() const;
    uint8 GetNodeId() const;

    /// @return true if verbose packet + state trace logging is currently
    ///         enabled via the ClusterServer.PacketTrace.Enable config option.
    /// Hot-path-safe: single atomic load + branch. Used by the PT_* macros
    /// in PacketTrace.h to gate every trace point.
    bool IsPacketTraceEnabled() const { return _packetTraceEnabled.load(std::memory_order_relaxed); }

    /// Refresh the cached PacketTrace flag from the live config. Called at
    /// init time (from LoadLocalMaps) and safe to call again on config reload.
    void RefreshPacketTraceConfig();

private:
    ClusterMgr() = default;

    mutable std::shared_mutex _mutex;
    std::unordered_map<uint64, ClusterPlayerInfo> _byGuid;
    std::unordered_map<std::string, uint64>       _byName; ///< lowercase name → guid

    // ── Peer node routing table ────────────────────────────────────────────
    mutable std::mutex _nodeMutex;
    std::unordered_map<uint8, ClusterNodeInfo> _nodes;      ///< nodeId → info
    std::unordered_map<uint32, uint8>          _mapToNode;  ///< mapId → nodeId

    // ── Local map set ──────────────────────────────────────────────────────
    mutable std::mutex _localMapsMutex;
    std::unordered_set<uint32> _localMaps; ///< non-empty explicit map list
    std::unordered_set<uint32> _localZones;
    std::unordered_map<uint32, uint8> _zoneToNode; ///< zoneId -> nodeId routing table ///< non-empty explicit zone list (zone-level sub-map routing)
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


    // ── Player ownership ──────────────────────────────────────────────────
    mutable std::mutex _ownershipMutex;
    std::unordered_map<uint64, uint8> _playerOwnership;

    // ── Pending player transfers ──────────────────────────────────────────
    mutable std::mutex _transferMutex;
    std::unordered_map<uint64, PlayerTransferData> _pendingTransfers;

    // ── Pending redirect tokens ──────────────────────────────────────────
    mutable std::mutex _redirectMutex;
    std::unordered_map<uint32, PendingRedirect> _pendingRedirects; ///< accountId → token

    // ── This node's config ───────────────────────────────────────────────
    std::string _gameAddress;
    uint16 _gamePort{ 0 };
    std::string _externalAddress;
    uint16 _externalPort{ 0 };
    uint8 _nodeId{ 0 };

    // ── Packet tracing (diagnostic, see PacketTrace.h) ───────────────────
    /// Cached snapshot of ClusterServer.PacketTrace.Enable. Refreshed on
    /// init and on config reload via RefreshPacketTraceConfig().
    /// Atomic so the hot-path IsPacketTraceEnabled() accessor is lock-free.
    std::atomic<bool> _packetTraceEnabled{ false };
};

#define sClusterMgr ClusterMgr::Instance()

#endif // ClusterMgr_h__
