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

#ifndef ClusterMessages_h__
#define ClusterMessages_h__

#include "Define.h"

/**
 * @file ClusterMessages.h
 * @brief Control-channel message-type constants shared between the proxy (NatsBus)
 *        and worldserver (ProxyClient) sides of the cluster transport layer.
 *
 * Binary payload layout (little-endian, all integers):
 *
 *  --- Worldserver → Proxy (published to cluster.proxy) ---
 *
 *  MSG_REGISTER (0x01):
 *    uint8  server_type   (0 = worldserver, 1 = instance server)
 *    uint16 game_port
 *    uint16 map_count
 *    uint32 map_id[map_count]
 *    uint8  addr_len
 *    char   addr[addr_len]   (node's own game IP — used for _nodeAddresses matching)
 *    → Proxy replies on NATS reply subject with [uint8 assigned_node_id].
 *
 *  MSG_REROUTE_PLAYER (0x02):
 *    uint64 player_guid
 *    uint8  address_len
 *    char   address[address_len]
 *    uint16 port
 *    uint32 map_id     (0 = login reroute, no SMSG_NEW_WORLD needed)
 *    float  x, y, z, ori
 *
 *  MSG_CLUSTER_PLAYER_ONLINE (0x03):
 *    uint64 player_guid
 *    uint8  name_len
 *    char   name[name_len]
 *    uint32 zone_id
 *    uint8  level, class_id, race_id, team_id, node_id
 *
 *  MSG_CLUSTER_PLAYER_OFFLINE (0x04):
 *    uint64 player_guid
 *
 *  MSG_CLUSTER_DELIVER_PACKET (0x05):
 *    uint64 target_guid
 *    uint16 packet_len
 *    uint8  packet[packet_len]
 *
 *  MSG_CLUSTER_RELAY_TO_NODE (0x06):
 *    uint8  target_node_id
 *    uint8  inner_type
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  MSG_CLUSTER_GROUP_UPDATE (0x07):
 *    uint64 group_guid
 *    uint8  member_count
 *    [per member: uint64 guid + uint8 subgroup + uint8 role_flags + uint8 node_id]
 *
 *  MSG_CLUSTER_GROUP_DISBAND (0x08):
 *    uint64 group_guid
 *
 *  MSG_CLUSTER_LFG_RELAY (0x09):
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  MSG_CLUSTER_LFG_RELAY_RESP (0x0A):
 *    uint8  target_node_id
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  MSG_REROUTE_TO_MAP (0x0B):
 *    uint64 player_guid
 *    uint32 map_id
 *
 *  MSG_CLUSTER_UNIT_UPDATE (0x0C):
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  MSG_PONG (0x0E): node → proxy echo of ping timestamp
 *    uint64 timestamp_ms
 *
 *  MSG_CLUSTER_CHAT (0x11):
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  MSG_CLUSTER_NOTIFY_MAIL (0x12):
 *    uint64 recipient_guid
 *
 *  MSG_CLUSTER_BG_QUEUE_JOIN (0x13):
 *    uint64 guid, uint32 bgTypeId, uint8 bracketId, uint8 teamId, uint8 minPerTeam
 *
 *  MSG_CLUSTER_BG_QUEUE_LEAVE (0x14):
 *    uint64 guid, uint32 bgTypeId
 *
 *  MSG_CLUSTER_BG_INST_CREATED (0x17):
 *    uint32 matchId, uint32 instanceId, uint32 mapId, uint32 clientInstanceId
 *
 *  MSG_CLUSTER_ARENA_RESULT (0x16):
 *    uint16 payload_len
 *    uint8  payload[payload_len]
 *
 *  --- Proxy → Worldserver (published to cluster.node.{N} or cluster.broadcast) ---
 *
 *  MSG_REGISTER_ACK (0x10): [delivered as NATS request reply — not on these subjects]
 *    uint8 assigned_node_id
 *
 *  MSG_PING (0x0D):
 *    uint64 timestamp_ms
 *
 *  MSG_CLUSTER_BG_CREATE_INST (0x15):  proxy → instance node
 *    uint32 matchId, uint32 bgTypeId, uint8 bracketId
 *    uint8 allianceCount, uint8 hordeCount
 *    uint64 allianceGuids[allianceCount]
 *    uint64 hordeGuids[hordeCount]
 *
 *  MSG_CLUSTER_BG_READY (0x18):  proxy → player nodes
 *    uint32 instanceId, uint32 bgTypeId, uint32 mapId, uint32 clientInstanceId
 *    uint8 count
 *    [per player: uint64 guid + uint8 teamId]
 *
 *  --- Worldserver → Proxy (periodic status updates on cluster.proxy) ---
 *
 *  MSG_NODE_STATUS (0x19):  node → proxy every 10s
 *    uint32 playerCount        — active WorldSession count on this node
 *    uint32 natsBytesTx        — NATS control bytes published since last heartbeat
 *    uint32 natsBytesRx        — NATS control bytes received since last heartbeat
 *
 *  MSG_NODE_REFRESH (0x1A):  node → proxy every 5 minutes (full state sync)
 *    uint8  server_type
 *    uint16 game_port
 *    uint16 map_count
 *    uint32 map_id[map_count]
 *    uint8  addr_len
 *    char   addr[addr_len]
 */

