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

#ifndef BackendSession_h__
#define BackendSession_h__

#include "AuthCrypt.h"
#include "AuthDefines.h"
#include "MessageBuffer.h"
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <queue>
#include <string>
#include <vector>

class ProxySocket;

/**
 * @brief Outgoing connection from the proxy to a backend worldserver or instance server.
 *
 * The proxy acts as a WoW client from the backend server's perspective.
 *
 * RC4 direction note (intentional inversion):
 *   AuthCrypt is designed from the server side. We reuse it here with inverted semantics:
 *     - worldserver encrypts S→C via _serverEncrypt; proxy decrypts by calling EncryptSend()
 *       (same ARC4 stream — XOR is symmetric).
 *     - worldserver decrypts C→S via _clientDecrypt; proxy encrypts by calling DecryptRecv()
 *       (same ARC4 stream).
 *   Both sides initialize with the same SessionKey, so the streams stay in sync.
 *
 * Reroute mode:
 *   When created for a cross-server player transfer, the constructor receives account info.
 *   The session intercepts SMSG_AUTH_CHALLENGE and replies with CMSG_AUTH_SESSION on behalf
 *   of the player, then intercepts SMSG_AUTH_RESPONSE and notifies ProxySocket::OnRerouteComplete.
 *   Only after that does it switch to normal relay mode.
 */
class BackendSession : public std::enable_shared_from_this<BackendSession>
{
public:
    /// Normal constructor: proxy transparently relays the auth handshake to/from the client.
    BackendSession(Acore::Asio::IoContext& ioContext, std::weak_ptr<ProxySocket> owner);

    /// Reroute constructor: proxy handles the auth handshake autonomously, then relays.
    BackendSession(Acore::Asio::IoContext& ioContext, std::weak_ptr<ProxySocket> owner,
                   std::string accountName, SessionKey const& sessionKey,
                   uint32 realmId, uint64 playerGuid);

    ~BackendSession();

    void Connect(std::string const& host, uint16 port);

    /// Send pre-built bytes (caller has already applied backend-direction re-encryption).
    void SendRaw(std::vector<uint8> const& data);

    /// Initialize RC4 crypto once session key is known.
    void InitCrypt(SessionKey const& key);
    bool IsCryptInitialized() const { return _cryptInitialized; }

    /// Access the backend AuthCrypt for re-encryption of C→S packets.
    /// Caller uses DecryptRecv() to encrypt toward backend (inverted semantics).
    AuthCrypt& GetCrypt() { return _backendCrypt; }

    void Close();
    bool IsOpen() const;

private:
    void AsyncRead();
    void AsyncWrite();
    void OnConnect(boost::system::error_code const& error);
    void OnRead(boost::system::error_code const& error, std::size_t transferred);

    void ProcessReadBuffer();
    bool TryConsumeFirstByte();
    bool TryConsumeRestOfHeader();
    bool TryConsumePayload();
    void DispatchToClient();

    /// Reroute handshake helpers (only called when _isReroute == true).
    void HandleAuthChallenge(); ///< Intercept SMSG_AUTH_CHALLENGE, reply with CMSG_AUTH_SESSION.
    void HandleAuthResponse();  ///< Intercept SMSG_AUTH_RESPONSE, init crypto, notify ProxySocket.
    void SendPlayerLogin();     ///< Synthesize and send CMSG_PLAYER_LOGIN to instance server.

    std::weak_ptr<ProxySocket> _owner;
    boost::asio::ip::tcp::socket _socket;
    boost::asio::ip::tcp::resolver _resolver;

    /// Inverted AuthCrypt: EncryptSend() decrypts S→C, DecryptRecv() encrypts C→S.
    AuthCrypt _backendCrypt;
    bool _cryptInitialized{ false };

    /// Reroute mode: proxy handles auth handshake autonomously.
    bool _isReroute{ false };
    enum class HandshakeState { WaitChallenge, WaitResponse, Done };
    HandshakeState _handshakeState{ HandshakeState::Done };

    /// Reroute data (valid when _isReroute == true).
    std::string _accountName;
    SessionKey  _sessionKey;
    uint32      _realmId{ 0 };
    uint64      _playerGuid{ 0 };

    /// Opcode constants used for reroute handshake interception.
    static constexpr uint16 SMSG_AUTH_CHALLENGE_OPCODE = 0x1EC;
    static constexpr uint16 SMSG_AUTH_RESPONSE_OPCODE  = 0x1EE;
    static constexpr uint32 WOTLK_CLIENT_BUILD         = 12340;
    static constexpr uint8  AUTH_OK_CODE               = 0x0C;

    static constexpr std::size_t READ_SIZE = 4096;
    MessageBuffer _readBuffer;

    /// Async write queue (one vector<uint8> per packet).
    std::queue<std::vector<uint8>> _sendQueue;

    /// SMSG header state machine.
    enum class ReadState { FirstByte, RestOfHeader, Payload };
    ReadState _readState{ ReadState::FirstByte };

    uint8 _encHeader[5]{};   ///< Raw encrypted header bytes from backend.
    uint8 _plainHeader[5]{}; ///< Decrypted plaintext header bytes (passed to ProxySocket).
    uint8 _headerLen{ 0 };   ///< Total header length (4 or 5), determined from first byte.
    uint8 _headerRead{ 0 };  ///< Header bytes accumulated so far.
    uint16 _opcode{ 0 };     ///< Parsed opcode (informational; not needed for forwarding).
    uint32 _payloadSize{ 0 };
    MessageBuffer _payloadBuffer;
};

#endif // BackendSession_h__
