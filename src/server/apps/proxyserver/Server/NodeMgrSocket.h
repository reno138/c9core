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

#ifndef NodeMgrSocket_h__
#define NodeMgrSocket_h__

#include "MessageBuffer.h"
#include "PskCrypt.h"
#include "Socket.h"
#include <string>
#include <vector>

/**
 * @brief Server-side socket for connections from nodemgr daemons (port 8091).
 *
 * Protocol (after PSK handshake):
 *
 *   nodemgr → proxy:
 *     MSG_NODEMGR_REGISTER (0x11):  uint8 configured_node_id + uint16 game_port
 *     MSG_NODE_STATUS      (0x14):  uint8 state + uint32 pid + uint32 uptime_secs
 *
 *   proxy → nodemgr:
 *     MSG_NODE_START (0x12):  (no payload — start the worldserver)
 *     MSG_NODE_STOP  (0x13):  (no payload — gracefully stop the worldserver)
 *
 * NodeState values (in MSG_NODE_STATUS):
 *   0 = UNKNOWN   1 = STOPPED   2 = STARTING
 *   3 = RUNNING   4 = STOPPING  5 = CRASHED
 *
 * Handshake:
 *   1. Proxy sends 16-byte random nonce (unencrypted).
 *   2. Both sides derive session key = HMAC-SHA256(PSK, nonce) and init AES-256-CTR.
 *   3. All subsequent traffic is encrypted.
 */
class NodeMgrSocket final : public Socket<NodeMgrSocket>
{
    typedef Socket<NodeMgrSocket> BaseSocket;

public:
    explicit NodeMgrSocket(IoContextTcpSocket&& socket);

    void Start() override;
    void OnClose() override;

    uint8 GetConfiguredNodeId() const { return _configuredNodeId; }
    uint16 GetGamePort()        const { return _gamePort; }

    /// Send "start worldserver" command (encrypted).
    void SendNodeStart();
    /// Send "stop worldserver" command (encrypted).
    void SendNodeStop();

    /// Send raw (already-formed, unencrypted) bytes — will be encrypted before queuing.
    void SendEncrypted(std::vector<uint8> const& data);

protected:
    SocketReadCallbackResult ReadHandler() final;

private:
    void ProcessBuffer();

    // ── Message handlers ──────────────────────────────────────────────────────
    void HandleRegister(uint8 nodeId, uint16 gamePort);
    void HandleStatus(uint8 state, uint32 pid, uint32 uptime);

    // ── Protocol constants ─────────────────────────────────────────────────────
    static constexpr uint8 MSG_NODEMGR_REGISTER = 0x11;
    static constexpr uint8 MSG_NODE_START        = 0x12;
    static constexpr uint8 MSG_NODE_STOP         = 0x13;
    static constexpr uint8 MSG_NODE_STATUS       = 0x14;

    static constexpr std::size_t NONCE_SIZE          = 16;
    static constexpr std::size_t REGISTER_PAYLOAD    = 3;  ///< uint8 nodeId + uint16 gamePort
    static constexpr std::size_t STATUS_PAYLOAD      = 9;  ///< uint8 state + uint32 pid + uint32 uptime

    // ── Parse state machine ───────────────────────────────────────────────────
    enum class ParseState
    {
        WaitType,
        ReadRegister,  ///< 3 bytes
        ReadStatus,    ///< 9 bytes
    };
    ParseState _parseState{ ParseState::WaitType };

    MessageBuffer _accumBuffer;

    // ── PSK encryption ─────────────────────────────────────────────────────────
    PskCrypt _crypt;
    bool _cryptReady{ false };

    // ── Node info (set on MSG_NODEMGR_REGISTER) ────────────────────────────────
    uint8  _configuredNodeId{ 0 };
    uint16 _gamePort{ 0 };
    uint8  _assignedNodeId{ 0 };  ///< ProxyMgr-assigned ID for this nodemgr
};

#endif // NodeMgrSocket_h__
