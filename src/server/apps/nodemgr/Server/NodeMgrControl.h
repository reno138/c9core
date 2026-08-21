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

#ifndef NodeMgrControl_h__
#define NodeMgrControl_h__

#include "Define.h"
#include <atomic>
#include <string>

struct __natsConnection;
struct __natsSubscription;
struct __natsMsg;
typedef struct __natsConnection   natsConnection;
typedef struct __natsSubscription natsSubscription;
typedef struct __natsMsg          natsMsg;

class NodeMgr;

/**
 * @brief Remote control + status channel for the worldserver supervisor.
 *
 * WHY THIS EXISTS
 * ---------------
 * clustermgr could previously only ask the *worldserver* to do things. That is
 * useless in the one case an operator actually needs it: a hung worldserver
 * cannot service its own stop command. NodeMgr is a separate process that
 * remains responsive, so control has to terminate here.
 *
 * It also publishes supervisor state independently of the worldserver, so the
 * UI can distinguish "node is hung" (supervisor says Running, worldserver has
 * stopped publishing mgmt.status) from "node is down" (supervisor says Stopped).
 * The old UI could not tell those apart.
 *
 * SUBJECTS
 * --------
 *   subscribe  cluster.nodemgr.{nodeId}   commands addressed to this supervisor
 *   publish    cluster.mgmt.nodemgr       supervisor status heartbeat
 *
 * All frames are ClusterAuth-sealed. An unauthenticated remote-kill subject
 * would be a trivially abusable denial-of-service against the whole cluster.
 */
class NodeMgrControl
{
public:
    /// Commands accepted on cluster.nodemgr.{nodeId}
    enum Command : uint8
    {
        CMD_START   = 0x01,
        CMD_STOP    = 0x02,  ///< SIGTERM, escalating to SIGKILL
        CMD_KILL    = 0x03,  ///< immediate SIGKILL
        CMD_RESTART = 0x04,
    };

    static constexpr uint8 MSG_NODEMGR_STATUS = 0x40;

    NodeMgrControl() = default;
    ~NodeMgrControl();

    NodeMgrControl(NodeMgrControl const&) = delete;
    NodeMgrControl& operator=(NodeMgrControl const&) = delete;

    /// @return false if NATS is unreachable or the auth key is unusable.
    ///         Failure is non-fatal: the supervisor still runs locally.
    bool Start(std::string const& natsUrl, uint8 nodeId, NodeMgr* mgr);

    /// Publish a supervisor status heartbeat. Call from the poll loop.
    void PublishStatus();

    void Stop();

    [[nodiscard]] bool IsConnected() const { return _connected.load(std::memory_order_relaxed); }

private:
    static void OnCommandMsg(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);

    natsConnection*   _nc{ nullptr };
    natsSubscription* _sub{ nullptr };
    NodeMgr*          _mgr{ nullptr };
    uint8             _nodeId{ 0 };
    std::atomic<bool> _connected{ false };
};

#endif // NodeMgrControl_h__
