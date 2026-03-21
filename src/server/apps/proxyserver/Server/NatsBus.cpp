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

#include "NatsBus.h"
#include "ClusterMessages.h"
#include "RAServer.h"
#include "Log.h"
#include "ProxyMgr.h"

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

// ── Initialize / Shutdown ─────────────────────────────────────────────────────

void NatsBus::Initialize(std::string const& natsUrl)
{
    natsStatus s = natsConnection_ConnectTo(&_nc, natsUrl.c_str());
    if (s != NATS_OK)
    {
        LOG_ERROR("proxy.nats", "NatsBus: Failed to connect to NATS at {} — {}", natsUrl, natsStatus_GetText(s));
        _nc = nullptr;
        return;
    }

    // Subscribe to registration requests (request-reply).
    s = natsConnection_Subscribe(&_subRegister, _nc, "cluster.register", OnRegisterMsg, nullptr);
    if (s != NATS_OK)
    {
        LOG_ERROR("proxy.nats", "NatsBus: Failed to subscribe to cluster.register — {}", natsStatus_GetText(s));
        Shutdown();
        return;
    }

    // Subscribe to all ongoing control messages from worldnodes.
    s = natsConnection_Subscribe(&_subProxy, _nc, "cluster.proxy", OnClusterProxyMsg, nullptr);
    if (s != NATS_OK)
    {
        LOG_ERROR("proxy.nats", "NatsBus: Failed to subscribe to cluster.proxy — {}", natsStatus_GetText(s));
        Shutdown();
        return;
    }

    // Subscribe to instance-address queries from worldnodes (request-reply).
    s = natsConnection_Subscribe(&_subInstQuery, _nc, "cluster.instance.query", OnInstanceQueryMsg, nullptr);
    if (s != NATS_OK)
    {
        LOG_ERROR("proxy.nats", "NatsBus: Failed to subscribe to cluster.instance.query — {}", natsStatus_GetText(s));
        Shutdown();
        return;
    }

    // Subscribe to worldserver peer-discovery announcements (cluster.announce).
    // Worldservers publish their identity here; the proxy registers them and sends
    // MSG_ANNOUNCE_ACK so they stop their 10-second retry loop.
    s = natsConnection_Subscribe(&_subAnnounce, _nc, "cluster.announce", OnAnnounceMsg, nullptr);
    if (s != NATS_OK)
    {
        LOG_ERROR("proxy.nats", "NatsBus: Failed to subscribe to cluster.announce — {}", natsStatus_GetText(s));
        // Non-fatal — cluster.register still works as a fallback.
    }

    LOG_INFO("proxy.nats", "NatsBus: Connected to NATS at {}", natsUrl);
}

void NatsBus::Shutdown()
{
    if (_subInstQuery) { natsSubscription_Destroy(_subInstQuery); _subInstQuery = nullptr; }
    if (_subProxy)     { natsSubscription_Destroy(_subProxy);     _subProxy     = nullptr; }
    if (_subAnnounce)  { natsSubscription_Destroy(_subAnnounce);  _subAnnounce  = nullptr; }
    if (_subRegister)  { natsSubscription_Destroy(_subRegister);  _subRegister  = nullptr; }
    if (_nc)           { natsConnection_Destroy(_nc);              _nc           = nullptr; }
}

// ── Publish helpers ───────────────────────────────────────────────────────────

void NatsBus::PublishToNode(uint8 nodeId, uint8 const* data, int len)
{
    if (!_nc) return;
    std::string subj = "cluster.node." + std::to_string(nodeId);
    natsStatus s = natsConnection_Publish(_nc, subj.c_str(), data, len);
    if (s != NATS_OK)
        LOG_WARN("proxy.nats", "NatsBus: PublishToNode({}) failed — {}", nodeId, natsStatus_GetText(s));
}

void NatsBus::PublishBroadcast(uint8 const* data, int len)
{
    if (!_nc) return;
    natsStatus s = natsConnection_Publish(_nc, "cluster.broadcast", data, len);
    if (s != NATS_OK)
        LOG_WARN("proxy.nats", "NatsBus: PublishBroadcast failed — {}", natsStatus_GetText(s));
}

