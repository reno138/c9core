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

#include "BackendSession.h"
#include "Config.h"
#include "IoContext.h"
#include "ProxySocket.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>

BackendSession::BackendSession(Acore::Asio::IoContext& ioContext, std::weak_ptr<ProxySocket> owner)
    : _owner(std::move(owner))
    , _socket(static_cast<boost::asio::io_context&>(ioContext))
    , _resolver(static_cast<boost::asio::io_context&>(ioContext))
    , _connectTimer(static_cast<boost::asio::io_context&>(ioContext))
    , _readBuffer(READ_SIZE)
    , _payloadBuffer(0)
{
}

BackendSession::BackendSession(Acore::Asio::IoContext& ioContext, std::weak_ptr<ProxySocket> owner,
                               std::string accountName, SessionKey const& sessionKey,
                               uint32 realmId, uint64 playerGuid, std::string clientIp,
                               bool isLoginReroute)
    : _owner(std::move(owner))
    , _socket(static_cast<boost::asio::io_context&>(ioContext))
    , _resolver(static_cast<boost::asio::io_context&>(ioContext))
    , _connectTimer(static_cast<boost::asio::io_context&>(ioContext))
    , _readBuffer(READ_SIZE)
    , _payloadBuffer(0)
    , _isReroute(true)
    , _handshakeState(HandshakeState::WaitChallenge)
    , _isLoginReroute(isLoginReroute)
    , _accountName(std::move(accountName))
    , _sessionKey(sessionKey)
    , _realmId(realmId)
    , _playerGuid(playerGuid)
    , _clientIp(std::move(clientIp))
{
}

BackendSession::~BackendSession()
{
    Close();
}

void BackendSession::Connect(std::string const& host, uint16 port)
{
    LOG_INFO("proxy", "BackendSession: Connecting to {}:{} (isReroute={})", host, port, _isReroute);
    // Arm a 10-second watchdog. If the backend is unreachable the client would
    // otherwise hang for the OS-level TCP timeout (30+ seconds). The timer fires
    // the callback with ec == 0; a successful connect cancels it (ec == operation_aborted).
    _connectTimer.expires_after(std::chrono::seconds(10));
    _connectTimer.async_wait([self = shared_from_this()](boost::system::error_code const& ec)
    {
        if (ec) return; // cancelled because connect succeeded in time
        LOG_WARN("proxy", "BackendSession: Backend connect timed out — dropping client");
        self->_resolver.cancel();
        boost::system::error_code closeEc;
        self->_socket.close(closeEc);
        if (auto owner = self->_owner.lock())
            owner->CloseSocket();
    });

    _resolver.async_resolve(host, std::to_string(port),
        [self = shared_from_this(), host](boost::system::error_code const& error, boost::asio::ip::tcp::resolver::results_type results)
        {
            if (error)
            {
                if (error == boost::asio::error::operation_aborted)
                    return; // timer already fired and closed the socket
                LOG_ERROR("proxy", "BackendSession: Failed to resolve '{}': {}", host, error.message());
                self->_connectTimer.cancel();
                if (auto owner = self->_owner.lock())
                    owner->CloseSocket();
                return;
            }
            boost::asio::async_connect(self->_socket, results,
                [self](boost::system::error_code const& connectError, boost::asio::ip::tcp::endpoint const&)
                {
                    self->OnConnect(connectError);
                });
        });
}

void BackendSession::OnConnect(boost::system::error_code const& error)
{
    _connectTimer.cancel(); // disarm watchdog — connect completed (success or failure)

    if (error)
    {
        if (error == boost::asio::error::operation_aborted)
            return; // timer already fired and closed things down
        LOG_ERROR("proxy", "BackendSession: Connection failed: {}", error.message());
        if (auto owner = _owner.lock())
            owner->CloseSocket();
        return;
    }

    LOG_INFO("proxy", "BackendSession: TCP connected to backend (isReroute={})", _isReroute);

    // For non-reroute sessions: signal the owner to start reading from the client.
    // This prevents the race where CMSG_AUTH_SESSION arrives before the backend
    // socket is open and would be silently dropped by SendRaw().
    // For reroute sessions the owner already has client reads paused via _rerouting;
    // OnRerouteComplete() handles the resume after the handshake.
    if (!_isReroute)
        if (auto owner = _owner.lock())
            owner->OnBackendConnected();

    AsyncRead();
}

