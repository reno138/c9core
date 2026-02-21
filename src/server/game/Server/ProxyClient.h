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

#ifndef ProxyClient_h__
#define ProxyClient_h__

#include "Define.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace Acore::Asio { class IoContext; }
class Player;
class WorldPacket;

/**
 * @brief Worldserver-side client for the proxy control channel (bidirectional).
 *
 * On startup (when ProxyServer.Enable = 1), the worldserver connects to the proxy's
 * control port, sends MSG_REGISTER, and waits for MSG_REGISTER_ACK with the assigned
 * node ID. After that, cluster messages flow in both directions.
 *
 * Outgoing (worldserver → proxy):
 *   MSG_REGISTER (0x01), MSG_REROUTE_PLAYER (0x02),
 *   MSG_CLUSTER_PLAYER_ONLINE (0x03), MSG_CLUSTER_PLAYER_OFFLINE (0x04),
 *   MSG_CLUSTER_DELIVER_PACKET (0x05)
 *
 * Incoming (proxy → worldserver):
 *   MSG_REGISTER_ACK (0x10)
 *   MSG_CLUSTER_PLAYER_ONLINE (0x03)   — another node's player came online
 *   MSG_CLUSTER_PLAYER_OFFLINE (0x04)  — another node's player went offline
 */
class ProxyClient
{
public:
    static ProxyClient& Instance()
    {
        static ProxyClient instance;
        return instance;
    }

    /// Call once from worldserver Main, before the world loop starts.
    void Initialize(Acore::Asio::IoContext& ioContext, std::string const& address,
                    uint16 controlPort, uint8 serverType, uint16 gamePort);

    bool IsConnected() const { return _connected; }

    /// Assigned node ID (0 = not yet assigned by proxy).
    uint8 GetNodeId() const { return _nodeId; }

    // ── Outgoing messages ─────────────────────────────────────────────────────

    /// Ask the proxy to reroute a player to the given backend.
    void SendReroute(uint64 playerGuid, std::string const& address, uint16 port);

    /// Announce that a player just logged in (broadcasts to all other nodes).
    void AnnounceOnline(Player const* player);

    /// Announce that a player just logged out (broadcasts to all other nodes).
    void AnnounceOffline(uint64 playerGuid);

    /// Ask the proxy to deliver a WoW game packet to a player on another node.
    void DeliverPacketToPlayer(uint64 targetGuid, WorldPacket const& packet);

    /// Relay an inner-typed message to a specific node (for group invites etc.).
    void RelayToNode(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload);

    /// Broadcast group membership state to all member nodes via the proxy.
    void SendGroupUpdate(uint64 groupGuid, std::vector<uint8> const& memberData);

    /// Notify all group member nodes that the group disbanded.
    void SendGroupDisband(uint64 groupGuid);

    // ── LFG relay (non-master nodes → proxy → master) ─────────────────────────

    /// Inner type for group-wide cross-node map reroute (carried in MSG_CLUSTER_RELAY_TO_NODE).
    static constexpr uint8 GROUP_INNER_REROUTE_TO_MAP = 0x03;

    /// Ask the proxy to reroute this player to the node handling mapId.
    void SendRerouteToMap(uint64 playerGuid, uint32 mapId);

    /// Broadcast this player's unit state (HP/power/auras) to all other cluster nodes.
    /// Safe to call from the game update thread.
    void SendClusterUnitUpdate(Player* player);

    /// LFG sub-message types carried inside LFG_RELAY payload.
    static constexpr uint8 LFG_INNER_JOIN             = 0x01; ///< guid+roles+dungeons
    static constexpr uint8 LFG_INNER_LEAVE            = 0x02; ///< guid
    static constexpr uint8 LFG_INNER_PROPOSAL_RESULT  = 0x03; ///< proposal_id+guid+accept
    static constexpr uint8 LFG_INNER_PROPOSAL_NOTIFY  = 0x10; ///< proxy→node: proposal data
    static constexpr uint8 LFG_INNER_MATCH_NOTIFY     = 0x11; ///< proxy→node: match + instance addr

    /// Relay CMSG_LFG_JOIN to the master node (call from non-master nodes).
    void SendLFGJoinRelay(uint64 playerGuid, uint8 roles, std::vector<uint32> const& dungeons);

    /// Relay CMSG_LFG_LEAVE to the master node.
    void SendLFGLeaveRelay(uint64 playerGuid);

    /// Relay CMSG_LFG_PROPOSAL_RESULT to the master node.
    void SendLFGProposalResultRelay(uint32 proposalId, uint64 playerGuid, bool accept);

    /// Send a LFG_RELAY_RESP to a specific node (master → proxy → target node).
    void SendLFGRelayResponse(uint8 targetNodeId, uint8 innerType, std::vector<uint8> const& payload);

private:
    ProxyClient() = default;
    ~ProxyClient() = default;

    // ── Connection management ─────────────────────────────────────────────────
    void Connect();
    void OnConnect(boost::system::error_code const& error);
    void SendRegister();
    void ScheduleReconnect();

    // ── Outgoing write loop ───────────────────────────────────────────────────
    void AsyncWrite();
    void EnqueueRaw(std::vector<uint8> msg);

