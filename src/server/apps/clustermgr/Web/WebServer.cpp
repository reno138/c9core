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

#include "WebServer.h"
#include "StaticAssets.h"
#include "Log.h"
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/asio/post.hpp>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace fs    = std::filesystem;

// ── JSON helpers ──────────────────────────────────────────────────────────────

static std::string JsonStr(std::string const& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out += '"';
    for (char c : s)
    {
        if      (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else                out += c;
    }
    out += '"';
    return out;
}

static std::string NodeToJson(NodeInfo const& n,
                              std::vector<NodeSample> const* samples = nullptr,
                              std::vector<CrashEvent>  const* crashes = nullptr)
{
    std::ostringstream j;
    j << "{"
      << "\"nodeId\":"     << static_cast<int>(n.nodeId)      << ","
      << "\"state\":"      << static_cast<int>(n.state)       << ","
      << "\"playerCount\":" << n.playerCount                   << ","
      << "\"maxPlayers\":" << n.maxPlayers                    << ","
      << "\"pid\":"        << n.pid                           << ","
      << "\"uptimeSecs\":" << n.uptimeSecs                    << ","
      << "\"memUsageMB\":" << n.memUsageMB                    << ","
      << "\"cpuPercent\":" << static_cast<int>(n.cpuPercent)  << ","
      << "\"crashCount\":" << n.crashCount                    << ","
      << "\"txBps\":"      << n.txBps                         << ","
      << "\"rxBps\":"      << n.rxBps                         << ","
      << "\"address\":"    << JsonStr(n.address)               << ","
      << "\"port\":"       << n.port                          << ","
      << "\"mapIds\":["    ;
    for (std::size_t i = 0; i < n.mapIds.size(); ++i)
    {
        if (i) j << ',';
        j << n.mapIds[i];
    }
    j << "]";

    if (samples)
    {
        j << ",\"history\":[";
        for (std::size_t i = 0; i < samples->size(); ++i)
        {
            auto const& s = (*samples)[i];
            if (i) j << ',';
            j << "{\"t\":" << s.timestampSec
              << ",\"p\":" << s.playerCount
              << ",\"m\":" << s.memUsageMB
              << ",\"c\":" << static_cast<int>(s.cpuPercent) << "}";
        }
        j << "]";
    }

    if (crashes)
    {
        j << ",\"crashes\":[";
        for (std::size_t i = 0; i < crashes->size(); ++i)
        {
            auto const& c = (*crashes)[i];
            if (i) j << ',';
            j << "{\"t\":" << c.timestampSec
              << ",\"u\":" << c.uptimeSecs
              << ",\"p\":" << c.playerCount << "}";
        }
        j << "]";
    }

    j << "}";
    return j.str();
}

static std::string PlayerToJson(PlayerInfo const& p)
{
    std::ostringstream j;
    j << "{"
      << "\"guid\":"    << p.guid                          << ","
      << "\"nodeId\":"  << static_cast<int>(p.nodeId)      << ","
      << "\"mapId\":"   << p.mapId                         << ","
      << "\"x\":"       << p.x                             << ","
      << "\"y\":"       << p.y                             << ","
      << "\"z\":"       << p.z                             << ","
      << "\"zoneId\":"  << p.zoneId                        << ","
      << "\"level\":"   << static_cast<int>(p.level)       << ","
      << "\"classId\":" << static_cast<int>(p.classId)     << ","
      << "\"raceId\":"  << static_cast<int>(p.raceId)      << ","
      << "\"teamId\":"  << static_cast<int>(p.teamId)      << ","
      << "\"name\":"    << JsonStr(p.name)
      << "}";
    return j.str();
}

// ── HTTP session (one per connection) ─────────────────────────────────────────

class HttpSession : public std::enable_shared_from_this<HttpSession>
{
public:
    HttpSession(tcp::socket socket, std::shared_ptr<WsHub> hub,
                std::shared_ptr<NatsMonitor> monitor,
                std::shared_ptr<HistoryStore> history,
                std::string tilesPath)
        : _stream(std::move(socket))
        , _hub(std::move(hub))
        , _monitor(std::move(monitor))
        , _history(std::move(history))
        , _tilesPath(std::move(tilesPath))
    {}

    void Run()
    {
        DoRead();
    }

private:
    void DoRead()
    {
        _req = {};
        beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
        http::async_read(_stream, _buf, _req,
            [self = shared_from_this()](beast::error_code ec, std::size_t)
            {
                if (!ec) self->HandleRequest();
                // else: connection closed
            });
    }

    void HandleRequest()
    {
        std::string target = std::string(_req.target());

        // Strip query string
        auto qpos = target.find('?');
        if (qpos != std::string::npos)
            target = target.substr(0, qpos);

        // WebSocket upgrade?
        if (websocket::is_upgrade(_req) && target == "/ws")
        {
            auto ws = std::make_shared<WsSession>(
                beast::get_lowest_layer(_stream).release_socket(), _hub);
            ws->Run(std::move(_req));
            return;
        }

        // REST / static routes
        if (_req.method() == http::verb::get)
        {
            if (target == "/" || target == "/index.html")
                return SendString(std::string(HTML_INDEX), "text/html");

            if (target == "/api/status")
                return HandleApiStatus();

            if (target == "/api/players")
                return HandleApiPlayers();

            if (target.rfind("/api/nodes/", 0) == 0)
                return HandleApiNode(target.substr(11));

            if (target.rfind("/tiles/", 0) == 0)
                return HandleTile(target.substr(7));
        }

        if (_req.method() == http::verb::post && target == "/api/deploy")
            return HandleApiDeploy();

        // 404
        SendError(http::status::not_found, "Not found");
    }

    void HandleApiStatus()
    {
        auto nodes = _monitor->GetNodes();
        std::ostringstream j;
        j << "{\"nodes\":[";
        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (i) j << ',';
            auto samples = _history->GetSamples(nodes[i].nodeId);
            auto crashes = _history->GetCrashes(nodes[i].nodeId);
            j << NodeToJson(nodes[i], &samples, &crashes);
        }
        j << "]}";
        SendString(j.str(), "application/json");
    }

    void HandleApiNode(std::string const& idStr)
    {
        uint8 nodeId = static_cast<uint8>(std::stoi(idStr));
        auto nodes = _monitor->GetNodes();
        for (auto const& n : nodes)
        {
            if (n.nodeId == nodeId)
            {
                auto samples = _history->GetSamples(nodeId);
                auto crashes = _history->GetCrashes(nodeId);
                SendString(NodeToJson(n, &samples, &crashes), "application/json");
                return;
            }
        }
        SendError(http::status::not_found, "Node not found");
    }

    void HandleApiPlayers()
    {
        auto players = _monitor->GetPlayers();
        std::ostringstream j;
        j << "{\"players\":[";
        for (std::size_t i = 0; i < players.size(); ++i)
        {
            if (i) j << ',';
            j << PlayerToJson(players[i]);
        }
        j << "]}";
        SendString(j.str(), "application/json");
    }

    void HandleApiDeploy()
    {
        // Parse minimal JSON body to extract host for logging.
        std::string body = _req.body();
        LOG_INFO("clustermgr.web", "WebServer: Deploy request body length={}", body.size());
        // Return a stub success for now — full SSH deploy wired to DeployWizard is a follow-up.
        SendString(R"({"success":true,"message":"Deploy queued — connect to TUI (F4) for full wizard."})",
                   "application/json");
    }

    void HandleTile(std::string const& tileSubpath)
    {
        // tileSubpath = "{continent}/{z}/{x}/{y}.png"
        if (_tilesPath.empty())
        {
            SendError(http::status::not_found, "Tiles directory not configured (Map.TilesPath)");
            return;
        }

        fs::path tilePath = fs::path(_tilesPath) / tileSubpath;

        // Safety: no path traversal
        auto canon = fs::weakly_canonical(tilePath);
        auto base  = fs::weakly_canonical(_tilesPath);
        std::string canonStr = canon.string();
        std::string baseStr  = base.string();
        if (canonStr.rfind(baseStr, 0) != 0)
        {
            SendError(http::status::forbidden, "Forbidden");
            return;
        }

        std::ifstream tileFile(tilePath, std::ios::binary);
        if (!tileFile)
        {
            // Return a 1×1 transparent PNG rather than 404 so Leaflet renders empty tiles cleanly.
            static constexpr uint8 TRANSPARENT_PNG[] = {
                0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a, 0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
                0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01, 0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
                0x89,0x00,0x00,0x00,0x0b,0x49,0x44,0x41, 0x54,0x78,0x9c,0x62,0x00,0x01,0x00,0x00,
                0x05,0x00,0x01,0x0d,0x0a,0x2d,0xb4,0x00, 0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,
                0x42,0x60,0x82
            };
            SendRaw(std::string(reinterpret_cast<char const*>(TRANSPARENT_PNG), sizeof(TRANSPARENT_PNG)),
                    "image/png");
            return;
        }

        std::string tileData((std::istreambuf_iterator<char>(tileFile)),
                              std::istreambuf_iterator<char>());
        SendRaw(std::move(tileData), "image/png");
    }

    template<class Body>
    void Send(http::response<Body>&& res)
    {
        res.set(http::field::server, "C9Core-ClusterMgr");
        res.set(http::field::access_control_allow_origin, "*");
        res.prepare_payload();

        auto sp = std::make_shared<http::response<Body>>(std::move(res));
        beast::get_lowest_layer(_stream).expires_after(std::chrono::seconds(30));
        http::async_write(_stream, *sp,
            [self = shared_from_this(), sp](beast::error_code ec, std::size_t)
            {
                beast::get_lowest_layer(self->_stream).socket().shutdown(
                    tcp::socket::shutdown_send, ec);
            });
    }

    void SendString(std::string body, std::string contentType)
    {
        http::response<http::string_body> res{http::status::ok, _req.version()};
        res.set(http::field::content_type, contentType + "; charset=utf-8");
        res.body() = std::move(body);
        Send(std::move(res));
    }

    void SendRaw(std::string body, std::string contentType)
    {
        http::response<http::string_body> res{http::status::ok, _req.version()};
        res.set(http::field::content_type, contentType);
        res.body() = std::move(body);
        Send(std::move(res));
    }

    void SendError(http::status status, std::string message)
    {
        http::response<http::string_body> res{status, _req.version()};
        res.set(http::field::content_type, "text/plain; charset=utf-8");
        res.body() = std::move(message);
        Send(std::move(res));
    }

    beast::tcp_stream                         _stream;
    beast::flat_buffer                        _buf;
    http::request<http::string_body>          _req;
    std::shared_ptr<WsHub>                    _hub;
    std::shared_ptr<NatsMonitor>              _monitor;
    std::shared_ptr<HistoryStore>             _history;
    std::string                               _tilesPath;
};