void BackendSession::AsyncRead()
{
    if (!IsOpen())
        return;

    _readBuffer.Normalize();
    _readBuffer.EnsureFreeSpace();

    _socket.async_read_some(
        boost::asio::buffer(_readBuffer.GetWritePointer(), _readBuffer.GetRemainingSpace()),
        [self = shared_from_this()](boost::system::error_code const& error, std::size_t transferred)
        {
            self->OnRead(error, transferred);
        });
}

void BackendSession::OnRead(boost::system::error_code const& error, std::size_t transferred)
{
    // Guard against any callbacks (error or data) that arrive after the proxy has
    // intentionally closed this backend (e.g. during a reroute). Without this check,
    // already-queued async_read_some callbacks with transferred > 0 and error == success
    // would still reach ProcessReadBuffer() and forward stale packets to the client,
    // causing ARC4 desync on the live backend.
    if (_closedByProxy)
        return;

    if (error)
    {
        LOG_DEBUG("proxy", "BackendSession: Read closed unexpectedly: {}", error.message());
        if (auto owner = _owner.lock())
            owner->CloseSocket();
        return;
    }

    _readBuffer.WriteCompleted(transferred);
    ProcessReadBuffer();

    if (IsOpen())
        AsyncRead();
}

void BackendSession::ProcessReadBuffer()
{
    bool progress = true;
    while (progress && _readBuffer.GetActiveSize() > 0)
    {
        progress = false;
        switch (_readState)
        {
            case ReadState::FirstByte:
                progress = TryConsumeFirstByte();
                break;
            case ReadState::RestOfHeader:
                progress = TryConsumeRestOfHeader();
                break;
            case ReadState::Payload:
                progress = TryConsumePayload();
                break;
        }
    }
}

bool BackendSession::TryConsumeFirstByte()
{
    if (_readBuffer.GetActiveSize() < 1)
        return false;

    // Store the encrypted first byte; decrypt a copy to check the large-packet bit.
    _encHeader[0] = *_readBuffer.GetReadPointer();
    _readBuffer.ReadCompleted(1);

    _plainHeader[0] = _encHeader[0];
    if (_cryptInitialized)
        _backendCrypt.EncryptSend(&_plainHeader[0], 1); // inverted: EncryptSend decrypts S→C

    _headerLen = (_plainHeader[0] & 0x80) ? 5 : 4;
    _headerRead = 1;
    _readState = ReadState::RestOfHeader;
    return true;
}

bool BackendSession::TryConsumeRestOfHeader()
{
    uint8 remaining = _headerLen - _headerRead;
    if (_readBuffer.GetActiveSize() < remaining)
        return false;

    // Store the encrypted remaining bytes.
    std::memcpy(_encHeader + _headerRead, _readBuffer.GetReadPointer(), remaining);
    _readBuffer.ReadCompleted(remaining);

    // Decrypt into _plainHeader.
    std::memcpy(_plainHeader + _headerRead, _encHeader + _headerRead, remaining);
    if (_cryptInitialized)
        _backendCrypt.EncryptSend(_plainHeader + _headerRead, remaining); // inverted

    // Parse size and opcode from plaintext header.
    uint32 size;
    if (_headerLen == 5)
    {
        size = (static_cast<uint32>(_plainHeader[0] & 0x7F) << 16) |
               (static_cast<uint32>(_plainHeader[1]) << 8) |
               static_cast<uint32>(_plainHeader[2]);
        _opcode = static_cast<uint16>(_plainHeader[3]) | (static_cast<uint16>(_plainHeader[4]) << 8);
    }
    else
    {
        size = (static_cast<uint32>(_plainHeader[0]) << 8) | static_cast<uint32>(_plainHeader[1]);
        _opcode = static_cast<uint16>(_plainHeader[2]) | (static_cast<uint16>(_plainHeader[3]) << 8);
    }

    _payloadSize = size - 2; // size includes 2-byte opcode field
    _payloadBuffer.Resize(_payloadSize);
    _payloadBuffer.Reset();
    _readState = ReadState::Payload;
    return true;
}

