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

#ifndef ProxyMgr_h__
#define ProxyMgr_h__

#include "Define.h"
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

class ControlSocket;
class ManagementSocket;
class NodeMgrSocket;
class ProxySocket;

/// Cross-node player information maintained in the proxy's global directory.
struct ClusterPlayerInfo
{
    uint64      guid{ 0 };
    uint8       nodeId{ 0 };
    std::string name;
    uint32      zoneId{ 0 };
    uint8       level{ 0 };
    uint8       classId{ 0 };
    uint8       raceId{ 0 };
    uint8       teamId{ 0 };
};

/// Per-node lifecycle state reported by nodemgr daemons.
enum class NodeState : uint8
{
    Unknown  = 0,
    Stopped  = 1,
    Starting = 2,
    Running  = 3,
    Stopping = 4,
    Crashed  = 5,
};

/// Status snapshot for a single node (used by management push and auto-scale).
struct NodeStatus
{
    uint8     nodeId{ 0 };
    NodeState state{ NodeState::Unknown };
    uint32    pid{ 0 };
    uint32    uptimeSecs{ 0 };
    uint32    playerCount{ 0 };
    uint32    maxPlayers{ 500 };
    std::string address;
    uint16    port{ 0 };
    uint32    latencyMs{ 0 };   ///< Control-channel RTT in ms (0 = not measured yet)
};

/**
 * @brief Central proxy manager.
 *
 * Responsibilities:
 *  1. Maps player GUIDs to their ProxySockets.
 *  2. Tracks registered backend worldserver nodes.
 *  3. Tracks registered nodemgr daemons.
 *  4. Tracks clustermgr management subscribers.
 *  5. Maintains a cross-node player directory.
 *  6. Delivers packets directly to player clients.
 *  7. Broadcasts cluster events to all nodes.
 *  8. Routes players via round-robin or least-loaded strategy.
 *  9. Drives auto-scale decisions (start/stop nodes based on load).
 */
class ProxyMgr
{
public:
    static ProxyMgr& Instance()
    {
        static ProxyMgr instance;
        return instance;
    }

