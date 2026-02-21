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

#include "ProxyMgr.h"
#include "Config.h"
#include "ControlSocket.h"
#include "Log.h"
#include "ManagementSocket.h"
#include "NodeMgrSocket.h"
#include "ProxySocket.h"
#include <algorithm>
#include <cstring>

// ── Node config + routing ─────────────────────────────────────────────────────

void ProxyMgr::LoadNodeConfig()
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    uint32 count = sConfigMgr->GetOption<uint32>("WorldServer.Node.Count", 0);
    if (count == 0)
    {
        std::string addr = sConfigMgr->GetOption<std::string>("WorldServer.Address", "127.0.0.1");
        uint16 port = static_cast<uint16>(sConfigMgr->GetOption<int32>("WorldServer.Port", 8086));
        _nodeAddresses[1] = { addr, port };
        _nodePlayerCounts[1] = 0;

        NodeStatus& ns = _nodeStatus[1];
        ns.nodeId = 1;  ns.address = addr;  ns.port = port;
        ns.maxPlayers = _maxPlayersPerNode;

        LOG_INFO("proxy", "ProxyMgr: Single-node mode — backend {}:{}", addr, port);
        return;
    }

    uint32 loaded = 0;
    for (uint32 i = 1; i <= count && i <= 10; ++i)
    {
        std::string keyAddr = "WorldServer.Node." + std::to_string(i) + ".Address";
        std::string keyPort = "WorldServer.Node." + std::to_string(i) + ".Port";
        std::string addr = sConfigMgr->GetOption<std::string>(keyAddr, "127.0.0.1");
        uint16 port = static_cast<uint16>(
            sConfigMgr->GetOption<int32>(keyPort, 8086 + static_cast<int32>(i) - 1));
        _nodeAddresses[static_cast<uint8>(i)] = { addr, port };
        _nodePlayerCounts[static_cast<uint8>(i)] = 0;

        NodeStatus& ns = _nodeStatus[static_cast<uint8>(i)];
        ns.nodeId = static_cast<uint8>(i);
        ns.address = addr;
        ns.port = port;
        ns.maxPlayers = _maxPlayersPerNode;

        LOG_INFO("proxy", "ProxyMgr: Configured node {} → {}:{}", i, addr, port);
        ++loaded;
    }
    LOG_INFO("proxy", "ProxyMgr: {} node(s) configured", loaded);
}

void ProxyMgr::LoadAutoScaleConfig()
{
    _useRoundRobin = (sConfigMgr->GetOption<std::string>("Cluster.RoutingMode", "roundrobin") != "leastloaded");
    _maxPlayersPerNode = sConfigMgr->GetOption<uint32>("Management.MaxPlayersPerNode", 500);

    _autoScaleEnabled    = sConfigMgr->GetOption<bool>("Cluster.AutoScale.Enable", false);
    _scaleUpThreshold    = sConfigMgr->GetOption<uint32>("Cluster.AutoScale.ScaleUpThreshold", 80);
    _scaleDownThreshold  = sConfigMgr->GetOption<uint32>("Cluster.AutoScale.ScaleDownThreshold", 30);
    _minNodes            = sConfigMgr->GetOption<uint32>("Cluster.AutoScale.MinNodes", 1);
    _maxNodes            = sConfigMgr->GetOption<uint32>("Cluster.AutoScale.MaxNodes", 5);
    _cooldownSeconds     = sConfigMgr->GetOption<uint32>("Cluster.AutoScale.CooldownSeconds", 300);

    LOG_INFO("proxy", "ProxyMgr: RoutingMode={} MaxPlayersPerNode={}",
             _useRoundRobin ? "roundrobin" : "leastloaded", _maxPlayersPerNode);

    if (_autoScaleEnabled)
        LOG_INFO("proxy", "ProxyMgr: AutoScale enabled — up={}% down={}% min={} max={} cooldown={}s",
                 _scaleUpThreshold, _scaleDownThreshold, _minNodes, _maxNodes, _cooldownSeconds);
}

std::tuple<uint8, std::string, uint16> ProxyMgr::ChooseNode()
{
    return _useRoundRobin ? ChooseRoundRobinNode() : ChooseLeastLoadedNode();
}

// ── Map-based routing ─────────────────────────────────────────────────────────

void ProxyMgr::LoadMapRoutingConfig()
{
    std::lock_guard<std::mutex> lock(_mapRoutingMutex);
    // Static per-map entries are now populated dynamically via RegisterNodeMaps()
    // at node registration time. We only load the default fallback node here.
    _defaultNodeId = static_cast<uint8>(sConfigMgr->GetOption<int32>("Cluster.MapRouting.Default", 0));
    LOG_INFO("proxy", "ProxyMgr: MapRouting default node={} (routes populated dynamically at node registration)",
             _defaultNodeId);
}

