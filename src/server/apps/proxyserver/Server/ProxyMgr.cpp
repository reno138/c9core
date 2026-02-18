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

#include "ProxyMgr.h"
#include "Config.h"
#include "ControlSocket.h"
#include "Log.h"
#include "ProxySocket.h"
#include <cstring>

// ── Node load-balancing config ────────────────────────────────────────────────

void ProxyMgr::LoadNodeConfig()
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    uint32 count = sConfigMgr->GetOption<uint32>("WorldServer.Node.Count", 0);
    if (count == 0)
    {
        // Legacy single-node config.
        std::string addr = sConfigMgr->GetOption<std::string>("WorldServer.Address", "127.0.0.1");
        uint16 port = static_cast<uint16>(sConfigMgr->GetOption<int32>("WorldServer.Port", 8086));
        _nodeAddresses[1] = { addr, port };
        _nodePlayerCounts[1] = 0;
        LOG_INFO("proxy", "ProxyMgr: Single-node mode — backend {}:{}", addr, port);
        return;
    }

    uint32 loaded = 0;
    for (uint32 i = 1; i <= count && i <= 5; ++i)
    {
        std::string keyAddr = "WorldServer.Node." + std::to_string(i) + ".Address";
        std::string keyPort = "WorldServer.Node." + std::to_string(i) + ".Port";
        std::string addr = sConfigMgr->GetOption<std::string>(keyAddr, "127.0.0.1");
        uint16 port = static_cast<uint16>(sConfigMgr->GetOption<int32>(keyPort, 8086 + static_cast<int32>(i) - 1));
        _nodeAddresses[static_cast<uint8>(i)] = { addr, port };
        _nodePlayerCounts[static_cast<uint8>(i)] = 0;
        LOG_INFO("proxy", "ProxyMgr: Configured node {} → {}:{}", i, addr, port);
        ++loaded;
    }

    LOG_INFO("proxy", "ProxyMgr: {} worldserver node(s) configured for load balancing", loaded);
}

std::pair<std::string, uint16> ProxyMgr::ChooseLeastLoadedNode()
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    if (_nodeAddresses.empty())
        return { "127.0.0.1", 8086 };

    uint8 best = _nodeAddresses.begin()->first;
    uint32 minCount = _nodePlayerCounts.count(best) ? _nodePlayerCounts[best] : 0;

    for (auto const& [nodeId, addr] : _nodeAddresses)
    {
        uint32 cnt = _nodePlayerCounts.count(nodeId) ? _nodePlayerCounts.at(nodeId) : 0;
        if (cnt < minCount)
        {
            best = nodeId;
            minCount = cnt;
        }
    }

    LOG_INFO("proxy", "ProxyMgr: ChooseLeastLoadedNode → node {} ({}:{}, {} players)",
             best, _nodeAddresses[best].first, _nodeAddresses[best].second, minCount);
    return _nodeAddresses[best];
}

// ── Session registry ──────────────────────────────────────────────────────────

void ProxyMgr::RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions[guid] = socket;
    LOG_DEBUG("proxy", "ProxyMgr: Registered GUID {:016X} ({} total sessions)",
              guid, _sessions.size());
}

void ProxyMgr::UnregisterSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions.erase(guid);
    LOG_DEBUG("proxy", "ProxyMgr: Unregistered GUID {:016X} ({} total sessions)",
              guid, _sessions.size());
}

std::shared_ptr<ProxySocket> ProxyMgr::GetSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _sessions.find(guid);
    if (it == _sessions.end())
        return nullptr;

    auto socket = it->second.lock();
    if (!socket)
        _sessions.erase(it); // stale entry

    return socket;
}

void ProxyMgr::ReroutePlayer(uint64 guid, std::string const& address, uint16 port)
{
    auto socket = GetSession(guid);
    if (!socket)
    {
        LOG_WARN("proxy", "ProxyMgr: ReroutePlayer — GUID {:016X} not found (player not connected?)",
                 guid);
        return;
    }

    LOG_INFO("proxy", "ProxyMgr: Rerouting GUID {:016X} to {}:{}", guid, address, port);
    socket->RerouteToBackend(address, port);
}

// ── Node registry ─────────────────────────────────────────────────────────────

