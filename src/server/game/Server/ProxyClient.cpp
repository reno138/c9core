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

#include "ProxyClient.h"
#include <nats.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include "ArenaTeam.h"
#include "ArenaTeamMgr.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "ClusterMgr.h"
#include "GameTime.h"
#include "Config.h"
#include "SpellAuras.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "SocialMgr.h"
#include "Entities/Player/Player.h"
#include "Entities/Transport/Transport.h"
#include "Globals/ObjectAccessor.h"
#include "Groups/Group.h"
#include "Log.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSessionMgr.h"
#include <chrono>
#include <cstring>
#include <shared_mutex>

// ── Initialize (peer-to-peer NATS) ───────────────────────────────────────────

void ProxyClient::Initialize(std::string const& natsUrl, uint8 serverType,
                              uint16 gamePort, std::string const& gameAddress)
{
    _natsUrl        = natsUrl;
    _serverType     = serverType;
    _gamePort       = gamePort;
    _gameAddress    = gameAddress;
    _startupTimeMs  = getMSTime();

    // NodeId comes from config — no proxy request-reply needed.
    _nodeId = static_cast<uint8>(sConfigMgr->GetOption<int32>("ClusterServer.NodeId", 0));
    if (_nodeId == 0)
    {
        LOG_ERROR("server.worldserver",
                  "ProxyClient: ClusterServer.NodeId not set or is 0 — cluster disabled");
        return;
    }

    // Read transport sync broadcast interval (seconds) from config.
    int32 const syncIntervalSec = sConfigMgr->GetOption<int32>(
        "ClusterServer.TransportSyncInterval", 60);
    _transportSyncIntervalMs = (syncIntervalSec > 0)
        ? static_cast<uint32>(syncIntervalSec) * 1000u
        : 0u;

    // Initialize BG coordinator to the configured node (may change on failover).
    _bgCoordNodeId = static_cast<uint8>(sConfigMgr->GetOption<int32>("ClusterServer.BgCoordinatorNode", 1));

    if (!ConnectNATS())
    {
        LOG_WARN("server.worldserver",
                 "ProxyClient: Initial NATS connect failed — will retry every 10s in Update()");
        return;
    }

    // Announce ourselves to peer nodes; Update() will retry every 10s until acknowledged.
    PublishAnnounce();
    _lastAnnounceRetryMs = getMSTime();

    LOG_INFO("server.worldserver",
             "ProxyClient: Connected to NATS as node {} (serverType={}, gamePort={} addr={}) "
             "transport_sync_interval={}s",
             _nodeId, serverType, gamePort, gameAddress, syncIntervalSec);
}

/// Connect to NATS and subscribe to all required subjects.
/// Returns true on success.  On failure cleans up and returns false.
bool ProxyClient::ConnectNATS()
{
    // Connect to NATS.
    natsStatus s = natsConnection_ConnectTo(&_nc, _natsUrl.c_str());
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to connect to NATS at {} — {}",
                  _natsUrl, natsStatus_GetText(s));
        return false;
    }

    // Subscribe to messages directed at this node.
    std::string nodeSub = "cluster.node." + std::to_string(_nodeId);
    s = natsConnection_Subscribe(&_subNode, _nc, nodeSub.c_str(), OnNatsMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to subscribe to {} — {}",
                  nodeSub, natsStatus_GetText(s));
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return false;
    }

    // Subscribe to broadcast fanout (all nodes).
    s = natsConnection_Subscribe(&_subBroadcast, _nc, "cluster.broadcast", OnNatsMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to subscribe to cluster.broadcast — {}",
                  natsStatus_GetText(s));
        natsSubscription_Destroy(_subNode);
        _subNode = nullptr;
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return false;
    }

    // Subscribe to peer discovery announcements.
    s = natsConnection_Subscribe(&_subAnnounce, _nc, "cluster.announce", OnAnnounceMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to subscribe to cluster.announce — {}",
                  natsStatus_GetText(s));
        natsSubscription_Destroy(_subBroadcast);
        _subBroadcast = nullptr;
        natsSubscription_Destroy(_subNode);
        _subNode = nullptr;
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return false;
    }

    // Subscribe to transport sync queries (new nodes query us on startup to seed
    // their transport positions without depending on the system clock).
    s = natsConnection_Subscribe(&_subTransportQuery, _nc,
                                  "cluster.transport.query", OnTransportQueryMsg, this);
    if (s != NATS_OK)
    {
        // Non-fatal: transport sync falls back to system_clock epoch.
        LOG_WARN("server.worldserver",
                 "ProxyClient: Failed to subscribe to cluster.transport.query — {} "
                 "(transport sync will use system clock as fallback)",
                 natsStatus_GetText(s));
    }

    _connected = true;
    return true;
}

// ── NATS publish helpers ──────────────────────────────────────────────────────

void ProxyClient::PublishToNode(uint8 targetNodeId, uint8 msgType,
                                 uint8 const* payload, int payloadLen)
{
    if (!_nc || !_connected)
        return;

    // Wire format: [msgType:1][payload] on subject cluster.node.{targetNodeId}
    std::vector<uint8> buf;
    buf.reserve(1 + payloadLen);
    buf.push_back(msgType);
    if (payloadLen > 0)
        buf.insert(buf.end(), payload, payload + payloadLen);

    std::string subject = "cluster.node." + std::to_string(targetNodeId);
    natsStatus s = natsConnection_Publish(_nc, subject.c_str(),
                                          buf.data(), static_cast<int>(buf.size()));
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient: Publish to {} failed — {}",
                 subject, natsStatus_GetText(s));
    else
        _natsBytesTx.fetch_add(static_cast<uint32>(buf.size()), std::memory_order_relaxed);
}

void ProxyClient::PublishBroadcast(uint8 msgType, uint8 const* payload, int payloadLen)
{
    if (!_nc || !_connected)
        return;

    // Wire format: [msgType:1][payload] on subject cluster.broadcast
    std::vector<uint8> buf;
    buf.reserve(1 + payloadLen);
    buf.push_back(msgType);
    if (payloadLen > 0)
        buf.insert(buf.end(), payload, payload + payloadLen);

    natsStatus s = natsConnection_Publish(_nc, "cluster.broadcast",
                                          buf.data(), static_cast<int>(buf.size()));
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient: Broadcast publish failed — {}",
                 natsStatus_GetText(s));
    else
        _natsBytesTx.fetch_add(static_cast<uint32>(buf.size()), std::memory_order_relaxed);
}

void ProxyClient::PublishAnnounce()
{
    if (!_nc)
        return;

    // Wire format for cluster.announce:
    //   [nodeId:1][serverType:1][gamePort:2][addrLen:1][addr:n][mapCount:2][mapIds:4*n]
    auto localMaps = sClusterMgr.GetLocalMaps();
    uint16 mapCount = static_cast<uint16>(localMaps.size());
    uint8  addrLen  = static_cast<uint8>(_gameAddress.size());

    std::vector<uint8> buf;
    buf.reserve(6 + addrLen + mapCount * 4);
    buf.push_back(_nodeId);
    buf.push_back(_serverType);
    buf.push_back(static_cast<uint8>(_gamePort & 0xFF));
    buf.push_back(static_cast<uint8>(_gamePort >> 8));
    buf.push_back(addrLen);
    buf.insert(buf.end(), _gameAddress.begin(), _gameAddress.end());
    buf.push_back(static_cast<uint8>(mapCount & 0xFF));
    buf.push_back(static_cast<uint8>(mapCount >> 8));
    for (uint32 mapId : localMaps)
    {
        buf.push_back(static_cast<uint8>(mapId & 0xFF));
        buf.push_back(static_cast<uint8>((mapId >> 8) & 0xFF));
        buf.push_back(static_cast<uint8>((mapId >> 16) & 0xFF));
        buf.push_back(static_cast<uint8>((mapId >> 24) & 0xFF));
    }

    natsStatus s = natsConnection_Publish(_nc, "cluster.announce",
                                          buf.data(), static_cast<int>(buf.size()));
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient: cluster.announce publish failed — {}",
                 natsStatus_GetText(s));
    else
        LOG_INFO("server.worldserver",
                 "ProxyClient: Published cluster.announce (nodeId={} maps={})", _nodeId, mapCount);
}

/// Called on the NATS dispatch thread for cluster.announce messages.
void ProxyClient::OnAnnounceMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                                  natsMsg* msg, void* closure)
{
    auto* self = static_cast<ProxyClient*>(closure);

    const uint8* d = reinterpret_cast<const uint8*>(natsMsg_GetData(msg));
    int n          = natsMsg_GetDataLength(msg);
    // NOTE: do NOT call natsMsg_Destroy here — d is an interior pointer into msg.
    // Destroy AFTER all parsing is done.

    // Minimum: nodeId(1)+serverType(1)+gamePort(2)+addrLen(1)+addr(1)+mapCount(2) = 8 bytes
    if (n < 8)
    {
        natsMsg_Destroy(msg);
        return;
    }

    uint8  nodeId     = d[0];
    uint8  serverType = d[1];
    uint16 gamePort   = static_cast<uint16>(d[2]) | (static_cast<uint16>(d[3]) << 8);
    uint8  addrLen    = d[4];

    if (n < 5 + addrLen + 2)
    {
        natsMsg_Destroy(msg);
        return;
    }

    std::string address(reinterpret_cast<char const*>(d + 5), addrLen);
    int off = 5 + addrLen;

    uint16 mapCount = static_cast<uint16>(d[off]) | (static_cast<uint16>(d[off + 1]) << 8);
    off += 2;

    // Cap map count to avoid unbounded allocation.
    if (mapCount > 128)
        mapCount = 128;

    if (n < off + mapCount * 4)
    {
        natsMsg_Destroy(msg);
        return;
    }

    ClusterNodeInfo info;
    info.nodeId  = nodeId;
    info.address = address;
    info.port    = gamePort;
    info.type    = serverType;
    for (uint16 i = 0; i < mapCount; ++i)
    {
        uint32 mapId = 0;
        std::memcpy(&mapId, d + off + i * 4, 4);
        info.maps.insert(mapId);
    }

    // All data copied — safe to release the NATS message now.
    natsMsg_Destroy(msg);

    // Skip our own announce.
    if (nodeId == self->_nodeId)
        return;

    // Register on world thread (ClusterMgr is world-thread-safe via mutex).
    // If the node was previously dead, RegisterRemoteNode returns true and we
    // restore the BG coordinator assignment if it was that node's role.
    sWorld->QueueCallback([info = std::move(info), announceNodeId = nodeId]() mutable
    {
        bool wasRevived = sClusterMgr.RegisterRemoteNode(std::move(info));
        if (wasRevived)
            sProxyClient.RestoreBgCoordIfNeeded(announceNodeId);
    });

    // Acknowledge receipt so the announcing node stops its 10-second retry loop.
    // This runs on the NATS dispatch thread — safe to publish directly.
    if (self->_nc && self->_connected)
    {
        std::string ackSubject = "cluster.node." + std::to_string(nodeId);
        uint8 ackBuf[2] = { ProxyClient::MSG_ANNOUNCE_ACK, self->_nodeId };
        natsConnection_Publish(self->_nc, ackSubject.c_str(), ackBuf, 2);
    }
}

// ── Incoming NATS message dispatcher ─────────────────────────────────────────

/// Called on the NATS dispatch thread.  Copies payload and queues Dispatch() on the world thread.
void ProxyClient::OnNatsMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                             natsMsg* msg, void* closure)
{
    auto* self = static_cast<ProxyClient*>(closure);

    const uint8* d = reinterpret_cast<const uint8*>(natsMsg_GetData(msg));
    int n          = natsMsg_GetDataLength(msg);

    if (n < 1)
    {
        natsMsg_Destroy(msg);
        return;
    }

    uint8 msgType = d[0];
    std::vector<uint8> payload(d + 1, d + n);
    self->_natsBytesRx.fetch_add(static_cast<uint32>(n), std::memory_order_relaxed);
    natsMsg_Destroy(msg);

    // Marshal all messages to world thread.
    sWorld->QueueCallback([self, msgType, pl = std::move(payload)]() mutable
    {
        self->Dispatch(msgType, std::move(pl));
    });
}