uint8 ProxyMgr::GetNodeForMap(uint32 mapId) const
{
    std::lock_guard<std::mutex> lock(_mapRoutingMutex);
    auto it = _mapRouting.find(mapId);
    if (it != _mapRouting.end())
        return it->second;
    return _defaultNodeId;
}

void ProxyMgr::RerouteToMap(uint64 guid, uint32 mapId)
{
    uint8 nodeId = GetNodeForMap(mapId);
    if (nodeId == 0)
    {
        // No route configured — fall back to round-robin ChooseNode
        auto [nid, addr, port] = ChooseNode();
        LOG_INFO("proxy", "ProxyMgr: RerouteToMap GUID {:016X} map {} — no route, using node {} ({}:{})",
                 guid, mapId, nid, addr, port);
        ReroutePlayer(guid, addr, port);
        return;
    }

    // Look up address/port in a short-lived lock scope, then reroute.
    std::string addr;
    uint16      port = 0;
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        auto it = _nodeAddresses.find(nodeId);
        if (it == _nodeAddresses.end())
        {
            LOG_ERROR("proxy", "ProxyMgr: RerouteToMap — node {} not in address table", nodeId);
            return;
        }
        addr = it->second.first;
        port = it->second.second;
    }
    LOG_INFO("proxy", "ProxyMgr: RerouteToMap GUID {:016X} map {} → node {} ({}:{})",
             guid, mapId, nodeId, addr, port);
    ReroutePlayer(guid, addr, port);
}

std::tuple<uint8, std::string, uint16> ProxyMgr::ChooseRoundRobinNode()
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    if (_nodeAddresses.empty())
        return { 0, "127.0.0.1", 8086 };

    // Collect running worldserver nodes (active ControlSocket, serverType == 0).
    // Instance-server nodes (serverType != 0) are excluded from direct player routing.
    std::vector<uint8> running;
    for (auto const& [nodeId, _] : _nodeAddresses)
    {
        auto it = _nodes.find(nodeId);
        if (it == _nodes.end() || it->second.expired())
            continue;
        auto typeIt = _nodeServerTypes.find(nodeId);
        if (typeIt != _nodeServerTypes.end() && typeIt->second != 0)
            continue; // skip instance servers
        running.push_back(nodeId);
    }

    if (running.empty())
    {
        // No active worldserver — fall back to first configured address.
        LOG_WARN("proxy", "ProxyMgr: ChooseRoundRobinNode — no running nodes, using first configured");
        uint8 firstId = _nodeAddresses.begin()->first;
        auto const& [addr, port] = _nodeAddresses.begin()->second;
        return { firstId, addr, port };
    }

    // Round-robin through running nodes, skip any at full capacity.
    std::size_t n = running.size();
    for (std::size_t i = 0; i < n; ++i)
    {
        uint8 nodeId = running[_rrIndex % n];
        _rrIndex = (_rrIndex + 1) % static_cast<uint32>(n);

        uint32 cnt = _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
        if (cnt < _maxPlayersPerNode)
        {
            LOG_INFO("proxy", "ProxyMgr: RoundRobin → node {} ({}:{}, {}/{} players)",
                     nodeId, _nodeAddresses[nodeId].first, _nodeAddresses[nodeId].second,
                     cnt, _maxPlayersPerNode);
            return { nodeId, _nodeAddresses[nodeId].first, _nodeAddresses[nodeId].second };
        }
    }

    // All nodes full — pick least loaded as overflow.
    LOG_WARN("proxy", "ProxyMgr: All nodes at capacity — falling back to least loaded");
    return ChooseLeastLoadedNode();
}

std::tuple<uint8, std::string, uint16> ProxyMgr::ChooseLeastLoadedNode()
{
    // NOTE: caller must NOT hold _nodeMutex (called internally, may be re-entered).
    // When called from ChooseRoundRobinNode, the lock is already held — safe because
    // _nodeMutex is not re-locked here; this is a pure read of already-locked data.

    if (_nodeAddresses.empty())
        return { 0, "127.0.0.1", 8086 };

    uint8    best     = 0;
    uint32   minCount = UINT32_MAX;

    for (auto const& [nodeId, addr] : _nodeAddresses)
    {
        // Only consider active worldserver nodes (not instance servers).
        auto it = _nodes.find(nodeId);
        if (it == _nodes.end() || it->second.expired())
            continue;
        auto typeIt = _nodeServerTypes.find(nodeId);
        if (typeIt != _nodeServerTypes.end() && typeIt->second != 0)
            continue; // skip instance servers

        uint32 cnt = _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
        if (best == 0 || cnt < minCount)
        {
            best     = nodeId;
            minCount = cnt;
        }
    }

    if (best == 0)
    {
        // No active worldserver at all — return first configured as last resort.
        LOG_WARN("proxy", "ProxyMgr: ChooseLeastLoadedNode — no active nodes");
        uint8 firstId = _nodeAddresses.begin()->first;
        auto const& [addr, port] = _nodeAddresses.begin()->second;
        return { firstId, addr, port };
    }

    LOG_INFO("proxy", "ProxyMgr: LeastLoaded → node {} ({}:{}, {} players)",
             best, _nodeAddresses[best].first, _nodeAddresses[best].second, minCount);
    return { best, _nodeAddresses[best].first, _nodeAddresses[best].second };
}

