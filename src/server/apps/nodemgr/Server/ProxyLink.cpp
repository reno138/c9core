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

#include "ProxyLink.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

ProxyLink::ProxyLink(boost::asio::io_context& ioCtx, NodeMgr& nodeMgr)
    : _ioCtx(ioCtx)
    , _nodeMgr(nodeMgr)
    , _socket(ioCtx)
    , _resolver(ioCtx)
    , _reconnectTimer(ioCtx)
    , _heartbeatTimer(ioCtx)
    , _recvBuf(RECV_BUF_SIZE)
{
}

void ProxyLink::Start(std::string proxyHost, uint16 proxyPort, std::string sharedSecret,
                      uint8 configuredNodeId, uint16 gamePort)
{
    _proxyHost        = std::move(proxyHost);
    _proxyPort        = proxyPort;
    _sharedSecret     = std::move(sharedSecret);
    _configuredNodeId = configuredNodeId;
    _gamePort         = gamePort;

    Connect();
}

void ProxyLink::Connect()
{
    LOG_INFO("nodemgr", "ProxyLink: Connecting to proxy {}:{}", _proxyHost, _proxyPort);

    _resolver.async_resolve(_proxyHost, std::to_string(_proxyPort),
        [self = shared_from_this()](boost::system::error_code const& ec, tcp::resolver::results_type endpoints)
        {
            if (ec)
            {
                LOG_ERROR("nodemgr", "ProxyLink: Resolve failed: {}", ec.message());
                self->Reconnect();
                return;
            }
            boost::asio::async_connect(self->_socket, endpoints,
                [self](boost::system::error_code const& ec2, tcp::endpoint const&)
                {
                    if (ec2)
                    {
                        LOG_ERROR("nodemgr", "ProxyLink: Connect failed: {}", ec2.message());
                        self->Reconnect();
                        return;
                    }
                    LOG_INFO("nodemgr", "ProxyLink: Connected to proxy");
                    self->ReadNonce();
                });
        });
}

void ProxyLink::Reconnect()
{
    _connected   = false;
    _cryptReady  = false;
    _waitingNonce = true;
    _parseAccum.clear();

    boost::system::error_code ec;
    _socket.close(ec);
    _heartbeatTimer.cancel();

    _reconnectTimer.expires_after(std::chrono::seconds(10));
    _reconnectTimer.async_wait([self = shared_from_this()](boost::system::error_code const& ec)
    {
        if (!ec)
        {
            // Re-create socket (boost::asio sockets can't be reconnected after close).
            self->_socket = tcp::socket(self->_ioCtx);
            self->Connect();
        }
    });
}

void ProxyLink::ReadNonce()
{
    // Read exactly 16 bytes (nonce, sent unencrypted by proxy).
    boost::asio::async_read(_socket,
        boost::asio::buffer(_nonce, 16),
        [self = shared_from_this()](boost::system::error_code const& ec, std::size_t)
        {
            if (ec)
            {
                LOG_ERROR("nodemgr", "ProxyLink: ReadNonce failed: {}", ec.message());
                self->Reconnect();
                return;
            }
            self->OnNonceReceived();
        });
}

void ProxyLink::OnNonceReceived()
{
    if (!_crypt.Init(_sharedSecret, _nonce, false /*client*/))
    {
        LOG_ERROR("nodemgr", "ProxyLink: PSK crypto init failed");
        Reconnect();
        return;
    }
    _cryptReady   = true;
    _waitingNonce = false;

    LOG_DEBUG("nodemgr", "ProxyLink: PSK handshake complete");
    SendRegister();
}

void ProxyLink::SendRegister()
{
    // MSG_NODEMGR_REGISTER: uint8 msg_type + uint8 node_id + uint16 game_port
    std::vector<uint8> msg(4);
    msg[0] = MSG_NODEMGR_REGISTER;
    msg[1] = _configuredNodeId;
    msg[2] = static_cast<uint8>(_gamePort & 0xFF);
    msg[3] = static_cast<uint8>(_gamePort >> 8);

    Send(std::move(msg));

    // Start reading commands from proxy.
    AsyncRead();

    // Start heartbeat.
    StartHeartbeatTimer();
}

void ProxyLink::AsyncRead()
{
    _socket.async_read_some(boost::asio::buffer(_recvBuf),
        [self = shared_from_this()](boost::system::error_code const& ec, std::size_t bytes)
        {
            self->OnRead(ec, bytes);
        });
}