bool BackendSession::TryConsumePayload()
{
    if (_payloadSize > 0 && _readBuffer.GetActiveSize() < _payloadSize)
        return false;

    if (_payloadSize > 0)
    {
        std::memcpy(_payloadBuffer.GetWritePointer(), _readBuffer.GetReadPointer(), _payloadSize);
        _readBuffer.ReadCompleted(_payloadSize);
        _payloadBuffer.WriteCompleted(_payloadSize);
    }

    DispatchToClient();

    // Reset for next packet.
    _readState = ReadState::FirstByte;
    _headerRead = 0;
    _headerLen = 0;
    _opcode = 0;
    _payloadSize = 0;
    _payloadBuffer.Reset();
    return true;
}

void BackendSession::DispatchToClient()
{
    bool const packetLog = sConfigMgr->GetOption<bool>("Proxy.PacketLog", false);

    // During the reroute handshake, intercept specific opcodes and discard all others.
    // Nothing gets forwarded to the client until the handshake is fully complete.
    if (_isReroute && _handshakeState != HandshakeState::Done)
    {
        if (packetLog)
        {
            bool handled = (_handshakeState == HandshakeState::WaitChallenge &&
                                (_opcode == SMSG_AUTH_CHALLENGE_OPCODE || _opcode == SMSG_AUTH_RESPONSE_OPCODE))
                        || (_handshakeState == HandshakeState::WaitResponse &&
                                _opcode == SMSG_AUTH_RESPONSE_OPCODE)
                        || (_handshakeState == HandshakeState::WaitCharEnum &&
                                _opcode == SMSG_CHAR_ENUM_OPCODE);
            LOG_DEBUG("proxy.packets", "S→C  GUID {:016X}  opcode 0x{:04X}  size {}  [reroute-{}]",
                      _playerGuid, _opcode, _payloadSize, handled ? "intercept" : "drop");
        }

        switch (_handshakeState)
        {
            case HandshakeState::WaitChallenge:
                if (_opcode == SMSG_AUTH_CHALLENGE_OPCODE) { HandleAuthChallenge(); return; }
                // IP-ban SMSG_AUTH_RESPONSE can arrive before SMSG_AUTH_CHALLENGE.
                if (_opcode == SMSG_AUTH_RESPONSE_OPCODE) { HandleAuthResponse(); return; }
                return; // discard unexpected packets
            case HandshakeState::WaitResponse:
                if (_opcode == SMSG_AUTH_RESPONSE_OPCODE) { HandleAuthResponse(); return; }
                return; // discard unexpected packets
            case HandshakeState::WaitCharEnum:
                // Discard SMSG_ADDON_INFO, SMSG_CLIENTCACHE_VERSION, SMSG_TUTORIAL_FLAGS, etc.
                // Wait only for SMSG_CHAR_ENUM which signals _legitCharacters is populated.
                if (_opcode == SMSG_CHAR_ENUM_OPCODE) { HandleCharEnum(); return; }
                return; // discard
            default:
                return;
        }
    }

    auto owner = _owner.lock();
    if (!owner)
        return;

    // After the reroute handshake, drop pre-login S→C packets and wait for
    // SMSG_LOGIN_VERIFY_WORLD.  When found, rewrite it to SMSG_NEW_WORLD
    // (identical payload: mapId + x + y + z + orientation) so the client
    // completes the SMSG_TRANSFER_PENDING loading-screen flow without disconnecting.
    if (_rerouteLoginPending)
    {
        if (_opcode == SMSG_LOGIN_VERIFY_WORLD_OPCODE)
        {
            _rerouteLoginPending = false;

            if (_isLoginReroute)
            {
                // GAP-1 login reroute: client is in initial login state and expects
                // SMSG_LOGIN_VERIFY_WORLD directly (not SMSG_NEW_WORLD).  Forward as-is.
                // No SetDropWorldportAck — the client won't send MSG_MOVE_WORLDPORT_ACK
                // in the normal login flow.
                if (packetLog)
                    LOG_DEBUG("proxy.packets", "S→C  GUID {:016X}  opcode 0x{:04X}  size {}  [login-reroute-verify-world]",
                              _playerGuid, _opcode, _payloadSize);

                owner->SetClientInWorld();
                owner->QueuePacketForClient(_plainHeader, _headerLen, _payloadBuffer);
            }
            else
            {
                // In-world cross-node teleport: rewrite SMSG_LOGIN_VERIFY_WORLD → SMSG_NEW_WORLD
                // so the client completes the SMSG_TRANSFER_PENDING loading-screen flow.
                // SMSG_NEW_WORLD (0x03E) and SMSG_LOGIN_VERIFY_WORLD (0x236) share the same
                // payload layout (mapId + x + y + z + orientation), only the opcode differs.
                uint8 rewrittenHeader[5];
                std::memcpy(rewrittenHeader, _plainHeader, _headerLen);
                uint8 opcLow  = static_cast<uint8>(SMSG_NEW_WORLD_OPCODE & 0xFF);
                uint8 opcHigh = static_cast<uint8>((SMSG_NEW_WORLD_OPCODE >> 8) & 0xFF);
                if (_headerLen == 5)
                {
                    rewrittenHeader[3] = opcLow;
                    rewrittenHeader[4] = opcHigh;
                }
                else
                {
                    rewrittenHeader[2] = opcLow;
                    rewrittenHeader[3] = opcHigh;
                }

                if (packetLog)
                    LOG_DEBUG("proxy.packets", "S→C  GUID {:016X}  opcode 0x{:04X}→0x{:04X}  size {}  [reroute-rewrite]",
                              _playerGuid, SMSG_LOGIN_VERIFY_WORLD_OPCODE, SMSG_NEW_WORLD_OPCODE, _payloadSize);

                owner->SetClientInWorld();
                owner->QueuePacketForClient(rewrittenHeader, _headerLen, _payloadBuffer);
                // Tell ProxySocket to silently drop the client's MSG_MOVE_WORLDPORT_ACK:
                // the destination node already spawned the player via PLAYER_LOGIN and does
                // not expect (or need) a worldport ack.
                owner->SetTranslateWorldportAck(true);
            }
            return;
        }

        // Pre-login packet — drop it.
        // For in-world reroutes: client already has character data from initial login.
        // For login reroutes: node2's pre-login packets (SMSG_ACCOUNT_DATA_TIMES, etc.)
        //   must be suppressed to prevent ARC4 desync; character data will arrive after
        //   SMSG_LOGIN_VERIFY_WORLD on the live stream.
        if (packetLog)
            LOG_DEBUG("proxy.packets", "S→C  GUID {:016X}  opcode 0x{:04X}  size {}  [reroute-prelogin-drop]",
                      _playerGuid, _opcode, _payloadSize);
        return;
    }

    if (packetLog)
    {
        uint64 guid = _isReroute ? _playerGuid : owner->GetPlayerGuid();
        LOG_DEBUG("proxy.packets", "S→C  GUID {:016X}  opcode 0x{:04X}  size {}",
                  guid, _opcode, _payloadSize);
    }

    // Track when SMSG_LOGIN_VERIFY_WORLD first reaches the client so subsequent reroutes
    // know whether the client has entered the world (in-world teleport) or is still in
    // the initial login state (GAP-1 login reroute).
    if (_opcode == SMSG_LOGIN_VERIFY_WORLD_OPCODE)
        owner->SetClientInWorld();

    // When the backend sends SMSG_LOGOUT_COMPLETE, the client returns to the character
    // select screen.  Reset proxy state so the next CMSG_PLAYER_LOGIN is treated as a
    // fresh login rather than a reroute or stale in-world state.
    if (_opcode == SMSG_LOGOUT_COMPLETE_OPCODE)
    {
        if (owner->IsRerouting())
        {
            LOG_INFO("proxy", "BackendSession: Suppressing SMSG_LOGOUT_COMPLETE during reroute (GUID {:016X})", _playerGuid);
            return;  // Don't forward logout to client during seamless reroute
        }
        LOG_INFO("proxy", "BackendSession: SMSG_LOGOUT_COMPLETE for GUID {:016X} — resetting proxy state", _playerGuid);
        owner->SetClientOutOfWorld();
    }

    // SMSG_CHARACTER_LOGIN_FAILED means the login was rejected — client returns to
    // character select.  Same state reset needed.
    if (_opcode == SMSG_CHARACTER_LOGIN_FAILED_OPCODE)
    {
        LOG_INFO("proxy", "BackendSession: SMSG_CHARACTER_LOGIN_FAILED for GUID {:016X} — resetting proxy state", _playerGuid);
        owner->SetClientOutOfWorld();
    }

    // Pass the plaintext header bytes and payload to ProxySocket.
    // ProxySocket will re-encrypt the header for the client direction.
    owner->QueuePacketForClient(_plainHeader, _headerLen, _payloadBuffer);
}

