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
#include "ClusterAuth.h"
#include "ClusterMgmtProtocol.h"
#include "Log.h"
#include <nats.h>
#include <cstring>
#include <algorithm>

NatsMonitor::NatsMonitor() = default;

NatsMonitor::~NatsMonitor()
{
    Stop();
}

void NatsMonitor::Stop()
{
    _watchdogRunning = false;
    if (_watchdog.joinable())
        _watchdog.join();

    // Unsubscribe (synchronously stops delivery) before destroying, and cover
    // all three subscriptions — the nodemgr one used to be left armed here and
    // could write _supervisors while this object was being destroyed.
    for (natsSubscription** sub : { &_subNodeMgr, &_subPlayers, &_subStatus })
    {
        if (*sub)
        {
            natsSubscription_Unsubscribe(*sub);
            natsSubscription_Destroy(*sub);
            *sub = nullptr;
        }
    }
    if (_nc)
    {
        natsConnection_Destroy(_nc);
        _nc = nullptr;
    }
    _connected = false;
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
    // Supervisor heartbeats. Separate from cluster.mgmt.status (published by the
    // worldserver) precisely so the two can disagree — that disagreement is the
    // hung-node signal.
    s = natsConnection_Subscribe(&_subNodeMgr, _nc, "cluster.mgmt.nodemgr", OnNodeMgrMsg, this);
    if (s != NATS_OK)
        LOG_WARN("clustermgr", "NatsMonitor: Failed to subscribe to cluster.mgmt.nodemgr — {}",
                 natsStatus_GetText(s));

    LOG_INFO("clustermgr", "NatsMonitor: Connected to NATS at {} and subscribed.", natsUrl);

    _watchdogRunning = true;
    _watchdog = std::thread(&NatsMonitor::WatchdogLoop, this);
}

// ── NATS callbacks (run on NATS internal thread) ──────────────────────────────

// The management feeds are sealed by the worldserver (NatsBus::PublishSealed).
// Before that, anyone on the NATS port could invent nodes, states and players,
// and the address/name strings landed in the operator's browser. Open() the
// frame, check the type, and then require the body's nodeId to match the
// frame's authenticated sender so one node cannot report as another.
static bool OpenMgmtFrame(natsMsg* msg, uint8 expectedType, char const* what,
                          uint8& outSrc, std::vector<uint8>& outBody)
{
    uint8 const* d = reinterpret_cast<uint8 const*>(natsMsg_GetData(msg));
    int          n = natsMsg_GetDataLength(msg);
    uint8 msgType = 0;
    bool const ok = ClusterAuth::Open(d, static_cast<std::size_t>(n), outSrc, msgType, outBody);
    natsMsg_Destroy(msg);
    if (!ok)
    {
        LOG_WARN("clustermgr", "NatsMonitor: dropped unauthenticated/replayed {} frame ({} bytes)", what, n);
        return false;
    }
    if (msgType != expectedType)
    {
        LOG_WARN("clustermgr", "NatsMonitor: {} frame from node {} has msgType 0x{:02X} — dropped",
                 what, outSrc, msgType);
        return false;
    }
    return true;
}

void NatsMonitor::OnStatusMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                              natsMsg* msg, void* closure)
{
    uint8 src = 0;
    std::vector<uint8> body;
    if (OpenMgmtFrame(msg, ClusterMgmt::MSG_MGMT_STATUS, "mgmt.status", src, body))
        static_cast<NatsMonitor*>(closure)->ParseStatus(src, body);
}

void NatsMonitor::OnPlayersMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                               natsMsg* msg, void* closure)
{
    uint8 src = 0;
    std::vector<uint8> body;
    if (OpenMgmtFrame(msg, ClusterMgmt::MSG_MGMT_PLAYERS, "mgmt.players", src, body))
        static_cast<NatsMonitor*>(closure)->ParsePlayers(src, body);
}

// ── Parsing ───────────────────────────────────────────────────────────────────

