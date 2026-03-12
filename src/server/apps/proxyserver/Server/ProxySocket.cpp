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

#include "ProxySocket.h"
#include "BackendSession.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "ProxyMgr.h"
#include "ProxySocketMgr.h"
#include "QueryResult.h"

/// Client packet header layout (wire format, after decryption):
///   uint16 size  (big-endian, includes 4-byte opcode field)
///   uint32 cmd   (little-endian opcode)
/// Total: 6 bytes.
static constexpr std::size_t CLIENT_HEADER_SIZE = 6;

ProxySocket::ProxySocket(IoContextTcpSocket&& socket)
    : BaseSocket(std::move(socket))
    , _headerBuffer(CLIENT_HEADER_SIZE)
    , _packetBuffer(0)
{
}

ProxySocket::~ProxySocket()
{
    if (_playerGuid != 0)
        sProxyMgr.UnregisterSession(_playerGuid);
}

void ProxySocket::Start()
{
    // Create and connect the backend session.
    // Client reads are deliberately deferred until OnBackendConnected() fires — this
    // prevents a race where the client sends CMSG_AUTH_SESSION before the backend TCP
    // socket is open, which would cause SendRaw() to silently drop the packet.
    _backend = std::make_shared<BackendSession>(
        sProxySocketMgr.GetIoContext(),
        shared_from_this());

    auto [backendNodeId, backendHost, backendPort] = sProxyMgr.ChooseNode();
    _backendNodeId = backendNodeId;
    _backend->Connect(backendHost, backendPort);
    // NOTE: AsyncRead() is NOT called here — see OnBackendConnected().
}

bool ProxySocket::Update()
{
    _queryProcessor.ProcessReadyCallbacks();
    return BaseSocket::Update();
}

SocketReadCallbackResult ProxySocket::ReadHandler()
{
    // The pending async_read_some has fired — mark the read slot as free.
    _asyncReadActive = false;

    // Pause client reads while a reroute is in progress.
    if (_rerouting)
        return SocketReadCallbackResult::Stop;

    MessageBuffer& rawBuf = GetReadBuffer();

    while (rawBuf.GetActiveSize() > 0)
    {
        // Phase 1: accumulate 6-byte header.
        if (_headerBuffer.GetRemainingSpace() > 0)
        {
            std::size_t toCopy = std::min(rawBuf.GetActiveSize(), _headerBuffer.GetRemainingSpace());
            _headerBuffer.Write(rawBuf.GetReadPointer(), toCopy);
            rawBuf.ReadCompleted(toCopy);

            if (_headerBuffer.GetRemainingSpace() > 0)
                break; // need more bytes

            if (!ReadHeaderHandler())
            {
                CloseSocket();
                return SocketReadCallbackResult::Stop;
            }
        }

        // Phase 2: accumulate payload.
        if (_packetBuffer.GetRemainingSpace() > 0)
        {
            std::size_t toCopy = std::min(rawBuf.GetActiveSize(), _packetBuffer.GetRemainingSpace());
            _packetBuffer.Write(rawBuf.GetReadPointer(), toCopy);
            rawBuf.ReadCompleted(toCopy);

            if (_packetBuffer.GetRemainingSpace() > 0)
                break; // need more bytes
        }

        // Full packet received: header + payload.
        if (!ReadDataHandler())
        {
            // Returning false means we're waiting for a query — stop reading until resumed.
            _headerBuffer.Reset();
            return SocketReadCallbackResult::Stop;
        }

        _headerBuffer.Reset();
    }

    // ReadHandlerInternal will call AsyncRead() after we return KeepReading — mark active now
    // so OnRerouteComplete() knows not to issue a second concurrent read on the same buffer.
    _asyncReadActive = true;
    return SocketReadCallbackResult::KeepReading;
}