// ── Session registry ──────────────────────────────────────────────────────────

void ProxyMgr::RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions[guid] = socket;
}

void ProxyMgr::UnregisterSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions.erase(guid);
}

std::shared_ptr<ProxySocket> ProxyMgr::GetSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _sessions.find(guid);
    if (it == _sessions.end())
        return nullptr;
    auto socket = it->second.lock();
    if (!socket)
        _sessions.erase(it);
    return socket;
}

void ProxyMgr::ReroutePlayer(uint64 guid, std::string const& address, uint16 port)
{
    auto socket = GetSession(guid);
    if (!socket)
    {
        LOG_WARN("proxy", "ProxyMgr: ReroutePlayer — GUID {:016X} not found", guid);
        return;
    }
    LOG_INFO("proxy", "ProxyMgr: Rerouting GUID {:016X} to {}:{}", guid, address, port);
    socket->RerouteToBackend(address, port);
}

// ── Worldserver node registry ─────────────────────────────────────────────────

uint8 ProxyMgr::RegisterNode(std::shared_ptr<ControlSocket> socket, uint8 serverType, uint16 gamePort,
                             std::string const& peerIp)
{
    uint8 assignedNodeId = 0;
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);

        // Match by peer IP first — handles multiple nodes sharing the same WorldServerPort.
        // Fall back to port-only match for single-machine / localhost setups.
        for (auto const& [nodeId, addrPort] : _nodeAddresses)
        {
            bool ipMatch   = !peerIp.empty() && addrPort.first == peerIp;
            bool portMatch = addrPort.second == gamePort;
            bool slotFree  = !_nodes.count(nodeId) || _nodes.at(nodeId).expired();
            if (ipMatch && slotFree)
            {
                _nodes[nodeId]           = socket;
                _nodeServerTypes[nodeId] = serverType;
                if (_nodeStatus.count(nodeId))
                    _nodeStatus[nodeId].state = NodeState::Running;
                LOG_INFO("proxy", "ProxyMgr: {} node {} online (game_port={})",
                         serverType == 0 ? "Worldserver" : "Instance-server", nodeId, gamePort);
                assignedNodeId = nodeId;
                break;
            }
        }

        if (assignedNodeId == 0)
        {
            while (_nextNodeId <= 10 && _nodes.count(_nextNodeId))
                _nextNodeId++;

            if (_nextNodeId > 10)
            {
                LOG_ERROR("proxy", "ProxyMgr: RegisterNode — max nodes reached, rejecting");
                return 0;
            }

            assignedNodeId = _nextNodeId++;
            _nodes[assignedNodeId]           = socket;
            _nodeServerTypes[assignedNodeId] = serverType;
            if (_nodeStatus.count(assignedNodeId))
                _nodeStatus[assignedNodeId].state = NodeState::Running;
            LOG_INFO("proxy", "ProxyMgr: {} node {} online (game_port={}, assigned)",
                     serverType == 0 ? "Worldserver" : "Instance-server", assignedNodeId, gamePort);
        }
    } // _nodeMutex released before PushStatusToSubscribers
    PushStatusToSubscribers();
    return assignedNodeId;
}

void ProxyMgr::UnregisterNode(uint8 nodeId)
{
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        _nodes.erase(nodeId);
        _nodeServerTypes.erase(nodeId);
        if (_nodeStatus.count(nodeId))
            _nodeStatus[nodeId].state = NodeState::Stopped;
        // Clear stale player count so the dead node isn't picked as least-loaded.
        _nodePlayerCounts[nodeId] = 0;
        if (_nodeStatus.count(nodeId))
            _nodeStatus[nodeId].playerCount = 0;
    }
    LOG_INFO("proxy", "ProxyMgr: Worldserver node {} offline", nodeId);

    // ── Reroute or disconnect players that were on the crashed node ────────────
    // Collect affected GUIDs under the directory lock (don't hold it while rerouting).
    std::vector<uint64> affected;
    {
        std::lock_guard<std::mutex> lock(_dirMutex);
        for (auto const& [guid, info] : _playerByGuid)
            if (info.nodeId == nodeId)
                affected.push_back(guid);
    }

    if (!affected.empty())
    {
        // Pick a surviving node to redirect players to (holds _nodeMutex internally).
        auto [fallbackNodeId, fallbackAddr, fallbackPort] = ChooseLeastLoadedNode();

        bool hasAlternative = (fallbackPort != 0);
        // If ChooseLeastLoadedNode returned the last-resort "no active nodes" address,
        // the port is still non-zero but no worldserver is behind it — safer to
        // check that at least one active node exists.
        {
            std::lock_guard<std::mutex> lock(_nodeMutex);
            hasAlternative = std::any_of(_nodes.begin(), _nodes.end(),
                [](auto const& kv) { return !kv.second.expired(); });
        }

        if (hasAlternative)
        {
            LOG_WARN("proxy", "ProxyMgr: Node {} crashed — rerouting {} player(s) to {}:{}",
                     nodeId, affected.size(), fallbackAddr, fallbackPort);
            for (uint64 guid : affected)
                ReroutePlayer(guid, fallbackAddr, fallbackPort);
        }
        else
        {
            LOG_WARN("proxy", "ProxyMgr: Node {} crashed — no surviving node; disconnecting {} player(s)",
                     nodeId, affected.size());
            for (uint64 guid : affected)
            {
                auto sock = GetSession(guid);
                if (sock)
                    sock->CloseSocket();
            }
        }

        // Clean the directory for these players now (they will re-register on reconnect).
        {
            std::lock_guard<std::mutex> lock(_dirMutex);
            for (uint64 guid : affected)
            {
                auto it = _playerByGuid.find(guid);
                if (it != _playerByGuid.end())
                {
                    _playerByName.erase(it->second.name);
                    _playerByGuid.erase(it);
                }
            }
        }
    }

    PushStatusToSubscribers();
}

