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

#ifndef ManagementClient_h__
#define ManagementClient_h__

#include "Define.h"
#include "PskCrypt.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

using tcp = boost::asio::ip::tcp;

/// Snapshot of one node's status, as received from the proxy.
struct NodeInfo
{
    uint8       nodeId      { 0 };
    uint8       state       { 0 };   ///< NodeState: 0=unknown 1=stopped 2=starting 3=running 4=stopping 5=crashed
    uint16      playerCount { 0 };
    uint16      maxPlayers  { 0 };
    uint32      pid         { 0 };
    uint32      uptimeSecs  { 0 };
    uint32      txBps       { 0 };   ///< Bytes/sec client→worldserver
    uint32      rxBps       { 0 };   ///< Bytes/sec worldserver→client
    std::string address;
    uint16      port        { 0 };
};

/**
 * @brief Async TCP client that connects to the proxy's management port (default 9090).
 *
 * Connection flow:
 *   1. Resolve proxy address → connect.
 *   2. Read 16-byte nonce (unencrypted from proxy).
 *   3. Init PSK crypto.
 *   4. Send MSG_MGMT_SUBSCRIBE (encrypted).
 *   5. Receive MSG_MGMT_STATUS_PUSH updates; call StatusCallback on each.
 *   6. On disconnect, retry after 10 seconds.
 *
 * Thread safety:
 *   SendStartNode / SendStopNode may be called from any thread; they post
 *   the write onto the io_context strand.
 */
class ManagementClient : public std::enable_shared_from_this<ManagementClient>
{
public:
    /// Called from the io_context thread when a status snapshot arrives.
    using StatusCallback = std::function<void(std::vector<NodeInfo>)>;
    /// Called from the io_context thread on connect / disconnect.
    using ConnectCallback = std::function<void(bool connected)>;

    explicit ManagementClient(boost::asio::io_context& ioCtx);

    /// Begin async connection; call once from main thread before ioCtx.run().
    void Start(std::string host, uint16 port, std::string sharedSecret,
               StatusCallback statusCb, ConnectCallback connectCb = nullptr);

    bool IsConnected() const { return _connected; }

    /// Thread-safe: send a NODE_START command.
    void SendStartNode(uint8 nodeId);

    /// Thread-safe: send a NODE_STOP command.
    void SendStopNode(uint8 nodeId);

private:
    // ── Connection flow ────────────────────────────────────────────────────────
    void Connect();
    void Reconnect();
    void ReadNonce();
    void OnNonceReceived();
    void Subscribe();
    void AsyncRead();
    void OnRead(boost::system::error_code const& ec, std::size_t bytes);
    void ProcessIncoming();
    void OnDisconnect(std::string const& reason);

    // ── Parsing state machine ─────────────────────────────────────────────────
    enum class ParseState
    {
        WaitMsgType,    ///< Waiting for the 0x21 byte
        WaitNodeCount,  ///< 1 byte: how many nodes follow
        WaitNodeFixed,  ///< 15 bytes: fixed-size node fields
        WaitNodeAddr,   ///< addrLen bytes: address string
        WaitNodePort,   ///< 2 bytes: port (LE)
    };

    void TryParseNodes();   ///< Drive the state machine until blocked

    // ── Send helpers ──────────────────────────────────────────────────────────
    void SendNodeCmd(uint8 cmd, uint8 nodeId);  ///< runs on io_context thread
    void Send(std::vector<uint8> data);
    void DoWrite();

    // ── Protocol constants ─────────────────────────────────────────────────────
    static constexpr uint8 MSG_MGMT_SUBSCRIBE   = 0x20;
    static constexpr uint8 MSG_MGMT_STATUS_PUSH = 0x21;
    static constexpr uint8 MSG_MGMT_NODE_CMD    = 0x22;

    static constexpr std::size_t NONCE_SIZE     = 16;
    static constexpr std::size_t NODE_FIXED_SIZE = 23; ///< nodeId+state+players(2)+max(2)+pid(4)+uptime(4)+txBps(4)+rxBps(4)+addrLen(1)
    static constexpr std::size_t RECV_BUF_SIZE  = 4096;

    boost::asio::io_context& _ioCtx;
    tcp::socket              _socket;
    tcp::resolver            _resolver;
    boost::asio::steady_timer _reconnectTimer;

    std::string _proxyHost;
    uint16      _proxyPort      { 0 };
    std::string _sharedSecret;
    StatusCallback  _statusCb;
    ConnectCallback _connectCb;

    bool  _connected { false };

    // Receive / parse
    std::vector<uint8> _recvBuf;
    std::vector<uint8> _parseAccum;
    ParseState         _parseState    { ParseState::WaitMsgType };
    uint8              _pendingNodes  { 0 };
    uint8              _pendingAddrLen{ 0 };
    NodeInfo           _currentNode;
    std::vector<NodeInfo> _parsedNodes;

    // PSK encryption
    PskCrypt _crypt;
    bool     _cryptReady   { false };
    uint8    _nonce[NONCE_SIZE]{};

    // Send queue
    std::mutex                    _sendMutex;
    std::queue<std::vector<uint8>> _sendQueue;
    bool                          _writing { false };
};

#endif // ManagementClient_h__
