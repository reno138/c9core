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

#ifndef ControlSocket_h__
#define ControlSocket_h__

#include "MessageBuffer.h"
#include "Socket.h"
#include <string>
#include <vector>

/**
 * @brief Server-side control channel socket — accepts connections from worldserver/instance servers.
 *
 * Binary protocol (little-endian, all ints):
 *
 *   --- Worldserver → Proxy ---
 *
 *   MSG_REGISTER (0x01):
 *     uint8  server_type   (0 = worldserver, 1 = instance server)
 *     uint16 game_port
 *     → Proxy responds with MSG_REGISTER_ACK immediately.
 *
 *   MSG_REROUTE_PLAYER (0x02):
 *     uint64 player_guid
 *     uint8  address_len
 *     char   address[address_len]   (NOT null-terminated)
 *     uint16 port
 *
 *   MSG_CLUSTER_PLAYER_ONLINE (0x03):
 *     uint64 player_guid
 *     uint8  name_len
 *     char   name[name_len]         (lowercase)
 *     uint32 zone_id
 *     uint8  level
 *     uint8  class_id
 *     uint8  race_id
 *     uint8  team_id                (0=Alliance 1=Horde)
 *
 *   MSG_CLUSTER_PLAYER_OFFLINE (0x04):
 *     uint64 player_guid
 *
 *   MSG_CLUSTER_DELIVER_PACKET (0x05):
 *     uint64 target_guid
 *     uint16 packet_len
 *     uint8  packet[packet_len]     (plaintext WoW game packet: header+payload)
 *
 *   MSG_CLUSTER_RELAY_TO_NODE (0x06):
 *     uint8  target_node_id
 *     uint8  inner_type
 *     uint16 payload_len
 *     uint8  payload[payload_len]
 *
 *   MSG_CLUSTER_GROUP_UPDATE (0x07):
 *     uint64 group_guid
 *     uint8  member_count
 *     [per member: uint64 guid + uint8 subgroup + uint8 role_flags + uint8 node_id]
 *
 *   MSG_CLUSTER_GROUP_DISBAND (0x08):
 *     uint64 group_guid
 *
 *   --- Proxy → Worldserver ---
 *
 *   MSG_REGISTER_ACK (0x10):
 *     uint8  assigned_node_id       (1–5)
 *
 *   MSG_CLUSTER_PLAYER_ONLINE (0x03):   (proxy broadcasts to other nodes)
 *   MSG_CLUSTER_PLAYER_OFFLINE (0x04):  (proxy broadcasts to other nodes)
 *   MSG_CLUSTER_RELAY_TO_NODE (0x06):   (proxy routes to target node)
 *   MSG_CLUSTER_GROUP_UPDATE (0x07):    (proxy broadcasts to all member nodes)
 */
class ControlSocket final : public Socket<ControlSocket>
{
    typedef Socket<ControlSocket> BaseSocket;

public:
    explicit ControlSocket(IoContextTcpSocket&& socket);

    void Start() override;
    void OnClose() override;

    /// Assigned node ID (1–5, set by ProxyMgr on MSG_REGISTER).
    uint8 GetNodeId() const { return _nodeId; }

    /// Send raw bytes to this control socket (proxy → worldserver direction).
    void SendRaw(std::vector<uint8> const& data);
    void SendPing();

protected:
    SocketReadCallbackResult ReadHandler() final;

private:
    void ProcessBuffer();

