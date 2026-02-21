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

#include "ControlSocket.h"
#include "Log.h"
#include "ProxyMgr.h"

ControlSocket::ControlSocket(IoContextTcpSocket&& socket)
    : BaseSocket(std::move(socket))
    , _accumBuffer(512)
{
}

void ControlSocket::Start()
{
    LOG_DEBUG("proxy.control", "ControlSocket: Backend connected from {}",
              GetRemoteIpAddress().to_string());
    AsyncRead();
}

void ControlSocket::OnClose()
{
    if (_nodeId != 0)
    {
        LOG_INFO("proxy.control", "ControlSocket: Node {} disconnected — unregistering", _nodeId);
        sProxyMgr.UnregisterNode(_nodeId);
        _nodeId = 0;
    }
}

// ── Outgoing ─────────────────────────────────────────────────────────────────

void ControlSocket::SendRaw(std::vector<uint8> const& data)
{
    if (data.empty())
        return;

    MessageBuffer buf(data.size());
    buf.Write(data.data(), data.size());
    QueuePacket(std::move(buf));
}

// ── Incoming ─────────────────────────────────────────────────────────────────

SocketReadCallbackResult ControlSocket::ReadHandler()
{
    MessageBuffer& raw = GetReadBuffer();
    std::size_t avail = raw.GetActiveSize();

    if (avail > 0)
    {
        // Compact before writing so remaining space is maximised.
        _accumBuffer.Normalize();
        _accumBuffer.EnsureFreeSpace();
        _accumBuffer.Write(raw.GetReadPointer(), avail);
        raw.ReadCompleted(avail);
    }

    ProcessBuffer();
    return SocketReadCallbackResult::KeepReading;
}