void BackendSession::HandleAuthChallenge()
{
    LOG_INFO("proxy", "BackendSession: Got SMSG_AUTH_CHALLENGE from backend, sending CMSG_AUTH_SESSION (account='{}')", _accountName);
    // SMSG_AUTH_CHALLENGE payload (72 bytes):
    //   uint32(1) | uint32 serverSeed | uint8[32] unk1 | uint8[32] unk2
    if (_payloadBuffer.GetActiveSize() < 8)
    {
        LOG_ERROR("proxy", "BackendSession: SMSG_AUTH_CHALLENGE payload too short ({})",
                  _payloadBuffer.GetActiveSize());
        if (auto owner = _owner.lock())
            owner->CloseSocket();
        return;
    }

    uint8* payload = _payloadBuffer.GetReadPointer();
    // serverSeed is at bytes 4-7 (after the leading uint32(1)).
    uint8 serverSeed[4];
    std::memcpy(serverSeed, payload + 4, 4);

    // Generate a random 4-byte client seed.
    auto clientSeedArr = Acore::Crypto::GetRandomBytes<4>();
    uint8 const* clientSeed = clientSeedArr.data();

    // Compute SHA1 digest:
    // SHA1(accountName || uint32(0) || clientSeed[4] || serverSeed[4] || sessionKey[40])
    uint32 zero = 0;
    Acore::Crypto::SHA1 sha;
    sha.UpdateData(_accountName);
    sha.UpdateData(reinterpret_cast<uint8 const*>(&zero), 4);
    sha.UpdateData(clientSeed, 4);
    sha.UpdateData(serverSeed, 4);
    sha.UpdateData(_sessionKey.data(), SESSION_KEY_LENGTH);
    sha.Finalize();
    auto const& digestArr = sha.GetDigest();
    uint8 const* digest = digestArr.data();

    // Build CMSG_AUTH_SESSION (opcode 0x1ED):
    // 6-byte header (size BE + opcode LE) + payload
    // Payload: uint32 build | uint32 serverId | string account\0 | uint32 serverType |
    //          uint8[4] clientSeed | uint32 region | uint32 battlegroup | uint32 realmId |
    //          uint64 dosResponse | uint8[20] digest | uint32 addonLen(0) |
    //          string realClientIp\0  ← extra suffix read by backends with ProxyServer.Enable=1
    uint32 ipSuffixLen = static_cast<uint32>(_clientIp.size()) + 1; // +1 for null terminator
    uint32 payloadSize = 4 + 4 + static_cast<uint32>(_accountName.size()) + 1
                       + 4 + 4 + 4 + 4 + 4 + 8 + 20 + 4 + ipSuffixLen;
    uint16 sizeField = static_cast<uint16>(payloadSize + 4); // +4 for the 4-byte opcode field

    std::vector<uint8> msg;
    msg.reserve(6 + payloadSize);

    // Header: size (BE) + opcode (LE)
    msg.push_back(static_cast<uint8>(sizeField >> 8));
    msg.push_back(static_cast<uint8>(sizeField & 0xFF));
    constexpr uint32 AUTH_SESSION_OPCODE = 0x1ED;
    msg.push_back(static_cast<uint8>(AUTH_SESSION_OPCODE & 0xFF));
    msg.push_back(static_cast<uint8>((AUTH_SESSION_OPCODE >> 8) & 0xFF));
    msg.push_back(static_cast<uint8>((AUTH_SESSION_OPCODE >> 16) & 0xFF));
    msg.push_back(static_cast<uint8>((AUTH_SESSION_OPCODE >> 24) & 0xFF));

    auto pushU32 = [&](uint32 v)
    {
        msg.push_back(v & 0xFF);
        msg.push_back((v >> 8) & 0xFF);
        msg.push_back((v >> 16) & 0xFF);
        msg.push_back((v >> 24) & 0xFF);
    };

    pushU32(WOTLK_CLIENT_BUILD); // Build 12340
    pushU32(0);                  // LoginServerID
    msg.insert(msg.end(), _accountName.begin(), _accountName.end());
    msg.push_back(0);            // null terminator
    pushU32(0);                  // LoginServerType
    msg.insert(msg.end(), clientSeed, clientSeed + 4);
    pushU32(0);                  // RegionID
    pushU32(0);                  // BattlegroupID
    pushU32(_realmId);           // RealmID
    for (int i = 0; i < 8; ++i) msg.push_back(0); // DosResponse (uint64 = 0)
    msg.insert(msg.end(), digest, digest + 20);
    pushU32(0);                  // AddonInfo size (no addons)
    // Real client IP suffix — read by backends with ProxyServer.Enable=1 to override peer address.
    msg.insert(msg.end(), _clientIp.begin(), _clientIp.end());
    msg.push_back(0);            // null terminator

    LOG_DEBUG("proxy", "BackendSession: Sending CMSG_AUTH_SESSION to backend for '{}' (realIp={})",
              _accountName, _clientIp);

    SendRaw(msg);

    // The worldserver initializes its _authCrypt immediately after processing CMSG_AUTH_SESSION.
    // Initialize _backendCrypt now so we can decrypt the encrypted SMSG_AUTH_RESPONSE and
    // all subsequent S→C packets from the backend.
    InitCrypt(_sessionKey);

    _handshakeState = HandshakeState::WaitResponse;
}