    // ── Session registry ───────────────────────────────────────────────────────
    void RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket);
    void UnregisterSession(uint64 guid);
    std::shared_ptr<ProxySocket> GetSession(uint64 guid);
    void ReroutePlayer(uint64 guid, std::string const& address, uint16 port);

    // ── Worldserver node registry ──────────────────────────────────────────────

    /// Load per-node address/port from config at startup.
    void LoadNodeConfig();

    /// Return the nodeId, address, and port for the next node (round-robin or least-loaded).
    std::tuple<uint8, std::string, uint16> ChooseNode();

    /// Register a worldserver control socket; return assigned node ID.
    /// peerIp is the remote IP of the control-channel TCP connection — used to match the
    /// configured node address when multiple nodes share the same WorldServerPort value.
    uint8 RegisterNode(std::shared_ptr<ControlSocket> socket, uint8 serverType, uint16 gamePort,
                       std::string const& peerIp);

    /// Unregister a worldserver when its control socket closes.
    void UnregisterNode(uint8 nodeId);

    /// Register the map IDs a node handles; populates dynamic routing table.
    void RegisterNodeMaps(uint8 nodeId, std::vector<uint32> const& maps);

    // ── nodemgr daemon registry ────────────────────────────────────────────────

    /// Register a nodemgr connection; return the node ID it will manage.
    uint8 RegisterNodeMgr(std::shared_ptr<NodeMgrSocket> socket, uint8 configuredNodeId, uint16 gamePort);

    /// Unregister a nodemgr (called from NodeMgrSocket::OnClose).
    void UnregisterNodeMgr(uint8 nodeId);

    /// Update per-node status from a MSG_NODE_STATUS message.
    void UpdateNodeMgrStatus(uint8 nodeId, uint8 state, uint32 pid, uint32 uptime);

    /// Send MSG_NODE_START to the nodemgr for a given node (from clustermgr or auto-scale).
    void StartNode(uint8 nodeId);

    /// Send MSG_NODE_STOP to the nodemgr for a given node.
    void StopNode(uint8 nodeId);

    // ── Management subscribers (clustermgr) ────────────────────────────────────

    /// Accumulate bytes transferred to/from a worldserver node (called from ProxySocket).
    /// tx = client→worldserver bytes, rx = worldserver→client bytes.
    void AddNodeTraffic(uint8 nodeId, uint64 txBytes, uint64 rxBytes);
    void OnNodePong(uint8 nodeId, uint32 latencyMs);
    void SendPingsToAllNodes();

    /// Add a clustermgr client to the push subscriber list.
    void AddMgmtSubscriber(std::shared_ptr<ManagementSocket> sock);

    /// Remove a clustermgr client (called from ManagementSocket::OnClose).
    void RemoveMgmtSubscriber(std::shared_ptr<ManagementSocket> sock);

    /// Serialize the current NodeStatus table and push to all subscribers.
    void PushStatusToSubscribers();

    // ── Auto-scale ────────────────────────────────────────────────────────────

    /// Called by a recurring 30-second timer in Main.cpp.
    void CheckAutoScale();

    /// Load auto-scale and routing config from proxyserver.conf.
    void LoadAutoScaleConfig();

    // ── Map-based routing ─────────────────────────────────────────────────────

    /// Load MapRouting.X.MapId / MapRouting.X.NodeId entries from proxyserver.conf.
    void LoadMapRoutingConfig();

    /// Reroute the player GUID to whichever cluster node handles mapId.
    void RerouteToMap(uint64 guid, uint32 mapId);

    /// Return the node ID responsible for mapId (falls back to _defaultNodeId).
    uint8 GetNodeForMap(uint32 mapId) const;

    // ── Cluster player directory ───────────────────────────────────────────────
    void OnPlayerOnline(uint64 guid, uint8 nodeId, std::string name,
                        uint32 zoneId, uint8 level, uint8 classId, uint8 raceId, uint8 teamId);
    void OnPlayerOffline(uint64 guid, uint8 nodeId);

    // ── Cross-node packet delivery ────────────────────────────────────────────
    void DeliverPacketToPlayer(uint64 targetGuid, std::vector<uint8> packetData);

    // ── Group state ───────────────────────────────────────────────────────────
    struct ProxyGroupMember { uint64 guid; uint8 subgroup; uint8 roleFlags; uint8 nodeId; };

    void OnGroupUpdate(uint64 groupGuid, uint8 sourceNodeId, uint8 memberCount, std::vector<uint8> memberData);
    void OnGroupDisband(uint64 groupGuid, uint8 sourceNodeId);

    // ── LFG master-node relay ─────────────────────────────────────────────────
    void SetLFGMasterNode(uint8 nodeId) { _lfgMasterNodeId = nodeId; }
    void RelayToLFGMaster(uint8 sourceNodeId, std::vector<uint8> payload);

    // ── Node relay & broadcast helpers ────────────────────────────────────────
    void RelayToNode(uint8 targetNodeId, std::vector<uint8> const& msg);
    void BroadcastToOtherNodes(std::vector<uint8> const& data, uint8 excludeNodeId);
    void BroadcastUnitUpdate(uint8 sourceNodeId, uint16 payloadLen, std::vector<uint8> const& payload);
    /// Broadcast a cross-node SAY/YELL/EMOTE chat message to all nodes except the source.
    void BroadcastChatRelay(uint8 sourceNodeId, uint16 payloadLen, std::vector<uint8> const& payload);
    /// Route a new-mail notification to the node that hosts recipientGuid's session.
    void RouteMailNotification(uint64 recipientGuid);