bool ProxySocket::ReadHeaderHandler()
{
    uint8* header = _headerBuffer.GetReadPointer();

    // Decrypt the 6-byte header in-place using the client-side crypto.
    if (_clientCrypt.IsInitialized())
        _clientCrypt.DecryptRecv(header, CLIENT_HEADER_SIZE);

    // Parse: size is big-endian uint16, cmd is little-endian uint32.
    uint16 size = (static_cast<uint16>(header[0]) << 8) | header[1];
    uint32 cmd  = static_cast<uint32>(header[2])
                | (static_cast<uint32>(header[3]) << 8)
                | (static_cast<uint32>(header[4]) << 16)
                | (static_cast<uint32>(header[5]) << 24);

    if (size < 4 || size > 10240)
    {
        LOG_ERROR("proxy", "ProxySocket: Invalid client packet size {}", size);
        return false;
    }

    uint32 payloadSize = size - 4; // size includes the 4-byte opcode

    // Store opcode in header[2..5] using our parsed values for ForwardToBackend.
    // (header is already decrypted in-place — ready to re-encrypt for backend.)
    (void)cmd; // opcode is available for interception logic

    _packetBuffer.Resize(payloadSize);
    _packetBuffer.Reset();
    return true;
}

bool ProxySocket::ReadDataHandler()
{
    uint8* header = _headerBuffer.GetReadPointer();

    // Re-parse opcode from decrypted header.
    uint32 opcode = static_cast<uint32>(header[2])
                  | (static_cast<uint32>(header[3]) << 8)
                  | (static_cast<uint32>(header[4]) << 16)
                  | (static_cast<uint32>(header[5]) << 24);

    if (sConfigMgr->GetOption<bool>("Proxy.PacketLog", false))
        LOG_DEBUG("proxy.packets", "C→S  GUID {:016X}  opcode 0x{:04X}  size {}",
                  _playerGuid, opcode, CLIENT_HEADER_SIZE + _packetBuffer.GetActiveSize());

    // After native cross-node reroute (SMSG_NEW_WORLD sent to client), the client
    // responds with MSG_MOVE_WORLDPORT_ACK.  Translate it to CMSG_PLAYER_LOGIN and
    // send to the new backend -- matching VB.NET On_MSG_MOVE_WORLDPORT_ACK.
    if (_translateWorldportAck && opcode == MSG_MOVE_WORLDPORT_ACK_OPCODE)
    {
        _translateWorldportAck = false;
        if (_hasWpDest)
        {
            // Cross-node reconnect: reroute to the correct destination node.
            // _clientInWorld is false here, so RerouteToBackend uses isLoginReroute=true
            // (no SMSG_NEW_WORLD sent, BackendSession sends PLAYER_LOGIN after char enum).
            LOG_INFO("proxy", "ProxySocket: Worldport ACK for GUID {:016X} — rerouting to {}:{} map={}",
                     _playerGuid, _wpDestAddr, _wpDestPort, _wpMapId);
            std::string dest = std::move(_wpDestAddr);
            uint16 port = _wpDestPort;
            uint32 mapId = _wpMapId;
            float x = _wpX, y = _wpY, z = _wpZ, ori = _wpOri;
            _hasWpDest = false;
            RerouteToBackend(dest, port, mapId, x, y, z, ori);
        }
        else
        {
            LOG_DEBUG("proxy", "ProxySocket: MSG_MOVE_WORLDPORT_ACK for GUID {:016X} -- sending CMSG_PLAYER_LOGIN",
                      _playerGuid);
            if (_backend && _backend->IsOpen())
                _backend->SendPlayerLogin();
            else
                LOG_ERROR("proxy", "ProxySocket: No backend for PLAYER_LOGIN after worldport ack, GUID {:016X}", _playerGuid);
        }
        return true;
    }

    // Guard: while we are waiting for MSG_MOVE_WORLDPORT_ACK, drop all other client
    // packets.  The new backend has not received CMSG_PLAYER_LOGIN yet, so forwarding
    // opcodes like CMSG_CANCEL_TRADE would hit "player has not logged in yet" errors.
    if (_translateWorldportAck)
    {
        LOG_DEBUG("proxy", "ProxySocket: Dropping opcode 0x{:04X} while awaiting WORLDPORT_ACK for GUID {:016X}",
                  opcode, _playerGuid);
        return true;
    }

    if (opcode == CMSG_AUTH_SESSION_OPCODE)
    {
        // Intercept: extract account name, query session key, pause reading.
        HandleAuthSessionIntercepted();
        return false; // stop reading; resume in HandleAuthSessionCallback
    }

    if (opcode == CMSG_PLAYER_LOGIN_OPCODE)
    {
        // Intercept: learn the player GUID so ProxyMgr can route reroute commands.
        // Payload is exactly 8 bytes: uint64 player GUID (little-endian).
        if (_packetBuffer.GetActiveSize() >= 8)
        {
            std::memcpy(&_playerGuid, _packetBuffer.GetReadPointer(), 8);
            sProxyMgr.RegisterSession(_playerGuid, shared_from_this());
            LOG_DEBUG("proxy", "ProxySocket: Player GUID {:016X} registered in ProxyMgr", _playerGuid);
        }
        // Fall through: forward to backend normally.
    }

    // Forward packet to backend with re-encrypted header.
    if (!_backend || !_backend->IsOpen())
    {
        LOG_WARN("proxy", "ProxySocket: No backend connected, dropping opcode 0x{:04X}", opcode);
        return true;
    }

    // Build forwarding buffer: re-encrypt the header for the backend direction,
    // then append payload (payload is always plaintext).
    uint8 backendHeader[CLIENT_HEADER_SIZE];
    std::memcpy(backendHeader, header, CLIENT_HEADER_SIZE);

    // Re-encrypt using inverted backend crypto: DecryptRecv() encrypts C→S toward backend.
    // Only applies when the backend ARC4 is initialized (direct-client mode without proxy).
    // In cluster proxy mode the backend runs plaintext, so we send the decrypted header as-is.
    // Note: AuthCrypt::DecryptRecv() ASSERTs on IsInitialized(), so the guard is mandatory.
    if (_clientCrypt.IsInitialized() && _backend->GetCrypt().IsInitialized())
        _backend->GetCrypt().DecryptRecv(backendHeader, CLIENT_HEADER_SIZE);

    std::vector<uint8> packet;
    packet.reserve(CLIENT_HEADER_SIZE + _packetBuffer.GetActiveSize());
    packet.insert(packet.end(), backendHeader, backendHeader + CLIENT_HEADER_SIZE);
    packet.insert(packet.end(),
        _packetBuffer.GetReadPointer(),
        _packetBuffer.GetReadPointer() + _packetBuffer.GetActiveSize());

    _backend->SendRaw(packet);
    if (_backendNodeId)
        sProxyMgr.AddNodeTraffic(_backendNodeId, packet.size(), 0);
    return true;
}

