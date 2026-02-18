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

#include "ControlSocket.h"
#include "Log.h"
#include "ProxyMgr.h"

ControlSocket::ControlSocket(IoContextTcpSocket&& socket)
    : BaseSocket(std::move(socket))
    , _accumBuffer(512)
{
}

void ControlSocket::Start()
{
    LOG_DEBUG("proxy.control", "ControlSocket: Backend connected from {}",
              GetRemoteIpAddress().to_string());
    AsyncRead();
}

SocketReadCallbackResult ControlSocket::ReadHandler()
{
    MessageBuffer& raw = GetReadBuffer();
    std::size_t avail = raw.GetActiveSize();

    if (avail > 0)
    {
        // Compact before writing so remaining space is maximised.
        _accumBuffer.Normalize();
        _accumBuffer.EnsureFreeSpace();
        _accumBuffer.Write(raw.GetReadPointer(), avail);
        raw.ReadCompleted(avail);
    }

    ProcessBuffer();
    return SocketReadCallbackResult::KeepReading;
}

void ControlSocket::ProcessBuffer()
{
    while (_accumBuffer.GetActiveSize() > 0)
    {
        switch (_parseState)
        {
            case ParseState::WaitType:
            {
                if (_accumBuffer.GetActiveSize() < 1)
                    return;

                uint8 msgType = *_accumBuffer.GetReadPointer();
                _accumBuffer.ReadCompleted(1);

                if (msgType == MSG_REGISTER)
                    _parseState = ParseState::ReadRegister;
                else if (msgType == MSG_REROUTE_PLAYER)
                    _parseState = ParseState::ReadRerouteP1;
                else
                {
                    LOG_WARN("proxy.control", "ControlSocket: Unknown message type 0x{:02X} — closing", msgType);
                    CloseSocket();
                    return;
                }
                break;
            }

            case ParseState::ReadRegister:
            {
                if (_accumBuffer.GetActiveSize() < REGISTER_PAYLOAD_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                uint8  serverType =  p[0];
                uint16 gamePort   = static_cast<uint16>(p[1]) | (static_cast<uint16>(p[2]) << 8);
                _accumBuffer.ReadCompleted(REGISTER_PAYLOAD_SIZE);

                HandleRegister(serverType, gamePort);
                _parseState = ParseState::WaitType;
                break;
            }

            case ParseState::ReadRerouteP1:
            {
                if (_accumBuffer.GetActiveSize() < REROUTE_PART1_SIZE)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::memcpy(&_rerouteGuid, p, 8);
                _rerouteAddrLen = p[8];
                _accumBuffer.ReadCompleted(REROUTE_PART1_SIZE);

                _parseState = ParseState::ReadRerouteP2;
                break;
            }

            case ParseState::ReadRerouteP2:
            {
                std::size_t need = static_cast<std::size_t>(_rerouteAddrLen) + 2;
                if (_accumBuffer.GetActiveSize() < need)
                    return;

                uint8* p = _accumBuffer.GetReadPointer();
                std::string address(reinterpret_cast<char*>(p), _rerouteAddrLen);
                uint16 port = static_cast<uint16>(p[_rerouteAddrLen])
                            | (static_cast<uint16>(p[_rerouteAddrLen + 1]) << 8);
                _accumBuffer.ReadCompleted(need);

                HandleReroute(_rerouteGuid, address, port);

                _rerouteGuid    = 0;
                _rerouteAddrLen = 0;
                _parseState = ParseState::WaitType;
                break;
            }
        }
    }

    _accumBuffer.Normalize();
}

void ControlSocket::HandleRegister(uint8 serverType, uint16 gamePort)
{
    _serverType = serverType;
    _gamePort   = gamePort;

    char const* typeStr = (serverType == 0) ? "worldserver" : "instance server";
    LOG_INFO("proxy.control", "ControlSocket: Backend registered as {} on port {}",
             typeStr, gamePort);
}

void ControlSocket::HandleReroute(uint64 guid, std::string const& address, uint16 port)
{
    LOG_DEBUG("proxy.control", "ControlSocket: Reroute request — GUID {:016X} → {}:{}",
              guid, address, port);

    sProxyMgr.ReroutePlayer(guid, address, port);
}
