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

#include "NodeMgrSocket.h"
#include "Config.h"
#include "Log.h"
#include "ProxyMgr.h"
#include <cstring>

NodeMgrSocket::NodeMgrSocket(IoContextTcpSocket&& socket)
    : BaseSocket(std::move(socket))
    , _accumBuffer(256)
{
}

void NodeMgrSocket::Start()
{
    // ── PSK handshake: generate nonce, init crypto, send nonce unencrypted ────
    std::string psk = sConfigMgr->GetOption<std::string>("Management.SharedSecret", "change-me");

    uint8 nonce[NONCE_SIZE]{};
    PskCrypt::GenerateNonce(nonce);

    if (!_crypt.Init(psk, nonce, true /*isServer*/))
    {
        LOG_ERROR("nodemgr", "NodeMgrSocket: PSK crypto init failed — closing");
        CloseSocket();
        return;
    }
    _cryptReady = true;

    // Send nonce as the very first bytes (unencrypted; client needs it to init crypto).
    MessageBuffer nonceBuf(NONCE_SIZE);
    nonceBuf.Write(nonce, NONCE_SIZE);
    QueuePacket(std::move(nonceBuf));

    AsyncRead();
    LOG_DEBUG("nodemgr", "NodeMgrSocket: Accepted nodemgr connection from {}", GetRemoteIpAddress().to_string());
}

void NodeMgrSocket::OnClose()
{
    if (_assignedNodeId != 0)
    {
        sProxyMgr.UnregisterNodeMgr(_assignedNodeId);
        LOG_INFO("nodemgr", "NodeMgrSocket: nodemgr for node {} disconnected", _assignedNodeId);
    }
}

void NodeMgrSocket::SendNodeStart()
{
    SendEncrypted({ MSG_NODE_START });
}

void NodeMgrSocket::SendNodeStop()
{
    SendEncrypted({ MSG_NODE_STOP });
}

void NodeMgrSocket::SendEncrypted(std::vector<uint8> const& data)
{
    if (data.empty())
        return;

    std::vector<uint8> enc(data);
    if (_cryptReady)
        _crypt.Encrypt(enc.data(), enc.size());

    MessageBuffer buf(enc.size());
    buf.Write(enc.data(), enc.size());
    QueuePacket(std::move(buf));
}

SocketReadCallbackResult NodeMgrSocket::ReadHandler()
{
    MessageBuffer& raw = GetReadBuffer();
    std::size_t avail = raw.GetActiveSize();

    if (avail == 0)
        return SocketReadCallbackResult::KeepReading;

    // Decrypt in-place.
    if (_cryptReady)
        _crypt.Decrypt(raw.GetReadPointer(), avail);

    // Copy decrypted bytes into accumulation buffer.
    _accumBuffer.EnsureFreeSpace();
    if (_accumBuffer.GetRemainingSpace() < avail)
        _accumBuffer.Resize(_accumBuffer.GetBufferSize() + avail + 256);

    _accumBuffer.Write(raw.GetReadPointer(), avail);
    raw.ReadCompleted(avail);

    ProcessBuffer();
    return SocketReadCallbackResult::KeepReading;
}

void NodeMgrSocket::ProcessBuffer()
{
    while (_accumBuffer.GetActiveSize() > 0)
    {
        uint8 const* p = _accumBuffer.GetReadPointer();
        std::size_t  avail = _accumBuffer.GetActiveSize();

        switch (_parseState)
        {
            case ParseState::WaitType:
            {
                uint8 msgType = p[0];
                _accumBuffer.ReadCompleted(1);

                switch (msgType)
                {
                    case MSG_NODEMGR_REGISTER: _parseState = ParseState::ReadRegister; break;
                    case MSG_NODE_STATUS:       _parseState = ParseState::ReadStatus;   break;
                    default:
                        LOG_WARN("nodemgr", "NodeMgrSocket: Unknown msg type 0x{:02X} — closing", msgType);
                        CloseSocket();
                        return;
                }
                break;
            }

            case ParseState::ReadRegister:
            {
                if (_accumBuffer.GetActiveSize() < REGISTER_PAYLOAD)
                    return;

                p = _accumBuffer.GetReadPointer();
                uint8  nodeId   = p[0];
                uint16 gamePort = static_cast<uint16>(p[1]) | (static_cast<uint16>(p[2]) << 8);
                _accumBuffer.ReadCompleted(REGISTER_PAYLOAD);
                _parseState = ParseState::WaitType;

                HandleRegister(nodeId, gamePort);
                break;
            }

            case ParseState::ReadStatus:
            {
                if (_accumBuffer.GetActiveSize() < STATUS_PAYLOAD)
                    return;

                p = _accumBuffer.GetReadPointer();
                uint8  state  = p[0];
                uint32 pid    = static_cast<uint32>(p[1])
                              | (static_cast<uint32>(p[2]) << 8)
                              | (static_cast<uint32>(p[3]) << 16)
                              | (static_cast<uint32>(p[4]) << 24);
                uint32 uptime = static_cast<uint32>(p[5])
                              | (static_cast<uint32>(p[6]) << 8)
                              | (static_cast<uint32>(p[7]) << 16)
                              | (static_cast<uint32>(p[8]) << 24);
                _accumBuffer.ReadCompleted(STATUS_PAYLOAD);
                _parseState = ParseState::WaitType;

                HandleStatus(state, pid, uptime);
                break;
            }

            default:
                return;
        }
        (void)avail;
    }
}

void NodeMgrSocket::HandleRegister(uint8 nodeId, uint16 gamePort)
{
    _configuredNodeId = nodeId;
    _gamePort         = gamePort;

    _assignedNodeId = sProxyMgr.RegisterNodeMgr(shared_from_this(), nodeId, gamePort);

    LOG_INFO("nodemgr", "NodeMgrSocket: nodemgr registered — configured_node={} game_port={} assigned_id={}",
             nodeId, gamePort, _assignedNodeId);

    // Send ack: MSG_NODEMGR_REGISTER | assigned_node_id
    SendEncrypted({ MSG_NODEMGR_REGISTER, _assignedNodeId });

    // Auto-start: immediately tell the nodemgr to launch its worldserver.
    // This avoids needing a separate management-socket START command.
    sProxyMgr.StartNode(_assignedNodeId);
}

void NodeMgrSocket::HandleStatus(uint8 state, uint32 pid, uint32 uptime)
{
    static const char* stateNames[] = { "UNKNOWN","STOPPED","STARTING","RUNNING","STOPPING","CRASHED" };
    char const* stateName = (state < 6) ? stateNames[state] : "INVALID";

    LOG_INFO("nodemgr", "NodeMgrSocket: node {} status={} pid={} uptime={}s",
             _assignedNodeId, stateName, pid, uptime);

    sProxyMgr.UpdateNodeMgrStatus(_assignedNodeId, state, pid, uptime);
}