void ProxySocket::HandleAuthSessionIntercepted()
{
    // Forward CMSG_AUTH_SESSION to the backend with the real client IP appended
    // as a null-terminated string after the addon data.  The worldserver reads
    // the extra bytes (guarded by ProxyServer.Enable) and uses them to override
    // the socket peer address (proxy IP) for IP-ban, IP-lock, country-lock, and
    // last-IP logging.
    std::string const realIp = GetRemoteIpAddress().to_string();
    std::size_t const ipLen  = realIp.size() + 1; // +1 for null terminator

    std::vector<uint8> packet;
    packet.reserve(CLIENT_HEADER_SIZE + _packetBuffer.GetActiveSize() + ipLen);
    packet.insert(packet.end(),
        _headerBuffer.GetReadPointer(),
        _headerBuffer.GetReadPointer() + CLIENT_HEADER_SIZE);
    packet.insert(packet.end(),
        _packetBuffer.GetReadPointer(),
        _packetBuffer.GetReadPointer() + _packetBuffer.GetActiveSize());

    // Append the real client IP (null-terminated).
    packet.insert(packet.end(), realIp.begin(), realIp.end());
    packet.push_back(0);

    // Update the big-endian uint16 size field (bytes 0-1).
    // size = original_payload_size + 4 (opcode) → we add ipLen more bytes to payload.
    uint16 oldSize = (static_cast<uint16>(packet[0]) << 8) | packet[1];
    uint16 newSize = static_cast<uint16>(oldSize + ipLen);
    packet[0] = static_cast<uint8>(newSize >> 8);
    packet[1] = static_cast<uint8>(newSize & 0xFF);

    // Do NOT send to backend yet — defer until session key is loaded from the DB.
    // If we sent immediately, the worldserver would initialize ARC4 before we have the
    // session key, and the encrypted SMSG_AUTH_RESPONSE would arrive before we can
    // initialize _backendCrypt.  Save the packet; HandleAuthSessionCallback will send it.
    _pendingAuthSession = std::move(packet);

    // Extract account name from payload:
    //   uint32 Build          (offset 0)
    //   uint32 LoginServerID  (offset 4)
    //   string Account        (offset 8, null-terminated)
    uint8* payload = _packetBuffer.GetReadPointer();
    std::size_t payloadLen = _packetBuffer.GetActiveSize();

    if (payloadLen < 9) // minimum: 8 bytes header + at least 1 char name + null
    {
        LOG_ERROR("proxy", "ProxySocket: CMSG_AUTH_SESSION payload too short");
        CloseSocket();
        return;
    }

    _accountName.assign(reinterpret_cast<char*>(payload + 8));

    LOG_DEBUG("proxy", "ProxySocket: Intercepted CMSG_AUTH_SESSION for account '{}'", _accountName);

    // Query the session key from the auth database.
    _waitingForQuery = true;
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_INFO_BY_NAME);
    stmt->SetData(0, sConfigMgr->GetOption<int32>("RealmID", 1));
    stmt->SetData(1, _accountName);

    _queryProcessor.AddCallback(
        LoginDatabase.AsyncQuery(stmt).WithPreparedCallback(
            std::bind(&ProxySocket::HandleAuthSessionCallback, this, std::placeholders::_1)));
}