namespace ClusterMsg
{
    /// Worldserver → proxy (registration — request-reply on cluster.register)
    static constexpr uint8 REGISTER                = 0x01;
    static constexpr uint8 REROUTE_PLAYER          = 0x02;
    static constexpr uint8 CLUSTER_PLAYER_ONLINE   = 0x03;
    static constexpr uint8 CLUSTER_PLAYER_OFFLINE  = 0x04;
    static constexpr uint8 CLUSTER_DELIVER_PACKET  = 0x05;
    static constexpr uint8 CLUSTER_RELAY_TO_NODE   = 0x06;
    static constexpr uint8 CLUSTER_GROUP_UPDATE    = 0x07;
    static constexpr uint8 CLUSTER_GROUP_DISBAND   = 0x08;
    static constexpr uint8 CLUSTER_LFG_RELAY       = 0x09;
    static constexpr uint8 CLUSTER_LFG_RELAY_RESP  = 0x0A;
    static constexpr uint8 REROUTE_TO_MAP          = 0x0B;
    static constexpr uint8 CLUSTER_UNIT_UPDATE     = 0x0C;
    static constexpr uint8 PING                    = 0x0D; ///< proxy → node: uint64 timestamp
    static constexpr uint8 PONG                    = 0x0E; ///< node → proxy: uint64 timestamp echo
    static constexpr uint8 REGISTER_ACK            = 0x10; ///< NATS reply only — not on cluster subjects
    static constexpr uint8 CLUSTER_CHAT            = 0x11;
    static constexpr uint8 CLUSTER_NOTIFY_MAIL     = 0x12;
    static constexpr uint8 CLUSTER_BG_QUEUE_JOIN   = 0x13;
    static constexpr uint8 CLUSTER_BG_QUEUE_LEAVE  = 0x14;
    static constexpr uint8 CLUSTER_BG_CREATE_INST  = 0x15; ///< proxy → instance node
    static constexpr uint8 CLUSTER_ARENA_RESULT    = 0x16;
    static constexpr uint8 CLUSTER_BG_INST_CREATED = 0x17; ///< instance node → proxy
    static constexpr uint8 CLUSTER_BG_READY        = 0x18; ///< proxy → player nodes
    static constexpr uint8 NODE_STATUS             = 0x19; ///< node → proxy: player count + NATS bandwidth (every 10s)
    static constexpr uint8 NODE_REFRESH            = 0x1A; ///< node → proxy: full port/map re-registration (every 5min)
    static constexpr uint8 RA_COMMAND  = 0x21; ///< proxy → all nodes: broadcast a CLI command for execution
    static constexpr uint8 RA_REPLY    = 0x22; ///< node → proxy: CLI command output
}

#endif // ClusterMessages_h__