    // ── Incoming read loop ────────────────────────────────────────────────────
    void AsyncRead();
    void OnRead(boost::system::error_code const& error, std::size_t transferred);
    void ParseIncoming();
    void HandleRegisterAck(uint8 nodeId);
    void HandleRemotePlayerOnline();
    void HandleRemotePlayerOffline(uint64 guid);
    void HandleIncomingRelay(uint8 innerType, std::vector<uint8> const& payload);
    void HandleGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> const& memberData);
    void HandleGroupDisband(uint64 groupGuid);
    void HandleLFGRelay(uint8 sourceNodeId, std::vector<uint8> const& payload);
    void HandleLFGRelayResponse(uint8 innerType, std::vector<uint8> const& payload);
    void HandleUnitUpdate(std::vector<uint8> const& payload);

    // ── Control protocol message types ────────────────────────────────────────
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
    static constexpr uint8 MSG_PING                    = 0x0D; ///< proxy→node: ping timestamp(8)
    static constexpr uint8 MSG_PONG                    = 0x0E; ///< node→proxy: pong echoed timestamp(8)
    static constexpr uint8 MSG_REGISTER_ACK            = 0x10;

    // ── Parse state machine for incoming data ─────────────────────────────────
    enum class InParseState
    {
        WaitType,
        ReadRegisterAck,        ///< 1 byte
        ReadPlayerOnlineMeta,   ///< 9 bytes: guid + name_len
        ReadPlayerOnlineBody,   ///< name_len + 8 bytes
        ReadPlayerOffline,      ///< 8 bytes: guid
        ReadRelayHeader,        ///< 4 bytes: target_node + inner_type + payload_len
        ReadRelayBody,          ///< payload_len bytes
        ReadGroupUpdateMeta,    ///< 9 bytes: group_guid + member_count
        ReadGroupUpdateBody,    ///< member_count * 11 bytes
        ReadGroupDisband,       ///< 8 bytes: group_guid
        ReadLFGRelayHeader,     ///< 2 bytes: uint16 payload_len
        ReadLFGRelayBody,       ///< payload_len bytes
        ReadLFGRelayRespHeader, ///< 3 bytes: uint8 target_node + uint16 payload_len
        ReadLFGRelayRespBody,   ///< payload_len bytes
        ReadUnitUpdateLen,      ///< 2 bytes: uint16 payload_len
        ReadUnitUpdateBody,     ///< payload_len bytes
        ReadPingTimestamp,      ///< 8 bytes: uint64 timestamp_ms
    };
    InParseState _inParseState{ InParseState::WaitType };

    // Fixed sizes matching the ControlSocket constants.
    static constexpr std::size_t PLAYER_ONLINE_META_SIZE = 9;
    static constexpr std::size_t PLAYER_ONLINE_TAIL_SIZE = 9; ///< zone(4)+level+class+race+team+node_id
    static constexpr std::size_t PLAYER_OFFLINE_SIZE     = 8;
    static constexpr std::size_t RELAY_HEADER_SIZE       = 4; ///< node + type + uint16 len
    static constexpr std::size_t GROUP_UPDATE_META_SIZE  = 9; ///< uint64 + uint8 count
    static constexpr std::size_t GROUP_MEMBER_SIZE       = 11;///< uint64 + uint8×3
    static constexpr std::size_t GROUP_DISBAND_SIZE      = 8; ///< uint64
    static constexpr std::size_t LFG_RELAY_HEADER_SIZE   = 2; ///< uint16 payload_len
    static constexpr std::size_t LFG_RELAY_RESP_HDR_SIZE = 3; ///< uint8 target_node + uint16 len
    static constexpr std::size_t UNIT_UPDATE_LEN_SIZE    = 2; ///< uint16 payload_len

    // ── Socket and connection state ───────────────────────────────────────────
    boost::asio::io_context* _ioContext{ nullptr };
    std::unique_ptr<boost::asio::ip::tcp::socket>   _socket;
    std::unique_ptr<boost::asio::ip::tcp::resolver> _resolver;
    std::unique_ptr<boost::asio::steady_timer>      _reconnectTimer;

    std::string _address;
    uint16 _controlPort{ 0 };
    uint8  _serverType{ 0 };
    uint16 _gamePort{ 0 };
    uint8  _nodeId{ 0 };     ///< Assigned by proxy on MSG_REGISTER_ACK.

    bool _connected{ false };
    bool _writing{ false };

    // ── Send queue ────────────────────────────────────────────────────────────
    std::mutex _queueMutex;
    std::queue<std::vector<uint8>> _sendQueue;

    // ── Receive buffer ────────────────────────────────────────────────────────
    static constexpr std::size_t READ_BUFFER_SIZE = 4096;
    std::vector<uint8> _readBuf;
    std::vector<uint8> _accumBuf; ///< bytes accumulated across read calls

    // ── Inter-parse state ─────────────────────────────────────────────────────
    uint64 _remotePlayerGuid{ 0 };
    uint8  _remotePlayerNameLen{ 0 };

    uint8  _relayTargetNode{ 0 };
    uint8  _relayInnerType{ 0 };
    uint16 _relayPayloadLen{ 0 };

    uint64 _groupUpdateGuid{ 0 };
    uint8  _groupUpdateMemberCount{ 0 };

    uint16 _lfgRelayPayloadLen{ 0 };
    uint8  _lfgRelayRespTargetNode{ 0 };
    uint16 _lfgRelayRespPayloadLen{ 0 };

    uint16 _unitUpdatePayloadLen{ 0 };
};

#define sProxyClient ProxyClient::Instance()

#endif // ProxyClient_h__