private:
    ProxyMgr() = default;

    // ── Internal routing ──────────────────────────────────────────────────────
    std::tuple<uint8, std::string, uint16> ChooseLeastLoadedNode();
    std::tuple<uint8, std::string, uint16> ChooseRoundRobinNode();

    /// Build the MSG_MGMT_STATUS_PUSH payload from current state.
    std::vector<uint8> BuildStatusPayload();

    // ── Session map ────────────────────────────────────────────────────────────
    std::mutex _sessionMutex;
    std::unordered_map<uint64, std::weak_ptr<ProxySocket>> _sessions;

    // ── Worldserver node map ───────────────────────────────────────────────────
    std::mutex _nodeMutex;
    std::unordered_map<uint8, std::weak_ptr<ControlSocket>> _nodes;
    uint8 _nextNodeId{ 1 };
    std::map<uint8, std::pair<std::string, uint16>> _nodeAddresses;
    std::map<uint8, uint32> _nodePlayerCounts;
    std::map<uint8, uint8>  _nodeServerTypes;   ///< 0 = worldserver, 1 = instance server

    // ── nodemgr map ────────────────────────────────────────────────────────────
    // Keyed by the node ID they manage (matches _nodeAddresses keys).
    std::unordered_map<uint8, std::weak_ptr<NodeMgrSocket>> _nodeMgrs;

    // ── Node status table (updated by nodemgr status messages) ────────────────
    std::map<uint8, NodeStatus> _nodeStatus;

    // ── Per-node bandwidth counters (protected by _nodeMutex) ─────────────────
    std::map<uint8, uint64> _nodeTxBytes;   ///< Cumulative bytes client→worldserver
    std::map<uint8, uint64> _nodeRxBytes;   ///< Cumulative bytes worldserver→client
    std::map<uint8, uint64> _prevTxBytes;   ///< Snapshot at last bps computation
    std::map<uint8, uint64> _prevRxBytes;
    std::map<uint8, uint32> _nodeTxBps;     ///< Last computed TX bytes/sec
    std::map<uint8, uint32> _nodeRxBps;     ///< Last computed RX bytes/sec
    std::map<uint8, uint32> _nodeLatencyMs; ///< Last measured control-channel RTT ms
    std::chrono::steady_clock::time_point _lastBwUpdate{ std::chrono::steady_clock::time_point::min() };

    // ── Management subscribers ─────────────────────────────────────────────────
    std::mutex _mgmtMutex;
    std::vector<std::weak_ptr<ManagementSocket>> _mgmtSubscribers;

    // ── Player directory ───────────────────────────────────────────────────────
    std::mutex _dirMutex;
    std::unordered_map<uint64, ClusterPlayerInfo> _playerByGuid;
    std::unordered_map<std::string, uint64>       _playerByName;

    // ── Group directory ────────────────────────────────────────────────────────
    std::mutex _groupMutex;
    std::unordered_map<uint64, std::vector<ProxyGroupMember>> _groupMembers;

    // ── LFG master routing ────────────────────────────────────────────────────
    uint8 _lfgMasterNodeId{ 1 };

    // ── Map routing table ─────────────────────────────────────────────────────
    mutable std::mutex _mapRoutingMutex;
    std::unordered_map<uint32, uint8> _mapRouting;  ///< mapId → nodeId
    uint8 _defaultNodeId{ 0 };                      ///< fallback node (0 = round-robin)

    // ── Routing strategy ──────────────────────────────────────────────────────
    bool  _useRoundRobin{ true };
    uint32 _rrIndex{ 0 };         ///< Protected by _nodeMutex

    // ── Auto-scale ────────────────────────────────────────────────────────────
    bool   _autoScaleEnabled{ false };
    uint32 _maxPlayersPerNode{ 500 };
    uint32 _scaleUpThreshold{ 80 };    ///< % of capacity → start a node
    uint32 _scaleDownThreshold{ 30 };  ///< % of capacity → stop a node
    uint32 _minNodes{ 1 };
    uint32 _maxNodes{ 5 };
    uint32 _cooldownSeconds{ 300 };
    std::chrono::steady_clock::time_point _lastScaleEvent{ std::chrono::steady_clock::time_point::min() };
};

#define sProxyMgr ProxyMgr::Instance()

#endif // ProxyMgr_h__
