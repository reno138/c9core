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

#include "ManagementSocket.h"
#include "Config.h"
#include "Log.h"
#include "ProxyMgr.h"

ManagementSocket::ManagementSocket(IoContextTcpSocket&& socket)
    : BaseSocket(std::move(socket))
    , _accumBuffer(256)
{
}

void ManagementSocket::Start()
{
    std::string psk = sConfigMgr->GetOption<std::string>("Management.SharedSecret", "change-me");

    uint8 nonce[NONCE_SIZE]{};
    PskCrypt::GenerateNonce(nonce);

    if (!_crypt.Init(psk, nonce, true /*isServer*/))
    {
        LOG_ERROR("mgmt", "ManagementSocket: PSK crypto init failed — closing");
        CloseSocket();
        return;
    }
    _cryptReady = true;

    MessageBuffer nonceBuf(NONCE_SIZE);
    nonceBuf.Write(nonce, NONCE_SIZE);
    QueuePacket(std::move(nonceBuf));

    AsyncRead();
    LOG_INFO("mgmt", "ManagementSocket: clustermgr connected from {}", GetRemoteIpAddress().to_string());
}

void ManagementSocket::OnClose()
{
    sProxyMgr.RemoveMgmtSubscriber(shared_from_this());
    LOG_INFO("mgmt", "ManagementSocket: clustermgr disconnected");
}

void ManagementSocket::SendStatusPush(std::vector<uint8> const& payload)
{
    if (!IsOpen())
        return;

    std::vector<uint8> enc;
    enc.reserve(1 + payload.size());
    enc.push_back(MSG_MGMT_STATUS_PUSH);
    enc.insert(enc.end(), payload.begin(), payload.end());

    if (_cryptReady)
        _crypt.Encrypt(enc.data(), enc.size());

    MessageBuffer buf(enc.size());
    buf.Write(enc.data(), enc.size());
    QueuePacket(std::move(buf));
}

SocketReadCallbackResult ManagementSocket::ReadHandler()
{
    MessageBuffer& raw = GetReadBuffer();
    std::size_t avail = raw.GetActiveSize();

    if (avail == 0)
        return SocketReadCallbackResult::KeepReading;

    if (_cryptReady)
        _crypt.Decrypt(raw.GetReadPointer(), avail);

    _accumBuffer.EnsureFreeSpace();
    if (_accumBuffer.GetRemainingSpace() < avail)
        _accumBuffer.Resize(_accumBuffer.GetBufferSize() + avail + 256);

    _accumBuffer.Write(raw.GetReadPointer(), avail);
    raw.ReadCompleted(avail);

    ProcessBuffer();
    return SocketReadCallbackResult::KeepReading;
}

void ManagementSocket::ProcessBuffer()
{
    while (_accumBuffer.GetActiveSize() > 0)
    {
        uint8 const* p = _accumBuffer.GetReadPointer();

        switch (_parseState)
        {
            case ParseState::WaitType:
            {
                uint8 msgType = p[0];
                _accumBuffer.ReadCompleted(1);

                switch (msgType)
                {
                    case MSG_MGMT_SUBSCRIBE: HandleSubscribe(); break;
                    case MSG_MGMT_NODE_CMD:  _parseState = ParseState::ReadNodeCmd; break;
                    default:
                        LOG_WARN("mgmt", "ManagementSocket: Unknown msg 0x{:02X} — closing", msgType);
                        CloseSocket();
                        return;
                }
                break;
            }

            case ParseState::ReadNodeCmd:
            {
                if (_accumBuffer.GetActiveSize() < NODE_CMD_SIZE)
                    return;

                p = _accumBuffer.GetReadPointer();
                uint8 cmd    = p[0];
                uint8 nodeId = p[1];
                _accumBuffer.ReadCompleted(NODE_CMD_SIZE);
                _parseState = ParseState::WaitType;

                HandleNodeCmd(cmd, nodeId);
                break;
            }

            default:
                return;
        }
    }
}

void ManagementSocket::HandleSubscribe()
{
    _subscribed = true;
    sProxyMgr.AddMgmtSubscriber(shared_from_this());
    LOG_INFO("mgmt", "ManagementSocket: clustermgr subscribed to status push");
    // Immediately send current state.
    sProxyMgr.PushStatusToSubscribers();
}

void ManagementSocket::HandleNodeCmd(uint8 cmd, uint8 nodeId)
{
    switch (cmd)
    {
        case 1:
            LOG_INFO("mgmt", "ManagementSocket: clustermgr requested START node {}", nodeId);
            sProxyMgr.StartNode(nodeId);
            break;
        case 2:
            LOG_INFO("mgmt", "ManagementSocket: clustermgr requested STOP node {}", nodeId);
            sProxyMgr.StopNode(nodeId);
            break;
        default:
            LOG_WARN("mgmt", "ManagementSocket: Unknown node cmd {} for node {}", cmd, nodeId);
            break;
    }
}