void ProxySocket::HandleAuthSessionCallback(PreparedQueryResult result)
{
    _waitingForQuery = false;

    if (!result)
    {
        LOG_WARN("proxy", "ProxySocket: No session key found for account '{}'", _accountName);
        // The backend will send an auth error to the client; let it handle the disconnect.
        ResumeAfterAuth();
        return;
    }

    Field* fields = result->Fetch();
    // Field 1: session_key (binary, 40 bytes). See LoginDatabase.cpp LOGIN_SEL_ACCOUNT_INFO_BY_NAME.
    SessionKey sessionKey = fields[1].Get<Binary, SESSION_KEY_LENGTH>();

    LOG_DEBUG("proxy", "ProxySocket: Session key loaded for '{}', initializing crypto.", _accountName);

    // Store session key and realm ID for potential future reroutes.
    _sessionKey = sessionKey;
    _realmId    = sConfigMgr->GetOption<int32>("RealmID", 1);

    // Extract account ID from the query result (field 0).
    _accountId = fields[0].Get<uint32>();

    // Check for a pending worldport from a cross-node reroute on a previous socket.
    PendingWorldport wp;
    if (sProxyMgr.GetAndClearPendingWorldport(_accountId, wp))
    {
        _playerGuid = wp.guid;
        _translateWorldportAck = true;
        _hasWpDest = true;
        _wpDestAddr = wp.destAddr;
        _wpDestPort = wp.destPort;
        _wpMapId    = wp.mapId;
        _wpX = wp.x;  _wpY = wp.y;  _wpZ = wp.z;  _wpOri = wp.ori;
        sProxyMgr.RegisterSession(_playerGuid, shared_from_this());
        LOG_INFO("proxy", "ProxySocket: Recovered worldport for GUID {:016X} account {} → {}:{} map={}",
                 _playerGuid, _accountId, _wpDestAddr, _wpDestPort, _wpMapId);
    }

    // Initialize client-side crypto (normal server perspective).
    _clientCrypt.Init(sessionKey);

    // Initialize backend ARC4 BEFORE sending CMSG_AUTH_SESSION.  The worldserver
    // initializes its _authCrypt immediately after processing CMSG_AUTH_SESSION, so
    // SMSG_AUTH_RESPONSE and all subsequent S→C packets will be ARC4-encrypted.
    // BackendSession must have _backendCrypt ready before any backend bytes arrive.
    if (_backend)
        _backend->InitCrypt(sessionKey);

    // Now forward CMSG_AUTH_SESSION to the backend (was deferred in HandleAuthSessionIntercepted).
    if (_backend && !_pendingAuthSession.empty())
    {
        _backend->SendRaw(_pendingAuthSession);
        _pendingAuthSession.clear();
    }

    ResumeAfterAuth();
}

