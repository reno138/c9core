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
#include "ClusterMgr.h"
#include "SpellAuras.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "Entities/Player/Player.h"
#include "Globals/ObjectAccessor.h"
#include "Groups/Group.h"
#include "IoContext.h"
#include "Log.h"
#include "Opcodes.h"
#include "World.h"
#include "WorldPacket.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>

void ProxyClient::Initialize(Acore::Asio::IoContext& ioContext, std::string const& address,
                              uint16 controlPort, uint8 serverType, uint16 gamePort)
{
    _ioContext   = &static_cast<boost::asio::io_context&>(ioContext);
    _address     = address;
    _controlPort = controlPort;
    _serverType  = serverType;
    _gamePort    = gamePort;

    _socket         = std::make_unique<boost::asio::ip::tcp::socket>(*_ioContext);
    _resolver       = std::make_unique<boost::asio::ip::tcp::resolver>(*_ioContext);
    _reconnectTimer = std::make_unique<boost::asio::steady_timer>(*_ioContext);
    _readBuf.resize(READ_BUFFER_SIZE);

    Connect();
}

// ── Connection management ─────────────────────────────────────────────────────

void ProxyClient::Connect()
{
    if (!_socket || !_resolver)
        return;

    LOG_INFO("server.worldserver", "ProxyClient: Connecting to proxy control channel at {}:{}...",
             _address, _controlPort);

    _resolver->async_resolve(_address, std::to_string(_controlPort),
        [this](boost::system::error_code const& error, boost::asio::ip::tcp::resolver::results_type results)
        {
            if (error)
            {
                LOG_WARN("server.worldserver", "ProxyClient: Resolve failed: {} — retrying in 10s",
                         error.message());
                ScheduleReconnect();
                return;
            }

            boost::asio::async_connect(*_socket, results,
                [this](boost::system::error_code const& connectError, boost::asio::ip::tcp::endpoint const&)
                {
                    OnConnect(connectError);
                });
        });
}

void ProxyClient::OnConnect(boost::system::error_code const& error)
{
    if (error)
    {
        LOG_WARN("server.worldserver", "ProxyClient: Connection failed: {} — retrying in 10s",
                 error.message());
        ScheduleReconnect();
        return;
    }

    _connected = true;
    _nodeId    = 0;
    _accumBuf.clear();
    _inParseState = InParseState::WaitType;

    LOG_INFO("server.worldserver", "ProxyClient: Connected to proxy control channel.");
    SendRegister();
    AsyncRead();
}

void ProxyClient::ScheduleReconnect()
{
    _connected = false;
    _nodeId    = 0;

    boost::system::error_code ec;
    _socket->close(ec);

    _reconnectTimer->expires_after(std::chrono::seconds(10));
    _reconnectTimer->async_wait(
        [this](boost::system::error_code const& error)
        {
            if (!error)
                Connect();
        });
}

// ── Outgoing write loop ───────────────────────────────────────────────────────

void ProxyClient::EnqueueRaw(std::vector<uint8> msg)
{
    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        wasEmpty = _sendQueue.empty();
        _sendQueue.push(std::move(msg));
    }

    if (wasEmpty)
        AsyncWrite();
}

void ProxyClient::AsyncWrite()
{
    std::vector<uint8> front;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_sendQueue.empty() || _writing)
            return;
        front = _sendQueue.front();
        _writing = true;
    }

    boost::asio::async_write(*_socket, boost::asio::buffer(front),
        [this, front](boost::system::error_code const& error, std::size_t /*transferred*/)
        {
            if (error)
            {
                LOG_WARN("server.worldserver", "ProxyClient: Write error: {} — reconnecting",
                         error.message());
                _connected = false;
                _writing   = false;
                ScheduleReconnect();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _sendQueue.pop();
                _writing = false;
            }

            AsyncWrite();
        });
}

void ProxyClient::SendRegister()
{
    // MSG_REGISTER: 0x01 | uint8 server_type | uint16 game_port (LE)
    std::vector<uint8> msg(4);
    msg[0] = MSG_REGISTER;
    msg[1] = _serverType;
    msg[2] = static_cast<uint8>(_gamePort & 0xFF);
    msg[3] = static_cast<uint8>(_gamePort >> 8);

    EnqueueRaw(std::move(msg));
}

// ── Incoming read loop ────────────────────────────────────────────────────────

