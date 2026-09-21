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

#include "WsHub.h"
#include "Log.h"
#include <algorithm>
#include <boost/asio/post.hpp>
#include <boost/beast/http.hpp>

namespace http = beast::http;

// ── WsHub ─────────────────────────────────────────────────────────────────────

WsHub::WsHub(net::io_context& ioc)
    : _ioc(ioc)
{
}

void WsHub::Join(std::shared_ptr<WsSession> session)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.insert(session);
}

void WsHub::Leave(std::shared_ptr<WsSession> session)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sessions.erase(session);
}

void WsHub::Broadcast(std::string message)
{
    // Copy session set under lock, then send outside lock to avoid deadlocks.
    std::vector<std::shared_ptr<WsSession>> snap;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        snap.assign(_sessions.begin(), _sessions.end());
    }
    for (auto const& s : snap)
        s->Send(message);
}

// ── WsSession ─────────────────────────────────────────────────────────────────

WsSession::WsSession(tcp::socket socket, std::shared_ptr<WsHub> hub)
    : _ws(std::move(socket))
    , _hub(std::move(hub))
{
}

void WsSession::Run(http::request<http::string_body> req)
{
    // Accept the WebSocket upgrade asynchronously.
    _ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    _ws.set_option(websocket::stream_base::decorator([](websocket::response_type& res)
    {
        res.set(http::field::server, "C9Core-ClusterMgr");
    }));

    _ws.async_accept(req,
        [self = shared_from_this()](beast::error_code ec)
        {
            self->OnAccept(ec);
        });
}

void WsSession::OnAccept(beast::error_code ec)
{
    if (ec)
    {
        LOG_DEBUG("clustermgr.web", "WsSession::OnAccept error: {}", ec.message());
        return;
    }
    _hub->Join(shared_from_this());
    DoRead();
}

void WsSession::DoRead()
{
    _ws.async_read(_readBuf,
        [self = shared_from_this()](beast::error_code ec, std::size_t bytes)
        {
            self->OnRead(ec, bytes);
        });
}

void WsSession::OnRead(beast::error_code ec, std::size_t /*bytes*/)
{
    if (ec == websocket::error::closed || ec == net::error::eof ||
        ec == net::error::connection_reset)
    {
        _hub->Leave(shared_from_this());
        return;
    }
    if (ec)
    {
        LOG_DEBUG("clustermgr.web", "WsSession read error: {}", ec.message());
        _hub->Leave(shared_from_this());
        return;
    }
    _readBuf.consume(_readBuf.size());  // discard client messages
    DoRead();
}

void WsSession::Send(std::string message)
{
    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(_sendMutex);
        if (_queuedBytes + message.size() > MAX_QUEUED_BYTES)
        {
            LOG_WARN("clustermgr.web", "WsSession: client not draining ({} bytes queued) — dropping connection",
                     _queuedBytes);
            _sendQueue.clear();
            _queuedBytes = 0;
            overflow = true;
        }
        else
        {
            _queuedBytes += message.size();
            _sendQueue.push_back(std::move(message));
            if (_writing)
                return;
            _writing = true;
        }
    }

    if (overflow)
    {
        // Slow client: leave the hub and close. The outstanding async_write
        // (if any) completes with an error and drops the last reference.
        _hub->Leave(shared_from_this());
        beast::error_code ec;
        beast::get_lowest_layer(_ws).socket().close(ec);
        return;
    }

    DoWrite();
}

void WsSession::DoWrite()
{
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(_sendMutex);
        if (_sendQueue.empty())
        {
            _writing = false;
            return;
        }
        msg = std::move(_sendQueue.front());
        _sendQueue.erase(_sendQueue.begin());
        _queuedBytes -= std::min(_queuedBytes, msg.size());
    }

    _ws.text(true);
    auto buf = std::make_shared<std::string>(std::move(msg));
    _ws.async_write(net::buffer(*buf),
        [self = shared_from_this(), buf](beast::error_code ec, std::size_t)
        {
            if (ec)
            {
                LOG_DEBUG("clustermgr.web", "WsSession write error: {}", ec.message());
                self->_hub->Leave(self);
                return;
            }
            self->DoWrite();
        });
}
