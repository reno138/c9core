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
 * nodemgr connects to the proxy's NodeMgr port (default 8091),
 * registers with its configured node ID, and waits for start/stop commands.
 * It also sends a heartbeat with worldserver status every 5 seconds.
 */

#include "Banner.h"
#include "Config.h"
#include "Errors.h"
#include "GitRevision.h"
#include "Log.h"
#include "NodeMgr.h"
#include "OpenSSLCrypto.h"
#include "ProxyLink.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
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
    std::string proxyHost  = sConfigMgr->GetOption<std::string>("ProxyAddress", "127.0.0.1");
    uint16 proxyPort       = static_cast<uint16>(sConfigMgr->GetOption<int32>("ProxyNodeMgrPort", 8091));
    std::string sharedSecret = sConfigMgr->GetOption<std::string>("Management.SharedSecret", "change-me");
    uint8 configuredNodeId = static_cast<uint8>(sConfigMgr->GetOption<int32>("NodeId", 1));
    uint16 gamePort        = static_cast<uint16>(sConfigMgr->GetOption<int32>("WorldServerPort", 8086));

    std::string worldserverBin  = sConfigMgr->GetOption<std::string>("WorldserverBin", "./worldserver");
    std::string worldserverConf = sConfigMgr->GetOption<std::string>("WorldserverConfig", "./worldserver.conf");
    std::string worldserverLog  = sConfigMgr->GetOption<std::string>("WorldserverLog", "/tmp/worldserver-node.log");
    bool        useGdb          = sConfigMgr->GetOption<bool>("NodeMgr.UseGdb", false);

    if (useGdb)
        LOG_INFO("server.nodemgr", "nodemgr: GDB mode ENABLED — crash backtraces will appear in {}", worldserverLog);

    // ── Create NodeMgr (process manager) ──────────────────────────────────────
    NodeMgr nodeMgr;
    nodeMgr.Configure(worldserverBin, worldserverConf, worldserverLog, useGdb);

    LOG_INFO("server.nodemgr", "nodemgr: node_id={} proxy={}:{} worldserver={}",
             configuredNodeId, proxyHost, proxyPort, worldserverBin);

    // ── Create IO context and ProxyLink ───────────────────────────────────────
    boost::asio::io_context ioCtx;

    auto link = std::make_shared<ProxyLink>(ioCtx, nodeMgr);
    link->Start(proxyHost, proxyPort, sharedSecret, configuredNodeId, gamePort);

    // ── Signal handling ────────────────────────────────────────────────────────
    boost::asio::signal_set signals(ioCtx, SIGINT, SIGTERM);
    signals.async_wait([&ioCtx, &nodeMgr](boost::system::error_code const& error, int)
    {
        if (!error)
        {
            LOG_INFO("server.nodemgr", "nodemgr: Shutting down...");
            nodeMgr.Stop();
            ioCtx.stop();
        }
    });

    LOG_INFO("server.nodemgr", "nodemgr: Running. Ctrl-C to stop.");
    ioCtx.run();

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