/// Called on the world thread — dispatches to the appropriate handler.
void ProxyClient::Dispatch(uint8 msgType, std::vector<uint8> payload)
{
    switch (msgType)
    {
        case MSG_CLUSTER_PLAYER_ONLINE:
            HandleRemotePlayerOnline(payload);
            break;
        case MSG_CLUSTER_PLAYER_OFFLINE:
        {
            if (payload.size() < 9) break;
            uint64 guid; std::memcpy(&guid, payload.data(), 8);
            uint8 srcNodeId = payload[8];
            if (srcNodeId == _nodeId) break; // our own broadcast — ignore
            HandleRemotePlayerOffline(guid);
            break;
        }
        case MSG_CLUSTER_RELAY_TO_NODE:
        {
            // targetNode(1) + innerType(1) + payloadLen(2) + innerPayload
            if (payload.size() < 4) break;
            uint8  innerType  = payload[1];
            uint16 innerLen;  std::memcpy(&innerLen, payload.data() + 2, 2);
            if (payload.size() < static_cast<std::size_t>(4 + innerLen)) break;
            std::vector<uint8> innerPayload(payload.begin() + 4, payload.begin() + 4 + innerLen);
            HandleIncomingRelay(innerType, innerPayload);
            break;
        }
        case MSG_CLUSTER_GROUP_UPDATE:
        {
            if (payload.size() < 9) break;
            uint64 gg; std::memcpy(&gg, payload.data(), 8);
            uint8 mc = payload[8];
            if (payload.size() < static_cast<std::size_t>(9 + mc * 11)) break;
            std::vector<uint8> memberData(payload.begin() + 9, payload.begin() + 9 + mc * 11);
            HandleGroupUpdate(gg, mc, memberData);
            break;
        }
        case MSG_CLUSTER_GROUP_DISBAND:
        {
            if (payload.size() < 8) break;
            uint64 gg; std::memcpy(&gg, payload.data(), 8);
            HandleGroupDisband(gg);
            break;
        }
        case MSG_CLUSTER_LFG_RELAY:
        {
            // sourceNodeId(1) + inner payload
            if (payload.size() < 1) break;
            uint8 srcNode = payload[0];
            std::vector<uint8> inner(payload.begin() + 1, payload.end());
            HandleLFGRelay(srcNode, inner);
            break;
        }
        case MSG_CLUSTER_LFG_RELAY_RESP:
        {
            // targetNode(1) + payloadLen(2) + innerType(1) + innerPayload
            // Body layout mirrors the old TCP wire: innerType is first byte of the body.
            if (payload.size() < 4) break;
            uint16 pl;      std::memcpy(&pl, payload.data() + 1, 2);
            if (pl < 1 || payload.size() < static_cast<std::size_t>(3 + pl)) break;
            uint8  innerType = payload[3];
            std::vector<uint8> inner(payload.begin() + 4, payload.begin() + 3 + pl);
            HandleLFGRelayResponse(innerType, inner);
            break;
        }
        case MSG_CLUSTER_UNIT_UPDATE:
        {
            if (payload.size() < 2) break;
            uint16 pl; std::memcpy(&pl, payload.data(), 2);
            if (payload.size() < static_cast<std::size_t>(2 + pl)) break;
            std::vector<uint8> inner(payload.begin() + 2, payload.begin() + 2 + pl);
            HandleUnitUpdate(inner);
            break;
        }
        case MSG_CLUSTER_CHAT:
        {
            if (payload.size() < 2) break;
            uint16 pl; std::memcpy(&pl, payload.data(), 2);
            if (payload.size() < static_cast<std::size_t>(2 + pl)) break;
            std::vector<uint8> inner(payload.begin() + 2, payload.begin() + 2 + pl);
            HandleIncomingChat(inner);
            break;
        }
        case MSG_CLUSTER_NOTIFY_MAIL:
        {
            if (payload.size() < 8) break;
            uint64 guid; std::memcpy(&guid, payload.data(), 8);
            HandleIncomingMailNotify(guid);
            break;
        }
        case MSG_CLUSTER_ARENA_RESULT:
        {
            if (payload.size() < 2) break;
            uint16 pl; std::memcpy(&pl, payload.data(), 2);
            if (payload.size() < static_cast<std::size_t>(2 + pl)) break;
            std::vector<uint8> inner(payload.begin() + 2, payload.begin() + 2 + pl);
            HandleIncomingArenaResult(inner);
            break;
        }
        case MSG_CLUSTER_BG_QUEUE_JOIN:
            // Coordinator-only: add player to BG queue and match when ready.
            HandleBgQueueJoin(payload);
            break;
        case MSG_CLUSTER_BG_QUEUE_LEAVE:
            // Coordinator-only: remove player from BG queue.
            HandleBgQueueLeave(payload);
            break;
        case MSG_CLUSTER_BG_CREATE_INST:
            // Instance node: create the BG and reply with BG_INST_CREATED.
            HandleBgCreateInst(payload);
            break;
        case MSG_CLUSTER_BG_INST_CREATED:
            // Coordinator-only: BG created on instance node; send BG_READY to player nodes.
            HandleBgInstCreated(payload);
            break;
        case MSG_CLUSTER_BG_READY:
            // Player nodes: invite matched players to the BG.
            HandleBgReady(payload);
            break;
        case MSG_TRANSPORT_SYNC:
            // Live correction: apply PathProgress values from a peer node.
            HandleTransportSync(payload);
            break;
        case MSG_NODE_STATUS:
            // Heartbeat from a peer node — update its lastSeen timestamp.
            HandleNodeStatus(payload);
            break;
        case MSG_NODE_DEAD:
            // A peer node detected that another node died — sync our routing table.
            HandleNodeDeadMsg(payload);
            break;
        case MSG_PING:
        {
            // Echo timestamp back as MSG_PONG on cluster.broadcast.
            // With peer-to-peer, any node can initiate pings; we broadcast the pong
            // so the pinger can measure round-trip latency regardless of nodeId.
            if (payload.size() < 8) break;
            uint64 ts; std::memcpy(&ts, payload.data(), 8);
            uint8 pongBuf[8];
            std::memcpy(pongBuf, &ts, 8);
            PublishBroadcast(MSG_PONG, pongBuf, 8);
            break;
        }
        case MSG_PONG:
            // Silently discard — pong is only consumed by the sender; peers ignore it.
            break;
        case MSG_ANNOUNCE_ACK:
            HandleAnnounceAck(payload);
            break;
        case MSG_RA_COMMAND:
        {
            // reqId(4) + cmdLen(2) + cmd[cmdLen]
            if (payload.size() < 6) break;
            uint32 reqId;  std::memcpy(&reqId,  payload.data(),     4);
            uint16 cmdLen; std::memcpy(&cmdLen, payload.data() + 4, 2);
            if (payload.size() < 6u + cmdLen) break;
            std::string cmd(reinterpret_cast<const char*>(payload.data() + 6), cmdLen);
            HandleRACommand(reqId, cmd);
            break;
        }
        default:
            LOG_WARN("server.worldserver", "ProxyClient: Unknown incoming msgType 0x{:02X}", msgType);
            break;
    }
}

// ── Incoming message handlers ─────────────────────────────────────────────────

void ProxyClient::HandleRemotePlayerOnline(std::vector<uint8> const& payload)
{
    // Payload: guid(8) + nameLen(1) + name[nameLen] + zone(4) + level + class + race + team + nodeId
    if (payload.size() < 9)
        return;

    uint64 guid;
    std::memcpy(&guid, payload.data(), 8);
    uint8 nameLen = payload[8];

    if (payload.size() < static_cast<std::size_t>(18) + nameLen)
        return;

    std::string name(reinterpret_cast<char const*>(payload.data() + 9), nameLen);
    std::size_t off = 9 + nameLen;

    uint32 zoneId;
    std::memcpy(&zoneId, payload.data() + off, 4);
    off += 4;

    uint8 level   = payload[off + 0];
    uint8 classId = payload[off + 1];
    uint8 raceId  = payload[off + 2];
    uint8 teamId  = payload[off + 3];
    uint8 nodeId  = payload[off + 4];

    // Ignore broadcasts about our own players — NATS delivers cluster.broadcast
    // to ALL subscribers including the sender's node, so we'd otherwise kick our
    // own freshly-logged-in players as "ghost sessions from another node".
    if (nodeId == _nodeId)
        return;

    LOG_INFO("server.worldserver", "ProxyClient: Remote player ONLINE  GUID {:016X} '{}' node={}",
             guid, name, nodeId);

    sClusterMgr.OnRemotePlayerOnline(guid, std::move(name),
                                      zoneId, level, classId, raceId, teamId, nodeId);

    uint64 const remoteGuid   = guid;
    uint32 const remoteZoneId = zoneId;
    uint8  const remoteLevel  = level;
    uint8  const remoteClass  = classId;

    sWorld->QueueCallback([remoteGuid, remoteZoneId, remoteLevel, remoteClass]()
    {
        // Cross-node session cleanup: kick any stale session for this GUID.
        if (Player* ghost = ObjectAccessor::FindConnectedPlayer(ObjectGuid(remoteGuid)))
            ghost->GetSession()->KickPlayer("cross-node reconnect");

        // Friend notification: tell local friends this player came online.
        sSocialMgr->NotifyRemoteFriendOnline(ObjectGuid(remoteGuid),
                                              remoteZoneId, remoteLevel, remoteClass);
    });
}

void ProxyClient::HandleRemotePlayerOffline(uint64 guid)
{
    LOG_INFO("server.worldserver", "ProxyClient: Remote player OFFLINE GUID {:016X}", guid);
    sClusterMgr.OnRemotePlayerOffline(guid);

    // Friend notification: tell local players who have this remote player as a
    // friend that they have gone offline.
    sWorld->QueueCallback([guid]()
    {
        sSocialMgr->NotifyRemoteFriendOffline(ObjectGuid(guid));
    });
}

// ── Instance address query ────────────────────────────────────────────────────

bool ProxyClient::QueryBestInstanceAddress(std::string& outAddr, uint16& outPort)
{
    // Look up the instance node from our local routing table (populated by cluster.announce).
    uint8 instanceNodeId = sClusterMgr.GetInstanceNodeId();
    if (instanceNodeId == 0)
    {
        LOG_WARN("server.worldserver", "ProxyClient: QueryBestInstanceAddress — no instance node registered");
        return false;
    }

    auto nodeInfo = sClusterMgr.GetNodeInfo(instanceNodeId);
    if (!nodeInfo)
    {
        LOG_WARN("server.worldserver", "ProxyClient: QueryBestInstanceAddress — no info for instance nodeId={}", instanceNodeId);
        return false;
    }

    outAddr = nodeInfo->address;
    outPort = nodeInfo->port;
    return true;
}

// ── Periodic updates (MSG_NODE_STATUS / MSG_NODE_REFRESH) ────────────────────

void ProxyClient::Update()
{
    constexpr uint32 NATS_RETRY_INTERVAL_MS     = 10 * 1000;    //  10 seconds
    constexpr uint32 ANNOUNCE_RETRY_INTERVAL_MS = 10 * 1000;    //  10 seconds
    constexpr uint32 HEARTBEAT_INTERVAL_MS      = 10 * 1000;    //  10 seconds
    constexpr uint32 REFRESH_INTERVAL_MS        = 5 * 60 * 1000; //  5 minutes

    uint32 now = getMSTime();

    if (!_connected)
    {
        // Retry NATS connection every 10 seconds.
        if (now - _lastNatsRetryMs >= NATS_RETRY_INTERVAL_MS)
        {
            _lastNatsRetryMs = now;
            if (ConnectNATS())
            {
                LOG_INFO("server.worldserver",
                         "ProxyClient: Reconnected to NATS as node {}", _nodeId);
                PublishAnnounce();
                _lastAnnounceRetryMs = now;
            }
        }
        return;
    }

    // Re-announce every 10 seconds until at least one peer sends MSG_ANNOUNCE_ACK.
    if (!_clusterRegistered && now - _lastAnnounceRetryMs >= ANNOUNCE_RETRY_INTERVAL_MS)
    {
        _lastAnnounceRetryMs = now;
        PublishAnnounce();
        LOG_INFO("server.worldserver",
                 "ProxyClient: Re-announcing to cluster (awaiting peer acknowledgement)");
    }

    if (now - _lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS)
    {
        SendNodeStatus();
        _lastHeartbeatMs = now;
    }

    if (_lastRefreshMs == 0 || now - _lastRefreshMs >= REFRESH_INTERVAL_MS)
    {
        SendNodeRefresh();
        _lastRefreshMs = now;
    }

    // Periodic transport position broadcast for drift correction across nodes.
    if (_transportSyncIntervalMs > 0 &&
        now - _lastTransportSyncMs >= _transportSyncIntervalMs)
    {
        SendTransportSync();
        _lastTransportSyncMs = now;
    }

    // Dead-node detection: check every 15 seconds whether any known peer has gone silent.
    constexpr uint32 DEAD_CHECK_INTERVAL_MS = 15 * 1000;
    if (now - _lastDeadCheckMs >= DEAD_CHECK_INTERVAL_MS)
    {
        _lastDeadCheckMs = now;
        uint32 deadThresholdMs = static_cast<uint32>(
            sConfigMgr->GetOption<int32>("ClusterServer.NodeDeadThreshold", 30)) * 1000u;
        for (uint8 deadNodeId : sClusterMgr.GetStaleNodeIds(deadThresholdMs, now))
            HandleNodeDead(deadNodeId);
    }

    // Management monitoring publishes (consumed by clustermgr).
    uint32 mgmtStatusIntervalMs =
        static_cast<uint32>(sConfigMgr->GetOption<int32>("ClusterServer.MgmtStatusInterval", 5)) * 1000u;
    if (mgmtStatusIntervalMs > 0 && now - _lastMgmtStatusMs >= mgmtStatusIntervalMs)
    {
        SendMgmtStatus();
        _lastMgmtStatusMs = now;
    }

    uint32 mgmtPlayersIntervalMs =
        static_cast<uint32>(sConfigMgr->GetOption<int32>("ClusterServer.MgmtPlayersInterval", 3)) * 1000u;
    if (mgmtPlayersIntervalMs > 0 && now - _lastMgmtPlayersMs >= mgmtPlayersIntervalMs)
    {
        SendMgmtPlayers();
        _lastMgmtPlayersMs = now;
    }
}