// ── nodemgr daemon registry ───────────────────────────────────────────────────

uint8 ProxyMgr::RegisterNodeMgr(std::shared_ptr<NodeMgrSocket> socket, uint8 configuredNodeId, uint16 /*gamePort*/)
{
    uint8 nodeId;
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);

        // Use the node ID the nodemgr claims if it's valid and unoccupied.
        nodeId = configuredNodeId;
        if (nodeId == 0 || !_nodeAddresses.count(nodeId))
        {
            nodeId = 0; // reset; will find a free slot below
            // Find first configured slot without a nodemgr.
            for (auto const& [id, _] : _nodeAddresses)
            {
                if (!_nodeMgrs.count(id) || _nodeMgrs.at(id).expired())
                {
                    nodeId = id;
                    break;
                }
            }
        }

        if (nodeId == 0)
        {
            LOG_ERROR("proxy", "ProxyMgr: RegisterNodeMgr — no slot available");
            return 0;
        }

        _nodeMgrs[nodeId] = socket;

        NodeStatus& ns = _nodeStatus[nodeId];
        if (ns.state == NodeState::Unknown)
            ns.state = NodeState::Stopped;

        LOG_INFO("proxy", "ProxyMgr: nodemgr registered for node {}", nodeId);
    } // _nodeMutex released before PushStatusToSubscribers
    PushStatusToSubscribers();
    return nodeId;
}

void ProxyMgr::UnregisterNodeMgr(uint8 nodeId)
{
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        _nodeMgrs.erase(nodeId);
    }
    LOG_INFO("proxy", "ProxyMgr: nodemgr for node {} disconnected", nodeId);
    PushStatusToSubscribers();
}

void ProxyMgr::UpdateNodeMgrStatus(uint8 nodeId, uint8 state, uint32 pid, uint32 uptime)
{
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        NodeStatus& ns = _nodeStatus[nodeId];
        ns.nodeId    = nodeId;
        ns.state     = static_cast<NodeState>(state < 6 ? state : 0);
        ns.pid       = pid;
        ns.uptimeSecs = uptime;
    }
    PushStatusToSubscribers();
}

void ProxyMgr::StartNode(uint8 nodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodeMgrs.find(nodeId);
    if (it == _nodeMgrs.end())
    {
        LOG_WARN("proxy", "ProxyMgr: StartNode({}) — no nodemgr registered", nodeId);
        return;
    }
    if (auto sock = it->second.lock())
    {
        _nodeStatus[nodeId].state = NodeState::Starting;
        sock->SendNodeStart();
        LOG_INFO("proxy", "ProxyMgr: Sent NODE_START to node {}", nodeId);
    }
}

void ProxyMgr::StopNode(uint8 nodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodeMgrs.find(nodeId);
    if (it == _nodeMgrs.end())
    {
        LOG_WARN("proxy", "ProxyMgr: StopNode({}) — no nodemgr registered", nodeId);
        return;
    }
    if (auto sock = it->second.lock())
    {
        _nodeStatus[nodeId].state = NodeState::Stopping;
        sock->SendNodeStop();
        LOG_INFO("proxy", "ProxyMgr: Sent NODE_STOP to node {}", nodeId);
    }
}

// ── Auto-scale ────────────────────────────────────────────────────────────────

