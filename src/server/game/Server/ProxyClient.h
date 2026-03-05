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

#include "AuthDefines.h"
#include "Define.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
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
 * reads its nodeId from ClusterServer.NodeId config, subscribes to its own
 * cluster.node.{N} subject plus cluster.broadcast, and publishes a cluster.announce
 * message so peer nodes can discover it.  All inter-node game messages flow via:
 *
 *   cluster.broadcast    — fanout to ALL worldservers (player online/offline, group, chat…)
 *   cluster.node.{N}     — targeted delivery to specific node (packet deliver, LFG relay…)
 *   cluster.announce     — startup broadcast for peer node discovery + routing table
 *
 * Cross-node player rerouting (cross-map travel) is handled by
 * WorldSession::SendRedirectClient (SMSG_REDIRECT_CLIENT) rather than the proxy.
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

    /// Pre-register a pending client redirect on @p destNodeId before sending
    /// SMSG_REDIRECT_CLIENT.  The destination node stores the session key keyed
    /// by @p clientIp so HandleRedirectAuthProof can authenticate the incoming
    /// CMSG_REDIRECTION_AUTH_PROOF without a database round-trip to the source.
    void SendRedirectPrep(uint32 accountId, std::string const& username,
                          SessionKey const& sessionKey, std::string const& clientIp,
                          uint8 destNodeId);

    /// Called from WorldSocket::HandleRedirectAuthProof (I/O thread) to retrieve
    /// and consume the pre-registered redirect context for @p clientIp.
    /// Returns true and populates @p out on success; returns false if not found
    /// (e.g. pre-notification never arrived or already consumed).
    struct PendingRedirect
    {
        uint32      accountId;
        std::string username;
        SessionKey  sessionKey;
        std::chrono::steady_clock::time_point expiry; ///< 30s TTL
    };
    bool ClaimPendingRedirect(std::string const& clientIp, PendingRedirect& out);

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

    /// Query the proxy for the best available instance server address.
    /// Uses NATS request-reply to cluster.instance.query.
    /// @param outAddr  Filled with the instance server IP on success.
    /// @param outPort  Filled with the instance server game port on success.
    /// @return true if an instance server is available; false if none or disconnected.
    bool QueryBestInstanceAddress(std::string& outAddr, uint16& outPort);

    /// Periodic housekeeping — call from World::Update() every world tick.
    /// Sends MSG_NODE_STATUS every 10s, MSG_NODE_REFRESH every 5min, and
    /// MSG_TRANSPORT_SYNC at the configured ClusterServer.TransportSyncInterval.
    /// Also checks for dead peers every 15s (ClusterServer.NodeDeadThreshold).
    void Update();

    /// Restore BG coordinator to its configured node if that node has come back online.
    /// Called from the world thread when a previously-dead node re-announces or heartbeats.
    void RestoreBgCoordIfNeeded(uint8 revivedNodeId);

    /// Broadcast all live MotionTransport PathProgress values to peer nodes via
    /// MSG_TRANSPORT_SYNC.  Peer nodes apply a correction if their local position
    /// drifts by more than 2 seconds from the received value.
    void SendTransportSync();

    /// Publish a node health snapshot to cluster.mgmt.status.
    /// Called from Update() every ClusterServer.MgmtStatusInterval seconds (default 5).
    void SendMgmtStatus();

    /// Publish all online player positions to cluster.mgmt.players.
    /// Called from Update() every ClusterServer.MgmtPlayersInterval seconds (default 3).
    void SendMgmtPlayers();

    /// Query a running peer node for its current transport PathProgress values via
    /// NATS request-reply on cluster.transport.query.  Returns a map of
    /// {guid_low → PathProgress} suitable for seeding CreateTransport() on this
    /// node's first startup.  Returns an empty map if no peer responds within 500 ms.
    std::unordered_map<uint32, uint32> QueryTransportSync();

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

    // ── NATS publish helpers ──────────────────────────────────────────────────

    /// Publish [msgType:1][payload] directly to cluster.node.{targetNodeId}.
    void PublishToNode(uint8 targetNodeId, uint8 msgType, uint8 const* payload, int payloadLen);

    /// Publish [msgType:1][payload] to cluster.broadcast (all nodes).
    void PublishBroadcast(uint8 msgType, uint8 const* payload, int payloadLen);

    /// Build and publish a cluster.announce payload with this node's identity.
    void PublishAnnounce();

    /// Connect to NATS and subscribe to all required subjects.
    /// Returns true on success; false if connection or any required subscription fails.
    /// Can be called again after a failed Initialize() to retry the connection.
    bool ConnectNATS();

    /// Publish raw bytes to an arbitrary NATS subject (no msgType prefix).
    void PublishRaw(std::string const& subject, uint8 const* data, int len);

    /// NATS callback for cluster.announce messages — registers peer nodes.
    static void OnAnnounceMsg(natsConnection* nc, natsSubscription* sub,
                               natsMsg* msg, void* closure);

    /// Send MSG_NODE_STATUS (player count + NATS bandwidth) as a broadcast.
    void SendNodeStatus();

    /// Send MSG_NODE_REFRESH (full port/map re-registration) to the proxy.
    void SendNodeRefresh();

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

    // ── BG coordinator — only active on ClusterServer.BgCoordinatorNode ──────
    void HandleBgQueueJoin(std::vector<uint8> const& payload);
    void HandleBgQueueLeave(std::vector<uint8> const& payload);
    void HandleBgInstCreated(std::vector<uint8> const& payload);

    // ── Node health / failover ─────────────────────────────────────────────────

    /// Handle an incoming MSG_NODE_STATUS heartbeat from a peer node.
    void HandleNodeStatus(std::vector<uint8> const& payload);

    /// Declare a peer node dead: orphan its maps, elect new BG coordinator, broadcast MSG_NODE_DEAD.
    void HandleNodeDead(uint8 deadNodeId);

    /// Handle MSG_NODE_DEAD received from another node (they detected the death first).
    void HandleNodeDeadMsg(std::vector<uint8> const& payload);

    /// Apply transport PathProgress corrections received in MSG_TRANSPORT_SYNC.
    void HandleTransportSync(std::vector<uint8> const& payload);

    /// Store a pending redirect received via MSG_CLUSTER_REDIRECT_PREP.
    void HandleRedirectPrep(std::vector<uint8> const& payload);

    /// Handle MSG_ANNOUNCE_ACK from a peer confirming our cluster.announce was received.
    /// Sets _clusterRegistered = true, stopping the 10-second re-announce retry.
    void HandleAnnounceAck(std::vector<uint8> const& payload);

    /// NATS callback for cluster.transport.query request-reply.
    /// Runs on the NATS dispatch thread; replies with current PathProgress data.
    static void OnTransportQueryMsg(natsConnection* nc, natsSubscription* sub,
                                     natsMsg* msg, void* closure);

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
    static constexpr uint8 MSG_NODE_STATUS             = 0x19;
    static constexpr uint8 MSG_NODE_REFRESH            = 0x1A;
    /// Periodic broadcast of all live transport PathProgress values to peer nodes.
    /// Payload: [count:2][guid_low:4][path_progress:4]...
    static constexpr uint8 MSG_TRANSPORT_SYNC          = 0x1B;
    /// Broadcast when a node is declared dead after missing heartbeats.
    /// Payload: [deadNodeId:1]
    static constexpr uint8 MSG_NODE_DEAD               = 0x1C;
    /// Published to cluster.mgmt.status every MgmtStatusInterval seconds.
    /// Payload documented in SendMgmtStatus().  Consumed by clustermgr.
    static constexpr uint8 MSG_MGMT_STATUS             = 0x1D;
    /// Published to cluster.mgmt.players every MgmtPlayersInterval seconds.
    /// Payload documented in SendMgmtPlayers().  Consumed by clustermgr.
    static constexpr uint8 MSG_MGMT_PLAYERS            = 0x1E;
    /// Sent from source node to destination node BEFORE SMSG_REDIRECT_CLIENT.
    /// Pre-registers the pending redirect so the destination can authenticate the
    /// incoming CMSG_REDIRECTION_AUTH_PROOF without a round-trip to the source.
    /// Payload: [accountId:4][sessionKey:40][usernameLen:1][username:var][clientIpLen:1][clientIp:var]
    static constexpr uint8 MSG_CLUSTER_REDIRECT_PREP   = 0x1F;
    /// Sent from any peer back to a node that published cluster.announce, confirming
    /// that the registration was received.  Triggers _clusterRegistered = true on the
    /// announcing node, stopping the 10-second re-announce retry.
    /// Payload: [senderNodeId:1]
    static constexpr uint8 MSG_ANNOUNCE_ACK            = 0x20;

    // ── NATS handles ──────────────────────────────────────────────────────────
    natsConnection*   _nc{nullptr};
    natsSubscription* _subNode{nullptr};           ///< cluster.node.{_nodeId}
    natsSubscription* _subBroadcast{nullptr};      ///< cluster.broadcast
    natsSubscription* _subAnnounce{nullptr};       ///< cluster.announce (peer discovery)
    natsSubscription* _subTransportQuery{nullptr}; ///< cluster.transport.query (req-reply)

    // ── Node identity ─────────────────────────────────────────────────────────
    uint8       _nodeId{ 0 };    ///< Config-derived node ID (ClusterServer.NodeId).
    uint8       _serverType{ 0 };
    uint16      _gamePort{ 0 };
    std::string _gameAddress;    ///< Own LAN IP (sent in registration so proxy can match it)
    std::string _natsUrl;

    bool _connected{ false };
    bool _clusterRegistered{ false };  ///< true after at least one peer sends MSG_ANNOUNCE_ACK

    // ── Periodic update timers ────────────────────────────────────────────────
    uint32 _lastHeartbeatMs{ 0 };        ///< getMSTime() at last MSG_NODE_STATUS send
    uint32 _lastRefreshMs{ 0 };          ///< getMSTime() at last MSG_NODE_REFRESH send
    uint32 _lastTransportSyncMs{ 0 };    ///< getMSTime() at last MSG_TRANSPORT_SYNC broadcast
    uint32 _transportSyncIntervalMs{ 60000 }; ///< broadcast interval (from config, ms)
    uint32 _lastDeadCheckMs{ 0 };        ///< getMSTime() at last dead-node check
    uint32 _lastMgmtStatusMs{ 0 };       ///< getMSTime() at last cluster.mgmt.status publish
    uint32 _lastMgmtPlayersMs{ 0 };      ///< getMSTime() at last cluster.mgmt.players publish
    uint32 _lastNatsRetryMs{ 0 };        ///< getMSTime() at last NATS reconnect attempt
    uint32 _lastAnnounceRetryMs{ 0 };    ///< getMSTime() at last cluster.announce retry

    // ── Startup time (for uptime reporting) ───────────────────────────────────
    uint32 _startupTimeMs{ 0 };        ///< getMSTime() at Initialize()

    // ── CPU tracking for mgmt status ─────────────────────────────────────────
    uint32 _lastCpuJiffies{ 0 };       ///< utime+stime from last /proc/self/stat read
    uint32 _lastCpuCheckMs{ 0 };       ///< getMSTime() at last CPU measurement

    // ── Cluster instability counter ───────────────────────────────────────────
    uint16 _nodeCrashCount{ 0 };       ///< Incremented each time we detect a peer node death

    // ── Pending cross-node redirects (keyed by client IP) ────────────────────
    // Populated by HandleRedirectPrep; consumed by ClaimPendingRedirect (I/O thread).
    std::mutex _pendingRedirectsMutex;
    std::unordered_map<std::string, PendingRedirect> _pendingRedirects;

    // ── Dynamic BG coordinator tracking ──────────────────────────────────────
    uint8  _bgCoordNodeId{ 0 };        ///< Currently elected BG coordinator node (from config, updated on failover)

    // ── NATS bandwidth counters (reset after each MSG_NODE_STATUS) ────────────
    std::atomic<uint32> _natsBytesTx{ 0 }; ///< bytes published via NATS since last heartbeat
    std::atomic<uint32> _natsBytesRx{ 0 }; ///< bytes received via NATS since last heartbeat

    // ── BG coordinator state (world-thread-only; no mutex needed) ─────────────
    struct BgQueueEntry
    {
        uint64 guid{ 0 };
        uint8  nodeId{ 0 };
        uint8  teamId{ 0 };
    };
    struct BgMatchState
    {
        std::vector<BgQueueEntry> alliance;
        std::vector<BgQueueEntry> horde;
        uint8  minPlayersPerTeam{ 1 };
    };
    struct PendingBgMatch
    {
        uint32 bgTypeId{ 0 };
        uint8  bracketId{ 0 };
        std::vector<BgQueueEntry> alliance;
        std::vector<BgQueueEntry> horde;
    };
    /// _bgQueues[bgTypeId][bracketId] → queue state.  Only used on coordinator node.
    std::unordered_map<uint32, std::unordered_map<uint8, BgMatchState>> _bgQueues;
    std::unordered_map<uint32, PendingBgMatch> _pendingBgMatches;
    uint32 _nextBgMatchId{ 1 };
};

#define sProxyClient ProxyClient::Instance()

#endif // ProxyClient_h__