    // ── Message handlers ──────────────────────────────────────────────────────
    void HandleRegister(uint8 serverType, uint16 gamePort);
    void HandleReroute(uint64 guid, std::string const& address, uint16 port);
    void HandlePlayerOnline();
    void HandlePlayerOffline(uint64 guid);
    void HandleDeliverPacket(uint64 targetGuid, std::vector<uint8> packetData);
    void HandleRelayToNode(uint8 targetNodeId, uint8 innerType, std::vector<uint8> payload);
    void HandleGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> memberData);
    void HandleGroupDisband(uint64 groupGuid);
    void HandleLFGRelay(std::vector<uint8> payload);
    void HandleLFGRelayResponse(uint8 targetNodeId, std::vector<uint8> payload);
    void HandleRerouteToMap(uint64 guid, uint32 mapId);
    void HandleUnitUpdate(std::vector<uint8> payload);

    // ── Message type constants ─────────────────────────────────────────────────
    static constexpr uint8 MSG_REGISTER                = 0x01;
    static constexpr uint8 MSG_REROUTE_PLAYER          = 0x02;
    static constexpr uint8 MSG_CLUSTER_PLAYER_ONLINE   = 0x03;
    static constexpr uint8 MSG_CLUSTER_PLAYER_OFFLINE  = 0x04;
    static constexpr uint8 MSG_CLUSTER_DELIVER_PACKET  = 0x05;
    static constexpr uint8 MSG_CLUSTER_RELAY_TO_NODE   = 0x06;
    static constexpr uint8 MSG_CLUSTER_GROUP_UPDATE    = 0x07;
    static constexpr uint8 MSG_CLUSTER_GROUP_DISBAND   = 0x08;
    static constexpr uint8 MSG_CLUSTER_LFG_RELAY       = 0x09;
    static constexpr uint8 MSG_CLUSTER_LFG_RELAY_RESP  = 0x0A;
    static constexpr uint8 MSG_REROUTE_TO_MAP          = 0x0B;
    static constexpr uint8 MSG_CLUSTER_UNIT_UPDATE     = 0x0C;
    static constexpr uint8 MSG_PING                      = 0x0D;
    static constexpr uint8 MSG_PONG                      = 0x0E;
    static constexpr uint8 MSG_REGISTER_ACK            = 0x10;

    // ── Fixed payload sizes ───────────────────────────────────────────────────
    static constexpr std::size_t REGISTER_PAYLOAD_SIZE    = 3; ///< uint8 + uint16
    static constexpr std::size_t REROUTE_PART1_SIZE       = 9; ///< uint64 + uint8
    static constexpr std::size_t PLAYER_ONLINE_META_SIZE  = 9; ///< uint64 guid + uint8 name_len
    static constexpr std::size_t PLAYER_ONLINE_TAIL_SIZE  = 9; ///< uint32 zone + uint8×4 + uint8 node_id
    static constexpr std::size_t PLAYER_OFFLINE_SIZE      = 8; ///< uint64 guid
    static constexpr std::size_t DELIVER_P1_SIZE         = 10; ///< uint64 guid + uint16 len
    static constexpr std::size_t RELAY_HEADER_SIZE        = 4; ///< uint8 node + uint8 type + uint16 len
    static constexpr std::size_t GROUP_UPDATE_META_SIZE   = 9; ///< uint64 guid + uint8 member_count
    static constexpr std::size_t GROUP_MEMBER_SIZE        = 11;///< uint64 guid + uint8×3
    static constexpr std::size_t GROUP_DISBAND_SIZE       = 8; ///< uint64 group_guid
    static constexpr std::size_t LFG_RELAY_HEADER_SIZE    = 2; ///< uint16 payload_len
    static constexpr std::size_t LFG_RELAY_RESP_HDR_SIZE  = 3; ///< uint8 target_node + uint16 payload_len
    static constexpr std::size_t REROUTE_TO_MAP_SIZE      = 12;///< uint64 guid + uint32 mapId
    static constexpr std::size_t UNIT_UPDATE_LEN_SIZE     = 2; ///< uint16 payload_len

    // ── Parse state machine ───────────────────────────────────────────────────
    enum class ParseState
    {
        WaitType,
        ReadRegister,
        ReadRerouteP1,
        ReadRerouteP2,
        ReadPlayerOnlineMeta,   ///< 9 bytes: guid + name_len
        ReadPlayerOnlineBody,   ///< name_len + 8 trailing bytes
        ReadPlayerOffline,      ///< 8 bytes: guid
        ReadDeliverP1,          ///< 10 bytes: target_guid + packet_len
        ReadDeliverBody,        ///< packet_len bytes
        ReadRelayHeader,        ///< 4 bytes: target_node + inner_type + payload_len
        ReadRelayBody,          ///< payload_len bytes
        ReadGroupUpdateMeta,    ///< 9 bytes: group_guid + member_count
        ReadGroupUpdateBody,    ///< member_count * 11 bytes
        ReadGroupDisband,       ///< 8 bytes: group_guid
        ReadLFGRelayHeader,     ///< 2 bytes: uint16 payload_len
        ReadLFGRelayBody,       ///< payload_len bytes
        ReadLFGRelayRespHeader, ///< 3 bytes: uint8 target_node + uint16 payload_len
        ReadLFGRelayRespBody,   ///< payload_len bytes
        ReadRerouteToMap,       ///< 12 bytes: uint64 guid + uint32 mapId
        ReadUnitUpdateLen,      ///< 2 bytes: uint16 payload_len
        ReadUnitUpdateBody,     ///< payload_len bytes
        ReadPongTimestamp,      ///< 8 bytes: echoed uint64 timestamp_ms
    };
    std::chrono::steady_clock::time_point _pingSentAt{};
    ParseState _parseState{ ParseState::WaitType };

    /// Accumulation buffer — bytes are copied here from GetReadBuffer() across calls.
    MessageBuffer _accumBuffer;

    /// State saved between parser phases.
    uint64 _rerouteGuid{ 0 };
    uint8  _rerouteAddrLen{ 0 };

    uint64 _playerOnlineGuid{ 0 };
    uint8  _playerOnlineNameLen{ 0 };

    uint64 _deliverGuid{ 0 };
    uint16 _deliverPacketLen{ 0 };

    uint8  _relayTargetNode{ 0 };
    uint8  _relayInnerType{ 0 };
    uint16 _relayPayloadLen{ 0 };

    uint64 _groupUpdateGuid{ 0 };
    uint8  _groupUpdateMemberCount{ 0 };

    uint16 _lfgRelayPayloadLen{ 0 };
    uint8  _lfgRelayRespTargetNode{ 0 };
    uint16 _lfgRelayRespPayloadLen{ 0 };
    uint16 _unitUpdatePayloadLen{ 0 };

    /// Server info learned from MSG_REGISTER.
    uint8  _serverType{ 0xFF };
    uint16 _gamePort{ 0 };
    uint8  _nodeId{ 0 };         ///< Assigned by ProxyMgr on MSG_REGISTER; 0 = unregistered.
};

#endif // ControlSocket_h__
