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
 * @file nodemgr/Main.cpp
 * @brief nodemgr daemon — manages a single worldserver process.
 *
 * nodemgr starts the worldserver automatically (after an optional startup
 * delay) and monitors it for crashes.  On crash it restarts with exponential
 * backoff up to NodeMgr.MaxRestarts consecutive failures before giving up.
 *
 * No proxy connection is required.  Cross-node coordination is handled
 * directly by the worldservers via NATS (see ClusterServer.NatsUrl config).
 */

#include "Banner.h"
#include "Config.h"
#include "Errors.h"
#include "GitRevision.h"
#include "Log.h"
#include "NodeMgr.h"
#include "NodeMgrControl.h"
#include "ClusterAuth.h"
#include "OpenSSLCrypto.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#ifndef _ACORE_NODEMGR_CONFIG
#define _ACORE_NODEMGR_CONFIG "nodemgr.conf"
#endif

using namespace boost::program_options;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile);

int main(int argc, char** argv)
{
    signal(SIGABRT, &Acore::AbortHandler);

    auto configFile = fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_NODEMGR_CONFIG));
    auto vm = GetConsoleArguments(argc, argv, configFile);

    if (vm.count("help") || vm.count("version"))
        return 0;

    sConfigMgr->Configure(configFile.generic_string(), std::vector<std::string>(argv, argv + argc));

    if (!sConfigMgr->LoadAppConfigs())
    {
        std::cerr << "nodemgr: Failed to load config file " << configFile << "\n";
        return 1;
    }

    sLog->Initialize(nullptr);

    Acore::Banner::Show("nodemgr",
        [](std::string_view text) { LOG_INFO("server.nodemgr", text); },
        []()
        {
            LOG_INFO("server.nodemgr", "> Config: {}", sConfigMgr->GetFilename());
            LOG_INFO("server.nodemgr", "> SSL:    {}", OPENSSL_VERSION_TEXT);
            LOG_INFO("server.nodemgr", "> Boost:  {}.{}.{}",
                     BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    OpenSSLCrypto::threadsSetup();
    std::shared_ptr<void> opensslHandle(nullptr, [](void*) { OpenSSLCrypto::threadsCleanup(); });

    // ── Read config ────────────────────────────────────────────────────────────
    std::string worldserverBin  = sConfigMgr->GetOption<std::string>("WorldserverBin",    "./worldserver");
    std::string worldserverConf = sConfigMgr->GetOption<std::string>("WorldserverConfig", "./worldserver.conf");
    std::string worldserverLog  = sConfigMgr->GetOption<std::string>("WorldserverLog",    "/tmp/worldserver-node.log");
    bool        useGdb          = sConfigMgr->GetOption<bool>("NodeMgr.UseGdb",           false);

    // Seconds to wait before the first worldserver launch (useful when bringing
    // up multiple nodes simultaneously so NATS is ready first).
    int startupDelaySecs = sConfigMgr->GetOption<int32>("NodeMgr.StartupDelay", 3);

    // Maximum consecutive crash restarts before giving up (0 = unlimited).
    int maxRestarts = sConfigMgr->GetOption<int32>("NodeMgr.MaxRestarts", 10);

    // Seconds to wait after SIGTERM before escalating to SIGKILL.
    int killTimeoutSecs = sConfigMgr->GetOption<int32>("NodeMgr.KillTimeout", 15);

    // Remote control channel (clustermgr -> this supervisor).
    uint8       ctlNodeId = static_cast<uint8>(sConfigMgr->GetOption<int32>("NodeMgr.NodeId", 0));
    std::string ctlNatsUrl = sConfigMgr->GetOption<std::string>("NodeMgr.NatsUrl", "");
    std::string ctlAuthKey = sConfigMgr->GetOption<std::string>("NodeMgr.AuthKey", "");

    if (useGdb)
        LOG_INFO("server.nodemgr", "nodemgr: GDB mode ENABLED — crash backtraces will appear in {}", worldserverLog);

    // ── Create NodeMgr (process manager) ──────────────────────────────────────
    NodeMgr nodeMgr;
    nodeMgr.Configure(worldserverBin, worldserverConf, worldserverLog, useGdb,
                      static_cast<uint32>(killTimeoutSecs));

    // ── Remote control channel ────────────────────────────────────────────────
    // Optional: if NodeMgr.NatsUrl/NodeId/AuthKey are unset the supervisor still
    // runs, it just cannot be driven remotely. We do NOT fail closed here — a
    // misconfigured control channel must never prevent a node from starting.
    NodeMgrControl nodeCtl;
    if (!ctlNatsUrl.empty() && ctlNodeId != 0)
    {
        if (!ClusterAuth::Init(ctlAuthKey))
        {
            LOG_ERROR("server.nodemgr",
                      "nodemgr: NodeMgr.AuthKey missing or shorter than {} bytes — remote control DISABLED. "
                      "Generate one with: openssl rand -hex 32",
                      ClusterAuth::MIN_KEY_BYTES);
        }
        else if (nodeCtl.Start(ctlNatsUrl, ctlNodeId, &nodeMgr))
        {
            LOG_INFO("server.nodemgr", "nodemgr: remote control active on cluster.nodemgr.{}", ctlNodeId);
        }
    }
    else
    {
        LOG_INFO("server.nodemgr", "nodemgr: remote control not configured (NodeMgr.NatsUrl / NodeMgr.NodeId unset)");
    }

    LOG_INFO("server.nodemgr", "nodemgr: worldserver={} startupDelay={}s maxRestarts={}",
             worldserverBin, startupDelaySecs, maxRestarts);

    // ── IO context and timers ─────────────────────────────────────────────────
    boost::asio::io_context ioCtx;

    // Crash-restart state
    int  consecutiveCrashes = 0;
    bool giveUp             = false;

    // Steady poll timer — fires every 5s to check worldserver health.
    auto pollTimer = std::make_shared<boost::asio::steady_timer>(ioCtx);

    // Forward-declare so the lambda can schedule itself recursively.
    std::function<void()> schedulePoll;
    schedulePoll = [&]()
    {
        pollTimer->expires_after(5s);
        pollTimer->async_wait([&](boost::system::error_code const& ec)
        {
            if (ec) return; // cancelled (shutdown)

            nodeMgr.Poll();

            // Heartbeat the supervisor's own view of the world. This is what
            // lets the UI tell "worldserver hung" (supervisor Running, but the
            // worldserver stopped publishing mgmt.status) from "node down".
            nodeCtl.PublishStatus();

            NodeMgr::State state = nodeMgr.GetState();

            if (state == NodeMgr::State::Crashed)
            {
                ++consecutiveCrashes;
                int backoffSecs = std::min(1 << std::min(consecutiveCrashes - 1, 6), 60); // 1,2,4,8,16,32,60

                if (maxRestarts > 0 && consecutiveCrashes > maxRestarts)
                {
                    LOG_ERROR("server.nodemgr",
                              "nodemgr: worldserver crashed {} times — giving up. Restart nodemgr manually.",
                              consecutiveCrashes);
                    giveUp = true;
                    ioCtx.stop();
                    return;
                }

                LOG_WARN("server.nodemgr",
                         "nodemgr: worldserver crashed (#{}) — restarting in {}s",
                         consecutiveCrashes, backoffSecs);

                auto restartTimer = std::make_shared<boost::asio::steady_timer>(ioCtx);
                restartTimer->expires_after(std::chrono::seconds(backoffSecs));
                restartTimer->async_wait([&nodeMgr, &consecutiveCrashes, restartTimer](boost::system::error_code const& ec2)
                {
                    if (!ec2)
                    {
                        LOG_INFO("server.nodemgr", "nodemgr: Restarting worldserver (attempt #{})...", consecutiveCrashes);
                        nodeMgr.Start();
                    }
                });
            }
            else if (state == NodeMgr::State::Stopped && consecutiveCrashes > 0)
            {
                // Clean exit after a crash-restart cycle — treat as clean, reset counter.
                LOG_INFO("server.nodemgr", "nodemgr: worldserver stopped cleanly — resetting crash counter");
                consecutiveCrashes = 0;
            }
            else if (state == NodeMgr::State::Running)
            {
                consecutiveCrashes = 0; // reset after stable run
            }

            if (!giveUp)
                schedulePoll();
        });
    };

    // ── Signal handling ────────────────────────────────────────────────────────
    boost::asio::signal_set signals(ioCtx, SIGINT, SIGTERM);
    signals.async_wait([&ioCtx, &nodeMgr, &pollTimer](boost::system::error_code const& error, int)
    {
        if (!error)
        {
            LOG_INFO("server.nodemgr", "nodemgr: Shutting down...");
            pollTimer->cancel();
            nodeMgr.Stop();
            ioCtx.stop();
        }
    });

    // ── Initial startup ────────────────────────────────────────────────────────
    auto startTimer = std::make_shared<boost::asio::steady_timer>(ioCtx);
    startTimer->expires_after(std::chrono::seconds(startupDelaySecs));
    startTimer->async_wait([&nodeMgr, &schedulePoll, startTimer](boost::system::error_code const& ec)
    {
        if (ec) return;
        LOG_INFO("server.nodemgr", "nodemgr: Starting worldserver...");
        nodeMgr.Start();
        schedulePoll();
    });

    LOG_INFO("server.nodemgr", "nodemgr: Running. Ctrl-C to stop.");
    ioCtx.run();

    if (giveUp)
        LOG_ERROR("server.nodemgr", "nodemgr: Exited due to too many crashes.");
    else
        LOG_INFO("server.nodemgr", "nodemgr: Stopped.");

    return 0;
}

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile)
{
    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("version,v", "print version build info")
        ("config,c", value<fs::path>(&configFile)->default_value(
            fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_NODEMGR_CONFIG))),
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