void ControlSocket::ProcessBuffer()
{
    while (_accumBuffer.GetActiveSize() > 0)
    {
        switch (_parseState)
        {
            // ── Wait for message type byte ────────────────────────────────────
            case ParseState::WaitType:
            {
                uint8 msgType = *_accumBuffer.GetReadPointer();
                _accumBuffer.ReadCompleted(1);

                switch (msgType)
                {
                    case MSG_REGISTER:              _parseState = ParseState::ReadRegister;      break;
                    case MSG_REROUTE_PLAYER:        _parseState = ParseState::ReadRerouteP1;     break;
                    case MSG_CLUSTER_PLAYER_ONLINE: _parseState = ParseState::ReadPlayerOnlineMeta; break;
                    case MSG_CLUSTER_PLAYER_OFFLINE:_parseState = ParseState::ReadPlayerOffline;  break;
                    case MSG_CLUSTER_DELIVER_PACKET:_parseState = ParseState::ReadDeliverP1;      break;
                    case MSG_CLUSTER_RELAY_TO_NODE: _parseState = ParseState::ReadRelayHeader;    break;
                    case MSG_CLUSTER_GROUP_UPDATE:  _parseState = ParseState::ReadGroupUpdateMeta;   break;
                    case MSG_CLUSTER_GROUP_DISBAND: _parseState = ParseState::ReadGroupDisband;      break;
                    case MSG_CLUSTER_LFG_RELAY:     _parseState = ParseState::ReadLFGRelayHeader;   break;
                    case MSG_CLUSTER_LFG_RELAY_RESP:_parseState = ParseState::ReadLFGRelayRespHeader;break;
                    case MSG_REROUTE_TO_MAP:        _parseState = ParseState::ReadRerouteToMap;     break;
                    case MSG_CLUSTER_UNIT_UPDATE:   _parseState = ParseState::ReadUnitUpdateLen;     break;
                    case MSG_CLUSTER_CHAT:          _parseState = ParseState::ReadChatRelayLen;      break;
                    case MSG_CLUSTER_NOTIFY_MAIL:   _parseState = ParseState::ReadNotifyMail;        break;
                    case MSG_PONG:                  _parseState = ParseState::ReadPongTimestamp;     break;
                    default:
                        LOG_WARN("proxy.control", "ControlSocket: Unknown message type 0x{:02X} — closing", msgType);
                        CloseSocket();
                        return;
                }
                break;
            }

            // ── MSG_REGISTER (base: type + port + map_count) ─────────────────
            case ParseState::ReadRegister:
            {
                if (_accumBuffer.GetActiveSize() < REGISTER_PAYLOAD_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                _serverType = p[0];
                _gamePort   = static_cast<uint16>(p[1]) | (static_cast<uint16>(p[2]) << 8);
                _registerMapCount = static_cast<uint16>(p[3]) | (static_cast<uint16>(p[4]) << 8);
                _accumBuffer.ReadCompleted(REGISTER_PAYLOAD_SIZE);

                // Sanity-bound the map count: a worldserver cannot own more than
                // MAX_REGISTER_MAP_COUNT maps. Reject connections that claim otherwise
                // to prevent memory exhaustion (65535 × 4 = 262 KB stall per attempt).
                static constexpr uint16 MAX_REGISTER_MAP_COUNT = 128;
                if (_registerMapCount > MAX_REGISTER_MAP_COUNT)
                {
                    LOG_WARN("proxy.control",
                             "ControlSocket: MSG_REGISTER map count {} exceeds limit {}; closing",
                             _registerMapCount, MAX_REGISTER_MAP_COUNT);
                    CloseSocket();
                    return;
                }

                if (_registerMapCount > 0)
                    _parseState = ParseState::ReadRegisterMaps;
                else
                {
                    HandleRegister(_serverType, _gamePort, {});
                    _parseState = ParseState::WaitType;
                }
                break;
            }

            // ── MSG_REGISTER (map list) ───────────────────────────────────────
            case ParseState::ReadRegisterMaps:
            {
                std::size_t need = static_cast<std::size_t>(_registerMapCount) * 4;
                if (_accumBuffer.GetActiveSize() < need)
                    return;

                std::vector<uint32> maps;
                maps.reserve(_registerMapCount);
                uint8* p = _accumBuffer.GetReadPointer();
                for (uint16 i = 0; i < _registerMapCount; ++i)
                {
                    uint32 mapId = static_cast<uint32>(p[0])
                                 | (static_cast<uint32>(p[1]) << 8)
                                 | (static_cast<uint32>(p[2]) << 16)
                                 | (static_cast<uint32>(p[3]) << 24);
                    maps.push_back(mapId);
                    p += 4;
                }
                _accumBuffer.ReadCompleted(need);

                HandleRegister(_serverType, _gamePort, std::move(maps));
                _registerMapCount = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_REROUTE_PLAYER (part 1) ───────────────────────────────────
            case ParseState::ReadRerouteP1:
            {
                if (_accumBuffer.GetActiveSize() < REROUTE_PART1_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_rerouteGuid, p, 8);
                _rerouteAddrLen = p[8];
                _accumBuffer.ReadCompleted(REROUTE_PART1_SIZE);

                _parseState = ParseState::ReadRerouteP2;
                break;
            }

            // ── MSG_REROUTE_PLAYER (part 2) ───────────────────────────────────
            case ParseState::ReadRerouteP2:
            {
                std::size_t need = static_cast<std::size_t>(_rerouteAddrLen) + 2;
                if (_accumBuffer.GetActiveSize() < need)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::string address(reinterpret_cast<char*>(p), _rerouteAddrLen);
                uint16 port = static_cast<uint16>(p[_rerouteAddrLen])
                            | (static_cast<uint16>(p[_rerouteAddrLen + 1]) << 8);
                _accumBuffer.ReadCompleted(need);

                HandleReroute(_rerouteGuid, address, port);

                _rerouteGuid    = 0;
                _rerouteAddrLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_PLAYER_ONLINE (meta: guid + name_len) ─────────────
            case ParseState::ReadPlayerOnlineMeta:
            {
                if (_accumBuffer.GetActiveSize() < PLAYER_ONLINE_META_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_playerOnlineGuid, p, 8);
                _playerOnlineNameLen = p[8];
                _accumBuffer.ReadCompleted(PLAYER_ONLINE_META_SIZE);

                _parseState = ParseState::ReadPlayerOnlineBody;
                break;
            }

            // ── MSG_CLUSTER_PLAYER_ONLINE (body: name + zone/level/class/race/team) ──
            case ParseState::ReadPlayerOnlineBody:
            {
                std::size_t need = static_cast<std::size_t>(_playerOnlineNameLen) + PLAYER_ONLINE_TAIL_SIZE;
                if (_accumBuffer.GetActiveSize() < need)
                    return;

                HandlePlayerOnline();   // reads directly from _accumBuffer
                _accumBuffer.ReadCompleted(need);

                _playerOnlineGuid    = 0;
                _playerOnlineNameLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_PLAYER_OFFLINE ────────────────────────────────────
            case ParseState::ReadPlayerOffline:
            {
                if (_accumBuffer.GetActiveSize() < PLAYER_OFFLINE_SIZE)
                    return;

                uint64 guid;
                std::memcpy(&guid, _accumBuffer.GetReadPointer(), 8);
                _accumBuffer.ReadCompleted(PLAYER_OFFLINE_SIZE);

                HandlePlayerOffline(guid);
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_DELIVER_PACKET (part 1: guid + len) ──────────────
            case ParseState::ReadDeliverP1:
            {
                if (_accumBuffer.GetActiveSize() < DELIVER_P1_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_deliverGuid, p, 8);
                _deliverPacketLen = static_cast<uint16>(p[8]) | (static_cast<uint16>(p[9]) << 8);
                _accumBuffer.ReadCompleted(DELIVER_P1_SIZE);

                _parseState = ParseState::ReadDeliverBody;
                break;
            }

            // ── MSG_CLUSTER_DELIVER_PACKET (body: packet bytes) ───────────────
            case ParseState::ReadDeliverBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_deliverPacketLen))
                    return;

                std::vector<uint8> packetData(_deliverPacketLen);
                std::memcpy(packetData.data(), _accumBuffer.GetReadPointer(), _deliverPacketLen);
                _accumBuffer.ReadCompleted(_deliverPacketLen);

                HandleDeliverPacket(_deliverGuid, std::move(packetData));

                _deliverGuid      = 0;
                _deliverPacketLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_RELAY_TO_NODE (header: target_node + inner_type + len) ─
            case ParseState::ReadRelayHeader:
            {
                if (_accumBuffer.GetActiveSize() < RELAY_HEADER_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                _relayTargetNode  = p[0];
                _relayInnerType   = p[1];
                _relayPayloadLen  = static_cast<uint16>(p[2]) | (static_cast<uint16>(p[3]) << 8);
                _accumBuffer.ReadCompleted(RELAY_HEADER_SIZE);

                _parseState = ParseState::ReadRelayBody;
                break;
            }

            case ParseState::ReadRelayBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_relayPayloadLen))
                    return;

                std::vector<uint8> payload(_relayPayloadLen);
                std::memcpy(payload.data(), _accumBuffer.GetReadPointer(), _relayPayloadLen);
                _accumBuffer.ReadCompleted(_relayPayloadLen);

                HandleRelayToNode(_relayTargetNode, _relayInnerType, std::move(payload));

                _relayTargetNode = 0;
                _relayInnerType  = 0;
                _relayPayloadLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_GROUP_UPDATE (meta: group_guid + member_count) ────
            case ParseState::ReadGroupUpdateMeta:
            {
                if (_accumBuffer.GetActiveSize() < GROUP_UPDATE_META_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_groupUpdateGuid, p, 8);
                _groupUpdateMemberCount = p[8];
                _accumBuffer.ReadCompleted(GROUP_UPDATE_META_SIZE);

                _parseState = ParseState::ReadGroupUpdateBody;
                break;
            }

            case ParseState::ReadGroupUpdateBody:
            {
                std::size_t need = static_cast<std::size_t>(_groupUpdateMemberCount) * GROUP_MEMBER_SIZE;
                if (_accumBuffer.GetActiveSize() < need)
                    return;

                std::vector<uint8> memberData(need);
                std::memcpy(memberData.data(), _accumBuffer.GetReadPointer(), need);
                _accumBuffer.ReadCompleted(need);

                HandleGroupUpdate(_groupUpdateGuid, _groupUpdateMemberCount, std::move(memberData));

                _groupUpdateGuid        = 0;
                _groupUpdateMemberCount = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_GROUP_DISBAND ─────────────────────────────────────
            case ParseState::ReadGroupDisband:
            {
                if (_accumBuffer.GetActiveSize() < GROUP_DISBAND_SIZE)
                    return;

                uint64 groupGuid;
                std::memcpy(&groupGuid, _accumBuffer.GetReadPointer(), 8);
                _accumBuffer.ReadCompleted(GROUP_DISBAND_SIZE);

                HandleGroupDisband(groupGuid);
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_LFG_RELAY (header: uint16 payload_len) ────────────
            case ParseState::ReadLFGRelayHeader:
            {
                if (_accumBuffer.GetActiveSize() < LFG_RELAY_HEADER_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                _lfgRelayPayloadLen = static_cast<uint16>(p[0]) | (static_cast<uint16>(p[1]) << 8);
                _accumBuffer.ReadCompleted(LFG_RELAY_HEADER_SIZE);
                _parseState = ParseState::ReadLFGRelayBody;
                break;
            }

            case ParseState::ReadLFGRelayBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_lfgRelayPayloadLen))
                    return;

                std::vector<uint8> payload(_lfgRelayPayloadLen);
                std::memcpy(payload.data(), _accumBuffer.GetReadPointer(), _lfgRelayPayloadLen);
                _accumBuffer.ReadCompleted(_lfgRelayPayloadLen);

                HandleLFGRelay(std::move(payload));
                _lfgRelayPayloadLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_LFG_RELAY_RESP (header: uint8 target + uint16 len) ─
            case ParseState::ReadLFGRelayRespHeader:
            {
                if (_accumBuffer.GetActiveSize() < LFG_RELAY_RESP_HDR_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                _lfgRelayRespTargetNode = p[0];
                _lfgRelayRespPayloadLen = static_cast<uint16>(p[1]) | (static_cast<uint16>(p[2]) << 8);
                _accumBuffer.ReadCompleted(LFG_RELAY_RESP_HDR_SIZE);
                _parseState = ParseState::ReadLFGRelayRespBody;
                break;
            }

            case ParseState::ReadLFGRelayRespBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_lfgRelayRespPayloadLen))
                    return;

                std::vector<uint8> payload(_lfgRelayRespPayloadLen);
                std::memcpy(payload.data(), _accumBuffer.GetReadPointer(), _lfgRelayRespPayloadLen);
                _accumBuffer.ReadCompleted(_lfgRelayRespPayloadLen);

                HandleLFGRelayResponse(_lfgRelayRespTargetNode, std::move(payload));
                _lfgRelayRespTargetNode = 0;
                _lfgRelayRespPayloadLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_REROUTE_TO_MAP ────────────────────────────────────────────
            case ParseState::ReadRerouteToMap:
            {
                if (_accumBuffer.GetActiveSize() < REROUTE_TO_MAP_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                uint64 guid  = 0;
                uint32 mapId = 0;
                std::memcpy(&guid,  p,     8);
                std::memcpy(&mapId, p + 8, 4);
                _accumBuffer.ReadCompleted(REROUTE_TO_MAP_SIZE);

                HandleRerouteToMap(guid, mapId);
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_UNIT_UPDATE ───────────────────────────────────────
            case ParseState::ReadUnitUpdateLen:
            {
                if (_accumBuffer.GetActiveSize() < UNIT_UPDATE_LEN_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_unitUpdatePayloadLen, p, 2);
                _accumBuffer.ReadCompleted(UNIT_UPDATE_LEN_SIZE);
                _parseState = ParseState::ReadUnitUpdateBody;
                break;
            }

            case ParseState::ReadUnitUpdateBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_unitUpdatePayloadLen))
                    return;

                std::vector<uint8> payload(_unitUpdatePayloadLen);
                std::memcpy(payload.data(), _accumBuffer.GetReadPointer(), _unitUpdatePayloadLen);
                _accumBuffer.ReadCompleted(_unitUpdatePayloadLen);

                HandleUnitUpdate(std::move(payload));
                _unitUpdatePayloadLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_CHAT ──────────────────────────────────────────────
            case ParseState::ReadChatRelayLen:
            {
                if (_accumBuffer.GetActiveSize() < CHAT_RELAY_LEN_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_chatRelayPayloadLen, p, 2);
                _accumBuffer.ReadCompleted(CHAT_RELAY_LEN_SIZE);
                _parseState = ParseState::ReadChatRelayBody;
                break;
            }

            case ParseState::ReadChatRelayBody:
            {
                if (_accumBuffer.GetActiveSize() < static_cast<std::size_t>(_chatRelayPayloadLen))
                    return;

                std::vector<uint8> payload(_chatRelayPayloadLen);
                std::memcpy(payload.data(), _accumBuffer.GetReadPointer(), _chatRelayPayloadLen);
                _accumBuffer.ReadCompleted(_chatRelayPayloadLen);

                HandleChatRelay(std::move(payload));
                _chatRelayPayloadLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_NOTIFY_MAIL ───────────────────────────────────────
            case ParseState::ReadNotifyMail:
            {
                if (_accumBuffer.GetActiveSize() < NOTIFY_MAIL_SIZE)
                    return;

                uint64 recipientGuid = 0;
                std::memcpy(&recipientGuid, _accumBuffer.GetReadPointer(), 8);
                _accumBuffer.ReadCompleted(NOTIFY_MAIL_SIZE);

                HandleNotifyMail(recipientGuid);
                _parseState = ParseState::WaitType;
                break;
            }

            // ── MSG_PONG: worldserver echoed our timestamp — compute RTT ──────
            case ParseState::ReadPongTimestamp:
            {
                if (_accumBuffer.GetActiveSize() < 8)
                    return;
                // Read the 8 echoed bytes (we don't actually use the value —
                // wall-clock elapsed since SendPing() is the RTT).
                _accumBuffer.ReadCompleted(8);
                _parseState = ParseState::WaitType;

                if (_pingSentAt != std::chrono::steady_clock::time_point{})
                {
                    auto elapsed = std::chrono::steady_clock::now() - _pingSentAt;
                    uint32 latMs = static_cast<uint32>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
                    sProxyMgr.OnNodePong(_nodeId, latMs);
                    _pingSentAt = {};
                }
                break;
            }
        }
    }

    _accumBuffer.Normalize();
}

// ── Message handlers ──────────────────────────────────────────────────────────

void ControlSocket::SendPing()
{
    // Send MSG_PING with 8 zero bytes (timestamp placeholder — we measure wall-clock RTT).
    std::vector<uint8> ping(9, 0);
    ping[0] = MSG_PING;
    _pingSentAt = std::chrono::steady_clock::now();
    SendRaw(ping);
}

void ControlSocket::HandleRegister(uint8 serverType, uint16 gamePort, std::vector<uint32> maps)
{
    _serverType = serverType;
    _gamePort   = gamePort;

    char const* typeStr = (serverType == 0) ? "worldserver" : "instance server";

    // Ask ProxyMgr to assign a node ID and track this socket.
    _nodeId = sProxyMgr.RegisterNode(shared_from_this(), serverType, gamePort,
                                     GetRemoteIpAddress().to_string());

    // Register map routing dynamically (replaces static proxyserver.conf entries for these maps).
    if (!maps.empty())
        sProxyMgr.RegisterNodeMaps(_nodeId, maps);

    LOG_INFO("proxy.control", "ControlSocket: Backend registered as {} on port {} (node_id={})",
             typeStr, gamePort, _nodeId);

    // Send the ack with the assigned node ID.
    std::vector<uint8> ack = { MSG_REGISTER_ACK, _nodeId };
    SendRaw(ack);
}

void ControlSocket::HandleReroute(uint64 guid, std::string const& address, uint16 port)
{
    LOG_DEBUG("proxy.control", "ControlSocket: Reroute request — GUID {:016X} → {}:{}",
              guid, address, port);

    sProxyMgr.ReroutePlayer(guid, address, port);
}

void ControlSocket::HandlePlayerOnline()
{
    // Body layout (accumulated buffer already positioned at start of body):
    //   char   name[name_len]     (lowercase)
    //   uint32 zone_id
    //   uint8  level
    //   uint8  class_id
    //   uint8  race_id
    //   uint8  team_id
    uint8* p = _accumBuffer.GetReadPointer();

    std::string name(reinterpret_cast<char*>(p), _playerOnlineNameLen);
    p += _playerOnlineNameLen;

    uint32 zoneId;
    std::memcpy(&zoneId, p, 4);
    p += 4;

    uint8 level   = p[0];
    uint8 classId = p[1];
    uint8 raceId  = p[2];
    uint8 teamId  = p[3];
    // Use the sender's registered node ID rather than the wire-provided value.
    // Trusting a node-supplied nodeId would allow a compromised or malicious
    // backend to falsely report players as residing on arbitrary nodes.
    uint8 nodeId  = _nodeId;

    LOG_INFO("proxy.control", "ControlSocket: Player ONLINE  GUID {:016X} '{}' zone={} level={} from node={}",
             _playerOnlineGuid, name, zoneId, level, nodeId);

    sProxyMgr.OnPlayerOnline(_playerOnlineGuid, nodeId, std::move(name),
                              zoneId, level, classId, raceId, teamId);
}

void ControlSocket::HandlePlayerOffline(uint64 guid)
{
    LOG_INFO("proxy.control", "ControlSocket: Player OFFLINE GUID {:016X} (node {})", guid, _nodeId);
    sProxyMgr.OnPlayerOffline(guid, _nodeId);
}

void ControlSocket::HandleDeliverPacket(uint64 targetGuid, std::vector<uint8> packetData)
{
    LOG_INFO("proxy.control", "ControlSocket: DeliverPacket to GUID {:016X} ({} bytes)",
             targetGuid, packetData.size());
    sProxyMgr.DeliverPacketToPlayer(targetGuid, std::move(packetData));
}

void ControlSocket::HandleRelayToNode(uint8 targetNodeId, uint8 innerType, std::vector<uint8> payload)
{
    LOG_DEBUG("proxy.control", "ControlSocket: Relay inner=0x{:02X} → node {} ({} bytes)",
              innerType, targetNodeId, payload.size());

    // Re-wrap in MSG_CLUSTER_RELAY_TO_NODE frame and forward to the target node.
    std::vector<uint8> msg;
    msg.reserve(1 + 4 + payload.size());
    msg.push_back(MSG_CLUSTER_RELAY_TO_NODE);
    msg.push_back(targetNodeId);
    msg.push_back(innerType);
    msg.push_back(static_cast<uint8>(payload.size() & 0xFF));
    msg.push_back(static_cast<uint8>(payload.size() >> 8));
    msg.insert(msg.end(), payload.begin(), payload.end());

    sProxyMgr.RelayToNode(targetNodeId, msg);
}

void ControlSocket::HandleGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> memberData)
{
    LOG_DEBUG("proxy.control", "ControlSocket: GroupUpdate group {:016X} {} members",
              groupGuid, memberCount);

    sProxyMgr.OnGroupUpdate(groupGuid, _nodeId, memberCount, std::move(memberData));
}

void ControlSocket::HandleGroupDisband(uint64 groupGuid)
{
    LOG_DEBUG("proxy.control", "ControlSocket: GroupDisband group {:016X}", groupGuid);
    sProxyMgr.OnGroupDisband(groupGuid, _nodeId);
}

void ControlSocket::HandleLFGRelay(std::vector<uint8> payload)
{
    LOG_DEBUG("proxy.control", "ControlSocket: LFGRelay from node {} ({} bytes)", _nodeId, payload.size());
    sProxyMgr.RelayToLFGMaster(_nodeId, std::move(payload));
}

void ControlSocket::HandleLFGRelayResponse(uint8 targetNodeId, std::vector<uint8> payload)
{
    LOG_DEBUG("proxy.control", "ControlSocket: LFGRelayResponse → node {} ({} bytes)", targetNodeId, payload.size());

    // Re-wrap and forward to the target node.
    uint16 payloadLen = static_cast<uint16>(payload.size());
    std::vector<uint8> msg;
    msg.reserve(1 + 3 + payload.size());
    msg.push_back(MSG_CLUSTER_LFG_RELAY_RESP);
    msg.push_back(targetNodeId);
    msg.push_back(static_cast<uint8>(payloadLen & 0xFF));
    msg.push_back(static_cast<uint8>(payloadLen >> 8));
    msg.insert(msg.end(), payload.begin(), payload.end());

    sProxyMgr.RelayToNode(targetNodeId, msg);
}

void ControlSocket::HandleRerouteToMap(uint64 guid, uint32 mapId)
{
    LOG_INFO("proxy.control", "ControlSocket: RerouteToMap — GUID {:016X} → map {}", guid, mapId);
    sProxyMgr.RerouteToMap(guid, mapId);
}

void ControlSocket::HandleUnitUpdate(std::vector<uint8> payload)
{
    if (_nodeId == 0)
        return;
    uint16 len = static_cast<uint16>(payload.size());
    LOG_DEBUG("proxy.control", "ControlSocket: UnitUpdate from node {} payload={} bytes", _nodeId, len);
    sProxyMgr.BroadcastUnitUpdate(_nodeId, len, payload);
}

void ControlSocket::HandleNotifyMail(uint64 recipientGuid)
{
    LOG_DEBUG("proxy.control", "ControlSocket: NotifyMail — recipient GUID {:016X} from node {}", recipientGuid, _nodeId);
    sProxyMgr.RouteMailNotification(recipientGuid);
}

void ControlSocket::HandleChatRelay(std::vector<uint8> payload)
{
    if (_nodeId == 0)
        return;
    uint16 len = static_cast<uint16>(payload.size());
    LOG_DEBUG("proxy.control", "ControlSocket: ChatRelay from node {} payload={} bytes", _nodeId, len);
    sProxyMgr.BroadcastChatRelay(_nodeId, len, payload);
}