void ProxyMgr::CheckAutoScale()
{
    if (!_autoScaleEnabled)
        return;

    // Check cooldown.
    auto now = std::chrono::steady_clock::now();
    if (_lastScaleEvent != std::chrono::steady_clock::time_point::min())
    {
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - _lastScaleEvent).count();
        if (elapsed < static_cast<int64_t>(_cooldownSeconds))
            return;
    }

    std::lock_guard<std::mutex> lock(_nodeMutex);

    // Count running nodes and total players.
    uint32 runningNodes = 0;
    uint32 totalPlayers = 0;
    for (auto const& [nodeId, _] : _nodeAddresses)
    {
        auto it = _nodes.find(nodeId);
        if (it != _nodes.end() && !it->second.expired())
        {
            ++runningNodes;
            totalPlayers += _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
        }
    }

    if (runningNodes == 0)
        return;

    uint32 capacity = runningNodes * _maxPlayersPerNode;
    uint32 loadPct  = capacity > 0 ? (totalPlayers * 100 / capacity) : 0;

    if (loadPct >= _scaleUpThreshold && runningNodes < _maxNodes)
    {
        // Find the first configured node that has a nodemgr but no running worldserver.
        for (auto const& [nodeId, _] : _nodeAddresses)
        {
            bool hasWorldserver = _nodes.count(nodeId) && !_nodes.at(nodeId).expired();
            bool hasMgr        = _nodeMgrs.count(nodeId) && !_nodeMgrs.at(nodeId).expired();

            if (!hasWorldserver && hasMgr)
            {
                if (auto sock = _nodeMgrs[nodeId].lock())
                {
                    LOG_INFO("proxy", "ProxyMgr: AutoScale UP — load={}% starting node {}", loadPct, nodeId);
                    _nodeStatus[nodeId].state = NodeState::Starting;
                    sock->SendNodeStart();
                    _lastScaleEvent = now;
                }
                return;
            }
        }
    }
    else if (loadPct < _scaleDownThreshold && runningNodes > _minNodes)
    {
        // Stop the running node with fewest players (highest node ID as tiebreak).
        uint8 targetNode = 0;
        uint32 fewest = UINT32_MAX;
        for (auto const& [nodeId, _] : _nodeAddresses)
        {
            bool hasWorldserver = _nodes.count(nodeId) && !_nodes.at(nodeId).expired();
            bool hasMgr        = _nodeMgrs.count(nodeId) && !_nodeMgrs.at(nodeId).expired();

            if (hasWorldserver && hasMgr)
            {
                uint32 cnt = _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
                if (cnt <= fewest)
                {
                    fewest = cnt;
                    targetNode = nodeId;
                }
            }
        }

        if (targetNode != 0)
        {
            if (auto sock = _nodeMgrs[targetNode].lock())
            {
                LOG_INFO("proxy", "ProxyMgr: AutoScale DOWN — load={}% stopping node {} ({} players)",
                         loadPct, targetNode, fewest);
                _nodeStatus[targetNode].state = NodeState::Stopping;
                sock->SendNodeStop();
                _lastScaleEvent = now;
            }
        }
    }
}

// ── Management subscribers ────────────────────────────────────────────────────

void ProxyMgr::AddMgmtSubscriber(std::shared_ptr<ManagementSocket> sock)
{
    std::lock_guard<std::mutex> lock(_mgmtMutex);
    _mgmtSubscribers.emplace_back(sock);
}

void ProxyMgr::RemoveMgmtSubscriber(std::shared_ptr<ManagementSocket> sock)
{
    std::lock_guard<std::mutex> lock(_mgmtMutex);
    _mgmtSubscribers.erase(
        std::remove_if(_mgmtSubscribers.begin(), _mgmtSubscribers.end(),
            [&sock](std::weak_ptr<ManagementSocket> const& wp)
            {
                auto sp = wp.lock();
                return !sp || sp.get() == sock.get();
            }),
        _mgmtSubscribers.end());
}