void BackendSession::HandleAuthResponse()
{
    uint8 status = 0;
    if (_payloadBuffer.GetActiveSize() >= 1)
        status = *_payloadBuffer.GetReadPointer();

    if (status != AUTH_OK_CODE)
    {
        LOG_ERROR("proxy", "BackendSession: Instance server auth failed (status=0x{:02X})", status);
        if (auto owner = _owner.lock())
            owner->CloseSocket();
        return;
    }

    LOG_INFO("proxy", "BackendSession: Instance server auth OK — sending CMSG_CHAR_ENUM");

    // Crypto was already initialized in HandleAuthChallenge() so SMSG_AUTH_RESPONSE
    // could be decrypted.  Do NOT re-init here — that would reset the ARC4 stream
    // and desync subsequent packet encryption with the worldserver.

    // We must send CMSG_CHAR_ENUM before CMSG_PLAYER_LOGIN so the worldserver
    // populates _legitCharacters for this account.  Without it, HandlePlayerLoginOpcode
    // rejects the CMSG_PLAYER_LOGIN with "can't login with that character".
    _handshakeState = HandshakeState::WaitCharEnum;
    SendCharEnum();
}

void BackendSession::SendCharEnum()
{
    // CMSG_CHAR_ENUM (0x037): 6-byte header + 0-byte payload.
    // size field = 4 (opcode only, no payload).
    constexpr uint16 SIZE_FIELD      = 4;
    constexpr uint32 CHAR_ENUM_OPCODE = 0x037;

    uint8 header[6];
    header[0] = static_cast<uint8>(SIZE_FIELD >> 8);
    header[1] = static_cast<uint8>(SIZE_FIELD & 0xFF);
    header[2] = static_cast<uint8>(CHAR_ENUM_OPCODE & 0xFF);
    header[3] = static_cast<uint8>((CHAR_ENUM_OPCODE >> 8) & 0xFF);
    header[4] = static_cast<uint8>((CHAR_ENUM_OPCODE >> 16) & 0xFF);
    header[5] = static_cast<uint8>((CHAR_ENUM_OPCODE >> 24) & 0xFF);

    // Encrypt header for the C→S backend direction (inverted: DecryptRecv() encrypts C→S).
    if (_cryptInitialized)
        _backendCrypt.DecryptRecv(header, 6);

    std::vector<uint8> packet(header, header + 6);

    LOG_DEBUG("proxy", "BackendSession: Sending synthesized CMSG_CHAR_ENUM to populate _legitCharacters");

    SendRaw(packet);
}

