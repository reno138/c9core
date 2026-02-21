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
                               uint32 realmId, uint64 playerGuid)
    : _owner(std::move(owner))
    , _socket(static_cast<boost::asio::io_context&>(ioContext))
    , _resolver(static_cast<boost::asio::io_context&>(ioContext))
    , _connectTimer(static_cast<boost::asio::io_context&>(ioContext))
    , _readBuffer(READ_SIZE)
    , _payloadBuffer(0)
    , _isReroute(true)
    , _handshakeState(HandshakeState::WaitChallenge)
    , _accountName(std::move(accountName))
    , _sessionKey(sessionKey)
    , _realmId(realmId)
    , _playerGuid(playerGuid)
{
}

BackendSession::~BackendSession()
{
    Close();
}

void BackendSession::Connect(std::string const& host, uint16 port)
{
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

    LOG_DEBUG("proxy", "BackendSession: Connected to backend.");

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
    if (error)
    {
        LOG_DEBUG("proxy", "BackendSession: Read closed: {}", error.message());
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
    // During reroute handshake, intercept auth opcodes — do NOT forward to client.
    if (_isReroute && _handshakeState != HandshakeState::Done)
    {
        if (_opcode == SMSG_AUTH_CHALLENGE_OPCODE)
        {
            HandleAuthChallenge();
            return;
        }
        if (_opcode == SMSG_AUTH_RESPONSE_OPCODE)
        {
            HandleAuthResponse();
            return;
        }
    }

    auto owner = _owner.lock();
    if (!owner)
        return;

    // Pass the plaintext header bytes and payload to ProxySocket.
    // ProxySocket will re-encrypt the header for the client direction.
    owner->QueuePacketForClient(_plainHeader, _headerLen, _payloadBuffer);
}

void BackendSession::HandleAuthChallenge()
{
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
    //          uint64 dosResponse | uint8[20] digest | uint32 addonLen(0)
    uint32 payloadSize = 4 + 4 + static_cast<uint32>(_accountName.size()) + 1
                       + 4 + 4 + 4 + 4 + 4 + 8 + 20 + 4;
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

    LOG_DEBUG("proxy", "BackendSession: Sending CMSG_AUTH_SESSION to instance server for '{}'",
              _accountName);

    SendRaw(msg);
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

    LOG_INFO("proxy", "BackendSession: Instance server auth OK — initializing crypto and loading player");

    // Initialize backend RC4 crypto with the player's session key.
    InitCrypt(_sessionKey);
    _handshakeState = HandshakeState::Done;

    // Synthesize CMSG_PLAYER_LOGIN so the instance server loads the character.
    SendPlayerLogin();

    // Notify ProxySocket to complete the backend switch.
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