void ProxyClient::AsyncRead()
{
    if (!_socket || !_socket->is_open())
        return;

    _socket->async_read_some(boost::asio::buffer(_readBuf),
        [this](boost::system::error_code const& error, std::size_t transferred)
        {
            OnRead(error, transferred);
        });
}

void ProxyClient::OnRead(boost::system::error_code const& error, std::size_t transferred)
{
    if (error)
    {
        if (_connected)
        {
            LOG_WARN("server.worldserver", "ProxyClient: Read error: {} — reconnecting",
                     error.message());
            _connected = false;
            ScheduleReconnect();
        }
        return;
    }

    _accumBuf.insert(_accumBuf.end(), _readBuf.begin(), _readBuf.begin() + transferred);
    LOG_INFO("server.worldserver", "ProxyClient: Received {} bytes from proxy (total buffered: {})",
             transferred, _accumBuf.size());
    ParseIncoming();
    AsyncRead();
}

void ProxyClient::ParseIncoming()
{
    while (!_accumBuf.empty())
    {
        switch (_inParseState)
        {
            case InParseState::WaitType:
            {
                uint8 msgType = _accumBuf[0];
                _accumBuf.erase(_accumBuf.begin());

                switch (msgType)
                {
                    case MSG_REGISTER_ACK:           _inParseState = InParseState::ReadRegisterAck;      break;
                    case MSG_CLUSTER_PLAYER_ONLINE:  _inParseState = InParseState::ReadPlayerOnlineMeta; break;
                    case MSG_CLUSTER_PLAYER_OFFLINE: _inParseState = InParseState::ReadPlayerOffline;    break;
                    case MSG_CLUSTER_RELAY_TO_NODE:  _inParseState = InParseState::ReadRelayHeader;      break;
                    case MSG_CLUSTER_GROUP_UPDATE:   _inParseState = InParseState::ReadGroupUpdateMeta;   break;
                    case MSG_CLUSTER_GROUP_DISBAND:  _inParseState = InParseState::ReadGroupDisband;      break;
                    case MSG_CLUSTER_LFG_RELAY:      _inParseState = InParseState::ReadLFGRelayHeader;   break;
                    case MSG_CLUSTER_LFG_RELAY_RESP: _inParseState = InParseState::ReadLFGRelayRespHeader;break;
                    case MSG_CLUSTER_UNIT_UPDATE:    _inParseState = InParseState::ReadUnitUpdateLen;     break;
                    case MSG_PING:                   _inParseState = InParseState::ReadPingTimestamp;    break;
                    case MSG_PONG:                   /* proxy echoes ignored */ break;
                    default:
                        LOG_WARN("server.worldserver", "ProxyClient: Unknown incoming message type 0x{:02X}", msgType);
                        _accumBuf.clear(); // desync — drop buffer, reconnect
                        ScheduleReconnect();
                        return;
                }
                break;
            }

            case InParseState::ReadRegisterAck:
            {
                if (_accumBuf.size() < 1)
                    return;
                HandleRegisterAck(_accumBuf[0]);
                _accumBuf.erase(_accumBuf.begin());
                _inParseState = InParseState::WaitType;
                break;
            }

            case InParseState::ReadPlayerOnlineMeta:
            {
                if (_accumBuf.size() < PLAYER_ONLINE_META_SIZE)
                    return;
                std::memcpy(&_remotePlayerGuid, _accumBuf.data(), 8);
                _remotePlayerNameLen = _accumBuf[8];
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + PLAYER_ONLINE_META_SIZE);
                _inParseState = InParseState::ReadPlayerOnlineBody;
                break;
            }

            case InParseState::ReadPlayerOnlineBody:
            {
                std::size_t need = static_cast<std::size_t>(_remotePlayerNameLen) + PLAYER_ONLINE_TAIL_SIZE;
                if (_accumBuf.size() < need)
                    return;
                HandleRemotePlayerOnline();
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + need);
                _remotePlayerGuid    = 0;
                _remotePlayerNameLen = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            case InParseState::ReadPlayerOffline:
            {
                if (_accumBuf.size() < PLAYER_OFFLINE_SIZE)
                    return;
                uint64 guid;
                std::memcpy(&guid, _accumBuf.data(), 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + PLAYER_OFFLINE_SIZE);
                HandleRemotePlayerOffline(guid);
                _inParseState = InParseState::WaitType;
                break;
            }

            case InParseState::ReadRelayHeader:
            {
                if (_accumBuf.size() < RELAY_HEADER_SIZE)
                    return;
                _relayTargetNode = _accumBuf[0];
                _relayInnerType  = _accumBuf[1];
                _relayPayloadLen = static_cast<uint16>(_accumBuf[2]) | (static_cast<uint16>(_accumBuf[3]) << 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + RELAY_HEADER_SIZE);
                _inParseState = InParseState::ReadRelayBody;
                break;
            }

            case InParseState::ReadRelayBody:
            {
                if (_accumBuf.size() < static_cast<std::size_t>(_relayPayloadLen))
                    return;
                std::vector<uint8> payload(_accumBuf.begin(), _accumBuf.begin() + _relayPayloadLen);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + _relayPayloadLen);
                HandleIncomingRelay(_relayInnerType, payload);
                _relayTargetNode = 0;
                _relayInnerType  = 0;
                _relayPayloadLen = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            case InParseState::ReadGroupUpdateMeta:
            {
                if (_accumBuf.size() < GROUP_UPDATE_META_SIZE)
                    return;
                std::memcpy(&_groupUpdateGuid, _accumBuf.data(), 8);
                _groupUpdateMemberCount = _accumBuf[8];
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + GROUP_UPDATE_META_SIZE);
                _inParseState = InParseState::ReadGroupUpdateBody;
                break;
            }

            case InParseState::ReadGroupUpdateBody:
            {
                std::size_t need = static_cast<std::size_t>(_groupUpdateMemberCount) * GROUP_MEMBER_SIZE;
                if (_accumBuf.size() < need)
                    return;
                std::vector<uint8> memberData(_accumBuf.begin(), _accumBuf.begin() + need);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + need);
                HandleGroupUpdate(_groupUpdateGuid, _groupUpdateMemberCount, memberData);
                _groupUpdateGuid        = 0;
                _groupUpdateMemberCount = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            case InParseState::ReadGroupDisband:
            {
                if (_accumBuf.size() < GROUP_DISBAND_SIZE)
                    return;
                uint64 groupGuid;
                std::memcpy(&groupGuid, _accumBuf.data(), 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + GROUP_DISBAND_SIZE);
                HandleGroupDisband(groupGuid);
                _inParseState = InParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_LFG_RELAY (header: uint16 payload_len) ────────────
            case InParseState::ReadLFGRelayHeader:
            {
                if (_accumBuf.size() < LFG_RELAY_HEADER_SIZE)
                    return;
                _lfgRelayPayloadLen = static_cast<uint16>(_accumBuf[0]) | (static_cast<uint16>(_accumBuf[1]) << 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + LFG_RELAY_HEADER_SIZE);
                _inParseState = InParseState::ReadLFGRelayBody;
                break;
            }

            case InParseState::ReadLFGRelayBody:
            {
                if (_accumBuf.size() < static_cast<std::size_t>(_lfgRelayPayloadLen))
                    return;
                // First byte of body is source_node_id (prepended by proxy).
                uint8 sourceNodeId = _lfgRelayPayloadLen > 0 ? _accumBuf[0] : 0;
                std::vector<uint8> payload(_accumBuf.begin() + 1, _accumBuf.begin() + _lfgRelayPayloadLen);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + _lfgRelayPayloadLen);
                HandleLFGRelay(sourceNodeId, payload);
                _lfgRelayPayloadLen = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_LFG_RELAY_RESP (header: uint8 target + uint16 len) ─
            case InParseState::ReadLFGRelayRespHeader:
            {
                if (_accumBuf.size() < LFG_RELAY_RESP_HDR_SIZE)
                    return;
                _lfgRelayRespTargetNode = _accumBuf[0];
                _lfgRelayRespPayloadLen = static_cast<uint16>(_accumBuf[1]) | (static_cast<uint16>(_accumBuf[2]) << 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + LFG_RELAY_RESP_HDR_SIZE);
                _inParseState = InParseState::ReadLFGRelayRespBody;
                break;
            }

            case InParseState::ReadLFGRelayRespBody:
            {
                if (_accumBuf.size() < static_cast<std::size_t>(_lfgRelayRespPayloadLen))
                    return;
                if (_lfgRelayRespPayloadLen > 0)
                {
                    uint8 innerType = _accumBuf[0];
                    std::vector<uint8> payload(_accumBuf.begin() + 1, _accumBuf.begin() + _lfgRelayRespPayloadLen);
                    HandleLFGRelayResponse(innerType, payload);
                }
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + _lfgRelayRespPayloadLen);
                _lfgRelayRespTargetNode = 0;
                _lfgRelayRespPayloadLen = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            // ── MSG_CLUSTER_UNIT_UPDATE (header: uint16 payload_len) ──────────
            case InParseState::ReadUnitUpdateLen:
            {
                if (_accumBuf.size() < UNIT_UPDATE_LEN_SIZE)
                    return;
                _unitUpdatePayloadLen = static_cast<uint16>(_accumBuf[0]) | (static_cast<uint16>(_accumBuf[1]) << 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + UNIT_UPDATE_LEN_SIZE);
                _inParseState = InParseState::ReadUnitUpdateBody;
                break;
            }

            case InParseState::ReadUnitUpdateBody:
            {
                if (_accumBuf.size() < static_cast<std::size_t>(_unitUpdatePayloadLen))
                    return;
                std::vector<uint8> payload(_accumBuf.begin(), _accumBuf.begin() + _unitUpdatePayloadLen);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + _unitUpdatePayloadLen);
                HandleUnitUpdate(payload);
                _unitUpdatePayloadLen = 0;
                _inParseState = InParseState::WaitType;
                break;
            }

            // ── MSG_PING (proxy → node): echo timestamp back as MSG_PONG ─────
            case InParseState::ReadPingTimestamp:
            {
                if (_accumBuf.size() < 8)
                    return;
                // Echo the 8-byte timestamp unchanged.
                std::vector<uint8> pong;
                pong.reserve(9);
                pong.push_back(MSG_PONG);
                pong.insert(pong.end(), _accumBuf.begin(), _accumBuf.begin() + 8);
                _accumBuf.erase(_accumBuf.begin(), _accumBuf.begin() + 8);
                if (_socket && _socket->is_open())
                    boost::asio::write(*_socket, boost::asio::buffer(pong));
                _inParseState = InParseState::WaitType;
                break;
            }
        }
    }
}