std::vector<uint8> ProxyMgr::BuildStatusPayload()
{
    // Lock must NOT be held when this is called (we take it here).
    std::lock_guard<std::mutex> nodeLock(_nodeMutex);
    std::lock_guard<std::mutex> dirLock(_dirMutex);

    // ── Compute per-node bandwidth rates ────────────────────────────────────────
    auto now = std::chrono::steady_clock::now();
    if (_lastBwUpdate != std::chrono::steady_clock::time_point::min())
    {
        double elapsed = std::chrono::duration<double>(now - _lastBwUpdate).count();
        if (elapsed >= 1.0)
        {
            for (auto const& [nodeId, _] : _nodeStatus)
            {
                uint64 tx = _nodeTxBytes.count(nodeId) ? _nodeTxBytes.at(nodeId) : 0;
                uint64 rx = _nodeRxBytes.count(nodeId) ? _nodeRxBytes.at(nodeId) : 0;
                uint64 prevTx = _prevTxBytes.count(nodeId) ? _prevTxBytes.at(nodeId) : 0;
                uint64 prevRx = _prevRxBytes.count(nodeId) ? _prevRxBytes.at(nodeId) : 0;

                _nodeTxBps[nodeId] = static_cast<uint32>((tx - prevTx) / elapsed);
                _nodeRxBps[nodeId] = static_cast<uint32>((rx - prevRx) / elapsed);
                _prevTxBytes[nodeId] = tx;
                _prevRxBytes[nodeId] = rx;
            }
            _lastBwUpdate = now;
        }
    }
    else
    {
        _lastBwUpdate = now;
    }

    // ── Build payload ────────────────────────────────────────────────────────────
    std::vector<uint8> payload;
    payload.push_back(static_cast<uint8>(_nodeStatus.size()));

    for (auto const& [nodeId, ns] : _nodeStatus)
    {
        uint32 players = _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
        uint32 txBps   = _nodeTxBps.count(nodeId) ? _nodeTxBps.at(nodeId) : 0;
        uint32 rxBps   = _nodeRxBps.count(nodeId) ? _nodeRxBps.at(nodeId) : 0;

        payload.push_back(ns.nodeId);
        payload.push_back(static_cast<uint8>(ns.state));

        // player_count (LE uint16)
        payload.push_back(static_cast<uint8>(players & 0xFF));
        payload.push_back(static_cast<uint8>((players >> 8) & 0xFF));

        // max_players (LE uint16)
        payload.push_back(static_cast<uint8>(ns.maxPlayers & 0xFF));
        payload.push_back(static_cast<uint8>((ns.maxPlayers >> 8) & 0xFF));

        // pid (LE uint32)
        payload.push_back(static_cast<uint8>(ns.pid & 0xFF));
        payload.push_back(static_cast<uint8>((ns.pid >> 8) & 0xFF));
        payload.push_back(static_cast<uint8>((ns.pid >> 16) & 0xFF));
        payload.push_back(static_cast<uint8>((ns.pid >> 24) & 0xFF));

        // uptime_secs (LE uint32)
        payload.push_back(static_cast<uint8>(ns.uptimeSecs & 0xFF));
        payload.push_back(static_cast<uint8>((ns.uptimeSecs >> 8) & 0xFF));
        payload.push_back(static_cast<uint8>((ns.uptimeSecs >> 16) & 0xFF));
        payload.push_back(static_cast<uint8>((ns.uptimeSecs >> 24) & 0xFF));

        // tx_bps (LE uint32) — bytes/sec client→worldserver
        payload.push_back(static_cast<uint8>(txBps & 0xFF));
        payload.push_back(static_cast<uint8>((txBps >> 8) & 0xFF));
        payload.push_back(static_cast<uint8>((txBps >> 16) & 0xFF));
        payload.push_back(static_cast<uint8>((txBps >> 24) & 0xFF));

        // rx_bps (LE uint32) — bytes/sec worldserver→client
        payload.push_back(static_cast<uint8>(rxBps & 0xFF));
        payload.push_back(static_cast<uint8>((rxBps >> 8) & 0xFF));
        payload.push_back(static_cast<uint8>((rxBps >> 16) & 0xFF));
        payload.push_back(static_cast<uint8>((rxBps >> 24) & 0xFF));

        // latency_ms (LE uint16) — control-channel RTT in ms
        uint32 latMs = _nodeLatencyMs.count(ns.nodeId) ? _nodeLatencyMs.at(ns.nodeId) : 0;
        uint16 latMs16 = static_cast<uint16>(std::min(latMs, uint32{65535}));
        payload.push_back(static_cast<uint8>(latMs16 & 0xFF));
        payload.push_back(static_cast<uint8>((latMs16 >> 8) & 0xFF));

        // addr_len + addr
        payload.push_back(static_cast<uint8>(ns.address.size()));
        payload.insert(payload.end(), ns.address.begin(), ns.address.end());

        // port (LE uint16)
        payload.push_back(static_cast<uint8>(ns.port & 0xFF));
        payload.push_back(static_cast<uint8>((ns.port >> 8) & 0xFF));
    }

    return payload;
}

void ProxyMgr::OnNodePong(uint8 nodeId, uint32 latencyMs)
{
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        _nodeLatencyMs[nodeId] = latencyMs;
        if (_nodeStatus.count(nodeId))
            _nodeStatus[nodeId].latencyMs = latencyMs;
    }
    PushStatusToSubscribers();
}

void ProxyMgr::SendPingsToAllNodes()
{
    // Called periodically (e.g. every 5s) to measure control-channel latency.
    // Each ControlSocket handles its own _pingSentAt timestamp.
    std::lock_guard<std::mutex> lock(_nodeMutex);
    for (auto& [nodeId, ws] : _nodes)
    {
        if (auto sock = ws.lock())
            sock->SendPing();
    }
}

void ProxyMgr::AddNodeTraffic(uint8 nodeId, uint64 txBytes, uint64 rxBytes)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    _nodeTxBytes[nodeId] += txBytes;
    _nodeRxBytes[nodeId] += rxBytes;
}

void ProxyMgr::PushStatusToSubscribers()
{
    std::vector<uint8> payload = BuildStatusPayload();

    std::lock_guard<std::mutex> lock(_mgmtMutex);
    _mgmtSubscribers.erase(
        std::remove_if(_mgmtSubscribers.begin(), _mgmtSubscribers.end(),
            [&payload](std::weak_ptr<ManagementSocket> const& wp)
            {
                auto sock = wp.lock();
                if (!sock || !sock->IsOpen() || !sock->IsSubscribed())
                    return true; // prune stale/unsubscribed entries
                sock->SendStatusPush(payload);
                return false;
            }),
        _mgmtSubscribers.end());
}