void ProxySocket::OnBackendConnected()
{
    // Backend TCP handshake is established — now safe to read from the client.
    // The backend is ready to receive CMSG_AUTH_SESSION, so no packet will be dropped.
    _asyncReadActive = true;
    AsyncRead();
}

void ProxySocket::ResumeAfterAuth()
{
    // Resume reading from the client — next packets will be encrypted.
    _asyncReadActive = true;
    AsyncRead();
}

void ProxySocket::RerouteToBackend(std::string const& address, uint16 port,
                                   uint32 mapId, float x, float y, float z, float ori)
{
    if (_rerouting)
    {
        LOG_WARN("proxy", "ProxySocket: RerouteToBackend called while reroute already in progress");
        return;
    }

    if (_playerGuid == 0)
    {
        LOG_ERROR("proxy", "ProxySocket: RerouteToBackend called but player GUID unknown");
        return;
    }

    LOG_INFO("proxy", "ProxySocket: Rerouting GUID {:016X} to {}:{} map={}", _playerGuid, address, port, mapId);
    _rerouting = true;

    bool isLoginReroute = !_clientInWorld; // mapId==0 is Eastern Kingdoms, not "no map"

    // Native in-world reroute: send SMSG_NEW_WORLD to client now so the loading screen
    // appears immediately.  Mirrors VB.NET ClientTransfer() → SendNewWorld() flow.
    if (!isLoginReroute)
    {
        // Build SMSG_NEW_WORLD (0x03E): mapId(4) + x(4) + y(4) + z(4) + ori(4) = 20 bytes payload
        // Header: size(2 BE) + opcode(2 LE) = 4 bytes total header for small packets.
        constexpr uint16 SMSG_NEW_WORLD_OPCODE = 0x03E;
        constexpr uint16 PAYLOAD_SIZE = 20;
        constexpr uint16 SIZE_FIELD   = 2 + PAYLOAD_SIZE; // S→C size field = 2-byte opcode + payload

        uint8 plainHeader[4];
        plainHeader[0] = static_cast<uint8>(SIZE_FIELD >> 8);   // big-endian size high
        plainHeader[1] = static_cast<uint8>(SIZE_FIELD & 0xFF); // big-endian size low
        plainHeader[2] = static_cast<uint8>(SMSG_NEW_WORLD_OPCODE & 0xFF);
        plainHeader[3] = static_cast<uint8>((SMSG_NEW_WORLD_OPCODE >> 8) & 0xFF);

        auto floatBytes = [](float v) -> std::array<uint8, 4> {
            uint32 bits; std::memcpy(&bits, &v, 4);
            return { static_cast<uint8>(bits & 0xFF),
                     static_cast<uint8>((bits >> 8) & 0xFF),
                     static_cast<uint8>((bits >> 16) & 0xFF),
                     static_cast<uint8>((bits >> 24) & 0xFF) };
        };

        uint8 payload[20];
        payload[0] = static_cast<uint8>(mapId & 0xFF);
        payload[1] = static_cast<uint8>((mapId >> 8) & 0xFF);
        payload[2] = static_cast<uint8>((mapId >> 16) & 0xFF);
        payload[3] = static_cast<uint8>((mapId >> 24) & 0xFF);
        auto bx = floatBytes(x);   std::copy(bx.begin(), bx.end(), payload + 4);
        auto by = floatBytes(y);   std::copy(by.begin(), by.end(), payload + 8);
        auto bz = floatBytes(z);   std::copy(bz.begin(), bz.end(), payload + 12);
        auto bo = floatBytes(ori); std::copy(bo.begin(), bo.end(), payload + 16);

        MessageBuffer payloadBuf(PAYLOAD_SIZE);
        payloadBuf.Write(payload, PAYLOAD_SIZE);
        QueuePacketForClient(plainHeader, 4, payloadBuf);

        LOG_DEBUG("proxy", "ProxySocket: Sent SMSG_NEW_WORLD to GUID {:016X} map={} pos=({:.1f},{:.1f},{:.1f},{:.2f})",
                  _playerGuid, mapId, x, y, z, ori);

        // Arm the worldport ACK → PLAYER_LOGIN translation.
        _translateWorldportAck = true;

        // Immediately close the old backend — stops the source WorldServer from sending
        // stale packets to the client while it is on the loading screen.
        if (_backend)
        {
            LOG_DEBUG("proxy", "ProxySocket: Closing source backend for GUID {:016X} (native reroute)", _playerGuid);
            _backend->Close();
            _backend = nullptr;
        }
    }

    // Save destination so the reconnecting ProxySocket can recover.
    if (!isLoginReroute)
        sProxyMgr.RegisterPendingWorldport(_accountId, {_playerGuid, address, port, mapId, x, y, z, ori});

    _pendingBackend = std::make_shared<BackendSession>(
        sProxySocketMgr.GetIoContext(),
        shared_from_this(),
        _accountName, _sessionKey, _realmId, _playerGuid,
        GetRemoteIpAddress().to_string(),
        /*isLoginReroute=*/ isLoginReroute);

    _pendingBackend->Connect(address, port);
}

