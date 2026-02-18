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

#ifndef ProxyClient_h__
#define ProxyClient_h__

#include "Define.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace Acore::Asio { class IoContext; }

/**
 * @brief Worldserver-side client for the proxy control channel.
 *
 * On startup (when ProxyServer.Enable = 1), the worldserver connects to
 * the proxy's control port, sends MSG_REGISTER, and thereafter can send
 * MSG_REROUTE_PLAYER when a player needs to be moved to the instance server.
 *
 * Binary protocol (little-endian):
 *
 *   MSG_REGISTER (0x01):
 *     uint8  server_type   0 = worldserver, 1 = instance server
 *     uint16 game_port
 *
 *   MSG_REROUTE_PLAYER (0x02):
 *     uint64 player_guid
 *     uint8  address_len
 *     char   address[address_len]
 *     uint16 port
 */
class ProxyClient
{
public:
    static ProxyClient& Instance()
    {
        static ProxyClient instance;
        return instance;
    }

    /// Call once from worldserver Main, before the world loop starts.
    void Initialize(Acore::Asio::IoContext& ioContext, std::string const& address,
                    uint16 controlPort, uint8 serverType, uint16 gamePort);

    bool IsConnected() const { return _connected; }

    /**
     * @brief Ask the proxy to reroute a player to the given backend.
     *
     * Thread-safe: may be called from any worldserver thread.
     *
     * @param playerGuid   The player's object GUID (uint64).
     * @param address      IP of the target backend (instance server).
     * @param port         Port of the target backend.
     */
    void SendReroute(uint64 playerGuid, std::string const& address, uint16 port);

private:
    ProxyClient() = default;
    ~ProxyClient() = default;

    void Connect();
    void OnConnect(boost::system::error_code const& error);
    void SendRegister();
    void AsyncWrite();
    void ScheduleReconnect();

    /// Control protocol message types.
    static constexpr uint8 MSG_REGISTER       = 0x01;
    static constexpr uint8 MSG_REROUTE_PLAYER = 0x02;

    boost::asio::io_context* _ioContext{ nullptr };
    std::unique_ptr<boost::asio::ip::tcp::socket>  _socket;
    std::unique_ptr<boost::asio::ip::tcp::resolver> _resolver;
    std::unique_ptr<boost::asio::steady_timer>      _reconnectTimer;

    std::string _address;
    uint16 _controlPort{ 0 };
    uint8  _serverType{ 0 };
    uint16 _gamePort{ 0 };

    bool _connected{ false };
    bool _writing{ false };

    std::mutex _queueMutex;
    std::queue<std::vector<uint8>> _sendQueue;
};

#define sProxyClient ProxyClient::Instance()

#endif // ProxyClient_h__
