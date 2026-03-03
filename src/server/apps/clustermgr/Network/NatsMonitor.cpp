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

#include "NatsMonitor.h"
#include "Log.h"
#include <nats.h>
#include <cstring>
#include <algorithm>

NatsMonitor::NatsMonitor() = default;

NatsMonitor::~NatsMonitor()
{
    _watchdogRunning = false;
    if (_watchdog.joinable())
        _watchdog.join();

    if (_subPlayers)
    {
        natsSubscription_Unsubscribe(_subPlayers);
        natsSubscription_Destroy(_subPlayers);
    }
    if (_subStatus)
    {
        natsSubscription_Unsubscribe(_subStatus);
        natsSubscription_Destroy(_subStatus);
    }
    if (_nc)
        natsConnection_Destroy(_nc);
}

void NatsMonitor::Start(std::string const& natsUrl, StatusCallback statusCb,
                        uint32 deadThresholdSecs)
{
    _statusCb           = std::move(statusCb);
    _deadThresholdSecs  = deadThresholdSecs;

    natsStatus s = natsConnection_ConnectTo(&_nc, natsUrl.c_str());
    if (s != NATS_OK)
    {
        LOG_ERROR("clustermgr", "NatsMonitor: Failed to connect to NATS at {} — {}",
                  natsUrl, natsStatus_GetText(s));
        return;
    }

    s = natsConnection_Subscribe(&_subStatus, _nc, "cluster.mgmt.status", OnStatusMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("clustermgr", "NatsMonitor: Failed to subscribe to cluster.mgmt.status — {}",
                  natsStatus_GetText(s));
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    s = natsConnection_Subscribe(&_subPlayers, _nc, "cluster.mgmt.players", OnPlayersMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("clustermgr", "NatsMonitor: Failed to subscribe to cluster.mgmt.players — {}",
                  natsStatus_GetText(s));
        // Non-fatal: player positions are optional.
    }

    _connected = true;
    LOG_INFO("clustermgr", "NatsMonitor: Connected to NATS at {} and subscribed.", natsUrl);

    _watchdogRunning = true;
    _watchdog = std::thread(&NatsMonitor::WatchdogLoop, this);
}

// ── NATS callbacks (run on NATS internal thread) ──────────────────────────────

void NatsMonitor::OnStatusMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                              natsMsg* msg, void* closure)
{
    static_cast<NatsMonitor*>(closure)->ParseStatus(msg);
    natsMsg_Destroy(msg);
}

void NatsMonitor::OnPlayersMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                               natsMsg* msg, void* closure)
{
    static_cast<NatsMonitor*>(closure)->ParsePlayers(msg);
    natsMsg_Destroy(msg);
}

// ── Parsing ───────────────────────────────────────────────────────────────────

void NatsMonitor::ParseStatus(natsMsg* msg)
{
    int  dataLen = natsMsg_GetDataLength(msg);
    auto data    = reinterpret_cast<uint8 const*>(natsMsg_GetData(msg));

    // Minimum fixed part: nodeId(1)+state(1)+players(2)+max(2)+pid(4)+uptime(4)+
    //   mem(4)+cpu(1)+crash(2)+txBps(4)+rxBps(4)+mapCount(1) = 30 bytes
    static constexpr int FIXED_MIN = 30;
    if (dataLen < FIXED_MIN)
    {
        LOG_WARN("clustermgr", "NatsMonitor: mgmt.status too short ({}B)", dataLen);
        return;
    }

    int off = 0;
    NodeInfo n;
    n.nodeId = data[off++];
    n.state  = data[off++];

    std::memcpy(&n.playerCount, data + off, 2); off += 2;
    std::memcpy(&n.maxPlayers,  data + off, 2); off += 2;
    std::memcpy(&n.pid,         data + off, 4); off += 4;
    std::memcpy(&n.uptimeSecs,  data + off, 4); off += 4;
    std::memcpy(&n.memUsageMB,  data + off, 4); off += 4;
    n.cpuPercent = data[off++];
    std::memcpy(&n.crashCount,  data + off, 2); off += 2;
    std::memcpy(&n.txBps,       data + off, 4); off += 4;
    std::memcpy(&n.rxBps,       data + off, 4); off += 4;

    uint8 mapCount = data[off++];
    if (off + static_cast<int>(mapCount) * 4 + 1 > dataLen)
    {
        LOG_WARN("clustermgr", "NatsMonitor: mgmt.status truncated (node {})", n.nodeId);
        return;
    }
    n.mapIds.reserve(mapCount);
    for (uint8 i = 0; i < mapCount; ++i)
    {
        uint32 mapId = 0;
        std::memcpy(&mapId, data + off, 4); off += 4;
        n.mapIds.push_back(mapId);
    }

    uint8 addrLen = data[off++];
    if (addrLen > 0 && off + static_cast<int>(addrLen) <= dataLen)
    {
        n.address.assign(reinterpret_cast<char const*>(data + off), addrLen);
        off += addrLen;
    }

    n.lastSeen = std::chrono::steady_clock::now();

    // Update node map under lock; preserve ICMP latency.
    std::vector<NodeInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        auto it = _nodes.find(n.nodeId);
        if (it != _nodes.end())
            n.latencyMs = it->second.latencyMs;  // keep ICMP measurement
        _nodes[n.nodeId] = std::move(n);

        snapshot.reserve(_nodes.size());
        for (auto const& [id, node] : _nodes)
            snapshot.push_back(node);
    }

    if (_statusCb)
        _statusCb(std::move(snapshot));
}