void ProxyLink::OnRead(boost::system::error_code const& ec, std::size_t bytes)
{
    if (ec)
    {
        OnDisconnect(ec.message());
        return;
    }

    // Decrypt in-place.
    if (_cryptReady && bytes > 0)
        _crypt.Decrypt(_recvBuf.data(), bytes);

    _parseAccum.insert(_parseAccum.end(), _recvBuf.begin(), _recvBuf.begin() + bytes);
    ProcessIncoming();
    AsyncRead();
}

void ProxyLink::ProcessIncoming()
{
    while (!_parseAccum.empty())
    {
        uint8 msgType = _parseAccum[0];

        switch (msgType)
        {
            case MSG_NODEMGR_REGISTER:
                // ACK from proxy: type(1) + assigned_node_id(1)
                if (_parseAccum.size() < 2)
                    return;
                _assignedNodeId = _parseAccum[1];
                _connected      = true;
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + 2);
                LOG_INFO("nodemgr", "ProxyLink: Registered — assigned node_id={}", _assignedNodeId);
                break;

            case MSG_NODE_START:
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + 1);
                LOG_INFO("nodemgr", "ProxyLink: Received NODE_START from proxy");
                _nodeMgr.Start();
                break;

            case MSG_NODE_STOP:
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + 1);
                LOG_INFO("nodemgr", "ProxyLink: Received NODE_STOP from proxy");
                _nodeMgr.Stop();
                break;

            default:
                LOG_WARN("nodemgr", "ProxyLink: Unknown msg type 0x{:02X} — discarding", msgType);
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + 1);
                break;
        }
    }
}

void ProxyLink::SendStatus()
{
    _nodeMgr.Poll();

    uint8  state  = static_cast<uint8>(_nodeMgr.GetState());
    uint32 pid    = _nodeMgr.GetPid();
    uint32 uptime = _nodeMgr.GetUptime();

    // MSG_NODE_STATUS: type(1) + state(1) + pid(4) + uptime(4) = 10 bytes
    std::vector<uint8> msg(10);
    msg[0] = MSG_NODE_STATUS;
    msg[1] = state;
    msg[2] = static_cast<uint8>(pid & 0xFF);
    msg[3] = static_cast<uint8>((pid >> 8) & 0xFF);
    msg[4] = static_cast<uint8>((pid >> 16) & 0xFF);
    msg[5] = static_cast<uint8>((pid >> 24) & 0xFF);
    msg[6] = static_cast<uint8>(uptime & 0xFF);
    msg[7] = static_cast<uint8>((uptime >> 8) & 0xFF);
    msg[8] = static_cast<uint8>((uptime >> 16) & 0xFF);
    msg[9] = static_cast<uint8>((uptime >> 24) & 0xFF);

    Send(std::move(msg));
}

void ProxyLink::StartHeartbeatTimer()
{
    _heartbeatTimer.expires_after(std::chrono::seconds(5));
    _heartbeatTimer.async_wait([self = shared_from_this()](boost::system::error_code const& ec)
    {
        if (ec)
            return;
        if (self->_connected)
            self->SendStatus();
        self->StartHeartbeatTimer();
    });
}

void ProxyLink::Send(std::vector<uint8> data)
{
    if (_cryptReady)
        _crypt.Encrypt(data.data(), data.size());

    std::lock_guard<std::mutex> lock(_sendMutex);
    _sendQueue.push(std::move(data));
    if (!_writing)
        DoWrite();
}

void ProxyLink::DoWrite()
{
    if (_sendQueue.empty())
    {
        _writing = false;
        return;
    }

    _writing = true;
    auto const& front = _sendQueue.front();
    boost::asio::async_write(_socket, boost::asio::buffer(front),
        [self = shared_from_this()](boost::system::error_code const& ec, std::size_t)
        {
            std::lock_guard<std::mutex> lock(self->_sendMutex);
            self->_sendQueue.pop();
            if (ec)
            {
                self->_writing = false;
                self->OnDisconnect(ec.message());
                return;
            }
            self->DoWrite();
        });
}

void ProxyLink::OnDisconnect(std::string const& reason)
{
    LOG_WARN("nodemgr", "ProxyLink: Disconnected from proxy: {} — reconnecting in 10s", reason);
    Reconnect();
}
