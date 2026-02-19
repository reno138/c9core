/*
 * This file is part of the AzerothCore Project. See AUTHORS file for Copyright information
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

#ifndef ProxyLink_h__
#define ProxyLink_h__

#include "Define.h"
#include "NodeMgr.h"
#include "PskCrypt.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

using tcp = boost::asio::ip::tcp;

/**
 * @brief Async TCP client that connects to the proxy's nodemgr port (default 8091).
 *
 * Connection flow:
 *   1. Resolve proxy address → connect.
 *   2. Read 16-byte nonce (unencrypted).
 *   3. Init PSK crypto from nonce + shared secret.
 *   4. Send MSG_NODEMGR_REGISTER (encrypted).
 *   5. Start status heartbeat timer (sends MSG_NODE_STATUS every 5s).
 *   6. Process incoming MSG_NODE_START / MSG_NODE_STOP commands.
 *   7. On disconnect, retry after 10 seconds.
 */
class ProxyLink : public std::enable_shared_from_this<ProxyLink>
{
public:
    ProxyLink(boost::asio::io_context& ioCtx, NodeMgr& nodeMgr);

    /// Begin async connection; call once from main thread.
    void Start(std::string proxyHost, uint16 proxyPort, std::string sharedSecret,
               uint8 configuredNodeId, uint16 gamePort);

    bool IsConnected() const { return _connected; }
    uint8 GetAssignedNodeId() const { return _assignedNodeId; }

private:
    void Connect();
    void Reconnect();
    void ReadNonce();
    void OnNonceReceived();
    void SendRegister();
    void AsyncRead();
    void OnRead(boost::system::error_code const& ec, std::size_t bytes);
    void ProcessIncoming();
    void Send(std::vector<uint8> data);
    void DoWrite();
    void StartHeartbeatTimer();
    void SendStatus();
    void OnDisconnect(std::string const& reason);

    // ── Protocol constants ─────────────────────────────────────────────────────
    static constexpr uint8 MSG_NODEMGR_REGISTER = 0x11;
    static constexpr uint8 MSG_NODE_START        = 0x12;
    static constexpr uint8 MSG_NODE_STOP         = 0x13;
    static constexpr uint8 MSG_NODE_STATUS       = 0x14;

    boost::asio::io_context&              _ioCtx;
    NodeMgr&                              _nodeMgr;
    tcp::socket                           _socket;
    tcp::resolver                         _resolver;
    boost::asio::steady_timer             _reconnectTimer;
    boost::asio::steady_timer             _heartbeatTimer;

    std::string _proxyHost;
    uint16      _proxyPort{ 0 };
    std::string _sharedSecret;
    uint8       _configuredNodeId{ 0 };
    uint16      _gamePort{ 0 };

    bool  _connected{ false };
    uint8 _assignedNodeId{ 0 };

    // Receive buffer
    std::vector<uint8> _recvBuf;
    std::vector<uint8> _parseAccum;

    // PSK encryption
    PskCrypt _crypt;
    bool     _cryptReady{ false };
    uint8    _nonce[16]{};
    bool     _waitingNonce{ true };

    // Send queue
    std::mutex               _sendMutex;
    std::queue<std::vector<uint8>> _sendQueue;
    bool                     _writing{ false };

    static constexpr std::size_t RECV_BUF_SIZE = 4096;
};

#endif // ProxyLink_h__