void NatsMonitor::ParsePlayers(natsMsg* msg)
{
    int  dataLen = natsMsg_GetDataLength(msg);
    auto data    = reinterpret_cast<uint8 const*>(natsMsg_GetData(msg));

    // Header: nodeId(1) + playerCount(2) = 3 bytes minimum
    if (dataLen < 3)
        return;

    int off = 0;
    uint8  nodeId = data[off++];
    uint16 count  = 0;
    std::memcpy(&count, data + off, 2); off += 2;

    std::vector<PlayerInfo> newPlayers;
    newPlayers.reserve(count);

    for (uint16 i = 0; i < count; ++i)
    {
        // Per-player fixed: guid(8)+mapId(2)+x(4)+y(4)+z(4)+zoneId(2)+level(1)+class(1)+race(1)+team(1)+nameLen(1) = 29
        static constexpr int PER_PLAYER_FIXED = 29;
        if (off + PER_PLAYER_FIXED > dataLen)
            break;

        PlayerInfo p;
        p.nodeId = nodeId;
        std::memcpy(&p.guid,   data + off, 8); off += 8;
        std::memcpy(&p.mapId,  data + off, 2); off += 2;
        std::memcpy(&p.x,      data + off, 4); off += 4;
        std::memcpy(&p.y,      data + off, 4); off += 4;
        std::memcpy(&p.z,      data + off, 4); off += 4;
        std::memcpy(&p.zoneId, data + off, 2); off += 2;
        p.level   = data[off++];
        p.classId = data[off++];
        p.raceId  = data[off++];
        p.teamId  = data[off++];
        uint8 nameLen = data[off++];
        if (nameLen > 0)
        {
            if (off + static_cast<int>(nameLen) > dataLen)
                break;
            p.name.assign(reinterpret_cast<char const*>(data + off), nameLen);
            off += nameLen;
        }
        newPlayers.push_back(std::move(p));
    }

    // Replace all players from this node atomically.
    {
        std::lock_guard<std::mutex> lock(_playersMutex);
        // Remove old entries from this node, then append new ones.
        _players.erase(
            std::remove_if(_players.begin(), _players.end(),
                           [nodeId](PlayerInfo const& p) { return p.nodeId == nodeId; }),
            _players.end());
        for (auto& p : newPlayers)
            _players.push_back(std::move(p));
    }
}

// ── Watchdog (dead-node detection) ────────────────────────────────────────────

void NatsMonitor::WatchdogLoop()
{
    // Check every 5 seconds whether any node has gone silent.
    while (_watchdogRunning)
    {
        for (int i = 0; i < 10 && _watchdogRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now = std::chrono::steady_clock::now();
        auto threshold = std::chrono::seconds(_deadThresholdSecs);

        bool changed = false;
        std::vector<NodeInfo> snapshot;
        {
            std::lock_guard<std::mutex> lock(_nodesMutex);
            for (auto& [id, node] : _nodes)
            {
                if (node.state == 3 &&  // was RUNNING
                    node.lastSeen != std::chrono::steady_clock::time_point{} &&
                    now - node.lastSeen > threshold)
                {
                    node.state = 5;  // CRASHED
                    changed = true;
                    LOG_WARN("clustermgr",
                             "NatsMonitor: Node {} declared CRASHED (no heartbeat for {}s)",
                             id, _deadThresholdSecs);
                }
            }
            if (changed)
            {
                snapshot.reserve(_nodes.size());
                for (auto const& [id, node] : _nodes)
                    snapshot.push_back(node);
            }
        }

        if (changed && _statusCb)
            _statusCb(std::move(snapshot));
    }
}

// ── Public accessors ──────────────────────────────────────────────────────────

std::vector<NodeInfo> NatsMonitor::GetNodes() const
{
    std::lock_guard<std::mutex> lock(_nodesMutex);
    std::vector<NodeInfo> out;
    out.reserve(_nodes.size());
    for (auto const& [id, node] : _nodes)
        out.push_back(node);
    return out;
}

std::vector<PlayerInfo> NatsMonitor::GetPlayers() const
{
    std::lock_guard<std::mutex> lock(_playersMutex);
    return _players;
}

void NatsMonitor::SendStartNode(uint8 nodeId)
{
    LOG_INFO("clustermgr", "NatsMonitor::SendStartNode({}) — not yet implemented via NATS control channel", nodeId);
}

void NatsMonitor::SendStopNode(uint8 nodeId)
{
    LOG_INFO("clustermgr", "NatsMonitor::SendStopNode({}) — not yet implemented via NATS control channel", nodeId);
}
