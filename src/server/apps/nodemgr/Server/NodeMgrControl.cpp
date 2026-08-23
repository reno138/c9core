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

#include "NodeMgrControl.h"
#include "NodeMgr.h"
#include "ClusterAuth.h"
#include "Log.h"

#include <nats.h>
#include <cstring>
#include <vector>

NodeMgrControl::~NodeMgrControl()
{
    Stop();
}

bool NodeMgrControl::Start(std::string const& natsUrl, uint8 nodeId, NodeMgr* mgr)
{
    _nodeId = nodeId;
    _mgr    = mgr;

    if (!ClusterAuth::IsInitialised())
    {
        LOG_ERROR("nodemgr", "NodeMgrControl: cluster auth not initialised — remote control disabled");
        return false;
    }

    natsStatus s = natsConnection_ConnectTo(&_nc, natsUrl.c_str());
    if (s != NATS_OK)
    {
        LOG_WARN("nodemgr", "NodeMgrControl: cannot reach NATS at {} — {} (supervisor continues locally)",
                 natsUrl, natsStatus_GetText(s));
        _nc = nullptr;
        return false;
    }

    std::string subject = "cluster.nodemgr." + std::to_string(nodeId);
    s = natsConnection_Subscribe(&_sub, _nc, subject.c_str(), OnCommandMsg, this);
    if (s != NATS_OK)
    {
        LOG_ERROR("nodemgr", "NodeMgrControl: subscribe to {} failed — {}", subject, natsStatus_GetText(s));
        natsConnection_Destroy(_nc);
        _nc = nullptr;
        return false;
    }

    _connected.store(true, std::memory_order_relaxed);
    LOG_INFO("nodemgr", "NodeMgrControl: listening on {}", subject);
    return true;
}

void NodeMgrControl::Stop()
{
    _connected.store(false, std::memory_order_relaxed);
    if (_sub) { natsSubscription_Destroy(_sub); _sub = nullptr; }
    if (_nc)  { natsConnection_Destroy(_nc);    _nc  = nullptr; }
}

/*static*/
void NodeMgrControl::OnCommandMsg(natsConnection* /*nc*/, natsSubscription* /*sub*/,
                                  natsMsg* msg, void* closure)
{
    auto* self = static_cast<NodeMgrControl*>(closure);

    uint8 const* d = reinterpret_cast<uint8 const*>(natsMsg_GetData(msg));
    int          n = natsMsg_GetDataLength(msg);

    uint8 srcNode = 0;
    uint8 msgType = 0;
    std::vector<uint8> payload;
    bool const authentic = ClusterAuth::Open(d, static_cast<std::size_t>(n), srcNode, msgType, payload);
    natsMsg_Destroy(msg);

    if (!authentic)
    {
        LOG_WARN("nodemgr", "NodeMgrControl: dropped unauthenticated control frame ({} bytes)", n);
        return;
    }

    if (!self->_mgr)
        return;

    // msgType carries the command; payload is unused today but kept for
    // forward-compatibility (e.g. a reason string for audit logging).
    switch (msgType)
    {
        case CMD_START:
            LOG_INFO("nodemgr", "NodeMgrControl: START from node {}", srcNode);
            self->_mgr->Start();
            break;
        case CMD_STOP:
            LOG_INFO("nodemgr", "NodeMgrControl: STOP from node {}", srcNode);
            self->_mgr->Stop();
            break;
        case CMD_KILL:
            LOG_WARN("nodemgr", "NodeMgrControl: KILL from node {}", srcNode);
            self->_mgr->Kill();
            break;
        case CMD_RESTART:
            LOG_INFO("nodemgr", "NodeMgrControl: RESTART from node {}", srcNode);
            self->_mgr->Restart();
            break;
        default:
            LOG_WARN("nodemgr", "NodeMgrControl: unknown command 0x{:02X} from node {}", msgType, srcNode);
            break;
    }
}

void NodeMgrControl::PublishStatus()
{
    if (!_connected.load(std::memory_order_relaxed) || !_nc || !_mgr)
        return;

    // Wire: [nodeId:1][state:1][pid:4 LE][uptime:4 LE][restartPending:1]
    uint8 body[11];
    body[0] = _nodeId;
    body[1] = static_cast<uint8>(_mgr->GetState());

    uint32 const pid    = _mgr->GetPid();
    uint32 const uptime = _mgr->GetUptime();
    for (int i = 0; i < 4; ++i) body[2 + i] = static_cast<uint8>((pid    >> (i * 8)) & 0xFF);
    for (int i = 0; i < 4; ++i) body[6 + i] = static_cast<uint8>((uptime >> (i * 8)) & 0xFF);
    body[10] = _mgr->IsRestartPending() ? 1 : 0;

    std::vector<uint8> frame = ClusterAuth::Seal(_nodeId, MSG_NODEMGR_STATUS, body, sizeof(body));
    if (frame.empty())
        return;

    natsConnection_Publish(_nc, "cluster.mgmt.nodemgr", frame.data(), static_cast<int>(frame.size()));
}
