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

#ifndef WebServer_h__
#define WebServer_h__

#include "NatsMonitor.h"
#include "HistoryStore.h"
#include "WsHub.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <memory>
#include <string>

/**
 * @brief Embedded Boost.Beast HTTP + WebSocket server.
 *
 * Runs on a background io_context (separate from ncurses main thread).
 * Exposes:
 *   GET  /                  → embedded SPA (StaticAssets.h)
 *   GET  /api/status        → JSON: all node snapshots + history
 *   GET  /api/nodes/:id     → JSON: node detail + samples + crashes
 *   GET  /api/players       → JSON: all player positions
 *   POST /api/deploy        → trigger SSH deploy (via DeployWizard logic)
 *   GET  /tiles/:cont/:z/:x/:y.png → serve local WoW map tile
 *   WS   /ws                → real-time push
 *
 * Notification flow:
 *   NatsMonitor calls OnNodeUpdate() / OnPlayersUpdate() which
 *   serialize JSON and call WsHub::Broadcast().
 */
class WebServer
{
public:
    /// @param authToken  Bearer token required on every mutating route
    ///                   (POST /api/nodes/{id}/{action}). Empty = those routes
    ///                   answer 403 and nothing can be controlled over HTTP.
    WebServer(std::shared_ptr<NatsMonitor> monitor,
              std::shared_ptr<HistoryStore> history,
              std::string tilesPath,
              uint16 port,
              std::string bindAddr = "127.0.0.1",
              std::string authToken = "");
    ~WebServer();

    /// Start listening.  Returns immediately; the server runs on its own io_context thread.
    void Start();

    /// Stop the server and join the background thread.
    void Stop();

    /// Call from the NatsMonitor StatusCallback to push JSON to all WS clients.
    void OnNodeUpdate(std::vector<NodeInfo> const& nodes);

    /// Call from the NatsMonitor players callback to push JSON to all WS clients.
    void OnPlayersUpdate(std::vector<PlayerInfo> const& players);

    /// Call when a crash is detected.
    void OnCrash(uint8 nodeId, CrashEvent const& event);

    WsHub& GetHub() { return *_hub; }

private:
    void Run();
    void DoAccept();

    std::shared_ptr<NatsMonitor>  _monitor;
    std::shared_ptr<HistoryStore> _history;
    std::string                   _tilesPath;
    uint16                        _port;
    std::string                   _bindAddr;
    std::string                   _authToken;

    net::io_context               _ioc;
    net::ip::tcp::acceptor        _acceptor;
    std::shared_ptr<WsHub>        _hub;
    std::thread                   _thread;
};

#endif // WebServer_h__
