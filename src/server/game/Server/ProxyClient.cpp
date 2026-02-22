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

#include "ProxyClient.h"
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
#include "Globals/ObjectAccessor.h"
#include "Groups/Group.h"
#include "Log.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include <chrono>
#include <cstring>
#include <shared_mutex>

// ── Initialize (NATS-based) ───────────────────────────────────────────────────

void ProxyClient::Initialize(std::string const& natsUrl, uint8 serverType,
                              uint16 gamePort, std::string const& gameAddress)
{
    _natsUrl     = natsUrl;
    _serverType  = serverType;
    _gamePort    = gamePort;
    _gameAddress = gameAddress;

    // Connect to NATS synchronously.
    natsStatus s = natsConnection_ConnectTo(&_nc, natsUrl.c_str());
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to connect to NATS at {} — {}",
                  natsUrl, natsStatus_GetText(s));
        return;
    }

    // Build registration payload:
    //   [serverType:1][gamePort:2][mapCount:2][mapIds:4*n][addrLen:1][addr:n]
    auto localMaps = sClusterMgr.GetLocalMaps();
    uint16 mapCount = static_cast<uint16>(localMaps.size());
    uint8  addrLen  = static_cast<uint8>(gameAddress.size());

    std::vector<uint8> regPayload;
    regPayload.reserve(5 + mapCount * 4 + 1 + addrLen);
    regPayload.push_back(serverType);
    regPayload.push_back(static_cast<uint8>(gamePort & 0xFF));
    regPayload.push_back(static_cast<uint8>(gamePort >> 8));
    regPayload.push_back(static_cast<uint8>(mapCount & 0xFF));
    regPayload.push_back(static_cast<uint8>(mapCount >> 8));
    for (uint32 mapId : localMaps)
    {
        regPayload.push_back(static_cast<uint8>(mapId & 0xFF));
        regPayload.push_back(static_cast<uint8>((mapId >> 8) & 0xFF));
        regPayload.push_back(static_cast<uint8>((mapId >> 16) & 0xFF));
        regPayload.push_back(static_cast<uint8>((mapId >> 24) & 0xFF));
    }
    regPayload.push_back(addrLen);
    regPayload.insert(regPayload.end(), gameAddress.begin(), gameAddress.end());

    // Send registration request and wait for reply (up to 10s).
    natsMsg* reply = nullptr;
    s = natsConnection_Request(&reply, _nc, "cluster.register",
                               regPayload.data(), static_cast<int>(regPayload.size()), 10000);
    if (s != NATS_OK || !reply)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Registration request to proxy failed — {}",
                  natsStatus_GetText(s));
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    if (natsMsg_GetDataLength(reply) < 1)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Registration reply too short");
        natsMsg_Destroy(reply);
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    _nodeId = *reinterpret_cast<const uint8*>(natsMsg_GetData(reply));
    natsMsg_Destroy(reply);

    if (_nodeId == 0)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Proxy rejected registration (nodeId=0)");
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    // Subscribe to messages directed at this node.
    std::string nodeSub = "cluster.node." + std::to_string(_nodeId);
    s = natsConnection_Subscribe(&_subNode, _nc, nodeSub.c_str(), OnNatsMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to subscribe to {} — {}", nodeSub, natsStatus_GetText(s));
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    // Subscribe to broadcasts (fanout from proxy to all nodes).
    s = natsConnection_Subscribe(&_subBroadcast, _nc, "cluster.broadcast", OnNatsMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("server.worldserver", "ProxyClient: Failed to subscribe to cluster.broadcast — {}", natsStatus_GetText(s));
        natsSubscription_Destroy(_subNode);
        _subNode = nullptr;
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return;
    }

    _connected = true;
    LOG_INFO("server.worldserver",
             "ProxyClient: Registered via NATS as node {} (serverType={}, gamePort={})",
             _nodeId, serverType, gamePort);
}

// ── NATS publish helper ───────────────────────────────────────────────────────