// ── Transport sync ────────────────────────────────────────────────────────────

void ProxyClient::OnTransportQueryMsg(natsConnection* nc, natsSubscription* /*sub*/,
                                       natsMsg* msg, void* /*closure*/)
{
    // Called on NATS dispatch thread: build a reply with current PathProgress for
    // every live MotionTransport.  The caller (QueryTransportSync) is waiting with
    // a 500 ms timeout.
    auto& container = HashMapHolder<MotionTransport>::GetContainer();
    std::shared_lock lock(*HashMapHolder<MotionTransport>::GetLock());

    // Wire: [count:2][guid_low:4][path_progress:4]...
    uint16 const count = static_cast<uint16>(container.size());
    std::vector<uint8> reply(2 + static_cast<std::size_t>(count) * 8);
    std::memcpy(reply.data(), &count, 2);

    uint16 idx = 0;
    for (auto const& [guid, trans] : container)
    {
        uint32 guidLow  = guid.GetCounter();
        uint32 progress = trans->GetPathProgress();
        std::memcpy(reply.data() + 2 + idx * 8,     &guidLow,  4);
        std::memcpy(reply.data() + 2 + idx * 8 + 4, &progress, 4);
        ++idx;
    }

    char const* replySubj = natsMsg_GetReply(msg);
    if (replySubj && replySubj[0] != '\0')
    {
        natsConnection_Publish(nc, replySubj,
                               reply.data(), static_cast<int>(reply.size()));
    }
    natsMsg_Destroy(msg);
}

std::unordered_map<uint32, uint32> ProxyClient::QueryTransportSync()
{
    std::unordered_map<uint32, uint32> result;
    if (!_nc || !_connected)
        return result;

    natsMsg* reply = nullptr;
    // Empty request payload; 500 ms timeout.
    natsStatus s = natsConnection_Request(&reply, _nc, "cluster.transport.query",
                                          nullptr, 0, 500);
    if (s == NATS_TIMEOUT)
    {
        // No peer responded — this is the first node, or peers aren't up yet.
        return result;
    }
    if (s != NATS_OK)
    {
        LOG_WARN("server.worldserver",
                 "ProxyClient::QueryTransportSync — NATS request failed: {}",
                 natsStatus_GetText(s));
        return result;
    }

    // Parse [count:2][guid_low:4][path_progress:4]...
    void const* data = natsMsg_GetData(reply);
    int         len  = natsMsg_GetDataLength(reply);
    if (data && len >= 2)
    {
        uint8 const* p   = static_cast<uint8 const*>(data);
        uint8 const* end = p + len;
        uint16 count;
        std::memcpy(&count, p, 2);
        p += 2;
        for (uint16 i = 0; i < count && p + 8 <= end; ++i, p += 8)
        {
            uint32 guidLow, progress;
            std::memcpy(&guidLow,  p,     4);
            std::memcpy(&progress, p + 4, 4);
            result[guidLow] = progress;
        }
    }

    natsMsg_Destroy(reply); // caller owns the reply from natsConnection_Request
    return result;
}

void ProxyClient::SendTransportSync()
{
    auto& container = HashMapHolder<MotionTransport>::GetContainer();
    std::shared_lock lock(*HashMapHolder<MotionTransport>::GetLock());
    if (container.empty())
        return;

    uint16 const count = static_cast<uint16>(container.size());
    std::vector<uint8> payload(2 + static_cast<std::size_t>(count) * 8);
    std::memcpy(payload.data(), &count, 2);

    uint16 idx = 0;
    for (auto const& [guid, trans] : container)
    {
        uint32 guidLow  = guid.GetCounter();
        uint32 progress = trans->GetPathProgress();
        std::memcpy(payload.data() + 2 + idx * 8,     &guidLow,  4);
        std::memcpy(payload.data() + 2 + idx * 8 + 4, &progress, 4);
        ++idx;
    }

    PublishBroadcast(MSG_TRANSPORT_SYNC, payload.data(), static_cast<int>(payload.size()));
    LOG_DEBUG("server.worldserver",
              "ProxyClient: Sent transport sync for {} transport(s)", idx);
}

void ProxyClient::HandleTransportSync(std::vector<uint8> const& payload)
{
    if (payload.size() < 2)
        return;

    uint16 count;
    std::memcpy(&count, payload.data(), 2);
    if (payload.size() < static_cast<std::size_t>(2 + count * 8))
        return;

    for (uint16 i = 0; i < count; ++i)
    {
        uint32 guidLow, remoteProgress;
        std::memcpy(&guidLow,        payload.data() + 2 + i * 8,     4);
        std::memcpy(&remoteProgress, payload.data() + 2 + i * 8 + 4, 4);

        ObjectGuid guid = ObjectGuid(HighGuid::Mo_Transport, guidLow);
        MotionTransport* trans = HashMapHolder<MotionTransport>::Find(guid);
        if (!trans)
            continue;

        uint32 const period = trans->GetPeriod();
        if (period == 0)
            continue;

        uint32 const localProgress = trans->GetPathProgress();

        // Compute the circular distance between local and remote progress.
        // Take the shorter arc so wrap-around doesn't cause false corrections.
        uint32 fwd = (remoteProgress >= localProgress)
            ? remoteProgress - localProgress
            : period - (localProgress - remoteProgress);
        uint32 const diff = (fwd <= period / 2) ? fwd : period - fwd;

        if (diff > 2000)
        {
            trans->InitializeToTime(remoteProgress);
            LOG_DEBUG("server.worldserver",
                      "ProxyClient: Transport {:08X} synced: local={} remote={} drift={}ms",
                      guidLow, localProgress, remoteProgress, diff);
        }
    }
}

// ── RA command execution helpers ──────────────────────────────────────────────

struct RACommandState
{
    uint32      reqId;
    std::string output;
};

static void RACommandPrint(void* arg, std::string_view text)
{
    static_cast<RACommandState*>(arg)->output.append(text);
}

static void RACommandFinished(void* arg, bool /*success*/)
{
    auto* state = static_cast<RACommandState*>(arg);
    sProxyClient.SendRAReply(state->reqId, state->output);
    delete state;
}

void ProxyClient::HandleRACommand(uint32 reqId, std::string const& cmd)
{
    auto* state = new RACommandState();
    state->reqId = reqId;
    auto* holder = new CliCommandHolder(state, cmd.c_str(), RACommandPrint, RACommandFinished);
    sWorld->QueueCliCommand(holder);
}

void ProxyClient::SendRAReply(uint32 reqId, std::string const& output)
{
    if (!_connected || !_nc)
        return;
    uint16 outLen = static_cast<uint16>(std::min(output.size(), size_t(65535)));
    std::vector<uint8> data;
    data.reserve(1 + 1 + 4 + 2 + outLen);
    data.push_back(_nodeId);
    data.push_back(MSG_RA_REPLY);
    data.push_back(static_cast<uint8>(reqId & 0xFF));
    data.push_back(static_cast<uint8>((reqId >> 8)  & 0xFF));
    data.push_back(static_cast<uint8>((reqId >> 16) & 0xFF));
    data.push_back(static_cast<uint8>((reqId >> 24) & 0xFF));
    data.push_back(static_cast<uint8>(outLen & 0xFF));
    data.push_back(static_cast<uint8>(outLen >> 8));
    data.insert(data.end(), output.begin(), output.begin() + outLen);
    PublishRaw("cluster.proxy", data.data(), static_cast<int>(data.size()));
}

// ── Node health / failover ────────────────────────────────────────────────────

void ProxyClient::HandleNodeStatus(std::vector<uint8> const& payload)
{
    // Wire: [nodeId:1][playerCount:4][natsBytesTx:4][natsBytesRx:4] = 13 bytes
    if (payload.size() < 13)
        return;

    uint8 srcNodeId = payload[0];
    if (srcNodeId == _nodeId)
        return;  // our own broadcast — ignore

    uint32 playerCount;
    std::memcpy(&playerCount, payload.data() + 1, 4);
    // natsBytesTx/Rx at +5/+9 — not used for failover, ignore

    uint32 nowMs = getMSTime();
    bool wasRevived = sClusterMgr.UpdateNodeStatus(srcNodeId, playerCount, nowMs);
    if (wasRevived)
        RestoreBgCoordIfNeeded(srcNodeId);
}

void ProxyClient::HandleNodeDead(uint8 deadNodeId)
{
    auto orphanedMaps = sClusterMgr.MarkNodeDead(deadNodeId);

    LOG_WARN("server.worldserver",
             "ProxyClient: Node {} declared DEAD (missed heartbeats). Orphaned {} map(s).",
             deadNodeId, orphanedMaps.size());

    // Elect a new BG coordinator if the dead node held that role.
    if (deadNodeId == _bgCoordNodeId)
    {
        uint8 newCoord = sClusterMgr.GetLowestAliveNonInstanceNodeId();
        // If no peer qualifies, this node takes over.
        if (newCoord == 0)
            newCoord = _nodeId;
        _bgCoordNodeId = newCoord;
        LOG_WARN("server.worldserver",
                 "ProxyClient: BG coordinator was node {} (dead). Elected new coordinator: node {}.",
                 deadNodeId, _bgCoordNodeId);
    }

    // Track cluster instability for management monitoring.
    if (_nodeCrashCount < 65535u)
        ++_nodeCrashCount;

    // Broadcast so all surviving peer nodes update their routing tables.
    uint8 deadPayload[1] = { deadNodeId };
    PublishBroadcast(MSG_NODE_DEAD, deadPayload, 1);

    // Failover: claim orphaned maps from the dead node so players can still
    // reach those zones. The proxy will route to us after re-announce.
    if (!orphanedMaps.empty() && !sClusterMgr.IsInstanceServerMode())
    {
        LOG_WARN("server.worldserver",
                 "ProxyClient: Claiming {} orphaned maps from dead node {}",
                 orphanedMaps.size(), deadNodeId);
        sClusterMgr.ClaimOrphanedMaps(deadNodeId);
    }
}

void ProxyClient::HandleNodeDeadMsg(std::vector<uint8> const& payload)
{
    if (payload.empty())
        return;

    uint8 deadNodeId = payload[0];
    if (deadNodeId == _nodeId)
        return;  // guard against self-declaration

    auto orphanedMaps = sClusterMgr.MarkNodeDead(deadNodeId);

    LOG_WARN("server.worldserver",
             "ProxyClient: Received MSG_NODE_DEAD for node {}. Orphaned {} map(s).",
             deadNodeId, orphanedMaps.size());

    if (deadNodeId == _bgCoordNodeId)
    {
        uint8 newCoord = sClusterMgr.GetLowestAliveNonInstanceNodeId();
        if (newCoord == 0)
            newCoord = _nodeId;
        _bgCoordNodeId = newCoord;
        LOG_WARN("server.worldserver",
                 "ProxyClient: BG coordinator was node {} (dead). Elected new coordinator: node {}.",
                 deadNodeId, _bgCoordNodeId);
    }
}

void ProxyClient::RestoreBgCoordIfNeeded(uint8 revivedNodeId)
{
    uint8 configCoordId = static_cast<uint8>(
        sConfigMgr->GetOption<int32>("ClusterServer.BgCoordinatorNode", 1));
    if (revivedNodeId == configCoordId && _bgCoordNodeId != configCoordId)
    {
        _bgCoordNodeId = configCoordId;
        LOG_WARN("server.worldserver",
                 "ProxyClient: BG coordinator restored to configured node {} (revived).",
                 _bgCoordNodeId);
    }
}

void ProxyClient::SendNodeStatus()
{
    uint32 playerCount = static_cast<uint32>(sWorldSessionMgr->GetActiveSessionCount());
    uint32 natsBytesTx = _natsBytesTx.exchange(0, std::memory_order_relaxed);
    uint32 natsBytesRx = _natsBytesRx.exchange(0, std::memory_order_relaxed);

    // Wire: [nodeId:1][playerCount:4][natsBytesTx:4][natsBytesRx:4] = 13 bytes
    // nodeId is prepended so receiving nodes know which peer sent this heartbeat.
    uint8 payload[13];
    payload[0] = _nodeId;
    std::memcpy(payload + 1, &playerCount, 4);
    std::memcpy(payload + 5, &natsBytesTx, 4);
    std::memcpy(payload + 9, &natsBytesRx, 4);

    PublishBroadcast(MSG_NODE_STATUS, payload, 13);
    LOG_DEBUG("server.worldserver",
              "ProxyClient: Sent NODE_STATUS players={} nats_tx={}B nats_rx={}B",
              playerCount, natsBytesTx, natsBytesRx);
}

void ProxyClient::SendNodeRefresh()
{
    // Re-announce identity so late-joining peer nodes can build routing tables.
    PublishAnnounce();
    LOG_INFO("server.worldserver",
             "ProxyClient: Sent node re-announce (port={} nodeId={})", _gamePort, _nodeId);
}

// ── Management monitoring publishes (cluster.mgmt.*) ─────────────────────────

void ProxyClient::PublishRaw(std::string const& subject, uint8 const* data, int len)
{
    if (!_nc)
        return;
    natsStatus s = natsConnection_Publish(_nc, subject.c_str(), data, len);
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient: PublishRaw to {} failed — {}",
                 subject, natsStatus_GetText(s));
    else
        _natsBytesTx.fetch_add(static_cast<uint32>(len), std::memory_order_relaxed);
}