void NatsBus::BroadcastRACommand(uint32 reqId, std::string const& cmd)
{
    if (!_nc) return;
    uint16 cmdLen = static_cast<uint16>(std::min(cmd.size(), size_t(65535)));
    std::vector<uint8> buf;
    buf.reserve(1 + 4 + 2 + cmdLen);
    buf.push_back(ClusterMsg::RA_COMMAND);
    buf.push_back(static_cast<uint8>(reqId & 0xFF));
    buf.push_back(static_cast<uint8>((reqId >> 8)  & 0xFF));
    buf.push_back(static_cast<uint8>((reqId >> 16) & 0xFF));
    buf.push_back(static_cast<uint8>((reqId >> 24) & 0xFF));
    buf.push_back(static_cast<uint8>(cmdLen & 0xFF));
    buf.push_back(static_cast<uint8>(cmdLen >> 8));
    buf.insert(buf.end(), cmd.begin(), cmd.begin() + cmdLen);
    PublishBroadcast(buf.data(), static_cast<int>(buf.size()));
}

// ── Instance address query request-reply ─────────────────────────────────────

/// Called on the NATS dispatch thread when a worldnode asks "which instance should I use?"
///
/// Request payload: empty (no content required).
/// Reply payload: [addrLen:1][addr:addrLen][port:2]
///   addrLen = 0 means no instance is available.
void NatsBus::OnInstanceQueryMsg(natsConnection* nc, natsSubscription* /*sub*/,
                                  natsMsg* msg, void* /*closure*/)
{
    const char* replyTo = natsMsg_GetReply(msg);
    natsMsg_Destroy(msg);

    if (!replyTo || !replyTo[0])
        return;

    std::string outAddr;
    uint16      outPort = 0;
    bool found = sProxyMgr.GetBestInstanceAddress(outAddr, outPort);

    // Build reply: [addrLen:1][addr:addrLen][port:2]
    std::vector<uint8> reply;
    if (found && !outAddr.empty())
    {
        uint8 addrLen = static_cast<uint8>(std::min(outAddr.size(), size_t(255)));
        reply.reserve(1 + addrLen + 2);
        reply.push_back(addrLen);
        reply.insert(reply.end(), outAddr.begin(), outAddr.begin() + addrLen);
        reply.push_back(static_cast<uint8>(outPort & 0xFF));
        reply.push_back(static_cast<uint8>(outPort >> 8));
    }
    else
    {
        reply.push_back(0); // addrLen = 0 signals "no instance available"
    }

    natsStatus s = natsConnection_Publish(nc, replyTo,
                                          reply.data(), static_cast<int>(reply.size()));
    if (s != NATS_OK)
        LOG_WARN("proxy.nats", "NatsBus: OnInstanceQueryMsg reply failed — {}", natsStatus_GetText(s));
}

// ── Registration request-reply ────────────────────────────────────────────────