void BackendSession::HandleCharEnum()
{
    LOG_INFO("proxy", "BackendSession: Got SMSG_CHAR_ENUM — reroute handshake complete (GUID {:016X})", _playerGuid);
    _handshakeState = HandshakeState::Done;

    if (_isLoginReroute)
    {
        // GAP-1 login reroute: client is in initial login state.
        // Suppress pre-login packets; forward SMSG_LOGIN_VERIFY_WORLD as-is.
        _rerouteLoginPending = true;
        // Client never sends MSG_MOVE_WORLDPORT_ACK in initial login flow,
        // so we send PLAYER_LOGIN now.
        SendPlayerLogin();
    }
    else
    {
        // Native in-world reroute: proxy already sent SMSG_NEW_WORLD.
        // Wait for the client to send MSG_MOVE_WORLDPORT_ACK (intercepted by ProxySocket
        // which translates it to CMSG_PLAYER_LOGIN on the destination node).
        // _rerouteLoginPending=false: all S→C packets from the destination are forwarded
        // directly — the client is on the loading screen and safely handles pre-login
        // packets (SMSG_ACCOUNT_DATA_TIMES etc.) as well as SMSG_LOGIN_VERIFY_WORLD
        // (which positions it at the new location).  No rewrite needed because the client
        // has already received SMSG_NEW_WORLD from the proxy.
        _rerouteLoginPending = false;
    }

    if (auto owner = _owner.lock())
        owner->OnRerouteComplete(shared_from_this());
}