/// Read VmRSS from /proc/self/status in MB.
static uint32 ReadMemUsageMB()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("VmRSS:", 0) == 0)
        {
            uint32 kb = 0;
            std::sscanf(line.c_str(), "VmRSS: %u", &kb);
            return kb / 1024u;
        }
    }
    return 0;
}

/// Compute CPU% from /proc/self/stat delta between calls.
/// lastJiffies and lastCheckMs are persistent across calls (stored in ProxyClient).
static uint8 ComputeCpuPercent(uint32& lastJiffies, uint32& lastCheckMs)
{
    // /proc/self/stat fields: pid(1) comm(2) state(3) ... utime(14) stime(15)
    std::ifstream f("/proc/self/stat");
    if (!f)
        return 0;

    std::string token;
    unsigned long utime = 0, stime = 0;
    // Skip fields 1-13 by reading tokens; field 2 (comm) may contain spaces
    // inside parens so read whole line and parse from the end of the ')'
    std::string line;
    std::getline(f, line);
    f.close();

    // Find end of comm field — last ')'
    auto rp = line.rfind(')');
    if (rp == std::string::npos)
        return 0;

    // Fields after ')': state ppid pgrp session tty_nr tpgid flags
    //   minflt cminflt majflt cmajflt utime stime   (12 more tokens)
    std::istringstream ss(line.substr(rp + 1));
    std::string skip;
    for (int i = 0; i < 11; ++i)
        ss >> skip;          // state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt
    ss >> utime >> stime;

    uint32 totalJiffies = static_cast<uint32>(utime + stime);
    uint32 now = getMSTime();

    if (lastJiffies == 0 || lastCheckMs == 0)
    {
        lastJiffies  = totalJiffies;
        lastCheckMs  = now;
        return 0;
    }

    uint32 diffJiffies = totalJiffies - lastJiffies;
    uint32 diffMs      = now - lastCheckMs;
    lastJiffies        = totalJiffies;
    lastCheckMs        = now;

    if (diffMs == 0)
        return 0;

    // Linux HZ = 100: each jiffy is 10ms.
    // cpu% = (diffJiffies * 10ms) / diffMs * 100
    uint32 pct = diffJiffies * 1000u / diffMs;
    return static_cast<uint8>(std::min(pct, 100u));
}

void ProxyClient::SendMgmtStatus()
{
    if (!_nc || !_connected)
        return;

    // Collect metrics.
    uint32 playerCount = static_cast<uint32>(sWorldSessionMgr->GetPlayerCount());
    uint32 maxPlayers  = static_cast<uint32>(sWorldSessionMgr->GetPlayerAmountLimit());
    uint32 pid         = static_cast<uint32>(::getpid());
    uint32 uptimeSecs  = (getMSTime() - _startupTimeMs) / 1000u;
    uint32 memUsageMB  = ReadMemUsageMB();
    uint8  cpuPercent  = ComputeCpuPercent(_lastCpuJiffies, _lastCpuCheckMs);
    uint32 natsTxBps   = _natsBytesTx.load(std::memory_order_relaxed) / 10u;  // ÷ heartbeat interval
    uint32 natsRxBps   = _natsBytesRx.load(std::memory_order_relaxed) / 10u;

    // Maps list.
    std::unordered_set<uint32> localMaps = sClusterMgr.GetLocalMaps();
    uint8 mapCount = static_cast<uint8>(std::min(localMaps.size(), std::size_t(128)));

    // Address string.
    uint8 addrLen = static_cast<uint8>(std::min(_gameAddress.size(), std::size_t(255)));

    // Wire format (fixed 30 bytes + mapCount*4 + 1 + addrLen):
    // [nodeId:1][state:1][playerCount:2][maxPlayers:2][pid:4][uptimeSecs:4]
    // [memUsageMB:4][cpuPercent:1][crashCount:2][natsTxBps:4][natsRxBps:4]
    // [mapCount:1][mapIds:4×n][addrLen:1][addr:n]
    std::size_t fixedLen = 1+1+2+2+4+4+4+1+2+4+4+1;  // 30 bytes
    std::size_t totalLen = fixedLen + static_cast<std::size_t>(mapCount) * 4 + 1 + addrLen;
    std::vector<uint8> buf(totalLen, 0);

    std::size_t off = 0;
    buf[off++] = _nodeId;
    buf[off++] = 3;   // state = RUNNING (we only publish when alive)
    uint16 pc16 = static_cast<uint16>(std::min(playerCount, 65535u));
    uint16 mp16 = static_cast<uint16>(std::min(maxPlayers,  65535u));
    std::memcpy(buf.data() + off, &pc16, 2);      off += 2;
    std::memcpy(buf.data() + off, &mp16, 2);      off += 2;
    std::memcpy(buf.data() + off, &pid, 4);        off += 4;
    std::memcpy(buf.data() + off, &uptimeSecs, 4); off += 4;
    std::memcpy(buf.data() + off, &memUsageMB, 4); off += 4;
    buf[off++] = cpuPercent;
    std::memcpy(buf.data() + off, &_nodeCrashCount, 2); off += 2;
    std::memcpy(buf.data() + off, &natsTxBps, 4);  off += 4;
    std::memcpy(buf.data() + off, &natsRxBps, 4);  off += 4;
    buf[off++] = mapCount;
    uint8 mIdx = 0;
    for (uint32 mapId : localMaps)
    {
        if (mIdx >= mapCount) break;
        std::memcpy(buf.data() + off, &mapId, 4);
        off += 4;
        ++mIdx;
    }
    buf[off++] = addrLen;
    if (addrLen > 0)
        std::memcpy(buf.data() + off, _gameAddress.data(), addrLen);

    PublishRaw("cluster.mgmt.status", buf.data(), static_cast<int>(totalLen));
}

void ProxyClient::SendMgmtPlayers()
{
    if (!_nc || !_connected)
        return;

    // Collect all online players' positions.
    // DoForAllOnlinePlayers is thread-safe internally via session map mutex.
    std::vector<uint8> buf;
    buf.reserve(3 + 64 * 32);  // rough estimate

    // Header: [nodeId:1][playerCount:2]
    buf.push_back(_nodeId);
    std::size_t countOff = buf.size();
    buf.push_back(0);  // playerCount low byte — filled in below
    buf.push_back(0);  // playerCount high byte

    uint16 playerCount = 0;
    sWorldSessionMgr->DoForAllOnlinePlayers([&](Player* player)
    {
        if (!player || !player->IsInWorld())
            return;

        uint64 guid   = player->GetGUID().GetRawValue();
        uint16 mapId  = static_cast<uint16>(player->GetMapId());
        float  x      = player->GetPositionX();
        float  y      = player->GetPositionY();
        float  z      = player->GetPositionZ();
        uint16 zoneId = static_cast<uint16>(player->GetZoneId());
        uint8  level  = static_cast<uint8>(player->GetLevel());
        uint8  cls    = player->getClass();
        uint8  race   = player->getRace();
        uint8  team   = static_cast<uint8>(player->GetTeamId());

        std::string const& name = player->GetName();
        uint8 nameLen = static_cast<uint8>(std::min(name.size(), std::size_t(48)));

        // Per-player: guid(8)+mapId(2)+x(4)+y(4)+z(4)+zoneId(2)+level(1)+class(1)+race(1)+team(1)+nameLen(1)+name
        std::size_t needed = 8+2+4+4+4+2+1+1+1+1+1 + nameLen;
        std::size_t base   = buf.size();
        buf.resize(base + needed);

        std::size_t off = base;
        std::memcpy(buf.data() + off, &guid,   8); off += 8;
        std::memcpy(buf.data() + off, &mapId,  2); off += 2;
        std::memcpy(buf.data() + off, &x,      4); off += 4;
        std::memcpy(buf.data() + off, &y,      4); off += 4;
        std::memcpy(buf.data() + off, &z,      4); off += 4;
        std::memcpy(buf.data() + off, &zoneId, 2); off += 2;
        buf[off++] = level;
        buf[off++] = cls;
        buf[off++] = race;
        buf[off++] = team;
        buf[off++] = nameLen;
        if (nameLen > 0)
            std::memcpy(buf.data() + off, name.data(), nameLen);

        ++playerCount;
    });

    // Fill in actual count
    std::memcpy(buf.data() + countOff, &playerCount, 2);

    PublishRaw("cluster.mgmt.players", buf.data(), static_cast<int>(buf.size()));
}

// ── Outgoing cluster messages ─────────────────────────────────────────────────

void ProxyClient::SendReroute(uint64 playerGuid, std::string const& address, uint16 port,
                             uint32 mapId, float x, float y, float z, float ori)
{
    if (!_nc || !_connected)
        return;

    // Wire format for cluster.proxy MSG_REROUTE_PLAYER:
    //   [sourceNodeId:1][msgType:1][guid:8][addrLen:1][addr:n][port:2][map:4][x:4][y:4][z:4][ori:4]
    // map/x/y/z/ori are always included; zeros mean login reroute (no native SMSG_NEW_WORLD needed).
    uint8  addrLen = static_cast<uint8>(std::min(address.size(), std::size_t(255)));
    uint16 portLE  = port;

    std::vector<uint8> buf;
    buf.reserve(2 + 8 + 1 + addrLen + 2 + 4 + 4 + 4 + 4 + 4);

    // [sourceNodeId:1][msgType:1]
    buf.push_back(_nodeId);
    buf.push_back(MSG_REROUTE_PLAYER);

    // [guid:8]
    for (int i = 0; i < 8; ++i)
        buf.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));

    // [addrLen:1][addr:n]
    buf.push_back(addrLen);
    buf.insert(buf.end(), address.begin(), address.begin() + addrLen);

    // [port:2]
    buf.push_back(static_cast<uint8>(portLE & 0xFF));
    buf.push_back(static_cast<uint8>(portLE >> 8));

    // [map:4]
    buf.push_back(static_cast<uint8>(mapId & 0xFF));
    buf.push_back(static_cast<uint8>((mapId >> 8) & 0xFF));
    buf.push_back(static_cast<uint8>((mapId >> 16) & 0xFF));
    buf.push_back(static_cast<uint8>((mapId >> 24) & 0xFF));

    // [x:4][y:4][z:4][ori:4] — float little-endian
    auto pushFloat = [&](float v) {
        uint32 bits;
        std::memcpy(&bits, &v, 4);
        buf.push_back(static_cast<uint8>(bits & 0xFF));
        buf.push_back(static_cast<uint8>((bits >> 8) & 0xFF));
        buf.push_back(static_cast<uint8>((bits >> 16) & 0xFF));
        buf.push_back(static_cast<uint8>((bits >> 24) & 0xFF));
    };
    pushFloat(x);
    pushFloat(y);
    pushFloat(z);
    pushFloat(ori);

    natsStatus s = natsConnection_Publish(_nc, "cluster.proxy", buf.data(), static_cast<int>(buf.size()));
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient::SendReroute: publish failed — {}", natsStatus_GetText(s));
    else
    {
        _natsBytesTx.fetch_add(static_cast<uint32>(buf.size()), std::memory_order_relaxed);
        LOG_INFO("server.worldserver",
                 "ProxyClient::SendReroute GUID {:016X} → {}:{} map={} pos=({:.1f},{:.1f},{:.1f},{:.2f})",
                 playerGuid, address, port, mapId, x, y, z, ori);
    }
}

void ProxyClient::SendRerouteToMap(uint64 playerGuid, uint32 mapId)
{
    if (!_nc || !_connected)
        return;

    // Payload: [sourceNodeId:1][msgType:1][guid:8][mapId:4]
    uint8 buf[14];
    buf[0] = _nodeId;
    buf[1] = MSG_REROUTE_TO_MAP;
    std::memcpy(buf + 2, &playerGuid, 8);
    std::memcpy(buf + 10, &mapId, 4);

    natsStatus s = natsConnection_Publish(_nc, "cluster.proxy", buf, 14);
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient::SendRerouteToMap: publish failed — {}", natsStatus_GetText(s));
    else
        LOG_INFO("server.worldserver", "ProxyClient::SendRerouteToMap GUID {:016X} map={}", playerGuid, mapId);
}

void ProxyClient::AnnounceOnline(Player const* player)
{
    if (!_connected || !player)
        return;

    std::string name = player->GetName();
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);

    uint64 guid   = player->GetGUID().GetRawValue();
    uint32 zoneId = player->GetZoneId();
    uint8 level   = static_cast<uint8>(player->GetLevel());
    uint8 classId = static_cast<uint8>(player->getClass());
    uint8 raceId  = static_cast<uint8>(player->getRace());
    uint8 teamId  = static_cast<uint8>(player->GetTeamId());

    // Payload: guid(8)+nameLen(1)+name+zone(4)+level+classId+raceId+teamId+nodeId(1)
    // nodeId appended so receivers can filter their own broadcasts.
    std::vector<uint8> payload;
    payload.reserve(8 + 1 + name.size() + 4 + 5);

    for (int i = 0; i < 8; ++i)
        payload.push_back(static_cast<uint8>((guid >> (i * 8)) & 0xFF));

    payload.push_back(static_cast<uint8>(name.size()));
    payload.insert(payload.end(), name.begin(), name.end());

    payload.push_back(static_cast<uint8>(zoneId & 0xFF));
    payload.push_back(static_cast<uint8>((zoneId >> 8) & 0xFF));
    payload.push_back(static_cast<uint8>((zoneId >> 16) & 0xFF));
    payload.push_back(static_cast<uint8>((zoneId >> 24) & 0xFF));

    payload.push_back(level);
    payload.push_back(classId);
    payload.push_back(raceId);
    payload.push_back(teamId);
    payload.push_back(_nodeId); // receivers use this to filter self-broadcasts

    PublishBroadcast(MSG_CLUSTER_PLAYER_ONLINE, payload.data(), static_cast<int>(payload.size()));
}