// ── Incoming message handlers ─────────────────────────────────────────────────

void ProxyClient::HandleRegisterAck(uint8 nodeId)
{
    _nodeId = nodeId;
    LOG_INFO("server.worldserver", "ProxyClient: Registered with proxy, node_id={}", nodeId);
}

void ProxyClient::HandleRemotePlayerOnline()
{
    // Copy all values out of _accumBuf before touching ClusterMgr, so there is
    // no raw pointer into the vector alive across any external call.
    std::string name(_accumBuf.begin(), _accumBuf.begin() + _remotePlayerNameLen);
    std::size_t off = _remotePlayerNameLen;

    uint32 zoneId;
    std::memcpy(&zoneId, _accumBuf.data() + off, 4);
    off += 4;

    uint8 level   = _accumBuf[off + 0];
    uint8 classId = _accumBuf[off + 1];
    uint8 raceId  = _accumBuf[off + 2];
    uint8 teamId  = _accumBuf[off + 3];
    uint8 nodeId  = _accumBuf[off + 4];

    LOG_INFO("server.worldserver", "ProxyClient: Remote player ONLINE  GUID {:016X} '{}' node={}",
             _remotePlayerGuid, name, nodeId);

    sClusterMgr.OnRemotePlayerOnline(_remotePlayerGuid, std::move(name),
                                      zoneId, level, classId, raceId, teamId, nodeId);
}

void ProxyClient::HandleRemotePlayerOffline(uint64 guid)
{
    LOG_INFO("server.worldserver", "ProxyClient: Remote player OFFLINE GUID {:016X}", guid);
    sClusterMgr.OnRemotePlayerOffline(guid);
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

    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
}

void ProxyClient::AnnounceOffline(uint64 playerGuid)
{
    if (!_connected)
        return;

    std::vector<uint8> msg(9);
    msg[0] = MSG_CLUSTER_PLAYER_OFFLINE;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF);

    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
}

void ProxyClient::SendGroupDisband(uint64 groupGuid)
{
    if (!_connected)
        return;

    std::vector<uint8> msg(9);
    msg[0] = MSG_CLUSTER_GROUP_DISBAND;
    for (int i = 0; i < 8; ++i)
        msg[1 + i] = static_cast<uint8>((groupGuid >> (i * 8)) & 0xFF);

    EnqueueRaw(std::move(msg));
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
    EnqueueRaw(std::move(msg));
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
    EnqueueRaw(std::move(msg));
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
    EnqueueRaw(std::move(msg));
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
    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
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

    EnqueueRaw(std::move(msg));
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
