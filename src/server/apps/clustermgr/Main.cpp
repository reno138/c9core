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

/**
 * @file clustermgr/Main.cpp
 * @brief clustermgr — ncurses TUI + embedded web UI for C9Core cluster management.
 *
 * Architecture:
 *   - NatsMonitor subscribes to cluster.mgmt.status + cluster.mgmt.players
 *   - HistoryStore retains time-series samples (default 1 hour @ 5 s)
 *   - WebServer (Boost.Beast, port 9191) serves the HTML5 SPA + REST/WS API
 *   - ClusterUI (ncurses) runs on the main thread; updated via NatsMonitor callbacks
 *
 * Usage:
 *   clustermgr [-c clustermgr.conf]
 */

#include "Banner.h"
#include "Config.h"
#include "Errors.h"
#include "GitRevision.h"
#include "Log.h"
#include "NatsMonitor.h"
#include "HistoryStore.h"
#include "WebServer.h"
#include "ClusterUI.h"
#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <chrono>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

#ifndef _ACORE_CLUSTERMGR_CONFIG
#define _ACORE_CLUSTERMGR_CONFIG "clustermgr.conf"
#endif

using namespace boost::program_options;
namespace fs = std::filesystem;

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile);

int main(int argc, char** argv)
{
    signal(SIGABRT, &Acore::AbortHandler);

    auto configFile = fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_CLUSTERMGR_CONFIG));
    auto vm = GetConsoleArguments(argc, argv, configFile);

    if (vm.count("help") || vm.count("version"))
        return 0;

    sConfigMgr->Configure(configFile.generic_string(), std::vector<std::string>(argv, argv + argc));

    if (!sConfigMgr->LoadAppConfigs())
    {
        std::cerr << "clustermgr: Failed to load config file " << configFile << "\n";
        return 1;
    }

    sLog->Initialize(nullptr);

    Acore::Banner::Show("clustermgr",
        [](std::string_view text) { LOG_INFO("server.clustermgr", text); },
        []()
        {
            LOG_INFO("server.clustermgr", "> Config: {}", sConfigMgr->GetFilename());
            LOG_INFO("server.clustermgr", "> Boost:  {}.{}.{}",
                     BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    // ── Read config ────────────────────────────────────────────────────────────
    std::string natsUrl      = sConfigMgr->GetOption<std::string>("ClusterServer.NatsUrl", "nats://127.0.0.1:4222");
    bool        webEnabled   = sConfigMgr->GetOption<bool>("Web.Enabled", true);
    uint16      webPort      = static_cast<uint16>(sConfigMgr->GetOption<int32>("Web.Port",     9191));
    std::string webBind      = sConfigMgr->GetOption<std::string>("Web.BindAddr", "0.0.0.0");
    std::string tilesPath    = sConfigMgr->GetOption<std::string>("Map.TilesPath", "");
    uint32      retentionSec = static_cast<uint32>(sConfigMgr->GetOption<int32>("History.RetentionSeconds", 3600));
    uint32      deadThresh   = static_cast<uint32>(sConfigMgr->GetOption<int32>("ClusterServer.NodeDeadThreshold", 30));

    // MgmtStatusInterval tells us how many samples per hour.
    uint32 statusIntervalSec = static_cast<uint32>(sConfigMgr->GetOption<int32>("ClusterServer.MgmtStatusInterval", 5));
    std::size_t maxSamples = (statusIntervalSec > 0) ? (retentionSec / statusIntervalSec) : 720;

    // ── Construct core objects ─────────────────────────────────────────────────
    auto history = std::make_shared<HistoryStore>(maxSamples);
    auto monitor = std::make_shared<NatsMonitor>();

    // ── Create WebServer (optional) ────────────────────────────────────────────
    std::unique_ptr<WebServer> webServer;
    if (webEnabled)
        webServer = std::make_unique<WebServer>(monitor, history, tilesPath, webPort, webBind);

    // ── Create TUI ────────────────────────────────────────────────────────────
    auto ui = std::make_shared<ClusterUI>(natsUrl, monitor);

    // ── Wire NatsMonitor callbacks ────────────────────────────────────────────
    //
    // These lambdas run on NATS internal threads — all UI / WebServer calls
    // are thread-safe (mutex-protected or posted to io_context).
    //
    NatsMonitor::StatusCallback statusCb = [&](std::vector<NodeInfo> nodes)
    {
        // Record history samples.
        uint32 nowSec = static_cast<uint32>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        for (auto const& n : nodes)
        {
            // Detect RUNNING → CRASHED transition for crash log.
            // We infer this from state == 5 (watchdog set it).
            // Look up previous state from history — if we just got a new "state=5"
            // sample after previous samples, this is a new crash.
            // For simplicity, record a crash whenever state flips to 5 and the
            // most recent sample had playerCount > 0 or uptimeSecs > 0.
            if (n.state == 5)
            {
                auto prev = history->GetSamples(n.nodeId);
                bool wasCrash = prev.empty() || prev.back().playerCount > 0 ||
                                (prev.size() > 1 && prev.back().playerCount == 0 && n.uptimeSecs == 0);
                // Record crash only once (when we get the first crashed sample).
                if (!prev.empty() || wasCrash)
                {
                    CrashEvent ce;
                    ce.timestampSec = nowSec;
                    ce.uptimeSecs   = n.uptimeSecs;
                    ce.playerCount  = n.playerCount;
                    auto crashes = history->GetCrashes(n.nodeId);
                    // Avoid duplicate crash entries within 30s.
                    bool isDup = !crashes.empty() && (nowSec - crashes.back().timestampSec < 30);
                    if (!isDup)
                    {
                        history->RecordCrash(n.nodeId, ce);
                        if (webServer)
                            webServer->OnCrash(n.nodeId, ce);
                    }
                }
            }
            else if (n.state == 3)
            {
                NodeSample s;
                s.timestampSec = nowSec;
                s.playerCount  = n.playerCount;
                s.memUsageMB   = n.memUsageMB;
                s.cpuPercent   = n.cpuPercent;
                history->RecordSample(n.nodeId, s);
            }
        }

        ui->UpdateNodes(nodes);
        ui->SetConnected(monitor->IsConnected());

        if (webServer)
            webServer->OnNodeUpdate(nodes);
    };

    monitor->Start(natsUrl, std::move(statusCb), deadThresh);

    if (webServer)
        webServer->Start();

    // ── Run TUI (blocks main thread until user presses F10 / q) ───────────────
    LOG_INFO("server.clustermgr", "clustermgr: Starting TUI. Press F10 to quit.");
    LOG_INFO("server.clustermgr", "clustermgr: Web UI at http://{}:{}/", webBind == "0.0.0.0" ? "localhost" : webBind, webPort);

    ui->Run();

    // ── Shutdown ───────────────────────────────────────────────────────────────
    if (webServer)
        webServer->Stop();

    LOG_INFO("server.clustermgr", "clustermgr: Exited cleanly.");
    return 0;
}

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile)
{
    options_description all("Allowed options");
    all.add_options()
        ("help,h",    "print usage message")
        ("version,v", "print version build info")
        ("config,c",  value<fs::path>(&configFile)->default_value(
            fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_CLUSTERMGR_CONFIG))),
            "use <arg> as configuration file");

    variables_map variablesMap;
    try
    {
        store(command_line_parser(argc, argv).options(all).allow_unregistered().run(), variablesMap);
        notify(variablesMap);
    }
    catch (std::exception const& e)
    {
        std::cerr << e.what() << "\n";
    }

    if (variablesMap.count("help"))
        std::cout << all << "\n";
    else if (variablesMap.count("version"))
        std::cout << GitRevision::GetFullVersion() << "\n";

    return variablesMap;
}