/// Called on the NATS dispatch thread when a worldnode sends a registration request.
///
/// Wire format (payload of the NATS message on "cluster.register"):
///   [serverType:1][gamePort:2][mapCount:2][mapIds:4*mapCount][addrLen:1][addr:addrLen]
///
/// Reply: single byte — the assigned nodeId.
void NatsBus::OnRegisterMsg(natsConnection* nc, natsSubscription* /*sub*/,
                             natsMsg* msg, void* /*closure*/)
{
    const uint8* d = reinterpret_cast<const uint8*>(natsMsg_GetData(msg));
    int n          = natsMsg_GetDataLength(msg);
    const char* replyTo = natsMsg_GetReply(msg);

    // Minimum: serverType(1) + gamePort(2) + mapCount(2) + addrLen(1) = 6 bytes
    if (n < 6 || !replyTo || !replyTo[0])
    {
        LOG_WARN("proxy.nats", "NatsBus: OnRegisterMsg — malformed or missing reply subject");
        natsMsg_Destroy(msg);
        return;
    }

    uint8  serverType = d[0];
    uint16 gamePort;
    std::memcpy(&gamePort, d + 1, 2);
    uint16 mapCount;
    std::memcpy(&mapCount, d + 3, 2);

    int offset = 5;
    if (n < offset + mapCount * 4 + 1)
    {
        LOG_WARN("proxy.nats", "NatsBus: OnRegisterMsg — payload too short for {} maps", mapCount);
        natsMsg_Destroy(msg);
        return;
    }

    std::vector<uint32> maps;
    maps.reserve(mapCount);
    for (int i = 0; i < mapCount; ++i)
    {
        uint32 mapId;
        std::memcpy(&mapId, d + offset, 4);
        maps.push_back(mapId);
        offset += 4;
    }

    uint8 addrLen = d[offset++];
    if (n < offset + addrLen)
    {
        LOG_WARN("proxy.nats", "NatsBus: OnRegisterMsg — payload too short for addr");
        natsMsg_Destroy(msg);
        return;
    }
    std::string peerIp(reinterpret_cast<const char*>(d + offset), addrLen);

    natsMsg_Destroy(msg);

    // Register with ProxyMgr and get an assigned node ID.
    uint8 nodeId = sProxyMgr.RegisterNode(serverType, gamePort, peerIp, maps);
    if (nodeId == 0)
    {
        LOG_ERROR("proxy.nats", "NatsBus: RegisterNode failed — no slot available");
        // Reply with 0 to signal rejection.
    }

    // Reply with the assigned node ID.
    natsStatus s = natsConnection_Publish(nc, replyTo,
                                          reinterpret_cast<const void*>(&nodeId), 1);
    if (s != NATS_OK)
        LOG_WARN("proxy.nats", "NatsBus: OnRegisterMsg reply failed — {}", natsStatus_GetText(s));
}

// ── Ongoing control message dispatcher ───────────────────────────────────────