void ProxyClient::AnnounceOffline(uint64 playerGuid)
{
    if (!_connected)
        return;

    // Payload: guid(8)+nodeId(1) — nodeId used by receivers to filter self-broadcasts.
    uint8 payload[9];
    std::memcpy(payload, &playerGuid, 8);
    payload[8] = _nodeId;

    PublishBroadcast(MSG_CLUSTER_PLAYER_OFFLINE, payload, 9);
}

void ProxyClient::RelayToNode(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload)
{
    if (!_connected)
        return;

    // Build MSG_CLUSTER_RELAY_TO_NODE payload:
    // targetNode(1)+innerType(1)+payloadLen(2)+innerPayload
    uint16 payloadLen = static_cast<uint16>(payload.size());
    std::vector<uint8> msg;
    msg.reserve(4 + payload.size());
    msg.push_back(targetNodeId);
    msg.push_back(innerType);
    msg.push_back(static_cast<uint8>(payloadLen & 0xFF));
    msg.push_back(static_cast<uint8>(payloadLen >> 8));
    msg.insert(msg.end(), payload.begin(), payload.end());

    PublishToNode(targetNodeId, MSG_CLUSTER_RELAY_TO_NODE, msg.data(), static_cast<int>(msg.size()));
}

void ProxyClient::SendGroupUpdate(uint64 groupGuid, std::vector<uint8> const& memberData)
{
    if (!_connected)
        return;

    uint8 memberCount = static_cast<uint8>(memberData.size() / 11); // 11 = guid(8)+subgroup(1)+role(1)+nodeId(1)
    std::vector<uint8> payload;
    payload.reserve(8 + 1 + memberData.size());
    for (int i = 0; i < 8; ++i)
        payload.push_back(static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF));
    payload.push_back(memberCount);
    payload.insert(payload.end(), memberData.begin(), memberData.end());

    PublishBroadcast(MSG_CLUSTER_GROUP_UPDATE, payload.data(), static_cast<int>(payload.size()));
}

void ProxyClient::SendGroupDisband(uint64 groupGuid)
{
    if (!_connected)
        return;

    uint8 payload[8];
    std::memcpy(payload, &groupGuid, 8);
    PublishBroadcast(MSG_CLUSTER_GROUP_DISBAND, payload, 8);
}

// ── Incoming relay / group handlers ──────────────────────────────────────────

static constexpr uint8 CLUSTER_INNER_GROUP_INVITE        = 0x01;
static constexpr uint8 CLUSTER_INNER_GROUP_INVITE_RESULT = 0x02;
static constexpr uint8 CLUSTER_INNER_GROUP_REROUTE_TO_MAP = 0x03;

void ProxyClient::HandleIncomingRelay(uint8 innerType, std::vector<uint8> const& payload)
{
    switch (innerType)
    {
        case CLUSTER_INNER_GROUP_INVITE:
        {
            // payload: inviter_guid(8) + invitee_guid(8) + inviter_name_len(1) + inviter_name
            if (payload.size() < 17)
                return;
            uint64 inviterGuid, inviteeGuid;
            std::memcpy(&inviterGuid, payload.data(), 8);
            std::memcpy(&inviteeGuid, payload.data() + 8, 8);
            uint8 nameLen = payload[16];
            if (payload.size() < static_cast<std::size_t>(17) + nameLen)
                return;
            std::string inviterName(reinterpret_cast<char const*>(payload.data() + 17), nameLen);

            LOG_DEBUG("server.worldserver", "ProxyClient: Cross-node GROUP_INVITE from GUID {:016X} '{}' to GUID {:016X}",
                      inviterGuid, inviterName, inviteeGuid);

            // Store the pending invite now (ClusterMgr is mutex-protected, safe from I/O thread).
            sClusterMgr.SetPendingCrossNodeInvite(inviteeGuid, inviterGuid, inviterName);

            // Post the player lookup + packet send to the world update thread.
            sWorld->QueueCallback([inviteeGuid, inviterGuid, inviterName = std::move(inviterName)]()
            {
                Player* invitee = ObjectAccessor::FindPlayer(ObjectGuid(inviteeGuid));
                if (!invitee)
                {
                    LOG_DEBUG("server.worldserver", "ProxyClient: GROUP_INVITE — invitee {:016X} not found locally", inviteeGuid);
                    return;
                }
                WorldPacket data(SMSG_GROUP_INVITE, 10);
                data << uint8(1);           // invited flag
                data << inviterName;
                data << uint32(0);          // unk
                data << uint8(0);           // count
                data << uint32(0);          // unk
                invitee->SendDirectMessage(&data);
            });
            break;
        }

        case CLUSTER_INNER_GROUP_INVITE_RESULT:
        {
            // payload: invitee_guid(8) + result(1)
            if (payload.size() < 9)
                return;
            uint64 inviteeGuid;
            std::memcpy(&inviteeGuid, payload.data(), 8);
            uint8 result = payload[8];

            LOG_DEBUG("server.worldserver", "ProxyClient: Cross-node GROUP_INVITE_RESULT for invitee {:016X} result={}",
                      inviteeGuid, result);

            // Queue the invite result to be processed on the game update thread.
            sClusterMgr.QueueCrossNodeInviteResult(inviteeGuid, result);
            break;
        }

        case CLUSTER_INNER_GROUP_REROUTE_TO_MAP:
        {
            // payload: guid(8) + mapId(4)
            if (payload.size() < 12)
                return;
            uint64 guid  = 0;
            uint32 mapId = 0;
            std::memcpy(&guid,  payload.data(),     8);
            std::memcpy(&mapId, payload.data() + 8, 4);

            LOG_DEBUG("server.worldserver", "ProxyClient: GROUP_REROUTE_TO_MAP GUID {:016X} → map {}", guid, mapId);

            sWorld->QueueCallback([guid, mapId]()
            {
                Player* player = ObjectAccessor::FindPlayer(ObjectGuid(guid));
                if (!player)
                    return;
                MapEntry const* mEntry = sMapStore.LookupEntry(mapId);
                if (!mEntry)
                    return;
                player->TeleportTo(mapId, player->GetPositionX(), player->GetPositionY(),
                                   player->GetPositionZ(), player->GetOrientation());
            });
            break;
        }

        default:
            LOG_WARN("server.worldserver", "ProxyClient: Unknown relay inner type 0x{:02X}", innerType);
            break;
    }
}

void ProxyClient::HandleGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> const& memberData)
{
    LOG_DEBUG("server.worldserver", "ProxyClient: GroupUpdate group {:016X} {} members", groupGuid, memberCount);
    sClusterMgr.OnGroupUpdate(groupGuid, memberCount, memberData);

    // Trigger a full stats push from local players so remote group members
    // see their health/mana immediately after group formation or membership change.
    uint8 localNodeId = _nodeId;
    sWorld->QueueCallback([groupGuid, localNodeId]()
    {
        for (auto const& rm : sClusterMgr.GetGroupRemoteMembers(groupGuid))
        {
            if (rm.nodeId != localNodeId)
                continue;
            if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(rm.guid)))
                player->SetGroupUpdateFlag(GROUP_UPDATE_FULL);
        }
    });
}

void ProxyClient::HandleGroupDisband(uint64 groupGuid)
{
    LOG_DEBUG("server.worldserver", "ProxyClient: GroupDisband group {:016X}", groupGuid);
    sClusterMgr.OnGroupDisband(groupGuid);
}

// ── LFG relay send methods ────────────────────────────────────────────────────

void ProxyClient::SendLFGJoinRelay(uint64 playerGuid, uint8 roles, std::vector<uint32> const& dungeons)
{
    if (!_connected)
        return;

    uint8 lfgMasterNodeId = static_cast<uint8>(
        sConfigMgr->GetOption<int32>("ClusterServer.LFGMasterNode", 1));

    uint8 dungeonCount = static_cast<uint8>(dungeons.size());
    // payload: sourceNodeId(1)+payloadLen(2)+inner_type(1)+guid(8)+roles(1)+count(1)+dungeons
    uint16 innerLen = static_cast<uint16>(1 + 8 + 1 + 1 + 4 * dungeonCount);

    std::vector<uint8> msg;
    msg.reserve(3 + innerLen);
    msg.push_back(_nodeId);   // sourceNodeId so master can route responses back
    msg.push_back(static_cast<uint8>(innerLen & 0xFF));
    msg.push_back(static_cast<uint8>(innerLen >> 8));
    msg.push_back(LFG_INNER_JOIN);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    msg.push_back(roles);
    msg.push_back(dungeonCount);
    for (uint32 d : dungeons)
    {
        msg.push_back(static_cast<uint8>(d & 0xFF));
        msg.push_back(static_cast<uint8>((d >> 8) & 0xFF));
        msg.push_back(static_cast<uint8>((d >> 16) & 0xFF));
        msg.push_back(static_cast<uint8>((d >> 24) & 0xFF));
    }
    PublishToNode(lfgMasterNodeId, MSG_CLUSTER_LFG_RELAY, msg.data(), static_cast<int>(msg.size()));
}

void ProxyClient::SendLFGLeaveRelay(uint64 playerGuid)
{
    if (!_connected)
        return;

    uint8 lfgMasterNodeId = static_cast<uint8>(
        sConfigMgr->GetOption<int32>("ClusterServer.LFGMasterNode", 1));

    // payload: sourceNodeId(1)+innerLen(2)+inner_type(1)+guid(8) = 12 bytes
    std::vector<uint8> msg;
    msg.reserve(12);
    msg.push_back(_nodeId);
    msg.push_back(9); msg.push_back(0); // innerLen = 9 (inner_type+guid)
    msg.push_back(LFG_INNER_LEAVE);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    PublishToNode(lfgMasterNodeId, MSG_CLUSTER_LFG_RELAY, msg.data(), static_cast<int>(msg.size()));
}

void ProxyClient::SendLFGProposalResultRelay(uint32 proposalId, uint64 playerGuid, bool accept)
{
    if (!_connected)
        return;

    uint8 lfgMasterNodeId = static_cast<uint8>(
        sConfigMgr->GetOption<int32>("ClusterServer.LFGMasterNode", 1));

    // payload: sourceNodeId(1)+innerLen(2)+inner_type(1)+proposalId(4)+guid(8)+accept(1) = 17 bytes
    std::vector<uint8> msg;
    msg.reserve(17);
    msg.push_back(_nodeId);
    msg.push_back(14); msg.push_back(0); // innerLen = 14
    msg.push_back(LFG_INNER_PROPOSAL_RESULT);
    for (int i = 0; i < 4; ++i)
        msg.push_back(static_cast<uint8>((proposalId >> (i * 8)) & 0xFF));
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    msg.push_back(accept ? 1 : 0);
    PublishToNode(lfgMasterNodeId, MSG_CLUSTER_LFG_RELAY, msg.data(), static_cast<int>(msg.size()));
}

void ProxyClient::SendLFGRelayResponse(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload)
{
    if (!_connected)
        return;

    // Wire: targetNode(1)+totalLen(2)+innerType(1)+payload
    uint16 totalLen = static_cast<uint16>(1 + payload.size());
    std::vector<uint8> msg;
    msg.reserve(3 + 1 + payload.size());
    msg.push_back(targetNodeId);
    msg.push_back(static_cast<uint8>(totalLen & 0xFF));
    msg.push_back(static_cast<uint8>(totalLen >> 8));
    msg.push_back(innerType);
    msg.insert(msg.end(), payload.begin(), payload.end());
    PublishToNode(targetNodeId, MSG_CLUSTER_LFG_RELAY_RESP, msg.data(), static_cast<int>(msg.size()));
}

// ── LFG relay incoming handlers ───────────────────────────────────────────────