void ProxyClient::PublishToProxy(uint8 msgType, uint8 const* payload, int payloadLen)
{
    if (!_nc || !_connected)
        return;

    // Wire format on cluster.proxy: [sourceNodeId:1][msgType:1][payload...]
    std::vector<uint8> buf;
    buf.reserve(2 + payloadLen);
    buf.push_back(_nodeId);
    buf.push_back(msgType);
    if (payloadLen > 0)
        buf.insert(buf.end(), payload, payload + payloadLen);

    natsStatus s = natsConnection_Publish(_nc, "cluster.proxy", buf.data(), static_cast<int>(buf.size()));
    if (s != NATS_OK)
        LOG_WARN("server.worldserver", "ProxyClient: Publish to cluster.proxy failed — {}", natsStatus_GetText(s));
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
    natsMsg_Destroy(msg);

    // Marshal to world thread.
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
            if (payload.size() < 8) break;
            uint64 guid; std::memcpy(&guid, payload.data(), 8);
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
        case MSG_CLUSTER_BG_CREATE_INST:
        {
            if (payload.size() < 2) break;
            uint16 pl; std::memcpy(&pl, payload.data(), 2);
            if (payload.size() < static_cast<std::size_t>(2 + pl)) break;
            std::vector<uint8> inner(payload.begin() + 2, payload.begin() + 2 + pl);
            HandleBgCreateInst(inner);
            break;
        }
        case MSG_CLUSTER_BG_READY:
        {
            if (payload.size() < 2) break;
            uint16 pl; std::memcpy(&pl, payload.data(), 2);
            if (payload.size() < static_cast<std::size_t>(2 + pl)) break;
            std::vector<uint8> inner(payload.begin() + 2, payload.begin() + 2 + pl);
            HandleBgReady(inner);
            break;
        }
        case MSG_PING:
        {
            // Echo timestamp back to proxy as MSG_PONG on cluster.proxy.
            if (payload.size() < 8) break;
            uint64 ts; std::memcpy(&ts, payload.data(), 8);
            uint8 pongBuf[8];
            std::memcpy(pongBuf, &ts, 8);
            PublishToProxy(MSG_PONG, pongBuf, 8);
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

// ── Outgoing cluster messages ─────────────────────────────────────────────────

void ProxyClient::SendReroute(uint64 playerGuid, std::string const& address, uint16 port)
{
    if (!_connected)
    {
        LOG_WARN("server.worldserver", "ProxyClient: SendReroute called but not connected");
        return;
    }

    uint8 addrLen = static_cast<uint8>(std::min(address.size(), std::size_t(255)));

    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + addrLen + 2);
    msg.push_back(MSG_REROUTE_PLAYER);

    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));

    msg.push_back(addrLen);
    msg.insert(msg.end(), address.begin(), address.begin() + addrLen);
    msg.push_back(static_cast<uint8>(port & 0xFF));
    msg.push_back(static_cast<uint8>(port >> 8));

    PublishToProxy(MSG_REROUTE_PLAYER, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendRerouteToMap(uint64 playerGuid, uint32 mapId)
{
    if (!_connected)
    {
        LOG_WARN("server.worldserver", "ProxyClient: SendRerouteToMap called but not connected");
        return;
    }

    // Wire: MSG_REROUTE_TO_MAP | guid(8 LE) | mapId(4 LE)  = 13 bytes total
    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 4);
    msg.push_back(MSG_REROUTE_TO_MAP);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    for (int i = 0; i < 4; ++i)
        msg.push_back(static_cast<uint8>((mapId >> (i * 8)) & 0xFF));

    PublishToProxy(MSG_REROUTE_TO_MAP, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + name.size() + 4 + 4);
    msg.push_back(MSG_CLUSTER_PLAYER_ONLINE);

    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((guid >> (i * 8)) & 0xFF));

    msg.push_back(static_cast<uint8>(name.size()));
    msg.insert(msg.end(), name.begin(), name.end());

    msg.push_back(static_cast<uint8>(zoneId & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 8) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 16) & 0xFF));
    msg.push_back(static_cast<uint8>((zoneId >> 24) & 0xFF));

    msg.push_back(level);
    msg.push_back(classId);
    msg.push_back(raceId);
    msg.push_back(teamId);

    PublishToProxy(MSG_CLUSTER_PLAYER_ONLINE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::AnnounceOffline(uint64 playerGuid)
{
    if (!_connected)
        return;

    std::vector<uint8> msg(9);
    msg[0] = MSG_CLUSTER_PLAYER_OFFLINE;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF);

    PublishToProxy(MSG_CLUSTER_PLAYER_OFFLINE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::RelayToNode(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload)
{
    if (!_connected)
        return;

    uint16 payloadLen = static_cast<uint16>(payload.size());
    std::vector<uint8> msg;
    msg.reserve(1 + RELAY_HEADER_SIZE + payload.size());
    msg.push_back(MSG_CLUSTER_RELAY_TO_NODE);
    msg.push_back(targetNodeId);
    msg.push_back(innerType);
    msg.push_back(static_cast<uint8>(payloadLen & 0xFF));
    msg.push_back(static_cast<uint8>(payloadLen >> 8));
    msg.insert(msg.end(), payload.begin(), payload.end());

    PublishToProxy(MSG_CLUSTER_RELAY_TO_NODE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendGroupUpdate(uint64 groupGuid, std::vector<uint8> const& memberData)
{
    if (!_connected)
        return;

    uint8 memberCount = static_cast<uint8>(memberData.size() / GROUP_MEMBER_SIZE);
    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + memberData.size());
    msg.push_back(MSG_CLUSTER_GROUP_UPDATE);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF));
    msg.push_back(memberCount);
    msg.insert(msg.end(), memberData.begin(), memberData.end());

    PublishToProxy(MSG_CLUSTER_GROUP_UPDATE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendGroupDisband(uint64 groupGuid)
{
    if (!_connected)
        return;

    std::vector<uint8> msg(9);
    msg[0] = MSG_CLUSTER_GROUP_DISBAND;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF);

    PublishToProxy(MSG_CLUSTER_GROUP_DISBAND, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    uint8 dungeonCount = static_cast<uint8>(dungeons.size());
    // payload = inner_type(1) + guid(8) + roles(1) + count(1) + dungeons(4 each)
    uint16 payloadLen = static_cast<uint16>(1 + 8 + 1 + 1 + 4 * dungeonCount);

    std::vector<uint8> msg;
    msg.reserve(1 + 2 + payloadLen);
    msg.push_back(MSG_CLUSTER_LFG_RELAY);
    msg.push_back(static_cast<uint8>(payloadLen & 0xFF));
    msg.push_back(static_cast<uint8>(payloadLen >> 8));
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
    PublishToProxy(MSG_CLUSTER_LFG_RELAY, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendLFGLeaveRelay(uint64 playerGuid)
{
    if (!_connected)
        return;

    // payload = inner_type(1) + guid(8) = 9 bytes
    std::vector<uint8> msg;
    msg.reserve(1 + 2 + 9);
    msg.push_back(MSG_CLUSTER_LFG_RELAY);
    msg.push_back(9);
    msg.push_back(0);
    msg.push_back(LFG_INNER_LEAVE);
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    PublishToProxy(MSG_CLUSTER_LFG_RELAY, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendLFGProposalResultRelay(uint32 proposalId, uint64 playerGuid, bool accept)
{
    if (!_connected)
        return;

    // payload = inner_type(1) + proposalId(4) + guid(8) + accept(1) = 14 bytes
    std::vector<uint8> msg;
    msg.reserve(1 + 2 + 14);
    msg.push_back(MSG_CLUSTER_LFG_RELAY);
    msg.push_back(14);
    msg.push_back(0);
    msg.push_back(LFG_INNER_PROPOSAL_RESULT);
    for (int i = 0; i < 4; ++i)
        msg.push_back(static_cast<uint8>((proposalId >> (i * 8)) & 0xFF));
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));
    msg.push_back(accept ? 1 : 0);
    PublishToProxy(MSG_CLUSTER_LFG_RELAY, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendLFGRelayResponse(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload)
{
    if (!_connected)
        return;

    // Wire: MSG_CLUSTER_LFG_RELAY_RESP | target_node(1) | payload_len(2) | inner_type(1) | payload
    uint16 totalLen = static_cast<uint16>(1 + payload.size()); // inner_type byte + payload
    std::vector<uint8> msg;
    msg.reserve(1 + 1 + 2 + 1 + payload.size());
    msg.push_back(MSG_CLUSTER_LFG_RELAY_RESP);
    msg.push_back(targetNodeId);
    msg.push_back(static_cast<uint8>(totalLen & 0xFF));
    msg.push_back(static_cast<uint8>(totalLen >> 8));
    msg.push_back(innerType);
    msg.insert(msg.end(), payload.begin(), payload.end());
    PublishToProxy(MSG_CLUSTER_LFG_RELAY_RESP, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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
            // Reroute the local player to the instance server.
            SendReroute(guid, addr, port);
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

    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 2 + rawPacket.size());
    msg.push_back(MSG_CLUSTER_DELIVER_PACKET);

    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((targetGuid >> (i * 8)) & 0xFF));

    msg.push_back(static_cast<uint8>(pktLen & 0xFF));
    msg.push_back(static_cast<uint8>(pktLen >> 8));
    msg.insert(msg.end(), rawPacket.begin(), rawPacket.end());

    PublishToProxy(MSG_CLUSTER_DELIVER_PACKET, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    PublishToProxy(MSG_CLUSTER_UNIT_UPDATE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    // MSG_CLUSTER_NOTIFY_MAIL: type(1) + guid(8)
    std::vector<uint8> msg(9);
    msg[0] = MSG_CLUSTER_NOTIFY_MAIL;
    std::memcpy(msg.data() + 1, &recipientGuid, 8);
    PublishToProxy(MSG_CLUSTER_NOTIFY_MAIL, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    PublishToProxy(MSG_CLUSTER_CHAT, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    PublishToProxy(MSG_CLUSTER_ARENA_RESULT, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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

    // type(1)+guid(8)+bgTypeId(4)+bracketId(1)+teamId(1)+minPerTeam(1) = 16 bytes
    std::vector<uint8> msg(16);
    msg[0] = MSG_CLUSTER_BG_QUEUE_JOIN;
    std::memcpy(msg.data() + 1, &guid,     8);
    std::memcpy(msg.data() + 9, &bgTypeId, 4);
    msg[13] = bracketId;
    msg[14] = teamId;
    msg[15] = minPerTeam;
    PublishToProxy(MSG_CLUSTER_BG_QUEUE_JOIN, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendBgQueueLeave(uint64 guid, uint32 bgTypeId)
{
    if (!_connected || _nodeId == 0)
        return;

    // type(1)+guid(8)+bgTypeId(4) = 13 bytes
    std::vector<uint8> msg(13);
    msg[0] = MSG_CLUSTER_BG_QUEUE_LEAVE;
    std::memcpy(msg.data() + 1, &guid,     8);
    std::memcpy(msg.data() + 9, &bgTypeId, 4);
    PublishToProxy(MSG_CLUSTER_BG_QUEUE_LEAVE, msg.data() + 1, static_cast<int>(msg.size()) - 1);
}

void ProxyClient::SendBgInstCreated(uint32 matchId, uint32 instanceId, uint32 mapId, uint32 clientInstanceId)
{
    if (!_connected || _nodeId == 0)
        return;

    // type(1)+matchId(4)+instanceId(4)+mapId(4)+clientInstanceId(4) = 17 bytes
    std::vector<uint8> msg(17);
    msg[0] = MSG_CLUSTER_BG_INST_CREATED;
    std::memcpy(msg.data() + 1,  &matchId,          4);
    std::memcpy(msg.data() + 5,  &instanceId,       4);
    std::memcpy(msg.data() + 9,  &mapId,            4);
    std::memcpy(msg.data() + 13, &clientInstanceId, 4);
    PublishToProxy(MSG_CLUSTER_BG_INST_CREATED, msg.data() + 1, static_cast<int>(msg.size()) - 1);
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