// ── Cluster player directory ──────────────────────────────────────────────────

void ProxyMgr::OnPlayerOnline(uint64 guid, uint8 nodeId, std::string name,
                               uint32 zoneId, uint8 level, uint8 classId, uint8 raceId, uint8 teamId)
{
    ClusterPlayerInfo info;
    info.guid = guid;  info.nodeId = nodeId;  info.name = name;
    info.zoneId = zoneId;  info.level = level;  info.classId = classId;
    info.raceId = raceId;  info.teamId = teamId;

    {
        std::lock_guard<std::mutex> lock(_dirMutex);
        _playerByGuid[guid] = info;
        _playerByName[name] = guid;
    }

    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        _nodePlayerCounts[nodeId]++;
        if (_nodeStatus.count(nodeId))
            _nodeStatus[nodeId].playerCount = _nodePlayerCounts[nodeId];
    }

    LOG_INFO("proxy", "ProxyMgr: Player ONLINE  GUID {:016X} '{}' node={}", guid, name, nodeId);

    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + name.size() + 4 + 4);
    msg.push_back(0x03);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((guid >> (i * 8)) & 0xFF));
    msg.push_back(static_cast<uint8>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());
    msg.push_back(static_cast<uint8>(zoneId & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 8) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 16) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 24) & 0xFF));
    msg.push_back(level); msg.push_back(classId); msg.push_back(raceId); msg.push_back(teamId);
    msg.push_back(nodeId);
    BroadcastToOtherNodes(msg, nodeId);

    PushStatusToSubscribers();
}

void ProxyMgr::OnPlayerOffline(uint64 guid, uint8 nodeId)
{
    {
        std::lock_guard<std::mutex> lock(_dirMutex);
        auto it = _playerByGuid.find(guid);
        if (it != _playerByGuid.end())
        {
            _playerByName.erase(it->second.name);
            _playerByGuid.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        auto it = _nodePlayerCounts.find(nodeId);
        if (it != _nodePlayerCounts.end() && it->second > 0)
            it->second--;
        if (_nodeStatus.count(nodeId))
            _nodeStatus[nodeId].playerCount = _nodePlayerCounts[nodeId];
    }

    std::vector<uint8> msg(9);
    msg[0] = 0x04;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((guid >> (i * 8)) & 0xFF);
    BroadcastToOtherNodes(msg, nodeId);

    PushStatusToSubscribers();
}

// ── Cross-node packet delivery ────────────────────────────────────────────────

void ProxyMgr::DeliverPacketToPlayer(uint64 targetGuid, std::vector<uint8> packetData)
{
    auto socket = GetSession(targetGuid);
    if (!socket)
    {
        LOG_WARN("proxy", "ProxyMgr: DeliverPacket — GUID {:016X} not found", targetGuid);
        return;
    }
    if (!socket->IsOpen())
    {
        LOG_WARN("proxy", "ProxyMgr: DeliverPacket — GUID {:016X} socket closed", targetGuid);
        return;
    }

    if (packetData.size() < 4)
    {
        LOG_ERROR("proxy", "ProxyMgr: DeliverPacket — packet too short ({} bytes)", packetData.size());
        return;
    }

    std::size_t headerLen = (packetData[0] & 0x80) ? 5 : 4;
    if (packetData.size() < headerLen)
    {
        LOG_ERROR("proxy", "ProxyMgr: DeliverPacket — shorter than header ({} < {})",
                  packetData.size(), headerLen);
        return;
    }

    MessageBuffer payloadBuf(packetData.size() - headerLen);
    if (packetData.size() > headerLen)
        payloadBuf.Write(packetData.data() + headerLen, packetData.size() - headerLen);

    socket->QueuePacketForClient(packetData.data(), headerLen, payloadBuf);
}

// ── Group state management ────────────────────────────────────────────────────

void ProxyMgr::OnGroupUpdate(uint64 groupGuid, uint8 sourceNodeId, uint8 memberCount, std::vector<uint8> memberData)
{
    std::vector<ProxyGroupMember> members;
    members.reserve(memberCount);

    uint8 const* p = memberData.data();
    for (uint8 i = 0; i < memberCount && (p + 11) <= (memberData.data() + memberData.size()); ++i, p += 11)
    {
        ProxyGroupMember m;
        std::memcpy(&m.guid, p, 8);
        m.subgroup = p[8]; m.roleFlags = p[9]; m.nodeId = p[10];
        members.push_back(m);
    }

    {
        std::lock_guard<std::mutex> lock(_groupMutex);
        _groupMembers[groupGuid] = members;
    }

    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + memberData.size());
    msg.push_back(0x07);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF));
    msg.push_back(memberCount);
    msg.insert(msg.end(), memberData.begin(), memberData.end());

    std::unordered_map<uint8, bool> targetNodes;
    for (auto const& m : members)
        targetNodes[m.nodeId] = true;

    std::lock_guard<std::mutex> lock(_nodeMutex);
    for (auto const& [nodeId, _] : targetNodes)
    {
        if (nodeId == sourceNodeId)
            continue;
        auto it = _nodes.find(nodeId);
        if (it != _nodes.end())
            if (auto socket = it->second.lock())
                socket->SendRaw(msg);
    }
}

