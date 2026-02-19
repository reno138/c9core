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

#ifndef ManagementSocket_h__
#define ManagementSocket_h__

#include "MessageBuffer.h"
#include "PskCrypt.h"
#include "Socket.h"
#include <string>
#include <vector>

/**
 * @brief Server-side socket for connections from the clustermgr TUI (port 9090).
 *
 * Protocol (after PSK handshake):
 *
 *   clustermgr → proxy:
 *     MSG_MGMT_SUBSCRIBE (0x20):  (no payload — subscribe to status push)
 *     MSG_MGMT_NODE_CMD  (0x22):  uint8 cmd + uint8 node_id
 *                                 cmd: 1=start  2=stop
 *
 *   proxy → clustermgr:
 *     MSG_MGMT_STATUS_PUSH (0x21):
 *       uint8  node_count
 *       per node:
 *         uint8  node_id
 *         uint8  state      (0=unknown 1=stopped 2=starting 3=running 4=stopping 5=crashed)
 *         uint16 player_count  (LE)
 *         uint16 max_players   (LE)
 *         uint32 pid           (LE)
 *         uint32 uptime_secs   (LE)
 *         uint8  addr_len
 *         char   addr[addr_len]
 *         uint16 port          (LE)
 *
 * Handshake: same PSK nonce exchange as NodeMgrSocket.
 */
class ManagementSocket final : public Socket<ManagementSocket>
{
    typedef Socket<ManagementSocket> BaseSocket;

public:
    explicit ManagementSocket(IoContextTcpSocket&& socket);

    void Start() override;
    void OnClose() override;

    /// Push a pre-built status snapshot to this subscriber (encrypted).
    void SendStatusPush(std::vector<uint8> const& payload);

    bool IsSubscribed() const { return _subscribed; }

protected:
    SocketReadCallbackResult ReadHandler() final;

private:
    void ProcessBuffer();

    // ── Message handlers ──────────────────────────────────────────────────────
    void HandleSubscribe();
    void HandleNodeCmd(uint8 cmd, uint8 nodeId);

    // ── Protocol constants ─────────────────────────────────────────────────────
    static constexpr uint8 MSG_MGMT_SUBSCRIBE   = 0x20;
    static constexpr uint8 MSG_MGMT_STATUS_PUSH = 0x21;
    static constexpr uint8 MSG_MGMT_NODE_CMD    = 0x22;

    static constexpr std::size_t NONCE_SIZE   = 16;
    static constexpr std::size_t NODE_CMD_SIZE = 2;  ///< uint8 cmd + uint8 node_id

    // ── Parse state machine ───────────────────────────────────────────────────
    enum class ParseState
    {
        WaitType,
        ReadNodeCmd,  ///< 2 bytes
    };
    ParseState _parseState{ ParseState::WaitType };

    MessageBuffer _accumBuffer;

    // ── PSK encryption ─────────────────────────────────────────────────────────
    PskCrypt _crypt;
    bool _cryptReady{ false };
    bool _subscribed{ false };
};

#endif // ManagementSocket_h__