void ProxyClient::HandleLFGRelay(uint8 sourceNodeId, std::vector<uint8> const& payload)
{
    if (payload.empty())
        return;

    uint8 innerType = payload[0];
    switch (innerType)
    {
        case LFG_INNER_JOIN:
        {
            // payload: inner_type(1) + guid(8) + roles(1) + dungeon_count(1) + dungeons(4 each)
            if (payload.size() < 11)
                return;
            uint64 guid;
            std::memcpy(&guid, payload.data() + 1, 8);
            uint8 roles        = payload[9];
            uint8 dungeonCount = payload[10];
            if (payload.size() < static_cast<std::size_t>(11) + 4u * dungeonCount)
                return;

            lfg::LfgDungeonSet dungeons;
            for (uint8 i = 0; i < dungeonCount; ++i)
            {
                uint32 d;
                std::memcpy(&d, payload.data() + 11 + i * 4, 4);
                dungeons.insert(d);
            }

            // Look up team from ClusterMgr (player is remote from master's perspective).
            uint8 teamId = TEAM_ALLIANCE;
            if (ClusterPlayerInfo const* info = sClusterMgr.FindRemotePlayerByGuid(guid))
                teamId = info->teamId;

            LOG_DEBUG("server.worldserver", "ProxyClient: LFG_INNER_JOIN relay from node {} guid {:016X} roles={} dungeons={}",
                      sourceNodeId, guid, roles, dungeons.size());

            // sLFGMgr is not thread-safe; post to world update thread.
            sWorld->QueueCallback([guid, roles, dungeons = std::move(dungeons), teamId]()
            {
                sLFGMgr->JoinLfgByData(ObjectGuid(guid), roles, dungeons, teamId);
            });
            break;
        }

        case LFG_INNER_LEAVE:
        {
            // payload: inner_type(1) + guid(8)
            if (payload.size() < 9)
                return;
            uint64 guid;
            std::memcpy(&guid, payload.data() + 1, 8);
            LOG_DEBUG("server.worldserver", "ProxyClient: LFG_INNER_LEAVE relay from node {} guid {:016X}", sourceNodeId, guid);
            // sLFGMgr is not thread-safe; post to world update thread.
            sWorld->QueueCallback([guid]()
            {
                ObjectGuid objectGuid(guid);
                sLFGMgr->LeaveLfg(objectGuid);
                sLFGMgr->LeaveAllLfgQueues(objectGuid, true, ObjectGuid::Empty);
            });
            break;
        }

        case LFG_INNER_PROPOSAL_RESULT:
        {
            // payload: inner_type(1) + proposalId(4) + guid(8) + accept(1)
            if (payload.size() < 14)
                return;
            uint32 proposalId;
            std::memcpy(&proposalId, payload.data() + 1, 4);
            uint64 guid;
            std::memcpy(&guid, payload.data() + 5, 8);
            bool accept = payload[13] != 0;
            LOG_DEBUG("server.worldserver", "ProxyClient: LFG_INNER_PROPOSAL_RESULT relay from node {} guid {:016X} proposal={} accept={}",
                      sourceNodeId, guid, proposalId, accept ? 1 : 0);
            // sLFGMgr is not thread-safe; post to world update thread.
            sWorld->QueueCallback([proposalId, guid, accept]()
            {
                sLFGMgr->UpdateProposal(proposalId, ObjectGuid(guid), accept);
            });
            break;
        }

        default:
            LOG_WARN("server.worldserver", "ProxyClient: Unknown LFG relay inner type 0x{:02X}", innerType);
            break;
    }
}

void ProxyClient::HandleLFGRelayResponse(uint8 innerType, std::vector<uint8> const& payload)
{
    switch (innerType)
    {
        case LFG_INNER_MATCH_NOTIFY:
        {
            // payload: guid(8) + addr_len(1) + addr(addr_len) + port(2)
            if (payload.size() < 11)
                return;
            uint64 guid;
            std::memcpy(&guid, payload.data(), 8);
            uint8 addrLen = payload[8];
            if (payload.size() < static_cast<std::size_t>(9) + addrLen + 2u)
                return;
            std::string addr(reinterpret_cast<char const*>(payload.data() + 9), addrLen);
            uint16 port = static_cast<uint16>(payload[9 + addrLen]) |
                          (static_cast<uint16>(payload[10 + addrLen]) << 8);

            LOG_DEBUG("server.worldserver", "ProxyClient: LFG_INNER_MATCH_NOTIFY guid {:016X} → {}:{}", guid, addr, port);

            // Reroute the local player to the instance server via the proxy.
            // No position params — the instance server will handle initial placement.
            sProxyClient.SendReroute(guid, addr, port);
            break;
        }

        default:
            LOG_WARN("server.worldserver", "ProxyClient: Unknown LFG relay response inner type 0x{:02X}", innerType);
            break;
    }
}

void ProxyClient::DeliverPacketToPlayer(uint64 targetGuid, WorldPacket const& packet)
{
    if (!_connected)
        return;

    // Serialize the WoW packet into raw bytes (the header is the standard server-side format).
    // WorldPacket::size() gives the payload size; the actual wire packet needs the header prepended.
    // We build: header (4 or 5 bytes) + payload.
    // The proxy will re-encrypt for the client direction via QueuePacketForClient.

    // Build a minimal server-side packet header (size BE, opcode LE).
    // Size field = opcode_size(2) + payload_size.
    std::size_t payloadSize = packet.size();
    uint32 sizeField = static_cast<uint32>(payloadSize) + 2; // +2 for opcode uint16

    std::vector<uint8> rawPacket;
    if (sizeField > 0x7FFF)
    {
        // Large packet: 5-byte header with high bit set.
        rawPacket.reserve(5 + payloadSize);
        rawPacket.push_back(static_cast<uint8>(0x80 | ((sizeField >> 16) & 0x7F)));
        rawPacket.push_back(static_cast<uint8>((sizeField >> 8) & 0xFF));
        rawPacket.push_back(static_cast<uint8>(sizeField & 0xFF));
    }
    else
    {
        // Normal packet: 4-byte header.
        rawPacket.reserve(4 + payloadSize);
        rawPacket.push_back(static_cast<uint8>((sizeField >> 8) & 0xFF));
        rawPacket.push_back(static_cast<uint8>(sizeField & 0xFF));
    }

    uint16 opcode = static_cast<uint16>(packet.GetOpcode());
    rawPacket.push_back(static_cast<uint8>(opcode & 0xFF));
    rawPacket.push_back(static_cast<uint8>(opcode >> 8));

    // Append payload.
    if (payloadSize > 0)
    {
        rawPacket.insert(rawPacket.end(),
            reinterpret_cast<uint8 const*>(packet.contents()),
            reinterpret_cast<uint8 const*>(packet.contents()) + payloadSize);
    }

    uint16 pktLen = static_cast<uint16>(rawPacket.size());

    // Look up which node the target player is on.
    ClusterPlayerInfo const* info = sClusterMgr.FindRemotePlayerByGuid(targetGuid);
    if (!info)
    {
        LOG_DEBUG("server.worldserver",
                  "ProxyClient: DeliverPacketToPlayer — GUID {:016X} not in remote cache", targetGuid);
        return;
    }
    uint8 targetNodeId = info->nodeId;

    std::vector<uint8> payload;
    payload.reserve(8 + 2 + rawPacket.size());

    for (int i = 0; i < 8; ++i)
        payload.push_back(static_cast<uint8>((targetGuid >> (i * 8)) & 0xFF));

    payload.push_back(static_cast<uint8>(pktLen & 0xFF));
    payload.push_back(static_cast<uint8>(pktLen >> 8));
    payload.insert(payload.end(), rawPacket.begin(), rawPacket.end());

    PublishToNode(targetNodeId, MSG_CLUSTER_DELIVER_PACKET, payload.data(), static_cast<int>(payload.size()));
}

// ── Outgoing: cluster unit update ─────────────────────────────────────────────

void ProxyClient::SendClusterUnitUpdate(Player* player)
{
    if (!_connected || _nodeId == 0)
        return;

    uint64 auraMask = player->GetAuraUpdateMaskForRaid();
    uint16 auraCount = 0;
    for (uint32 i = 0; i < MAX_AURAS_GROUP_UPDATE; ++i)
        if (auraMask & (uint64(1) << i))
            ++auraCount;

    // Fixed payload: guid(8)+status(2)+hp(4)+maxHp(4)+pwrType(1)+pwr(2)+maxPwr(2)+lvl(2)+zone(2)+auraMask(8) = 35
    uint16 payloadLen = static_cast<uint16>(35 + auraCount * 5);

    std::vector<uint8> msg;
    msg.reserve(3 + payloadLen);

    auto pushU8  = [&](uint8  v){ msg.push_back(v); };
    auto pushU16 = [&](uint16 v){ msg.push_back(static_cast<uint8>(v & 0xFF)); msg.push_back(static_cast<uint8>(v >> 8)); };
    auto pushU32 = [&](uint32 v){ pushU16(static_cast<uint16>(v & 0xFFFF)); pushU16(static_cast<uint16>(v >> 16)); };
    auto pushU64 = [&](uint64 v){ pushU32(static_cast<uint32>(v)); pushU32(static_cast<uint32>(v >> 32)); };

    pushU8(MSG_CLUSTER_UNIT_UPDATE);
    pushU16(payloadLen);
    pushU64(player->GetGUID().GetRawValue());

    uint16 status = MEMBER_STATUS_ONLINE;
    if (player->IsPvP())   status |= MEMBER_STATUS_PVP;
    if (!player->IsAlive())
    {
        if (player->HasPlayerFlag(PLAYER_FLAGS_GHOST)) status |= MEMBER_STATUS_GHOST;
        else                                            status |= MEMBER_STATUS_DEAD;
    }
    if (player->isAFK()) status |= MEMBER_STATUS_AFK;
    if (player->isDND()) status |= MEMBER_STATUS_DND;

    pushU16(status);
    Powers power = player->getPowerType();
    pushU32(player->GetHealth());
    pushU32(player->GetMaxHealth());
    pushU8(static_cast<uint8>(power));
    pushU16(static_cast<uint16>(player->GetPower(power)));
    pushU16(static_cast<uint16>(player->GetMaxPower(power)));
    pushU16(static_cast<uint16>(player->GetLevel()));
    pushU16(static_cast<uint16>(player->GetZoneId()));
    pushU64(auraMask);

    for (uint32 i = 0; i < MAX_AURAS_GROUP_UPDATE; ++i)
    {
        if (auraMask & (uint64(1) << i))
        {
            AuraApplication const* aurApp = player->GetVisibleAura(i);
            pushU32(aurApp ? aurApp->GetBase()->GetId() : 0);
            pushU8(1);
        }
    }

    PublishBroadcast(MSG_CLUSTER_UNIT_UPDATE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

// ── Incoming: cluster unit update ─────────────────────────────────────────────

void ProxyClient::HandleUnitUpdate(std::vector<uint8> const& payload)
{
    static constexpr std::size_t FIXED_SIZE = 35; // see wire format
    if (payload.size() < FIXED_SIZE)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleUnitUpdate — payload too small ({})", payload.size());
        return;
    }

    ClusterMgr::ClusterUnitState state;
    std::size_t off = 0;

    std::memcpy(&state.guid,      payload.data() + off, 8); off += 8;
    std::memcpy(&state.status,    payload.data() + off, 2); off += 2;
    std::memcpy(&state.health,    payload.data() + off, 4); off += 4;
    std::memcpy(&state.maxHealth, payload.data() + off, 4); off += 4;
    state.powerType = payload[off++];
    std::memcpy(&state.power,     payload.data() + off, 2); off += 2;
    std::memcpy(&state.maxPower,  payload.data() + off, 2); off += 2;
    std::memcpy(&state.level,     payload.data() + off, 2); off += 2;
    std::memcpy(&state.zoneId,    payload.data() + off, 2); off += 2;
    std::memcpy(&state.auraMask,  payload.data() + off, 8); off += 8;

    for (uint32 i = 0; i < MAX_AURAS_GROUP_UPDATE; ++i)
    {
        if (state.auraMask & (uint64(1) << i))
        {
            if (off + 5 > payload.size())
                break;
            ClusterMgr::ClusterUnitState::AuraEntry entry;
            std::memcpy(&entry.spellId, payload.data() + off, 4); off += 4;
            entry.flags = payload[off++];
            state.auras.push_back(entry);
        }
    }

    LOG_DEBUG("server.worldserver", "ProxyClient: UnitUpdate GUID {:016X} HP {}/{}", state.guid, state.health, state.maxHealth);
    sClusterMgr.UpdateUnitState(std::move(state));
}

// ── Outgoing: cross-node mail notification ────────────────────────────────────

void ProxyClient::SendMailNotify(uint64 recipientGuid)
{
    if (!_connected || _nodeId == 0)
        return;

    // Look up which node the recipient is on and send directly.
    ClusterPlayerInfo const* info = sClusterMgr.FindRemotePlayerByGuid(recipientGuid);
    if (!info)
    {
        LOG_DEBUG("server.worldserver",
                  "ProxyClient: SendMailNotify — GUID {:016X} not in remote cache", recipientGuid);
        return;
    }

    uint8 payload[8];
    std::memcpy(payload, &recipientGuid, 8);
    PublishToNode(info->nodeId, MSG_CLUSTER_NOTIFY_MAIL, payload, 8);
}


void ProxyClient::HandleAnnounceAck(std::vector<uint8> const& payload)
{
    if (payload.size() < 1)
        return;

    uint8 senderNodeId = payload[0];

    if (_clusterRegistered)
        return; // Already acknowledged — ignore redundant ACKs.

    _clusterRegistered = true;
    LOG_INFO("server.worldserver",
             "ProxyClient: Cluster registration acknowledged by node {} — node {} is fully registered",
             senderNodeId, _nodeId);
}


// ── Incoming: cross-node mail notification ────────────────────────────────────

void ProxyClient::HandleIncomingMailNotify(uint64 recipientGuid)
{
    LOG_DEBUG("server.worldserver", "ProxyClient: IncomingMailNotify for GUID {:016X}", recipientGuid);

    sWorld->QueueCallback([recipientGuid]()
    {
        if (Player* player = ObjectAccessor::FindConnectedPlayer(ObjectGuid(recipientGuid)))
            player->SendNewMail();
    });
}

// ── Outgoing: cross-node chat relay ───────────────────────────────────────────

void ProxyClient::SendChatRelay(uint8 chatMsgType, uint32 zoneId, WorldPacket const& pkt)
{
    if (!_connected || _nodeId == 0)
        return;

    // Payload layout: uint8 chatMsgType | uint32 zoneId | uint16 innerLen | uint8 inner[innerLen]
    uint16 innerLen    = static_cast<uint16>(pkt.size());
    uint16 payloadLen  = static_cast<uint16>(1 + 4 + 2 + innerLen);

    std::vector<uint8> msg;
    msg.reserve(3 + payloadLen);

    auto pushU8  = [&](uint8  v){ msg.push_back(v); };
    auto pushU16 = [&](uint16 v){ msg.push_back(static_cast<uint8>(v & 0xFF)); msg.push_back(static_cast<uint8>(v >> 8)); };
    auto pushU32 = [&](uint32 v){ pushU16(static_cast<uint16>(v & 0xFFFF)); pushU16(static_cast<uint16>(v >> 16)); };

    pushU8(MSG_CLUSTER_CHAT);
    pushU16(payloadLen);
    pushU8(chatMsgType);
    pushU32(zoneId);
    pushU16(innerLen);
    if (innerLen > 0)
        msg.insert(msg.end(), pkt.contents(), pkt.contents() + innerLen);

    PublishBroadcast(MSG_CLUSTER_CHAT, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

// ── Incoming: cross-node chat relay ───────────────────────────────────────────

void ProxyClient::HandleIncomingChat(std::vector<uint8> const& payload)
{
    // Payload layout: uint8 chatMsgType | uint32 zoneId | uint16 innerLen | uint8 inner[innerLen]
    static constexpr std::size_t HEADER_SIZE = 7; // 1 + 4 + 2
    if (payload.size() < HEADER_SIZE)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleIncomingChat — payload too small ({})", payload.size());
        return;
    }

    uint8  chatMsgType = payload[0];
    uint32 zoneId      = 0;
    uint16 innerLen    = 0;
    std::memcpy(&zoneId,   payload.data() + 1, 4);
    std::memcpy(&innerLen, payload.data() + 5, 2);

    if (payload.size() < HEADER_SIZE + innerLen)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleIncomingChat — truncated inner packet ({} < {})",
                 payload.size(), HEADER_SIZE + innerLen);
        return;
    }

    if (innerLen == 0)
        return;

    // Copy inner packet bytes into a vector for capture.
    std::vector<uint8> innerData(payload.begin() + HEADER_SIZE, payload.begin() + HEADER_SIZE + innerLen);

    LOG_DEBUG("server.worldserver", "ProxyClient: IncomingChat msgType={} zone={} innerLen={}",
              chatMsgType, zoneId, innerLen);

    sWorld->QueueCallback([zoneId, innerData = std::move(innerData)]()
    {
        // Reconstruct the SMSG_MESSAGECHAT WorldPacket and deliver to all
        // local players in the matching zone.
        WorldPacket pkt(SMSG_MESSAGECHAT, innerData.size());
        pkt.append(innerData.data(), innerData.size());

        std::shared_lock<std::shared_mutex> slock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
        {
            if (player && player->IsInWorld() && player->GetZoneId() == zoneId)
                player->SendDirectMessage(&pkt);
        }
    });
}