/// Called on the NATS dispatch thread for every message on "cluster.proxy".
///
/// Wire format:
///   [sourceNodeId:1][msgType:1][type-specific payload...]
///
/// Dispatches directly to ProxyMgr handler methods.  No parse state machine
/// needed — NATS guarantees each callback receives exactly one complete message.
void NatsBus::OnClusterProxyMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                                 natsMsg* msg, void* /*closure*/)
{
    const uint8* d = reinterpret_cast<const uint8*>(natsMsg_GetData(msg));
    int n          = natsMsg_GetDataLength(msg);

    if (n < 2)
    {
        natsMsg_Destroy(msg);
        return;
    }

    uint8        nodeId  = d[0];
    uint8        msgType = d[1];
    const uint8* p       = d + 2;  // start of type-specific payload
    int          rem     = n - 2;

    // Dispatch.  Payload sizes mirror the ControlSocket constants exactly.
    switch (msgType)
    {
        // ── MSG_REROUTE_PLAYER (0x02) ─────────────────────────────────────────
        case ClusterMsg::REROUTE_PLAYER:
        {
            // guid(8) + addrLen(1) + addr[addrLen] + port(2) + map(4) + x(4) + y(4) + z(4) + ori(4)
            if (rem < 11) break;
            uint64 guid;    std::memcpy(&guid, p, 8);
            uint8  addrLen = p[8];
            if (rem < 9 + addrLen + 2) break;
            std::string addr(reinterpret_cast<const char*>(p + 9), addrLen);
            int addrEnd = 9 + addrLen;
            uint16 port;    std::memcpy(&port, p + addrEnd, 2);
            // Optional map/position fields (added for native SMSG_NEW_WORLD reroute).
            uint32 mapId = 0;
            float  rx = 0.f, ry = 0.f, rz = 0.f, rori = 0.f;
            if (rem >= addrEnd + 2 + 20)
            {
                std::memcpy(&mapId, p + addrEnd + 2,      4);
                std::memcpy(&rx,    p + addrEnd + 2 + 4,  4);
                std::memcpy(&ry,    p + addrEnd + 2 + 8,  4);
                std::memcpy(&rz,    p + addrEnd + 2 + 12, 4);
                std::memcpy(&rori,  p + addrEnd + 2 + 16, 4);
            }
            sProxyMgr.ReroutePlayer(guid, addr, port, mapId, rx, ry, rz, rori);
            break;
        }
        // ── MSG_SEAMLESS_REROUTE (0x23) ── zone-based same-map, no loading screen
        case ClusterMsg::SEAMLESS_REROUTE:
        {
            // guid(8) + addrLen(1) + addr[addrLen] + port(2)
            if (rem < 11) break;
            uint64 guid;    std::memcpy(&guid, p, 8);
            uint8  addrLen = p[8];
            if (rem < 9 + addrLen + 2) break;
            std::string addr(reinterpret_cast<const char*>(p + 9), addrLen);
            int addrEnd = 9 + addrLen;
            uint16 port;    std::memcpy(&port, p + addrEnd, 2);
            sProxyMgr.SeamlessReroutePlayer(guid, addr, port);
            break;
        }
        // ── MSG_CLUSTER_PLAYER_ONLINE (0x03) ─────────────────────────────────
        case ClusterMsg::CLUSTER_PLAYER_ONLINE:
        {
            // guid(8) + nameLen(1) + name[nameLen] + zone(4) + level + class + race + team + nodeId
            if (rem < 10) break;
            uint64 guid;    std::memcpy(&guid, p, 8);
            uint8  nameLen = p[8];
            // tail after name: zone(4) + level + class + race + team + (nodeId in old wire, ignored)
            if (rem < 9 + nameLen + 8) break;
            std::string name(reinterpret_cast<const char*>(p + 9), nameLen);
            const uint8* tail = p + 9 + nameLen;
            uint32 zoneId;  std::memcpy(&zoneId, tail, 4);
            uint8 level = tail[4], classId = tail[5], raceId = tail[6], teamId = tail[7];
            // Always trust the wire nodeId from the NATS message header, not the payload field.
            sProxyMgr.OnPlayerOnline(guid, nodeId, std::move(name), zoneId, level, classId, raceId, teamId);
            break;
        }
        // ── MSG_CLUSTER_PLAYER_OFFLINE (0x04) ────────────────────────────────
        case ClusterMsg::CLUSTER_PLAYER_OFFLINE:
        {
            if (rem < 8) break;
            uint64 guid; std::memcpy(&guid, p, 8);
            sProxyMgr.OnPlayerOffline(guid, nodeId);
            break;
        }
        // ── MSG_CLUSTER_DELIVER_PACKET (0x05) ────────────────────────────────
        case ClusterMsg::CLUSTER_DELIVER_PACKET:
        {
            // targetGuid(8) + packetLen(2) + packet[packetLen]
            if (rem < 10) break;
            uint64 targetGuid; std::memcpy(&targetGuid, p, 8);
            uint16 pktLen;     std::memcpy(&pktLen, p + 8, 2);
            if (rem < 10 + pktLen) break;
            std::vector<uint8> pktData(p + 10, p + 10 + pktLen);
            sProxyMgr.DeliverPacketToPlayer(targetGuid, std::move(pktData));
            break;
        }
        // ── MSG_CLUSTER_RELAY_TO_NODE (0x06) ─────────────────────────────────
        case ClusterMsg::CLUSTER_RELAY_TO_NODE:
        {
            // targetNode(1) + innerType(1) + payloadLen(2) + payload
            if (rem < 4) break;
            uint8  tgtNode   = p[0];
            uint8  innerType = p[1];
            uint16 pl;        std::memcpy(&pl, p + 2, 2);
            if (rem < 4 + pl) break;

            // Re-wrap and forward to the target node.
            std::vector<uint8> fwd;
            fwd.reserve(5 + pl);
            fwd.push_back(ClusterMsg::CLUSTER_RELAY_TO_NODE);
            fwd.push_back(tgtNode);
            fwd.push_back(innerType);
            fwd.push_back(static_cast<uint8>(pl & 0xFF));
            fwd.push_back(static_cast<uint8>(pl >> 8));
            fwd.insert(fwd.end(), p + 4, p + 4 + pl);
            sProxyMgr.RelayToNode(tgtNode, fwd);
            break;
        }
        // ── MSG_CLUSTER_GROUP_UPDATE (0x07) ──────────────────────────────────
        case ClusterMsg::CLUSTER_GROUP_UPDATE:
        {
            // groupGuid(8) + memberCount(1) + memberCount * 11 bytes
            if (rem < 9) break;
            uint64 groupGuid; std::memcpy(&groupGuid, p, 8);
            uint8  mc = p[8];
            if (rem < 9 + mc * 11) break;
            std::vector<uint8> memberData(p + 9, p + 9 + mc * 11);
            sProxyMgr.OnGroupUpdate(groupGuid, nodeId, mc, std::move(memberData));
            break;
        }
        // ── MSG_CLUSTER_GROUP_DISBAND (0x08) ─────────────────────────────────
        case ClusterMsg::CLUSTER_GROUP_DISBAND:
        {
            if (rem < 8) break;
            uint64 groupGuid; std::memcpy(&groupGuid, p, 8);
            sProxyMgr.OnGroupDisband(groupGuid, nodeId);
            break;
        }
        // ── MSG_CLUSTER_LFG_RELAY (0x09) ─────────────────────────────────────
        case ClusterMsg::CLUSTER_LFG_RELAY:
        {
            // payloadLen(2) + payload
            if (rem < 2) break;
            uint16 pl; std::memcpy(&pl, p, 2);
            if (rem < 2 + pl) break;
            std::vector<uint8> payload(p + 2, p + 2 + pl);
            sProxyMgr.RelayToLFGMaster(nodeId, std::move(payload));
            break;
        }
        // ── MSG_CLUSTER_LFG_RELAY_RESP (0x0A) ────────────────────────────────
        case ClusterMsg::CLUSTER_LFG_RELAY_RESP:
        {
            // targetNode(1) + payloadLen(2) + payload
            if (rem < 3) break;
            uint8  tgtNode = p[0];
            uint16 pl;      std::memcpy(&pl, p + 1, 2);
            if (rem < 3 + pl) break;

            std::vector<uint8> fwd;
            fwd.reserve(4 + pl);
            fwd.push_back(ClusterMsg::CLUSTER_LFG_RELAY_RESP);
            fwd.push_back(tgtNode);
            fwd.push_back(static_cast<uint8>(pl & 0xFF));
            fwd.push_back(static_cast<uint8>(pl >> 8));
            fwd.insert(fwd.end(), p + 3, p + 3 + pl);
            sProxyMgr.RelayToNode(tgtNode, fwd);
            break;
        }
        // ── MSG_REROUTE_TO_MAP (0x0B) ─────────────────────────────────────────
        case ClusterMsg::REROUTE_TO_MAP:
        {
            // guid(8) + mapId(4)
            if (rem < 12) break;
            uint64 guid;  std::memcpy(&guid,  p,     8);
            uint32 mapId; std::memcpy(&mapId, p + 8, 4);
            sProxyMgr.RerouteToMap(guid, mapId);
            break;
        }
        // ── MSG_CLUSTER_UNIT_UPDATE (0x0C) ────────────────────────────────────
        case ClusterMsg::CLUSTER_UNIT_UPDATE:
        {
            // payloadLen(2) + payload
            if (rem < 2) break;
            uint16 pl; std::memcpy(&pl, p, 2);
            if (rem < 2 + pl) break;
            std::vector<uint8> payload(p + 2, p + 2 + pl);
            sProxyMgr.BroadcastUnitUpdate(nodeId, pl, payload);
            break;
        }
        // ── MSG_PONG (0x0E) ───────────────────────────────────────────────────
        case ClusterMsg::PONG:
        {
            // echoed uint64 timestamp_ms from the ping we sent
            if (rem < 8) break;
            uint64 sentMs; std::memcpy(&sentMs, p, 8);
            using namespace std::chrono;
            uint64 nowMs = static_cast<uint64>(
                duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
            uint32 latencyMs = (nowMs > sentMs) ? static_cast<uint32>(nowMs - sentMs) : 0;
            sProxyMgr.OnNodePong(nodeId, latencyMs);
            break;
        }
        // ── MSG_CLUSTER_CHAT (0x11) ───────────────────────────────────────────
        case ClusterMsg::CLUSTER_CHAT:
        {
            if (rem < 2) break;
            uint16 pl; std::memcpy(&pl, p, 2);
            if (rem < 2 + pl) break;
            std::vector<uint8> payload(p + 2, p + 2 + pl);
            sProxyMgr.BroadcastChatRelay(nodeId, pl, payload);
            break;
        }
        // ── MSG_CLUSTER_NOTIFY_MAIL (0x12) ────────────────────────────────────
        case ClusterMsg::CLUSTER_NOTIFY_MAIL:
        {
            if (rem < 8) break;
            uint64 recipientGuid; std::memcpy(&recipientGuid, p, 8);
            sProxyMgr.RouteMailNotification(recipientGuid);
            break;
        }
        // ── MSG_CLUSTER_BG_QUEUE_JOIN (0x13) ─────────────────────────────────
        case ClusterMsg::CLUSTER_BG_QUEUE_JOIN:
        {
            // guid(8) + bgTypeId(4) + bracketId(1) + teamId(1) + minPerTeam(1)
            if (rem < 15) break;
            uint64 guid;     std::memcpy(&guid,     p,      8);
            uint32 bgTypeId; std::memcpy(&bgTypeId, p + 8,  4);
            uint8  bracketId  = p[12];
            uint8  teamId     = p[13];
            uint8  minPerTeam = p[14];
            sProxyMgr.OnBgQueueJoin(guid, nodeId, bgTypeId, bracketId, teamId, minPerTeam);
            break;
        }
        // ── MSG_CLUSTER_BG_QUEUE_LEAVE (0x14) ────────────────────────────────
        case ClusterMsg::CLUSTER_BG_QUEUE_LEAVE:
        {
            // guid(8) + bgTypeId(4)
            if (rem < 12) break;
            uint64 guid;     std::memcpy(&guid,     p,     8);
            uint32 bgTypeId; std::memcpy(&bgTypeId, p + 8, 4);
            sProxyMgr.OnBgQueueLeave(guid, bgTypeId);
            break;
        }
        // ── MSG_CLUSTER_BG_INST_CREATED (0x17) ───────────────────────────────
        case ClusterMsg::CLUSTER_BG_INST_CREATED:
        {
            // matchId(4) + instanceId(4) + mapId(4) + clientInstanceId(4)
            if (rem < 16) break;
            uint32 matchId, instanceId, mapId, clientInstanceId;
            std::memcpy(&matchId,          p,      4);
            std::memcpy(&instanceId,       p + 4,  4);
            std::memcpy(&mapId,            p + 8,  4);
            std::memcpy(&clientInstanceId, p + 12, 4);
            sProxyMgr.OnBgInstCreated(matchId, instanceId, mapId, clientInstanceId);
            break;
        }
        // ── MSG_CLUSTER_ARENA_RESULT (0x16) ──────────────────────────────────
        case ClusterMsg::CLUSTER_ARENA_RESULT:
        {
            if (rem < 2) break;
            uint16 pl; std::memcpy(&pl, p, 2);
            if (rem < 2 + pl) break;
            std::vector<uint8> payload(p + 2, p + 2 + pl);
            sProxyMgr.BroadcastArenaResult(nodeId, pl, payload);
            break;
        }
        // ── MSG_NODE_STATUS (0x19) — 10s player count + NATS bandwidth ──────────
        case ClusterMsg::NODE_STATUS:
        {
            if (rem < 12) break;
            uint32 playerCount, natsBytesTx, natsBytesRx;
            std::memcpy(&playerCount, p,     4);
            std::memcpy(&natsBytesTx, p + 4, 4);
            std::memcpy(&natsBytesRx, p + 8, 4);
            sProxyMgr.HandleNodeStatus(nodeId, playerCount, natsBytesTx, natsBytesRx);
            break;
        }
        // ── MSG_NODE_REFRESH (0x1A) — 5min full port/map re-registration ────────
        case ClusterMsg::NODE_REFRESH:
        {
            if (rem < 5) break;
            sProxyMgr.HandleNodeRefresh(nodeId, p, rem);
            break;
        }
        // ── MSG_RA_REPLY (0x22) ──────────────────────────────────────────────────────────────────────────────
        case ClusterMsg::RA_REPLY:
        {
            // reqId(4) + outputLen(2) + output[outputLen]
            if (rem < 6) break;
            uint32 reqId;    std::memcpy(&reqId,  p,     4);
            uint16 outLen;   std::memcpy(&outLen, p + 4, 2);
            if (rem < 6 + outLen) break;
            std::string output(reinterpret_cast<const char*>(p + 6), outLen);
            int totalNodes = sProxyMgr.GetRegisteredNodeCount();
            sRAServer.AppendReply(nodeId, reqId, output, totalNodes);
            break;
        }
        default:
            LOG_WARN("proxy.nats", "NatsBus: Unknown msgType 0x{:02X} from node {}", msgType, nodeId);
            break;
    }

    natsMsg_Destroy(msg);
}

// ── Worldserver announce handler ──────────────────────────────────────────────

/// Called on the NATS dispatch thread when a worldserver publishes cluster.announce.
///
/// Wire format (matches ProxyClient::PublishAnnounce):
///   [nodeId:1][serverType:1][gamePort:2][addrLen:1][addr:addrLen][mapCount:2][mapIds:4*mapCount]
///
/// Registers the node with ProxyMgr and sends MSG_ANNOUNCE_ACK (0x20) on
/// cluster.node.{nodeId} so the worldserver stops its 10-second retry loop.
void NatsBus::OnAnnounceMsg(natsConnection* nc, natsSubscription* /*sub*/,
                             natsMsg* msg, void* /*closure*/)
{
    const uint8* d = reinterpret_cast<const uint8*>(natsMsg_GetData(msg));
    int n          = natsMsg_GetDataLength(msg);

    // Minimum: nodeId(1)+serverType(1)+gamePort(2)+addrLen(1)+addr(1)+mapCount(2) = 8
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

    std::string peerIp(reinterpret_cast<const char*>(d + 5), addrLen);
    int off = 5 + addrLen;

    uint16 mapCount = static_cast<uint16>(d[off]) | (static_cast<uint16>(d[off + 1]) << 8);
    off += 2;

    if (mapCount > 128)
        mapCount = 128;

    if (n < off + mapCount * 4)
    {
        natsMsg_Destroy(msg);
        return;
    }

    std::vector<uint32> maps;
    maps.reserve(mapCount);
    for (uint16 i = 0; i < mapCount; ++i)
    {
        uint32 mapId = 0;
        std::memcpy(&mapId, d + off + i * 4, 4);
        maps.push_back(mapId);
    }

    natsMsg_Destroy(msg);

    // Register with ProxyMgr (thread-safe — uses internal mutex).
    uint8 assignedId = sProxyMgr.RegisterNode(serverType, gamePort, peerIp, maps);

    LOG_INFO("proxy.nats", "NatsBus: OnAnnounceMsg — registered node {} (type={} addr={}:{} maps={})",
             assignedId, serverType, peerIp, gamePort, mapCount);

    // Send MSG_ANNOUNCE_ACK (0x20) on cluster.node.{nodeId} so the worldserver
    // stops its 10-second retry loop.  Use nodeId from the announcement, not
    // assignedId (they should match for static configs, but the worldserver
    // listens on its own nodeId subject).
    if (nc)
    {
        std::string ackSubject = "cluster.node." + std::to_string(nodeId);
        uint8 ackBuf[2] = { 0x20, 0 }; // MSG_ANNOUNCE_ACK, proxy nodeId=0
        natsStatus s = natsConnection_Publish(nc, ackSubject.c_str(), ackBuf, 2);
        if (s != NATS_OK)
            LOG_WARN("proxy.nats", "NatsBus: OnAnnounceMsg ACK publish failed — {}", natsStatus_GetText(s));
    }
}
