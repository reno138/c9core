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

#ifndef ProxySocket_h__
#define ProxySocket_h__

#include "AsyncCallbackProcessor.h"
#include "AuthCrypt.h"
#include "AuthDefines.h"
#include "MessageBuffer.h"
#include "QueryCallback.h"
#include "Socket.h"
#include <memory>
#include <queue>
#include <string>
#include <vector>

class BackendSession;

/**
 * @brief Client-facing socket — accepts WoW client connections on the proxy's public port.
 *
 * Lifecycle with ProxyMgr:
 *   - CMSG_PLAYER_LOGIN intercepted → RegisterSession(guid, this)
 *   - Destructor → UnregisterSession(guid)
 *
 * Each ProxySocket owns one BackendSession (the current upstream worldserver or instance server).
 * The proxy holds the RC4 AuthCrypt state for the full session lifetime, surviving any backend switch.
 *
 * RC4 direction (normal, server-side perspective):
 *   - DecryptRecv(): decrypts packets coming FROM the client (C→S direction).
 *   - EncryptSend(): encrypts packets going TO the client (S→C direction).
 */
class ProxySocket final : public Socket<ProxySocket>
{
    typedef Socket<ProxySocket> BaseSocket;

public:
    explicit ProxySocket(IoContextTcpSocket&& socket);
    ~ProxySocket();

    void Start() override;
    bool Update() override;

    /// Called by BackendSession: forward a plaintext-header packet to the client.
    /// This method re-encrypts the header for the client direction before sending.
    void QueuePacketForClient(uint8 const* plainHeader, std::size_t headerLen, MessageBuffer& payload);

    /// Called by BackendSession once the backend TCP connection is established.
    /// Client reads are deferred until this point to prevent a race where the client
    /// sends CMSG_AUTH_SESSION before the backend is ready to receive it.
    void OnBackendConnected();

    /// Called by BackendSession when reroute handshake with instance server completes.
    void OnRerouteComplete(std::shared_ptr<BackendSession> newBackend);

    /// Initiate a backend switch to the given address:port (called by ProxyMgr).
    /// mapId/x/y/z/ori: when non-zero, send SMSG_NEW_WORLD to client (native in-world reroute).
    void RerouteToBackend(std::string const& address, uint16 port,
                          uint32 mapId = 0, float x = 0.f, float y = 0.f, float z = 0.f, float ori = 0.f);

protected:
    SocketReadCallbackResult ReadHandler() final;

private:
    bool ReadHeaderHandler();
    bool ReadDataHandler();

    void HandleAuthSessionIntercepted();
    void HandleAuthSessionCallback(PreparedQueryResult result);
    void ResumeAfterAuth();

    /// Opcode constants (avoid game library dependency).
    static constexpr uint32 CMSG_AUTH_SESSION_OPCODE    = 0x1ED;
    static constexpr uint32 CMSG_PLAYER_LOGIN_OPCODE    = 0x03D;
    static constexpr uint32 MSG_MOVE_WORLDPORT_ACK_OPCODE = 0x0DC;

    /// Client-side AuthCrypt (proxy acts as server to client).
    AuthCrypt _clientCrypt;

    /// The current backend connection (worldserver or instance server).
    std::shared_ptr<BackendSession> _backend;

    /// Packet framing state (mirrors WorldSocket pattern).
    MessageBuffer _headerBuffer; ///< Accumulates 6-byte CMSG header.
    MessageBuffer _packetBuffer; ///< Accumulates payload of current packet.

    /// Player GUID learned from CMSG_PLAYER_LOGIN — used to register with ProxyMgr.
    uint64 _playerGuid{ 0 };

    /// Node ID of the backend worldserver this socket is connected to (for bandwidth tracking).
    uint8 _backendNodeId{ 0 };

    /// Session key and realm ID stored for reroute auth handshake.
    SessionKey _sessionKey;
    uint32     _realmId{ 0 };

    /// Pending backend during a reroute (kept alive until handshake completes).
    std::shared_ptr<BackendSession> _pendingBackend;

    /// When true, ReadHandler() pauses client reads (reroute in progress or DB query).
    bool _rerouting{ false };

    /// True while an async_read_some is pending on the client socket.
    /// Prevents OnRerouteComplete() from posting a second overlapping read, which would
    /// corrupt _readBuffer when two concurrent async_read_some share the same write pointer.
    bool _asyncReadActive{ false };

    /// Set after native cross-node reroute (proxy sent SMSG_NEW_WORLD to client).
    /// The next MSG_MOVE_WORLDPORT_ACK from the client is translated to CMSG_PLAYER_LOGIN
    /// and forwarded to the new backend — matching VB.NET On_MSG_MOVE_WORLDPORT_ACK.
    bool _translateWorldportAck{ false };

    /// Set to true the first time SMSG_LOGIN_VERIFY_WORLD is forwarded to the client.
    /// Used to distinguish GAP-1 login reroutes (client not yet in world) from in-world
    /// cross-node teleport reroutes (client received SMSG_TRANSFER_PENDING).
    bool _clientInWorld{ false };

    /// Whether we are waiting for the DB auth query to complete.
    bool _waitingForQuery{ false };

    /// Async DB query processor (callbacks fired in Update()).
    QueryCallbackProcessor _queryProcessor;

    /// Account name extracted from CMSG_AUTH_SESSION (needed for DB query).
    std::string _accountName;
    std::vector<uint8> _pendingAuthSession; ///< Saved CMSG_AUTH_SESSION bytes — sent after session key is known.

    /// Account ID from DB query, used for pending worldport keying
    uint32 _accountId{ 0 };

    /// Pending worldport destination (set when recovering from cross-node reconnect)
    std::string _wpDestAddr;
    uint16      _wpDestPort{ 0 };
    uint32      _wpMapId{ 0 };
    float       _wpX{ 0.f }, _wpY{ 0.f }, _wpZ{ 0.f }, _wpOri{ 0.f };
    bool        _hasWpDest{ false };

public:
    /// Expose player GUID for opcode logging in BackendSession.
    uint64 GetPlayerGuid() const { return _playerGuid; }

    /// Called after native reroute (SMSG_NEW_WORLD sent to client) to arm the
    /// MSG_MOVE_WORLDPORT_ACK → CMSG_PLAYER_LOGIN translation in ReadDataHandler.
    void SetTranslateWorldportAck(bool translate) { _translateWorldportAck = translate; }

    /// Called by BackendSession when SMSG_LOGIN_VERIFY_WORLD is forwarded to the client
    /// (either directly or as a rewrite).  Marks this session as "client has entered world".
    void SetClientInWorld() { _clientInWorld = true; }

    /// True if the client has already entered the world (received SMSG_LOGIN_VERIFY_WORLD).
    bool IsClientInWorld() const { return _clientInWorld; }
};

#endif // ProxySocket_h__
