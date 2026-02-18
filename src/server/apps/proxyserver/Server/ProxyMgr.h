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

#ifndef ProxyMgr_h__
#define ProxyMgr_h__

#include "Define.h"
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class ControlSocket;
class ProxySocket;

/// Cross-node player information maintained in the proxy's global directory.
struct ClusterPlayerInfo
{
    uint64      guid{ 0 };
    uint8       nodeId{ 0 };
    std::string name;        ///< lowercase
    uint32      zoneId{ 0 };
    uint8       level{ 0 };
    uint8       classId{ 0 };
    uint8       raceId{ 0 };
    uint8       teamId{ 0 };
};

/**
 * @brief Central proxy manager.
 *
 * Responsibilities:
 *  1. Maps player GUIDs to their ProxySockets (for rerouting).
 *  2. Tracks registered backend nodes (node_id → ControlSocket).
 *  3. Maintains a cross-node player directory (guid → info, name → guid).
 *  4. Delivers packets directly to player clients (bypassing backend).
 *  5. Broadcasts cluster events (player online/offline) to all other nodes.
 */
class ProxyMgr
{
public:
    static ProxyMgr& Instance()
    {
        static ProxyMgr instance;
        return instance;
    }

    // ── Session registry (ProxySocket ↔ GUID) ─────────────────────────────────

    void RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket);
    void UnregisterSession(uint64 guid);
    std::shared_ptr<ProxySocket> GetSession(uint64 guid);

    void ReroutePlayer(uint64 guid, std::string const& address, uint16 port);

    // ── Node registry (worldserver/instanceserver nodes) ──────────────────────

    /**
     * @brief Load per-node address/port from config (call once at startup).
     * Reads WorldServer.Node.Count + WorldServer.Node.N.Address/Port.
     * Falls back to WorldServer.Address/Port for single-node setups.
     */
    void LoadNodeConfig();

    /**
     * @brief Return the address+port of the worldserver node with the fewest players.
     * Used by ProxySocket::Start() to pick which backend to connect to.
     */
    std::pair<std::string, uint16> ChooseLeastLoadedNode();

    /**
     * @brief Register a backend node, assign it a node ID (1–5), and return it.
     * Called by ControlSocket::HandleRegister().
     */
    uint8 RegisterNode(std::shared_ptr<ControlSocket> socket, uint8 serverType, uint16 gamePort);

    /**
     * @brief Unregister a node when its ControlSocket disconnects.
     */
    void UnregisterNode(uint8 nodeId);

    // ── Cluster player directory ──────────────────────────────────────────────

    /**
     * @brief Called when a worldserver reports a player came online.
     * Updates the directory and broadcasts to all other nodes.
     */
    void OnPlayerOnline(uint64 guid, uint8 nodeId, std::string name,
                        uint32 zoneId, uint8 level, uint8 classId, uint8 raceId, uint8 teamId);

    /**
     * @brief Called when a worldserver reports a player went offline.
     * Updates the directory and broadcasts to all other nodes.
     */
    void OnPlayerOffline(uint64 guid, uint8 nodeId);

    // ── Cross-node packet delivery ────────────────────────────────────────────

    /**
     * @brief Deliver a plaintext WoW game packet directly to a player's client.
     * The packet bytes are re-encrypted for the client direction by ProxySocket.
     */
    void DeliverPacketToPlayer(uint64 targetGuid, std::vector<uint8> packetData);

    // ── Group state ───────────────────────────────────────────────────────────

    struct ProxyGroupMember { uint64 guid; uint8 subgroup; uint8 roleFlags; uint8 nodeId; };

    void OnGroupUpdate(uint64 groupGuid, uint8 sourceNodeId, uint8 memberCount, std::vector<uint8> memberData);
    void OnGroupDisband(uint64 groupGuid, uint8 sourceNodeId);

    // ── LFG master-node relay ─────────────────────────────────────────────────

    /**
     * @brief Set which node ID runs the LFG master queue (default: 1).
     * Called at startup from config (ClusterServer.LFGMasterNode).
     */
    void SetLFGMasterNode(uint8 nodeId) { _lfgMasterNodeId = nodeId; }

    /**
     * @brief Forward an LFG relay payload to the designated LFG master node.
     * Wraps the payload in MSG_CLUSTER_LFG_RELAY before sending.
     * sourceNodeId is included so the master can route responses back.
     */
    void RelayToLFGMaster(uint8 sourceNodeId, std::vector<uint8> payload);

    // ── Node relay & broadcast helpers ────────────────────────────────────────

    /**
     * @brief Forward a pre-built relay frame to a specific node's ControlSocket.
     */
    void RelayToNode(uint8 targetNodeId, std::vector<uint8> const& msg);

    /**
     * @brief Send a raw binary message to all registered nodes except exclude_node_id.
     * Thread-safe.
     */
    void BroadcastToOtherNodes(std::vector<uint8> const& data, uint8 excludeNodeId);

private:
    ProxyMgr() = default;

    // ── Session map ────────────────────────────────────────────────────────────
    std::mutex _sessionMutex;
    std::unordered_map<uint64, std::weak_ptr<ProxySocket>> _sessions;

    // ── Node map ───────────────────────────────────────────────────────────────
    std::mutex _nodeMutex;
    std::unordered_map<uint8, std::weak_ptr<ControlSocket>> _nodes; ///< node_id → socket
    uint8 _nextNodeId{ 1 };                                          ///< 1–5
    std::map<uint8, std::pair<std::string, uint16>> _nodeAddresses;  ///< node_id → (address, port)
    std::map<uint8, uint32> _nodePlayerCounts;                       ///< node_id → online player count

    // ── Player directory ───────────────────────────────────────────────────────
    std::mutex _dirMutex;
    std::unordered_map<uint64, ClusterPlayerInfo> _playerByGuid;
    std::unordered_map<std::string, uint64>       _playerByName; ///< lowercase name → guid

    // ── Group directory (proxy-side group state) ──────────────────────────────
    std::mutex _groupMutex;
    std::unordered_map<uint64, std::vector<ProxyGroupMember>> _groupMembers; ///< group_guid → members

    // ── LFG master routing ────────────────────────────────────────────────────
    uint8 _lfgMasterNodeId{ 1 }; ///< Node ID that runs the authoritative LFG queue
};

#define sProxyMgr ProxyMgr::Instance()

#endif // ProxyMgr_h__