void ProxyMgr::OnGroupDisband(uint64 groupGuid, uint8 /*sourceNodeId*/)
{
    std::vector<ProxyGroupMember> members;
    {
        std::lock_guard<std::mutex> lock(_groupMutex);
        auto it = _groupMembers.find(groupGuid);
        if (it != _groupMembers.end())
        {
            members = it->second;
            _groupMembers.erase(it);
        }
    }

    std::vector<uint8> msg(9);
    msg[0] = 0x08;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF);

    std::lock_guard<std::mutex> lock(_nodeMutex);
    std::unordered_map<uint8, bool> seen;
    for (auto const& m : members)
    {
        if (seen[m.nodeId]) continue;
        seen[m.nodeId] = true;
        auto it = _nodes.find(m.nodeId);
        if (it != _nodes.end())
            if (auto socket = it->second.lock())
                socket->SendRaw(msg);
    }
}

// ── LFG master-node relay ─────────────────────────────────────────────────────

void ProxyMgr::RelayToLFGMaster(uint8 sourceNodeId, std::vector<uint8> payload)
{
    std::vector<uint8> msg;
    msg.reserve(1 + 2 + 1 + payload.size());
    uint16 innerLen = static_cast<uint16>(1 + payload.size());
    msg.push_back(0x09);
    msg.push_back(static_cast<uint8>(innerLen & 0xFF));
    msg.push_back(static_cast<uint8>(innerLen >> 8));
    msg.push_back(sourceNodeId);
    msg.insert(msg.end(), payload.begin(), payload.end());

    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(_lfgMasterNodeId);
    if (it == _nodes.end())
    {
        LOG_WARN("proxy", "ProxyMgr: RelayToLFGMaster — master node {} not registered", _lfgMasterNodeId);
        return;
    }
    if (auto socket = it->second.lock())
        socket->SendRaw(msg);
}

// ── Node relay & broadcast helpers ────────────────────────────────────────────

void ProxyMgr::RelayToNode(uint8 targetNodeId, std::vector<uint8> const& msg)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(targetNodeId);
    if (it == _nodes.end())
    {
        LOG_WARN("proxy", "ProxyMgr: RelayToNode — node {} not registered", targetNodeId);
        return;
    }
    if (auto socket = it->second.lock())
        socket->SendRaw(msg);
    else
        LOG_WARN("proxy", "ProxyMgr: RelayToNode — node {} socket expired", targetNodeId);
}

void ProxyMgr::BroadcastToOtherNodes(std::vector<uint8> const& data, uint8 excludeNodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    for (auto& [nodeId, weakSocket] : _nodes)
    {
        if (nodeId == excludeNodeId)
            continue;
        if (auto socket = weakSocket.lock())
            socket->SendRaw(data);
    }
}

// ── Cross-node unit update broadcast ─────────────────────────────────────────

void ProxyMgr::BroadcastUnitUpdate(uint8 sourceNodeId, uint16 payloadLen, std::vector<uint8> const& payload)
{
    std::vector<uint8> msg;
    msg.reserve(3 + payload.size());
    msg.push_back(0x0C); // MSG_CLUSTER_UNIT_UPDATE
    msg.push_back(static_cast<uint8>(payloadLen & 0xFF));
    msg.push_back(static_cast<uint8>(payloadLen >> 8));
    msg.insert(msg.end(), payload.begin(), payload.end());
    BroadcastToOtherNodes(msg, sourceNodeId);
}

// ── Dynamic map routing (populated at node registration) ─────────────────────

void ProxyMgr::RegisterNodeMaps(uint8 nodeId, std::vector<uint32> const& maps)
{
    // WoW 3.3.5a (build 12340) has no valid map ID beyond this value.
    // Reject anything higher to prevent routing-table pollution from a
    // misconfigured or malicious backend.
    static constexpr uint32 MAX_VALID_MAP_ID = 720;

    std::lock_guard<std::mutex> lock(_mapRoutingMutex);
    for (uint32 mapId : maps)
    {
        if (mapId > MAX_VALID_MAP_ID)
        {
            LOG_WARN("proxy", "ProxyMgr: RegisterNodeMaps — node {} sent invalid mapId {}; skipping",
                     nodeId, mapId);
            continue;
        }
        _mapRouting[mapId] = nodeId;
        LOG_INFO("proxy", "ProxyMgr: Dynamic routing map {} → node {}", mapId, nodeId);
    }
}