void ProxySocket::OnRerouteComplete(std::shared_ptr<BackendSession> newBackend)
{
    LOG_INFO("proxy", "ProxySocket: Reroute complete for GUID {:016X} — switching backend", _playerGuid);

    // Close old backend and replace with the new one.
    if (_backend)
        _backend->Close();

    _backend        = std::move(newBackend);
    _pendingBackend = nullptr;
    _rerouting      = false;

    // Resume reading from the client — but only if no read is already pending.
    // If _asyncReadActive is true, a previously-posted async_read_some is still outstanding.
    // It will fire soon (with _rerouting now false) and process client data normally.
    // Posting a second concurrent async_read_some against the same _readBuffer write pointer
    // corrupts framing: both reads share the same write slot and overwrite each other's data.
    if (!_asyncReadActive)
    {
        _asyncReadActive = true;
        AsyncRead();
    }
}

void ProxySocket::QueuePacketForClient(uint8 const* plainHeader, std::size_t headerLen, MessageBuffer& payload)
{
    // Re-encrypt the plaintext header for the client direction.
    // WoW 3.3.5a server headers are 4 or 5 bytes; anything else is a protocol error.
    uint8 encHeader[5];
    if (headerLen == 0 || headerLen > sizeof(encHeader))
    {
        LOG_ERROR("proxy", "ProxySocket: QueuePacketForClient — invalid headerLen {}", headerLen);
        return;
    }
    std::memcpy(encHeader, plainHeader, headerLen);

    if (_clientCrypt.IsInitialized())
        _clientCrypt.EncryptSend(encHeader, headerLen);

    // Build and queue the outgoing buffer.
    std::size_t payloadLen = payload.GetActiveSize();
    MessageBuffer outBuf(headerLen + payloadLen);
    outBuf.Write(encHeader, headerLen);
    if (payloadLen > 0)
        outBuf.Write(payload.GetReadPointer(), payloadLen);

    std::size_t totalBytes = headerLen + payloadLen;
    QueuePacket(std::move(outBuf));
    if (_backendNodeId)
        sProxyMgr.AddNodeTraffic(_backendNodeId, 0, totalBytes);
}