uint8 ProxyMgr::RegisterNode(std::shared_ptr<ControlSocket> socket, uint8 serverType, uint16 gamePort)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    // Only worldserver nodes (type 0) get cluster node IDs 1–5.
    // Instance servers (type 1) get node ID 0 — they are not cluster peers.
    if (serverType != 0)
        return 0;

    // Try to match the incoming game_port to a configured node address entry.
    for (auto const& [nodeId, addrPort] : _nodeAddresses)
    {
        if (addrPort.second == gamePort && !_nodes.count(nodeId))
        {
            _nodes[nodeId] = socket;
            LOG_INFO("proxy", "ProxyMgr: Node {} matched by game_port={} ({} nodes total)",
                     nodeId, gamePort, _nodes.size());
            return nodeId;
        }
    }

    // No config match — assign sequentially, skipping already-occupied IDs.
    while (_nextNodeId <= 5 && _nodes.count(_nextNodeId))
        _nextNodeId++;

    if (_nextNodeId > 5)
    {
        LOG_ERROR("proxy", "ProxyMgr: RegisterNode — maximum of 5 worldserver nodes reached, rejecting");
        return 0;
    }

    uint8 nodeId = _nextNodeId++;
    _nodes[nodeId] = socket;

    LOG_INFO("proxy", "ProxyMgr: Assigned node_id={} (game_port={}, {} nodes total)",
             nodeId, gamePort, _nodes.size());
    return nodeId;
}

void ProxyMgr::UnregisterNode(uint8 nodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    _nodes.erase(nodeId);
    LOG_INFO("proxy", "ProxyMgr: Node {} unregistered ({} nodes remaining)", nodeId, _nodes.size());
}

// ── Cluster player directory ──────────────────────────────────────────────────

void ProxyMgr::OnPlayerOnline(uint64 guid, uint8 nodeId, std::string name,
                               uint32 zoneId, uint8 level, uint8 classId, uint8 raceId, uint8 teamId)
{
    ClusterPlayerInfo info;
    info.guid    = guid;
    info.nodeId  = nodeId;
    info.name    = name;   // already lowercase from worldserver
    info.zoneId  = zoneId;
    info.level   = level;
    info.classId = classId;
    info.raceId  = raceId;
    info.teamId  = teamId;

    {
        std::lock_guard<std::mutex> lock(_dirMutex);
        _playerByGuid[guid] = info;
        _playerByName[name] = guid;
    }

    // Track per-node player count for load balancing.
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        _nodePlayerCounts[nodeId]++;
    }

    LOG_INFO("proxy", "ProxyMgr: Player ONLINE  GUID {:016X} '{}' node={} — broadcasting to other nodes", guid, name, nodeId);

    // Broadcast to all OTHER worldserver nodes.
    // Wire format: MSG_CLUSTER_PLAYER_ONLINE + payload matching the original message.
    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + name.size() + 4 + 4);
    msg.push_back(0x03); // MSG_CLUSTER_PLAYER_ONLINE

    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((guid >> (i * 8)) & 0xFF));

    msg.push_back(static_cast<uint8>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());

    // zone_id (LE uint32)
    msg.push_back(static_cast<uint8>(zoneId & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 8) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 16) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 24) & 0xFF));

    msg.push_back(level);
    msg.push_back(classId);
    msg.push_back(raceId);
    msg.push_back(teamId);
    msg.push_back(nodeId); // node_id — lets worldservers route relays

    BroadcastToOtherNodes(msg, nodeId);
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

    // Decrement per-node player count.
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        auto it = _nodePlayerCounts.find(nodeId);
        if (it != _nodePlayerCounts.end() && it->second > 0)
            it->second--;
    }

    LOG_DEBUG("proxy", "ProxyMgr: Player offline GUID {:016X} node={}", guid, nodeId);

    // Broadcast to all OTHER worldserver nodes.
    std::vector<uint8> msg(9);
    msg[0] = 0x04; // MSG_CLUSTER_PLAYER_OFFLINE
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((guid >> (i * 8)) & 0xFF);

    BroadcastToOtherNodes(msg, nodeId);
}

// ── Cross-node packet delivery ────────────────────────────────────────────────

