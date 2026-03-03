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

#ifndef WsHub_h__
#define WsHub_h__

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class WsSession;

/**
 * @brief Thread-safe registry + broadcast hub for WebSocket sessions.
 *
 * WsSession objects register themselves on construction and unregister on close.
 * Call Broadcast() from any thread to send a text message to all connected clients.
 */
class WsHub : public std::enable_shared_from_this<WsHub>
{
public:
    explicit WsHub(net::io_context& ioc);

    /// Register a session (called from session constructor).
    void Join(std::shared_ptr<WsSession> session);

    /// Unregister a session (called on close/error).
    void Leave(std::shared_ptr<WsSession> session);

    /// Broadcast a text message to all connected WebSocket clients.
    /// Thread-safe.
    void Broadcast(std::string message);

    net::io_context& IoContext() { return _ioc; }

private:
    net::io_context& _ioc;
    std::mutex _mutex;
    std::set<std::shared_ptr<WsSession>> _sessions;
};

/**
 * @brief One WebSocket connection managed by WsHub.
 */
class WsSession : public std::enable_shared_from_this<WsSession>
{
public:
    WsSession(tcp::socket socket, std::shared_ptr<WsHub> hub);

    /// Perform the WebSocket handshake and start reading.
    void Run(beast::http::request<beast::http::string_body> req);

    /// Queue a text message for async send.
    void Send(std::string message);

private:
    void OnAccept(beast::error_code ec);
    void DoRead();
    void OnRead(beast::error_code ec, std::size_t bytes);
    void DoWrite();

    websocket::stream<beast::tcp_stream> _ws;
    std::shared_ptr<WsHub>               _hub;
    beast::flat_buffer                   _readBuf;

    std::mutex              _sendMutex;
    std::vector<std::string> _sendQueue;
    bool                     _writing{ false };
};

#endif // WsHub_h__
