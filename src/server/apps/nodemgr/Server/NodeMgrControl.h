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
#include "ClusterMgmtProtocol.h"
#include <atomic>
#include <string>
#include <vector>

struct __natsConnection;
struct __natsSubscription;
struct __natsMsg;
typedef struct __natsConnection   natsConnection;
typedef struct __natsSubscription natsSubscription;
typedef struct __natsMsg          natsMsg;

namespace boost { namespace asio { class io_context; } }

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
 *
 * SUBJECTS
 * --------
 *   subscribe  cluster.nodemgr.{nodeId}   commands addressed to this supervisor
 *   publish    cluster.mgmt.nodemgr       supervisor status heartbeat
 *
 * AUTHORISATION
 * -------------
 * Frames are ClusterAuth-sealed, but the tag does not cover the subject and
 * the key is shared cluster-wide, so two more checks gate a command:
 *   - msgType must be one of ClusterMgmt::CMD_* (0xC1..0xC4). A captured
 *     worldserver frame therefore does not decode as a command.
 *   - srcNode must be in NodeMgr.ControllerNodeIds. Note this only proves the
 *     sender holds the key and *claims* that id; it keeps accidents and
 *     wrong-subject traffic out, not a peer that has the key.
 *
 * THREADING
 * ---------
 * OnCommandMsg runs on the nats.c dispatch thread. NodeMgr is single-threaded
 * by design (Poll() runs on the io_context), so commands are posted onto that
 * io_context rather than acted on in the callback. The previous direct call
 * raced Poll(): a remote START move-assigned bp::child while Poll() was inside
 * child.running()/wait(), and the SIGTERM->SIGKILL escalation could then
 * kill() a pid read across that assignment.
 */
class NodeMgrControl
{
public:
    NodeMgrControl() = default;
    ~NodeMgrControl();

    NodeMgrControl(NodeMgrControl const&) = delete;
    NodeMgrControl& operator=(NodeMgrControl const&) = delete;

    /// @param ioc          io_context that runs NodeMgr::Poll(); commands are posted here.
    /// @param controllers  srcNode ids allowed to issue commands (NodeMgr.ControllerNodeIds).
    /// @return false if NATS is unreachable or the auth key is unusable.
    ///         Failure is non-fatal: the supervisor still runs locally.
    bool Start(std::string const& natsUrl, uint8 nodeId, NodeMgr* mgr,
               boost::asio::io_context& ioc, std::vector<uint8> controllers);

    /// Publish a supervisor status heartbeat. Call from the poll loop.
    void PublishStatus();

    /// Unsubscribe and disconnect. Safe to call more than once. Must be called
    /// before the io_context passed to Start() is destroyed.
    void Stop();

    [[nodiscard]] bool IsConnected() const { return _connected.load(std::memory_order_relaxed); }

private:
    static void OnCommandMsg(natsConnection* nc, natsSubscription* sub, natsMsg* msg, void* closure);

    natsConnection*          _nc{ nullptr };
    natsSubscription*        _sub{ nullptr };
    NodeMgr*                 _mgr{ nullptr };
    boost::asio::io_context* _ioc{ nullptr };
    uint8                    _nodeId{ 0 };
    std::vector<uint8>       _controllers;
    std::atomic<bool>        _connected{ false };
};

#endif // NodeMgrControl_h__