// ── Outgoing: arena team stat broadcast ───────────────────────────────────────

void ProxyClient::SendArenaResult(uint32 teamId, uint16 rating, uint16 weekGames, uint16 weekWins,
                                  uint16 seasonGames, uint16 seasonWins, uint32 rank)
{
    if (!_connected || _nodeId == 0)
        return;

    // Payload: uint32 team_id | uint16 rating | uint16 weekGames | uint16 weekWins
    //          | uint16 seasonGames | uint16 seasonWins | uint32 rank  → 18 bytes
    static constexpr uint16 PAYLOAD_LEN = 18;
    std::vector<uint8> msg;
    msg.reserve(3 + PAYLOAD_LEN);

    msg.push_back(MSG_CLUSTER_ARENA_RESULT);
    msg.push_back(static_cast<uint8>(PAYLOAD_LEN & 0xFF));
    msg.push_back(static_cast<uint8>(PAYLOAD_LEN >> 8));

    auto pushU16 = [&](uint16 v) {
        msg.push_back(static_cast<uint8>(v & 0xFF));
        msg.push_back(static_cast<uint8>(v >> 8));
    };
    auto pushU32 = [&](uint32 v) {
        pushU16(static_cast<uint16>(v & 0xFFFF));
        pushU16(static_cast<uint16>(v >> 16));
    };

    pushU32(teamId);
    pushU16(rating);
    pushU16(weekGames);
    pushU16(weekWins);
    pushU16(seasonGames);
    pushU16(seasonWins);
    pushU32(rank);

    PublishBroadcast(MSG_CLUSTER_ARENA_RESULT, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

// ── Incoming: arena team stat broadcast ───────────────────────────────────────

void ProxyClient::HandleIncomingArenaResult(std::vector<uint8> const& payload)
{
    // Payload: uint32 teamId | uint16 rating | uint16 weekGames | uint16 weekWins
    //          | uint16 seasonGames | uint16 seasonWins | uint32 rank  → 18 bytes
    static constexpr std::size_t EXPECTED = 18;
    if (payload.size() < EXPECTED)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleIncomingArenaResult — payload too small ({})", payload.size());
        return;
    }

    uint32 teamId      = 0;
    uint16 rating      = 0;
    uint16 weekGames   = 0;
    uint16 weekWins    = 0;
    uint16 seasonGames = 0;
    uint16 seasonWins  = 0;
    uint32 rank        = 0;
    std::memcpy(&teamId,      payload.data() +  0, 4);
    std::memcpy(&rating,      payload.data() +  4, 2);
    std::memcpy(&weekGames,   payload.data() +  6, 2);
    std::memcpy(&weekWins,    payload.data() +  8, 2);
    std::memcpy(&seasonGames, payload.data() + 10, 2);
    std::memcpy(&seasonWins,  payload.data() + 12, 2);
    std::memcpy(&rank,        payload.data() + 14, 4);

    LOG_DEBUG("server.worldserver", "ProxyClient: IncomingArenaResult team={} rating={} rank={}", teamId, rating, rank);

    sWorld->QueueCallback([teamId, rating, weekGames, weekWins, seasonGames, seasonWins, rank]()
    {
        ArenaTeam* team = sArenaTeamMgr->GetArenaTeamById(teamId);
        if (!team)
            return;

        // Update in-memory stats.
        ArenaTeamStats newStats;
        newStats.Rating      = rating;
        newStats.WeekGames   = weekGames;
        newStats.WeekWins    = weekWins;
        newStats.SeasonGames = seasonGames;
        newStats.SeasonWins  = seasonWins;
        newStats.Rank        = rank;
        team->SetArenaTeamStats(newStats);

        // Send SMSG_ARENA_TEAM_STATS to every locally-connected member.
        for (ArenaTeam::MemberList::const_iterator itr = team->GetMembers().begin();
             itr != team->GetMembers().end(); ++itr)
        {
            if (Player* player = ObjectAccessor::FindConnectedPlayer(itr->Guid))
                team->SendStats(player->GetSession());
        }
    });
}

// ── BG queue relay — outgoing ─────────────────────────────────────────────────

void ProxyClient::SendBgQueueJoin(uint64 guid, uint32 bgTypeId, uint8 bracketId, uint8 teamId, uint8 minPerTeam)
{
    if (!_connected || _nodeId == 0)
        return;

    // payload: guid(8)+bgTypeId(4)+bracketId(1)+teamId(1)+minPerTeam(1)+srcNodeId(1) = 16 bytes
    uint8 payload[16];
    std::memcpy(payload,      &guid,     8);
    std::memcpy(payload + 8,  &bgTypeId, 4);
    payload[12] = bracketId;
    payload[13] = teamId;
    payload[14] = minPerTeam;
    payload[15] = _nodeId;   // so coordinator knows which node this player is on
    PublishToNode(_bgCoordNodeId, MSG_CLUSTER_BG_QUEUE_JOIN, payload, 16);
}

void ProxyClient::SendBgQueueLeave(uint64 guid, uint32 bgTypeId)
{
    if (!_connected || _nodeId == 0)
        return;

    // payload: guid(8)+bgTypeId(4) = 12 bytes
    uint8 payload[12];
    std::memcpy(payload,     &guid,     8);
    std::memcpy(payload + 8, &bgTypeId, 4);
    PublishToNode(_bgCoordNodeId, MSG_CLUSTER_BG_QUEUE_LEAVE, payload, 12);
}

void ProxyClient::SendBgInstCreated(uint32 matchId, uint32 instanceId, uint32 mapId, uint32 clientInstanceId)
{
    if (!_connected || _nodeId == 0)
        return;

    // payload: matchId(4)+instanceId(4)+mapId(4)+clientInstanceId(4) = 16 bytes
    uint8 payload[16];
    std::memcpy(payload,      &matchId,          4);
    std::memcpy(payload + 4,  &instanceId,       4);
    std::memcpy(payload + 8,  &mapId,            4);
    std::memcpy(payload + 12, &clientInstanceId, 4);
    PublishToNode(_bgCoordNodeId, MSG_CLUSTER_BG_INST_CREATED, payload, 16);
}

// ── BG queue relay — incoming handlers ────────────────────────────────────────

void ProxyClient::HandleBgCreateInst(std::vector<uint8> const& payload)
{
    // Payload: matchId(4)+bgTypeId(4)+bracketId(1)+allianceCount(1)+hordeCount(1)+guids
    static constexpr std::size_t HDR_SIZE = 11; // 4+4+1+1+1
    if (payload.size() < HDR_SIZE)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleBgCreateInst — payload too small ({})", payload.size());
        return;
    }

    uint32 matchId    = 0;
    uint32 bgTypeId   = 0;
    std::memcpy(&matchId,  payload.data(),     4);
    std::memcpy(&bgTypeId, payload.data() + 4, 4);
    uint8 bracketId      = payload[8];
    uint8 allianceCount  = payload[9];
    uint8 hordeCount     = payload[10];

    std::size_t expectedSize = HDR_SIZE + static_cast<std::size_t>(allianceCount + hordeCount) * 8;
    if (payload.size() < expectedSize)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleBgCreateInst — payload size mismatch (got {} need {})",
                 payload.size(), expectedSize);
        return;
    }

    // Copy guids (they'll be passed into the world callback).
    std::vector<uint64> allianceGuids(allianceCount);
    std::vector<uint64> hordeGuids(hordeCount);
    for (uint8 i = 0; i < allianceCount; ++i)
        std::memcpy(&allianceGuids[i], payload.data() + HDR_SIZE + i * 8, 8);
    for (uint8 i = 0; i < hordeCount; ++i)
        std::memcpy(&hordeGuids[i],    payload.data() + HDR_SIZE + allianceCount * 8 + i * 8, 8);

    LOG_INFO("server.worldserver", "ProxyClient: HandleBgCreateInst matchId={} bgType={} bracket={} ally={} horde={}",
             matchId, bgTypeId, bracketId, allianceCount, hordeCount);

    sWorld->QueueCallback([matchId, bgTypeId, bracketId, allianceGuids = std::move(allianceGuids),
                           hordeGuids = std::move(hordeGuids)]()
    {
        BattlegroundTypeId bgType = static_cast<BattlegroundTypeId>(bgTypeId);
        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(bgType);
        if (!bgt)
        {
            LOG_ERROR("server.worldserver", "ProxyClient: BgCreateInst matchId={} — no template for bgType {}", matchId, bgTypeId);
            return;
        }

        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketById(bgt->GetMapId(),
                                                                              static_cast<BattlegroundBracketId>(bracketId));
        if (!bracketEntry)
        {
            LOG_ERROR("server.worldserver", "ProxyClient: BgCreateInst matchId={} — no bracket {} for mapId {}",
                      matchId, bracketId, bgt->GetMapId());
            return;
        }

        Battleground* bg = sBattlegroundMgr->CreateNewBattleground(bgType, bracketEntry, 0, false);
        if (!bg)
        {
            LOG_ERROR("server.worldserver", "ProxyClient: BgCreateInst matchId={} — CreateNewBattleground failed for bgType {}", matchId, bgTypeId);
            return;
        }

        sBattlegroundMgr->AddBattleground(bg);

        uint32 instanceId       = bg->GetInstanceID();
        uint32 mapId            = bg->GetMapId();
        uint32 clientInstanceId = bg->GetClientInstanceID();

        LOG_INFO("server.worldserver", "ProxyClient: BgCreateInst matchId={} → instanceId={} mapId={} clientId={}",
                 matchId, instanceId, mapId, clientInstanceId);

        sProxyClient.SendBgInstCreated(matchId, instanceId, mapId, clientInstanceId);
    });
}

