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

#ifndef ProxyClient_h__
#define ProxyClient_h__

#include "Define.h"
#include <string>
#include <vector>

// Forward-declare nats.c opaque types so consumers of ProxyClient.h don't need
// to include <nats.h> directly (the implementation is in ProxyClient.cpp).
struct __natsConnection;
struct __natsSubscription;
struct __natsMsg;
typedef struct __natsConnection   natsConnection;
typedef struct __natsSubscription natsSubscription;
typedef struct __natsMsg          natsMsg;

class Player;
class WorldPacket;

/**
 * @brief Worldserver-side NATS client for the cluster control channel.
 *
 * On startup (when ProxyServer.Enable = 1), the worldserver connects to NATS,
 * sends a registration request on "cluster.register", and receives back its
 * assigned node ID.  After that, all control messages flow via NATS subjects:
 *
 *   cluster.proxy        — worldserver → proxy  (all outgoing control messages)
 *   cluster.node.{N}     — proxy → this worldserver (targeted delivery)
 *   cluster.broadcast    — proxy → ALL worldservers (fanout)
 *
 * The public SendXxx() / HandleXxx() API is identical to the old TCP version —
 * only the transport internals have changed.
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
    /// @param natsUrl      NATS server URL (e.g. "nats://192.0.2.70:4222")
    /// @param serverType   0 = worldserver, 1 = instance server
    /// @param gamePort     This node's WoW game port (typically 8085)
    /// @param gameAddress  This node's LAN IP (from ClusterServer.GameAddress config) —
    ///                     sent in the registration payload so the proxy can match it
    ///                     against its configured node address table.
    void Initialize(std::string const& natsUrl, uint8 serverType,
                    uint16 gamePort, std::string const& gameAddress);

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

    /// Relay a SAY/YELL/EMOTE chat message to all other cluster nodes.
    /// @param chatMsgType  CHAT_MSG_SAY / CHAT_MSG_YELL / CHAT_MSG_EMOTE
    /// @param zoneId       Sender's current zone (used for delivery filtering).
    /// @param pkt          The fully-built SMSG_MESSAGECHAT WorldPacket.
    void SendChatRelay(uint8 chatMsgType, uint32 zoneId, WorldPacket const& pkt);

    /// Notify the proxy that new mail arrived for recipientGuid so the proxy can
    /// route SMSG_RECEIVED_MAIL to whichever node hosts the recipient's session.
    void SendMailNotify(uint64 recipientGuid);

    // ── BG queue relay (non-instance nodes ↔ proxy) ───────────────────────────

    /// Notify the proxy that a player joined a BG queue on this node.
    /// @param guid           Raw player GUID (ObjectGuid::GetRawValue()).
    /// @param bgTypeId       BattlegroundTypeId value.
    /// @param bracketId      PvPDifficulty bracket (BattlegroundBracketId).
    /// @param teamId         0=Alliance, 1=Horde.
    /// @param minPerTeam     Minimum players per team required by this BG/bracket.
    void SendBgQueueJoin(uint64 guid, uint32 bgTypeId, uint8 bracketId, uint8 teamId, uint8 minPerTeam);

    /// Notify the proxy that a player left a BG queue on this node.
    void SendBgQueueLeave(uint64 guid, uint32 bgTypeId);

    /// Tell the proxy that a BG instance was created on this (instance) node.
    /// Called after CreateNewBattleground() + AddBattleground() succeed.
    void SendBgInstCreated(uint32 matchId, uint32 instanceId, uint32 mapId, uint32 clientInstanceId);

    /// Broadcast updated arena team stats to all other nodes after a rated match.
    /// @param teamId       ArenaTeam ID.
    /// @param rating       New team rating.
    /// @param weekGames    Games played this week.
    /// @param weekWins     Wins this week.
    /// @param seasonGames  Games played this season.
    /// @param seasonWins   Wins this season.
    /// @param rank         New rank.
    void SendArenaResult(uint32 teamId, uint16 rating, uint16 weekGames, uint16 weekWins,
                         uint16 seasonGames, uint16 seasonWins, uint32 rank);

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

    // ── NATS publish helper ───────────────────────────────────────────────────
    /// Prepend [nodeId][msgType] header and publish to "cluster.proxy".
    void PublishToProxy(uint8 msgType, uint8 const* payload, int payloadLen);

    // ── Incoming message handlers (called from NATS dispatch thread via QueueCallback) ──
    void HandleRemotePlayerOnline(std::vector<uint8> const& payload);
    void HandleRemotePlayerOffline(uint64 guid);
    void HandleIncomingRelay(uint8 innerType, std::vector<uint8> const& payload);
    void HandleGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> const& memberData);
    void HandleGroupDisband(uint64 groupGuid);
    void HandleLFGRelay(uint8 sourceNodeId, std::vector<uint8> const& payload);
    void HandleLFGRelayResponse(uint8 innerType, std::vector<uint8> const& payload);
    void HandleUnitUpdate(std::vector<uint8> const& payload);
    void HandleIncomingChat(std::vector<uint8> const& payload);
    void HandleIncomingMailNotify(uint64 recipientGuid);
    void HandleIncomingArenaResult(std::vector<uint8> const& payload);
    void HandleBgCreateInst(std::vector<uint8> const& payload);
    void HandleBgReady(std::vector<uint8> const& payload);

    /// NATS callback — fires on NATS dispatch thread for cluster.node.{N} and cluster.broadcast.
    static void OnNatsMsg(natsConnection* nc, natsSubscription* sub,
                          natsMsg* msg, void* closure);

    /// Dispatch a fully-parsed incoming message on the world thread (via QueueCallback).
    void Dispatch(uint8 msgType, std::vector<uint8> payload);

    // ── Control protocol message type constants ───────────────────────────────
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
    static constexpr uint8 MSG_PING                    = 0x0D;
    static constexpr uint8 MSG_PONG                    = 0x0E;
    static constexpr uint8 MSG_REGISTER_ACK            = 0x10;
    static constexpr uint8 MSG_CLUSTER_CHAT            = 0x11;
    static constexpr uint8 MSG_CLUSTER_NOTIFY_MAIL     = 0x12;
    static constexpr uint8 MSG_CLUSTER_BG_QUEUE_JOIN   = 0x13;
    static constexpr uint8 MSG_CLUSTER_BG_QUEUE_LEAVE  = 0x14;
    static constexpr uint8 MSG_CLUSTER_BG_CREATE_INST  = 0x15;
    static constexpr uint8 MSG_CLUSTER_ARENA_RESULT    = 0x16;
    static constexpr uint8 MSG_CLUSTER_BG_INST_CREATED = 0x17;
    static constexpr uint8 MSG_CLUSTER_BG_READY        = 0x18;

    // ── NATS handles ──────────────────────────────────────────────────────────
    natsConnection*   _nc{nullptr};
    natsSubscription* _subNode{nullptr};       ///< cluster.node.{_nodeId}
    natsSubscription* _subBroadcast{nullptr};  ///< cluster.broadcast

    // ── Node identity ─────────────────────────────────────────────────────────
    uint8       _nodeId{ 0 };    ///< Assigned by proxy after NATS registration request-reply.
    uint8       _serverType{ 0 };
    uint16      _gamePort{ 0 };
    std::string _gameAddress;    ///< Own LAN IP (sent in registration so proxy can match it)
    std::string _natsUrl;

    bool _connected{ false };
};

#define sProxyClient ProxyClient::Instance()

#endif // ProxyClient_h__