void ProxyMgr::DeliverPacketToPlayer(uint64 targetGuid, std::vector<uint8> packetData)
{
    auto socket = GetSession(targetGuid);
    if (!socket)
    {
        LOG_WARN("proxy", "ProxyMgr: DeliverPacket — GUID {:016X} not found (player gone?)", targetGuid);
        return;
    }

    if (!socket->IsOpen())
    {
        LOG_WARN("proxy", "ProxyMgr: DeliverPacket — GUID {:016X} socket already closed (disconnecting?)", targetGuid);
        return;
    }

    // Split into header and payload so QueuePacketForClient can re-encrypt.
    // The header is either 4 bytes (normal) or 5 bytes (large packet, high bit set on first byte).
    if (packetData.size() < 4)
    {
        LOG_ERROR("proxy", "ProxyMgr: DeliverPacket — packet too short ({} bytes)", packetData.size());
        return;
    }

    std::size_t headerLen = (packetData[0] & 0x80) ? 5 : 4;
    if (packetData.size() < headerLen)
    {
        LOG_ERROR("proxy", "ProxyMgr: DeliverPacket — packet shorter than header ({} < {})",
                  packetData.size(), headerLen);
        return;
    }

    MessageBuffer payloadBuf(packetData.size() - headerLen);
    if (packetData.size() > headerLen)
    {
        payloadBuf.Write(packetData.data() + headerLen, packetData.size() - headerLen);
    }

    socket->QueuePacketForClient(packetData.data(), headerLen, payloadBuf);
}

// ── Group state management ────────────────────────────────────────────────────

void ProxyMgr::OnGroupUpdate(uint64 groupGuid, uint8 sourceNodeId, uint8 memberCount, std::vector<uint8> memberData)
{
    // Parse member data: each member is [uint64 guid, uint8 subgroup, uint8 role, uint8 nodeId] = 11 bytes
    std::vector<ProxyGroupMember> members;
    members.reserve(memberCount);

    uint8 const* p = memberData.data();
    for (uint8 i = 0; i < memberCount && (p + 11) <= (memberData.data() + memberData.size()); ++i, p += 11)
    {
        ProxyGroupMember m;
        std::memcpy(&m.guid, p, 8);
        m.subgroup  = p[8];
        m.roleFlags = p[9];
        m.nodeId    = p[10];
        members.push_back(m);
    }

    {
        std::lock_guard<std::mutex> lock(_groupMutex);
        _groupMembers[groupGuid] = members;
    }

    // Build the broadcast message and send to all member nodes (including source for confirmation).
    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + memberData.size());
    msg.push_back(0x07); // MSG_CLUSTER_GROUP_UPDATE
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF));
    msg.push_back(memberCount);
    msg.insert(msg.end(), memberData.begin(), memberData.end());

    // Collect distinct node IDs that have at least one group member.
    std::unordered_map<uint8, bool> targetNodes;
    for (auto const& m : members)
        targetNodes[m.nodeId] = true;

    std::lock_guard<std::mutex> lock(_nodeMutex);
    for (auto const& [nodeId, _] : targetNodes)
    {
        if (nodeId == sourceNodeId)
            continue; // Source already has the latest state
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

    // Build disband message.
    std::vector<uint8> msg(9);
    msg[0] = 0x08; // MSG_CLUSTER_GROUP_DISBAND
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF);

    // Broadcast to all member nodes.
    std::lock_guard<std::mutex> lock(_nodeMutex);
    std::unordered_map<uint8, bool> seen;
    for (auto const& m : members)
    {
        if (seen[m.nodeId])
            continue;
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
    // The LFG relay wire format sent TO the master includes the source nodeId
    // so the master's LFGMgr can route responses back.  Wire:
    //   uint8  source_node_id
    //   uint8  payload[...]
    std::vector<uint8> msg;
    msg.reserve(1 + 2 + 1 + payload.size()); // type + len(2) + source(1) + payload

    // Wrap in MSG_CLUSTER_LFG_RELAY with a prepended source_node_id byte.
    uint16 innerLen = static_cast<uint16>(1 + payload.size()); // source byte + payload
    msg.push_back(0x09); // MSG_CLUSTER_LFG_RELAY
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
    else
        LOG_WARN("proxy", "ProxyMgr: RelayToLFGMaster — master node {} socket expired", _lfgMasterNodeId);
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
