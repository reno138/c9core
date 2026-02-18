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

#ifndef ControlSocket_h__
#define ControlSocket_h__

#include "MessageBuffer.h"
#include "Socket.h"
#include <string>

/**
 * @brief Server-side control channel socket — accepts connections from worldserver/instance servers.
 *
 * Backends connect here (port 8090) to register themselves and send reroute commands.
 *
 * Binary protocol (little-endian, all ints):
 *
 *   MSG_REGISTER (0x01):
 *     uint8  server_type   (0 = worldserver, 1 = instance server)
 *     uint16 game_port     (port this backend listens on for game connections)
 *
 *   MSG_REROUTE_PLAYER (0x02):
 *     uint64 player_guid
 *     uint8  address_len
 *     char   address[address_len]   (NOT null-terminated)
 *     uint16 port
 */
class ControlSocket final : public Socket<ControlSocket>
{
    typedef Socket<ControlSocket> BaseSocket;

public:
    explicit ControlSocket(IoContextTcpSocket&& socket);

    void Start() override;

protected:
    SocketReadCallbackResult ReadHandler() final;

private:
    void ProcessBuffer();
    void HandleRegister(uint8 serverType, uint16 gamePort);
    void HandleReroute(uint64 guid, std::string const& address, uint16 port);

    /// Control protocol message type constants.
    static constexpr uint8 MSG_REGISTER       = 0x01;
    static constexpr uint8 MSG_REROUTE_PLAYER = 0x02;

    /// Payload sizes for fixed-length messages.
    static constexpr std::size_t REGISTER_PAYLOAD_SIZE    = 3; ///< uint8 + uint16
    static constexpr std::size_t REROUTE_PART1_SIZE       = 9; ///< uint64 + uint8

    enum class ParseState { WaitType, ReadRegister, ReadRerouteP1, ReadRerouteP2 };
    ParseState _parseState{ ParseState::WaitType };

    /// Accumulation buffer — bytes are copied here from GetReadBuffer() across calls.
    MessageBuffer _accumBuffer;

    /// State saved between ReadRerouteP1 and ReadRerouteP2.
    uint64 _rerouteGuid{ 0 };
    uint8  _rerouteAddrLen{ 0 };

    /// Server info learned from MSG_REGISTER.
    uint8  _serverType{ 0xFF };
    uint16 _gamePort{ 0 };
};

#endif // ControlSocket_h__
