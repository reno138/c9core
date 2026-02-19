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

#include "ManagementClient.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

ManagementClient::ManagementClient(boost::asio::io_context& ioCtx)
    : _ioCtx(ioCtx)
    , _socket(ioCtx)
    , _resolver(ioCtx)
    , _reconnectTimer(ioCtx)
    , _recvBuf(RECV_BUF_SIZE)
{
}

void ManagementClient::Start(std::string host, uint16 port, std::string sharedSecret,
                             StatusCallback statusCb, ConnectCallback connectCb)
{
    _proxyHost    = std::move(host);
    _proxyPort    = port;
    _sharedSecret = std::move(sharedSecret);
    _statusCb     = std::move(statusCb);
    _connectCb    = std::move(connectCb);

    Connect();
}

void ManagementClient::Connect()
{
    LOG_INFO("clustermgr", "ManagementClient: Connecting to proxy {}:{}", _proxyHost, _proxyPort);

    _resolver.async_resolve(_proxyHost, std::to_string(_proxyPort),
        [self = shared_from_this()](boost::system::error_code const& ec, tcp::resolver::results_type endpoints)
        {
            if (ec)
            {
                LOG_ERROR("clustermgr", "ManagementClient: Resolve failed: {}", ec.message());
                self->Reconnect();
                return;
            }
            boost::asio::async_connect(self->_socket, endpoints,
                [self](boost::system::error_code const& ec2, tcp::endpoint const&)
                {
                    if (ec2)
                    {
                        LOG_ERROR("clustermgr", "ManagementClient: Connect failed: {}", ec2.message());
                        self->Reconnect();
                        return;
                    }
                    LOG_INFO("clustermgr", "ManagementClient: Connected to proxy");
                    self->ReadNonce();
                });
        });
}

void ManagementClient::Reconnect()
{
    bool wasConnected = _connected;
    _connected   = false;
    _cryptReady  = false;
    _parseState  = ParseState::WaitMsgType;
    _parseAccum.clear();
    _parsedNodes.clear();

    boost::system::error_code ec;
    _socket.close(ec);

    if (wasConnected && _connectCb)
        _connectCb(false);

    _reconnectTimer.expires_after(std::chrono::seconds(10));
    _reconnectTimer.async_wait([self = shared_from_this()](boost::system::error_code const& ec)
    {
        if (!ec)
        {
            self->_socket = tcp::socket(self->_ioCtx);
            self->Connect();
        }
    });
}

void ManagementClient::ReadNonce()
{
    boost::asio::async_read(_socket, boost::asio::buffer(_nonce, NONCE_SIZE),
        [self = shared_from_this()](boost::system::error_code const& ec, std::size_t)
        {
            if (ec)
            {
                LOG_ERROR("clustermgr", "ManagementClient: ReadNonce failed: {}", ec.message());
                self->Reconnect();
                return;
            }
            self->OnNonceReceived();
        });
}

void ManagementClient::OnNonceReceived()
{
    // isServer=false → we are the client
    if (!_crypt.Init(_sharedSecret, _nonce, false))
    {
        LOG_ERROR("clustermgr", "ManagementClient: PSK crypto init failed");
        Reconnect();
        return;
    }
    _cryptReady = true;
    LOG_DEBUG("clustermgr", "ManagementClient: PSK handshake complete");
    Subscribe();
}

void ManagementClient::Subscribe()
{
    // Send MSG_MGMT_SUBSCRIBE (0x20): 1-byte message, no payload
    std::vector<uint8> msg(1);
    msg[0] = MSG_MGMT_SUBSCRIBE;
    Send(std::move(msg));

    _connected = true;
    if (_connectCb)
        _connectCb(true);

    LOG_INFO("clustermgr", "ManagementClient: Subscribed to status push");
    AsyncRead();
}

void ManagementClient::AsyncRead()
{
    _socket.async_read_some(boost::asio::buffer(_recvBuf),
        [self = shared_from_this()](boost::system::error_code const& ec, std::size_t bytes)
        {
            self->OnRead(ec, bytes);
        });
}

void ManagementClient::OnRead(boost::system::error_code const& ec, std::size_t bytes)
{
    if (ec)
    {
        OnDisconnect(ec.message());
        return;
    }

    if (_cryptReady && bytes > 0)
        _crypt.Decrypt(_recvBuf.data(), bytes);

    _parseAccum.insert(_parseAccum.end(), _recvBuf.begin(), _recvBuf.begin() + bytes);
    TryParseNodes();
    AsyncRead();
}

