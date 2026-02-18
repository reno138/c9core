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

#include "ProxyClient.h"
#include "IoContext.h"
#include "Log.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/write.hpp>
#include <chrono>

void ProxyClient::Initialize(Acore::Asio::IoContext& ioContext, std::string const& address,
                              uint16 controlPort, uint8 serverType, uint16 gamePort)
{
    _ioContext   = &static_cast<boost::asio::io_context&>(ioContext);
    _address     = address;
    _controlPort = controlPort;
    _serverType  = serverType;
    _gamePort    = gamePort;

    _socket        = std::make_unique<boost::asio::ip::tcp::socket>(*_ioContext);
    _resolver      = std::make_unique<boost::asio::ip::tcp::resolver>(*_ioContext);
    _reconnectTimer = std::make_unique<boost::asio::steady_timer>(*_ioContext);

    Connect();
}

void ProxyClient::Connect()
{
    if (!_socket || !_resolver)
        return;

    LOG_INFO("server.worldserver", "ProxyClient: Connecting to proxy control channel at {}:{}...",
             _address, _controlPort);

    _resolver->async_resolve(_address, std::to_string(_controlPort),
        [this](boost::system::error_code const& error, boost::asio::ip::tcp::resolver::results_type results)
        {
            if (error)
            {
                LOG_WARN("server.worldserver", "ProxyClient: Resolve failed: {} — retrying in 10s",
                         error.message());
                ScheduleReconnect();
                return;
            }

            boost::asio::async_connect(*_socket, results,
                [this](boost::system::error_code const& connectError, boost::asio::ip::tcp::endpoint const&)
                {
                    OnConnect(connectError);
                });
        });
}

void ProxyClient::OnConnect(boost::system::error_code const& error)
{
    if (error)
    {
        LOG_WARN("server.worldserver", "ProxyClient: Connection failed: {} — retrying in 10s",
                 error.message());
        ScheduleReconnect();
        return;
    }

    _connected = true;
    LOG_INFO("server.worldserver", "ProxyClient: Connected to proxy control channel.");
    SendRegister();
}

void ProxyClient::SendRegister()
{
    // MSG_REGISTER: 0x01 | uint8 server_type | uint16 game_port (LE)
    std::vector<uint8> msg(4);
    msg[0] = MSG_REGISTER;
    msg[1] = _serverType;
    msg[2] = static_cast<uint8>(_gamePort & 0xFF);
    msg[3] = static_cast<uint8>(_gamePort >> 8);

    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _sendQueue.push(std::move(msg));
    }

    AsyncWrite();
}

void ProxyClient::SendReroute(uint64 playerGuid, std::string const& address, uint16 port)
{
    if (!_connected)
    {
        LOG_WARN("server.worldserver", "ProxyClient: SendReroute called but not connected");
        return;
    }

    uint8 addrLen = static_cast<uint8>(std::min(address.size(), std::size_t(255)));

    // MSG_REROUTE_PLAYER: 0x02 | uint64 guid | uint8 addr_len | addr | uint16 port
    std::vector<uint8> msg;
    msg.reserve(1 + 8 + 1 + addrLen + 2);
    msg.push_back(MSG_REROUTE_PLAYER);

    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<uint8>((playerGuid >> (i * 8)) & 0xFF));

    msg.push_back(addrLen);
    msg.insert(msg.end(), address.begin(), address.begin() + addrLen);
    msg.push_back(static_cast<uint8>(port & 0xFF));
    msg.push_back(static_cast<uint8>(port >> 8));

    bool wasEmpty;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        wasEmpty = _sendQueue.empty();
        _sendQueue.push(std::move(msg));
    }

    if (wasEmpty)
        AsyncWrite();
}

void ProxyClient::AsyncWrite()
{
    std::vector<uint8> front;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_sendQueue.empty() || _writing)
            return;
        front = _sendQueue.front();
        _writing = true;
    }

    boost::asio::async_write(*_socket, boost::asio::buffer(front),
        [this, front](boost::system::error_code const& error, std::size_t /*transferred*/)
        {
            if (error)
            {
                LOG_WARN("server.worldserver", "ProxyClient: Write error: {} — reconnecting",
                         error.message());
                _connected = false;
                _writing   = false;
                ScheduleReconnect();
                return;
            }

            {
                std::lock_guard<std::mutex> lock(_queueMutex);
                _sendQueue.pop();
                _writing = false;
            }

            AsyncWrite();
        });
}

void ProxyClient::ScheduleReconnect()
{
    _connected = false;
    _socket->close();

    _reconnectTimer->expires_after(std::chrono::seconds(10));
    _reconnectTimer->async_wait(
        [this](boost::system::error_code const& error)
        {
            if (!error)
                Connect();
        });
}