void ProxyClient::HandleBgReady(std::vector<uint8> const& payload)
{
    // Payload: instanceId(4)+bgTypeId(4)+mapId(4)+clientInstanceId(4)+count(1)+{guid(8)+team(1)}×count
    static constexpr std::size_t HDR_SIZE = 17; // 4+4+4+4+1
    if (payload.size() < HDR_SIZE)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleBgReady — payload too small ({})", payload.size());
        return;
    }

    uint32 instanceId       = 0;
    uint32 bgTypeId         = 0;
    uint32 mapId            = 0;
    uint32 clientInstanceId = 0;
    std::memcpy(&instanceId,       payload.data(),      4);
    std::memcpy(&bgTypeId,         payload.data() + 4,  4);
    std::memcpy(&mapId,            payload.data() + 8,  4);
    std::memcpy(&clientInstanceId, payload.data() + 12, 4);
    uint8 count = payload[16];

    if (payload.size() < HDR_SIZE + static_cast<std::size_t>(count) * 9)
    {
        LOG_WARN("server.worldserver", "ProxyClient: HandleBgReady — payload size mismatch (got {} need {})",
                 payload.size(), HDR_SIZE + count * 9);
        return;
    }

    struct BgReadyEntry { uint64 guid; uint8 teamId; };
    std::vector<BgReadyEntry> players(count);
    for (uint8 i = 0; i < count; ++i)
    {
        std::memcpy(&players[i].guid, payload.data() + HDR_SIZE + i * 9, 8);
        players[i].teamId = payload[HDR_SIZE + i * 9 + 8];
    }

    LOG_INFO("server.worldserver", "ProxyClient: HandleBgReady instanceId={} bgType={} mapId={} count={}",
             instanceId, bgTypeId, mapId, count);

    sWorld->QueueCallback([instanceId, bgTypeId, mapId, clientInstanceId,
                           players = std::move(players)]()
    {
        BattlegroundTypeId bgType    = static_cast<BattlegroundTypeId>(bgTypeId);
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgType, 0);
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
            return;

        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(bgType);
        if (!bgt)
            return;

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

        // Build a minimal STATUS_WAIT_JOIN packet manually (we don't have the BG object locally).
        // Packet: SMSG_BATTLEFIELD_STATUS
        //   uint32 queueSlot
        //   uint8  arenatype (0)
        //   uint8  0x0
        //   uint32 bgTypeId
        //   uint16 0x1F90
        //   uint8  minLevel
        //   uint8  maxLevel
        //   uint32 clientInstanceId
        //   uint8  isRated (0)
        //   uint32 STATUS_WAIT_JOIN
        //   uint32 mapId
        //   uint64 0 (unknown)
        //   uint32 INVITE_ACCEPT_WAIT_TIME

        uint32 removeTime = GameTime::GetGameTimeMS().count() + 60000u; // INVITE_ACCEPT_WAIT_TIME = 60s

        for (auto const& entry : players)
        {
            ObjectGuid guid{ HighGuid::Player, static_cast<uint32>(entry.guid) };
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            if (!player)
            {
                LOG_WARN("server.worldserver", "ProxyClient: HandleBgReady — player {:016X} not found locally", entry.guid);
                continue;
            }

            TeamId teamId = static_cast<TeamId>(entry.teamId);

            // Find this player's GroupQueueInfo in the local queue and set the invite.
            GroupQueueInfo ginfo;
            if (!bgQueue.GetPlayerGroupInfoData(player->GetGUID(), &ginfo))
            {
                LOG_WARN("server.worldserver", "ProxyClient: HandleBgReady — player {} not in local queue", player->GetName());
                continue;
            }

            // Mark the player as invited (mirrors what InviteGroupToBG does).
            player->SetInviteForBattlegroundQueueType(bgQueueTypeId, instanceId);

            uint32 queueSlot = player->GetBattlegroundQueueIndex(bgQueueTypeId);
            if (queueSlot >= PLAYER_MAX_BATTLEGROUND_QUEUES)
                continue;

            // Build SMSG_BATTLEFIELD_STATUS with STATUS_WAIT_JOIN
            WorldPacket data(SMSG_BATTLEFIELD_STATUS, 4 + 8 + 1 + 1 + 4 + 4 + 4);
            data << uint32(queueSlot);
            data << uint8(0);                // arenatype
            data << uint8(0x0);              // not arena
            data << uint32(bgTypeId);
            data << uint16(0x1F90);
            data << uint8(bgt->GetMinLevel());
            data << uint8(bgt->GetMaxLevel());
            data << uint32(clientInstanceId);
            data << uint8(0);                // not rated
            data << uint32(STATUS_WAIT_JOIN);
            data << uint32(mapId);
            data << uint64(0);
            data << uint32(60000);           // 60 second accept window

            player->SendDirectMessage(&data);

            // Schedule BGQueueRemoveEvent so the invite expires if not accepted.
            BGQueueRemoveEvent* removeEvent = new BGQueueRemoveEvent(
                player->GetGUID(), instanceId, bgType, bgQueueTypeId, removeTime);
            bgQueue.AddEvent(removeEvent, 60000u);

            LOG_DEBUG("server.worldserver", "ProxyClient: BgReady — invited player {} instanceId={} team={}",
                      player->GetName(), instanceId, entry.teamId);
        }
    });
}

// ── BG coordinator handlers (coordinator node only) ───────────────────────────

void ProxyClient::HandleBgQueueJoin(std::vector<uint8> const& payload)
{
    // Payload: guid(8)+bgTypeId(4)+bracketId(1)+teamId(1)+minPerTeam(1) = 15 bytes
    if (payload.size() < 15)
    {
        LOG_WARN("server.worldserver", "ProxyClient::HandleBgQueueJoin — payload too small ({})", payload.size());
        return;
    }

    uint64 guid;      std::memcpy(&guid,     payload.data(),      8);
    uint32 bgTypeId;  std::memcpy(&bgTypeId, payload.data() + 8,  4);
    uint8  bracketId  = payload[12];
    uint8  teamId     = payload[13];
    uint8  minPerTeam = payload[14];
    uint8  srcNodeId  = (payload.size() >= 16) ? payload[15] : 0;

    auto& state = _bgQueues[bgTypeId][bracketId];
    if (state.minPlayersPerTeam == 1)
        state.minPlayersPerTeam = minPerTeam;

    auto& factionQueue = (teamId == 0) ? state.alliance : state.horde;
    for (auto const& e : factionQueue)
        if (e.guid == guid)
            return; // duplicate join

    factionQueue.push_back({ guid, srcNodeId, teamId });

    LOG_DEBUG("server.worldserver", "ProxyClient::HandleBgQueueJoin bgType={} bracket={} ally={} horde={} min={}",
              bgTypeId, bracketId, state.alliance.size(), state.horde.size(), state.minPlayersPerTeam);

    if (state.alliance.size() < state.minPlayersPerTeam ||
        state.horde.size()    < state.minPlayersPerTeam)
        return; // not enough players yet

    // We have enough — take exactly minPlayersPerTeam from each side.
    uint32 matchId        = _nextBgMatchId++;
    uint8  allianceCount  = static_cast<uint8>(state.minPlayersPerTeam);
    uint8  hordeCount     = static_cast<uint8>(state.minPlayersPerTeam);

    std::vector<BgQueueEntry> matchAlliance(state.alliance.begin(),
                                             state.alliance.begin() + allianceCount);
    std::vector<BgQueueEntry> matchHorde(state.horde.begin(),
                                          state.horde.begin() + hordeCount);

    state.alliance.erase(state.alliance.begin(), state.alliance.begin() + allianceCount);
    state.horde.erase(state.horde.begin(), state.horde.begin() + hordeCount);

    _pendingBgMatches[matchId] = PendingBgMatch{ bgTypeId, bracketId, matchAlliance, matchHorde };

    // Find instance node and send MSG_CLUSTER_BG_CREATE_INST.
    uint8 instNodeId = static_cast<uint8>(sClusterMgr.GetInstanceNodeId());
    if (instNodeId == 0)
    {
        LOG_ERROR("server.worldserver", "ProxyClient::HandleBgQueueJoin matchId={} — no instance node, cannot create BG", matchId);
        _pendingBgMatches.erase(matchId);
        return;
    }

    // Payload: matchId(4)+bgTypeId(4)+bracketId(1)+allianceCount(1)+hordeCount(1)+guids(8*n)
    std::vector<uint8> createPayload;
    createPayload.reserve(11 + static_cast<std::size_t>(allianceCount + hordeCount) * 8);
    auto appendU32 = [&createPayload](uint32 v)
    {
        createPayload.push_back(v & 0xFF);
        createPayload.push_back((v >> 8) & 0xFF);
        createPayload.push_back((v >> 16) & 0xFF);
        createPayload.push_back((v >> 24) & 0xFF);
    };
    appendU32(matchId);
    appendU32(bgTypeId);
    createPayload.push_back(bracketId);
    createPayload.push_back(allianceCount);
    createPayload.push_back(hordeCount);
    for (auto const& e : matchAlliance) { uint8 t[8]; std::memcpy(t, &e.guid, 8); createPayload.insert(createPayload.end(), t, t + 8); }
    for (auto const& e : matchHorde)    { uint8 t[8]; std::memcpy(t, &e.guid, 8); createPayload.insert(createPayload.end(), t, t + 8); }

    PublishToNode(instNodeId, MSG_CLUSTER_BG_CREATE_INST, createPayload.data(), static_cast<int>(createPayload.size()));

    LOG_INFO("server.worldserver", "ProxyClient::HandleBgQueueJoin matchId={} bgType={} bracket={} ally={} horde={} → inst node {}",
             matchId, bgTypeId, bracketId, allianceCount, hordeCount, instNodeId);
}

void ProxyClient::HandleBgQueueLeave(std::vector<uint8> const& payload)
{
    // Payload: guid(8)+bgTypeId(4) = 12 bytes
    if (payload.size() < 12)
        return;

    uint64 guid;     std::memcpy(&guid,     payload.data(),     8);
    uint32 bgTypeId; std::memcpy(&bgTypeId, payload.data() + 8, 4);

    auto bgIt = _bgQueues.find(bgTypeId);
    if (bgIt == _bgQueues.end())
        return;

    for (auto& [bId, state] : bgIt->second)
    {
        auto& a = state.alliance;
        auto it = std::find_if(a.begin(), a.end(), [guid](BgQueueEntry const& e){ return e.guid == guid; });
        if (it != a.end()) { a.erase(it); return; }

        auto& h = state.horde;
        it = std::find_if(h.begin(), h.end(), [guid](BgQueueEntry const& e){ return e.guid == guid; });
        if (it != h.end()) { h.erase(it); return; }
    }
}

void ProxyClient::HandleBgInstCreated(std::vector<uint8> const& payload)
{
    // Payload: matchId(4)+instanceId(4)+mapId(4)+clientInstanceId(4) = 16 bytes
    if (payload.size() < 16)
    {
        LOG_WARN("server.worldserver", "ProxyClient::HandleBgInstCreated — payload too small ({})", payload.size());
        return;
    }

    uint32 matchId, instanceId, mapId, clientInstanceId;
    std::memcpy(&matchId,          payload.data(),      4);
    std::memcpy(&instanceId,       payload.data() + 4,  4);
    std::memcpy(&mapId,            payload.data() + 8,  4);
    std::memcpy(&clientInstanceId, payload.data() + 12, 4);

    auto it = _pendingBgMatches.find(matchId);
    if (it == _pendingBgMatches.end())
    {
        LOG_WARN("server.worldserver", "ProxyClient::HandleBgInstCreated matchId={} not in pending matches", matchId);
        return;
    }
    PendingBgMatch match = std::move(it->second);
    _pendingBgMatches.erase(it);

    // Group players by nodeId; send one MSG_CLUSTER_BG_READY per node.
    std::unordered_map<uint8, std::vector<std::pair<uint64, uint8>>> perNode;
    for (auto const& e : match.alliance) perNode[e.nodeId].emplace_back(e.guid, uint8(0));
    for (auto const& e : match.horde)    perNode[e.nodeId].emplace_back(e.guid, uint8(1));

    for (auto const& [targetNodeId, players] : perNode)
    {
        // Payload: instanceId(4)+bgTypeId(4)+mapId(4)+clientInstanceId(4)+count(1)+{guid(8)+team(1)}*n
        uint8 count = static_cast<uint8>(players.size());
        std::vector<uint8> readyPayload;
        readyPayload.reserve(17 + count * 9u);
        auto appendU32 = [&readyPayload](uint32 v)
        {
            readyPayload.push_back(v & 0xFF);
            readyPayload.push_back((v >> 8) & 0xFF);
            readyPayload.push_back((v >> 16) & 0xFF);
            readyPayload.push_back((v >> 24) & 0xFF);
        };
        appendU32(instanceId);
        appendU32(match.bgTypeId);
        appendU32(mapId);
        appendU32(clientInstanceId);
        readyPayload.push_back(count);
        for (auto const& [pGuid, pTeam] : players)
        {
            uint8 t[8]; std::memcpy(t, &pGuid, 8);
            readyPayload.insert(readyPayload.end(), t, t + 8);
            readyPayload.push_back(pTeam);
        }

        PublishToNode(targetNodeId, MSG_CLUSTER_BG_READY, readyPayload.data(), static_cast<int>(readyPayload.size()));

        LOG_INFO("server.worldserver", "ProxyClient::HandleBgInstCreated matchId={} instanceId={} → node {} ({} players)",
                 matchId, instanceId, targetNodeId, count);
    }
}
