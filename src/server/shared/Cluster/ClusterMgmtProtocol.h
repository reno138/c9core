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

#ifndef ClusterMgmtProtocol_h__
#define ClusterMgmtProtocol_h__

#include "Define.h"

/**
 * @brief Message-type values shared by the management plane (clustermgr,
 *        nodemgr) that ride on ClusterAuth-sealed frames.
 *
 * WHY A SEPARATE NUMBER SPACE
 * ---------------------------
 * ClusterAuth authenticates (srcNode, msgType, payload) but NOT the NATS
 * subject. Every participant holds the same key. So a frame captured on one
 * subject verifies on any other, and the only thing that stops a captured
 * worldserver frame from being replayed as a supervisor command is that the
 * msgType byte does not decode as a command. The supervisor commands used to
 * be 0x01..0x04 — identical to MSG_REGISTER / MSG_REROUTE_PLAYER /
 * MSG_CLUSTER_PLAYER_ONLINE / MSG_CLUSTER_PLAYER_OFFLINE, which every
 * worldserver seals and broadcasts on every login. Replaying one of those to
 * cluster.nodemgr.N inside the 30 s window SIGKILLed that node's worldserver
 * without knowing the key.
 *
 * Everything here lives at 0xC0 and above. NatsBus.h (worldserver) owns
 * 0x00..0x3F and must never allocate in this range; MSG_NODEMGR_STATUS
 * predates this header and keeps its value for wire compatibility.
 */
namespace ClusterMgmt
{
    // ── Supervisor commands: clustermgr -> cluster.nodemgr.{nodeId} ──────────
    constexpr uint8 CMD_START   = 0xC1;
    constexpr uint8 CMD_STOP    = 0xC2;  ///< SIGTERM, escalating to SIGKILL
    constexpr uint8 CMD_KILL    = 0xC3;  ///< immediate SIGKILL
    constexpr uint8 CMD_RESTART = 0xC4;

    constexpr bool IsSupervisorCommand(uint8 msgType)
    {
        return msgType >= CMD_START && msgType <= CMD_RESTART;
    }

    // ── Supervisor heartbeat: nodemgr -> cluster.mgmt.nodemgr ────────────────
    constexpr uint8 MSG_NODEMGR_STATUS = 0x40;

    // ── Worldserver management feeds (values owned by NatsBus.h) ─────────────
    // clustermgr verifies these msgTypes on cluster.mgmt.status / .players.
    constexpr uint8 MSG_MGMT_STATUS  = 0x1D;
    constexpr uint8 MSG_MGMT_PLAYERS = 0x1E;
}

#endif // ClusterMgmtProtocol_h__