void NatsMonitor::ParseStatus(uint8 srcNode, std::vector<uint8> const& body)
{
    int  dataLen = static_cast<int>(body.size());
    auto data    = body.data();

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

    if (n.nodeId != srcNode)
    {
        LOG_WARN("clustermgr", "NatsMonitor: mgmt.status from node {} claims nodeId {} — dropped", srcNode, n.nodeId);
        return;
    }

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

void NatsMonitor::ParsePlayers(uint8 srcNode, std::vector<uint8> const& body)
{
    int  dataLen = static_cast<int>(body.size());
    auto data    = body.data();

    // Header: nodeId(1) + playerCount(2) = 3 bytes minimum
    if (dataLen < 3)
        return;

    int off = 0;
    uint8  nodeId = data[off++];
    uint16 count  = 0;
    std::memcpy(&count, data + off, 2); off += 2;

    if (nodeId != srcNode)
    {
        LOG_WARN("clustermgr", "NatsMonitor: mgmt.players from node {} claims nodeId {} — dropped", srcNode, nodeId);
        return;
    }

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
    std::vector<PlayerInfo> snapshot;
    {
        std::lock_guard<std::mutex> lock(_playersMutex);
        // Remove old entries from this node, then append new ones.
        _players.erase(
            std::remove_if(_players.begin(), _players.end(),
                           [nodeId](PlayerInfo const& p) { return p.nodeId == nodeId; }),
            _players.end());
        for (auto& p : newPlayers)
            _players.push_back(std::move(p));
        if (_playersCb)
            snapshot = _players;
    }

    // Push to the web UI. Nothing used to call this, so the live map only ever
    // showed the players present at page load.
    if (_playersCb)
        _playersCb(std::move(snapshot));
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

void NatsMonitor::SendNodeMgrCommand(uint8 nodeId, uint8 cmd, char const* what)
{
    if (!_nc || !_connected.load())
    {
        LOG_WARN("clustermgr", "NatsMonitor: cannot {} node {} — not connected to NATS", what, nodeId);
        return;
    }

    if (!ClusterAuth::IsInitialised())
    {
        LOG_ERROR("clustermgr", "NatsMonitor: cannot {} node {} — cluster auth not initialised "
                                "(set ClusterMgr.AuthKey)", what, nodeId);
        return;
    }

    // Commands are addressed to the SUPERVISOR, not the worldserver. A hung
    // worldserver cannot act on its own stop request; NodeMgr can.
    std::vector<uint8> frame = ClusterAuth::Seal(0 /*console*/, cmd, nullptr, 0);
    if (frame.empty())
    {
        LOG_ERROR("clustermgr", "NatsMonitor: failed to seal {} command for node {}", what, nodeId);
        return;
    }

    std::string subject = "cluster.nodemgr." + std::to_string(nodeId);
    natsStatus st = natsConnection_Publish(_nc, subject.c_str(),
                                           frame.data(), static_cast<int>(frame.size()));
    if (st != NATS_OK)
        LOG_ERROR("clustermgr", "NatsMonitor: publish {} to {} failed — {}",
                  what, subject, natsStatus_GetText(st));
    else
        LOG_INFO("clustermgr", "NatsMonitor: sent {} to node {}", what, nodeId);
}

void NatsMonitor::SendStartNode(uint8 nodeId)
{
    SendNodeMgrCommand(nodeId, ClusterMgmt::CMD_START, "START");
}

void NatsMonitor::SendStopNode(uint8 nodeId)
{
    SendNodeMgrCommand(nodeId, ClusterMgmt::CMD_STOP, "STOP");
}

void NatsMonitor::SendKillNode(uint8 nodeId)
{
    SendNodeMgrCommand(nodeId, ClusterMgmt::CMD_KILL, "KILL");
}

void NatsMonitor::SendRestartNode(uint8 nodeId)
{
    SendNodeMgrCommand(nodeId, ClusterMgmt::CMD_RESTART, "RESTART");
}

std::vector<NatsMonitor::SupervisorInfo> NatsMonitor::GetSupervisors() const
{
    std::lock_guard<std::mutex> lock(_supMutex);
    std::vector<SupervisorInfo> out;
    out.reserve(_supervisors.size());
    for (auto const& kv : _supervisors)
        out.push_back(kv.second);
    return out;
}

bool NatsMonitor::IsNodeHung(uint8 nodeId, uint32 staleSecs) const
{
    // Supervisor says the process is Running (3)...
    bool supervisorRunning = false;
    {
        std::lock_guard<std::mutex> lock(_supMutex);
        auto it = _supervisors.find(nodeId);
        if (it == _supervisors.end())
            return false;              // no supervisor data — cannot judge
        supervisorRunning = (it->second.state == 3);
    }
    if (!supervisorRunning)
        return false;

    // ...but the worldserver itself has gone quiet on cluster.mgmt.status.
    std::lock_guard<std::mutex> lock(_nodesMutex);
    auto it = _nodes.find(nodeId);
    if (it == _nodes.end())
        return true;                   // process up, never announced — hung

    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - it->second.lastSeen).count();
    return age >= static_cast<long long>(staleSecs);
}

/*static*/
void NatsMonitor::OnNodeMgrMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                               natsMsg* msg, void* closure)
{
    auto* self = static_cast<NatsMonitor*>(closure);

    uint8 const* d = reinterpret_cast<uint8 const*>(natsMsg_GetData(msg));
    int          n = natsMsg_GetDataLength(msg);

    uint8 srcNode = 0, msgType = 0;
    std::vector<uint8> body;
    bool const ok = ClusterAuth::Open(d, static_cast<std::size_t>(n), srcNode, msgType, body);
    natsMsg_Destroy(msg);

    if (!ok || msgType != ClusterMgmt::MSG_NODEMGR_STATUS || body.size() < 11)
        return;

    SupervisorInfo info;
    info.nodeId = body[0];
    if (info.nodeId != srcNode)
        return; // a supervisor may only report on itself
    info.state  = body[1];
    info.pid    = uint32(body[2]) | (uint32(body[3]) << 8) | (uint32(body[4]) << 16) | (uint32(body[5]) << 24);
    info.uptimeSecs = uint32(body[6]) | (uint32(body[7]) << 8) | (uint32(body[8]) << 16) | (uint32(body[9]) << 24);
    info.restartPending = body[10] != 0;
    info.lastSeen = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(self->_supMutex);
    self->_supervisors[info.nodeId] = info;
}