// ── WebServer ─────────────────────────────────────────────────────────────────

WebServer::WebServer(std::shared_ptr<NatsMonitor> monitor,
                     std::shared_ptr<HistoryStore> history,
                     std::string tilesPath,
                     uint16 port,
                     std::string bindAddr)
    : _monitor(std::move(monitor))
    , _history(std::move(history))
    , _tilesPath(std::move(tilesPath))
    , _port(port)
    , _bindAddr(std::move(bindAddr))
    , _acceptor(_ioc)
    , _hub(std::make_shared<WsHub>(_ioc))
{
}

WebServer::~WebServer()
{
    Stop();
}

void WebServer::Start()
{
    try
    {
        auto endpoint = tcp::endpoint{net::ip::make_address(_bindAddr),
                                      static_cast<uint16_t>(_port)};
        _acceptor.open(endpoint.protocol());
        _acceptor.set_option(net::socket_base::reuse_address(true));
        _acceptor.bind(endpoint);
        _acceptor.listen(net::socket_base::max_listen_connections);
        DoAccept();
        LOG_INFO("clustermgr", "WebServer: Listening on {}:{}", _bindAddr, _port);
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("clustermgr", "WebServer: Failed to start — {}", e.what());
        return;
    }

    _thread = std::thread([this]() { Run(); });
}