void BackendSession::SendPlayerLogin()
{
    // CMSG_PLAYER_LOGIN (0x03D): 6-byte header + 8-byte GUID payload.
    // size field = 4 (opcode) + 8 (guid) = 12
    constexpr uint16 SIZE_FIELD = 12;
    constexpr uint32 PLAYER_LOGIN_OPCODE = 0x03D;

    uint8 header[6];
    header[0] = static_cast<uint8>(SIZE_FIELD >> 8);
    header[1] = static_cast<uint8>(SIZE_FIELD & 0xFF);
    header[2] = static_cast<uint8>(PLAYER_LOGIN_OPCODE & 0xFF);
    header[3] = static_cast<uint8>((PLAYER_LOGIN_OPCODE >> 8) & 0xFF);
    header[4] = static_cast<uint8>((PLAYER_LOGIN_OPCODE >> 16) & 0xFF);
    header[5] = static_cast<uint8>((PLAYER_LOGIN_OPCODE >> 24) & 0xFF);

    // Encrypt header for the C→S backend direction.
    if (_cryptInitialized)
        _backendCrypt.DecryptRecv(header, 6); // inverted: encrypts C→S

    std::vector<uint8> packet;
    packet.reserve(6 + 8);
    packet.insert(packet.end(), header, header + 6);

    // Player GUID as uint64 LE.
    for (int i = 0; i < 8; ++i)
        packet.push_back(static_cast<uint8>((_playerGuid >> (i * 8)) & 0xFF));

    LOG_DEBUG("proxy", "BackendSession: Sending synthesized CMSG_PLAYER_LOGIN (GUID {:016X})",
              _playerGuid);

    SendRaw(packet);
}

void BackendSession::InitCrypt(SessionKey const& key)
{
    _backendCrypt.Init(key);
    _cryptInitialized = true;
}

void BackendSession::SendRaw(std::vector<uint8> const& data)
{
    if (!IsOpen())
    {
        LOG_WARN("proxy", "BackendSession::SendRaw: backend not open, dropping {} bytes", data.size());
        return;
    }

    // Queue the data for async writing.
    bool wasEmpty = _sendQueue.empty();
    // Guard against unbounded queue growth if backend is stalled
    if (_sendQueue.size() > 2048)
    {
        LOG_WARN("proxy", "BackendSession: Send queue overflow ({}), dropping connection", _sendQueue.size());
        Close();
        return;
    }
    _sendQueue.push(data);

    if (wasEmpty)
        AsyncWrite();
}

void BackendSession::AsyncWrite()
{
    if (_sendQueue.empty() || !IsOpen())
        return;

    auto& front = _sendQueue.front();
    boost::asio::async_write(_socket, boost::asio::buffer(front),
        [self = shared_from_this()](boost::system::error_code const& error, std::size_t /*transferred*/)
        {
            if (error)
            {
                LOG_DEBUG("proxy", "BackendSession: Write error: {}", error.message());
                self->Close();
                return;
            }
            self->_sendQueue.pop();
            self->AsyncWrite();
        });
}

void BackendSession::Close()
{
    _closedByProxy = true; // suppress OnRead error-cascade before closing
    if (_socket.is_open())
    {
        boost::system::error_code ec;
        _socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
        _socket.close(ec);
    }
}

bool BackendSession::IsOpen() const
{
    return _socket.is_open();
}