void ManagementClient::TryParseNodes()
{
    while (true)
    {
        switch (_parseState)
        {
            case ParseState::WaitMsgType:
            {
                if (_parseAccum.empty())
                    return;
                uint8 msgType = _parseAccum[0];
                _parseAccum.erase(_parseAccum.begin());
                if (msgType != MSG_MGMT_STATUS_PUSH)
                {
                    LOG_WARN("clustermgr", "ManagementClient: Unexpected msg 0x{:02X} — ignoring", msgType);
                    return;
                }
                _parseState = ParseState::WaitNodeCount;
                break;
            }

            case ParseState::WaitNodeCount:
            {
                if (_parseAccum.empty())
                    return;
                _pendingNodes = _parseAccum[0];
                _parseAccum.erase(_parseAccum.begin());
                _parsedNodes.clear();
                if (_pendingNodes == 0)
                {
                    // Empty snapshot — fire callback immediately
                    if (_statusCb)
                        _statusCb(_parsedNodes);
                    _parseState = ParseState::WaitMsgType;
                }
                else
                {
                    _parseState = ParseState::WaitNodeFixed;
                }
                break;
            }

            case ParseState::WaitNodeFixed:
            {
                // Fixed part: nodeId(1)+state(1)+players(2)+max(2)+pid(4)+uptime(4)+txBps(4)+rxBps(4)+addrLen(1) = 23 bytes
                if (_parseAccum.size() < NODE_FIXED_SIZE)
                    return;

                uint8 const* p = _parseAccum.data();
                _currentNode           = NodeInfo{};
                _currentNode.nodeId    = p[0];
                _currentNode.state     = p[1];
                _currentNode.playerCount = static_cast<uint16>(p[2]) | (static_cast<uint16>(p[3]) << 8);
                _currentNode.maxPlayers  = static_cast<uint16>(p[4]) | (static_cast<uint16>(p[5]) << 8);
                _currentNode.pid         = static_cast<uint32>(p[6])
                                         | (static_cast<uint32>(p[7])  << 8)
                                         | (static_cast<uint32>(p[8])  << 16)
                                         | (static_cast<uint32>(p[9])  << 24);
                _currentNode.uptimeSecs  = static_cast<uint32>(p[10])
                                         | (static_cast<uint32>(p[11]) << 8)
                                         | (static_cast<uint32>(p[12]) << 16)
                                         | (static_cast<uint32>(p[13]) << 24);
                _currentNode.txBps       = static_cast<uint32>(p[14])
                                         | (static_cast<uint32>(p[15]) << 8)
                                         | (static_cast<uint32>(p[16]) << 16)
                                         | (static_cast<uint32>(p[17]) << 24);
                _currentNode.rxBps       = static_cast<uint32>(p[18])
                                         | (static_cast<uint32>(p[19]) << 8)
                                         | (static_cast<uint32>(p[20]) << 16)
                                         | (static_cast<uint32>(p[21]) << 24);
                _pendingAddrLen = p[22];
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + NODE_FIXED_SIZE);

                if (_pendingAddrLen > 0)
                    _parseState = ParseState::WaitNodeAddr;
                else
                    _parseState = ParseState::WaitNodePort;
                break;
            }

            case ParseState::WaitNodeAddr:
            {
                if (_parseAccum.size() < _pendingAddrLen)
                    return;
                _currentNode.address.assign(
                    reinterpret_cast<char const*>(_parseAccum.data()), _pendingAddrLen);
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + _pendingAddrLen);
                _parseState = ParseState::WaitNodePort;
                break;
            }

            case ParseState::WaitNodePort:
            {
                if (_parseAccum.size() < 2)
                    return;
                _currentNode.port = static_cast<uint16>(_parseAccum[0]) | (static_cast<uint16>(_parseAccum[1]) << 8);
                _parseAccum.erase(_parseAccum.begin(), _parseAccum.begin() + 2);

                _parsedNodes.push_back(_currentNode);
                --_pendingNodes;

                if (_pendingNodes > 0)
                {
                    _parseState = ParseState::WaitNodeFixed;
                }
                else
                {
                    // Full snapshot received — fire callback
                    if (_statusCb)
                        _statusCb(_parsedNodes);
                    _parsedNodes.clear();
                    _parseState = ParseState::WaitMsgType;
                }
                break;
            }
        }
    }
}

void ManagementClient::SendStartNode(uint8 nodeId)
{
    boost::asio::post(_ioCtx, [self = shared_from_this(), nodeId]()
    {
        self->SendNodeCmd(1, nodeId);
    });
}

void ManagementClient::SendStopNode(uint8 nodeId)
{
    boost::asio::post(_ioCtx, [self = shared_from_this(), nodeId]()
    {
        self->SendNodeCmd(2, nodeId);
    });
}

void ManagementClient::SendNodeCmd(uint8 cmd, uint8 nodeId)
{
    // MSG_MGMT_NODE_CMD (0x22): type(1) + cmd(1) + nodeId(1)
    std::vector<uint8> msg(3);
    msg[0] = MSG_MGMT_NODE_CMD;
    msg[1] = cmd;
    msg[2] = nodeId;
    Send(std::move(msg));
}

void ManagementClient::Send(std::vector<uint8> data)
{
    if (_cryptReady)
        _crypt.Encrypt(data.data(), data.size());

    std::lock_guard<std::mutex> lock(_sendMutex);
    _sendQueue.push(std::move(data));
    if (!_writing)
        DoWrite();
}

void ManagementClient::DoWrite()
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

void ManagementClient::OnDisconnect(std::string const& reason)
{
    LOG_WARN("clustermgr", "ManagementClient: Disconnected: {} — reconnecting in 10s", reason);
    Reconnect();
}