void WebServer::Stop()
{
    _ioc.stop();
    if (_thread.joinable())
        _thread.join();
}

void WebServer::Run()
{
    _ioc.run();
}

void WebServer::DoAccept()
{
    _acceptor.async_accept(
        net::make_strand(_ioc),
        [this](beast::error_code ec, tcp::socket socket)
        {
            if (!ec)
            {
                auto session = std::make_shared<HttpSession>(
                    std::move(socket), _hub, _monitor, _history, _tilesPath);
                session->Run();
            }
            DoAccept();
        });
}

// ── Push helpers (called from NatsMonitor callbacks) ─────────────────────────

void WebServer::OnNodeUpdate(std::vector<NodeInfo> const& nodes)
{
    std::ostringstream j;
    j << "{\"type\":\"status\",\"nodes\":[";
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        if (i) j << ',';
        auto samples = _history->GetSamples(nodes[i].nodeId);
        auto crashes = _history->GetCrashes(nodes[i].nodeId);
        j << NodeToJson(nodes[i], &samples, &crashes);
    }
    j << "]}";

    // Post the broadcast to the io_context thread so WS writes are safe.
    net::post(_ioc, [hub = _hub, msg = j.str()]()
    {
        hub->Broadcast(msg);
    });
}

void WebServer::OnPlayersUpdate(std::vector<PlayerInfo> const& players)
{
    std::ostringstream j;
    j << "{\"type\":\"players\",\"players\":[";
    for (std::size_t i = 0; i < players.size(); ++i)
    {
        if (i) j << ',';
        j << PlayerToJson(players[i]);
    }
    j << "]}";

    net::post(_ioc, [hub = _hub, msg = j.str()]()
    {
        hub->Broadcast(msg);
    });
}

void WebServer::OnCrash(uint8 nodeId, CrashEvent const& event)
{
    std::ostringstream j;
    j << "{\"type\":\"crash\""
      << ",\"nodeId\":"     << static_cast<int>(nodeId)
      << ",\"uptimeSecs\":" << event.uptimeSecs
      << ",\"playerCount\":" << event.playerCount
      << ",\"timestampSec\":" << event.timestampSec << "}";

    net::post(_ioc, [hub = _hub, msg = j.str()]()
    {
        hub->Broadcast(msg);
    });
}
