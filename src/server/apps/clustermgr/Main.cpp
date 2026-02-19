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
 * @brief clustermgr — ncurses TUI for C9Core cluster management.
 *
 * Connects to the proxy's management port (default 9090) using a PSK-encrypted
 * channel, subscribes to node status updates, and presents a Midnight Commander-
 * style interface for monitoring and controlling cluster nodes.
 *
 * Usage:
 *   clustermgr [-c clustermgr.conf]
 */

#include "Banner.h"
#include "Config.h"
#include "Errors.h"
#include "GitRevision.h"
#include "Log.h"
#include "ManagementClient.h"
#include "ClusterUI.h"
#include "OpenSSLCrypto.h"
#include <boost/asio/io_context.hpp>
#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <memory>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>
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
            LOG_INFO("server.clustermgr", "> SSL:    {}", OPENSSL_VERSION_TEXT);
            LOG_INFO("server.clustermgr", "> Boost:  {}.{}.{}",
                     BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    OpenSSLCrypto::threadsSetup();
    std::shared_ptr<void> opensslHandle(nullptr, [](void*) { OpenSSLCrypto::threadsCleanup(); });

    // ── Read config ────────────────────────────────────────────────────────────
    std::string proxyHost    = sConfigMgr->GetOption<std::string>("ProxyAddress",         "127.0.0.1");
    uint16 proxyPort         = static_cast<uint16>(sConfigMgr->GetOption<int32>("ProxyManagementPort", 9090));
    std::string sharedSecret = sConfigMgr->GetOption<std::string>("Management.SharedSecret", "change-me");

    // ── Create IO context and ManagementClient ─────────────────────────────────
    auto ioCtx = std::make_shared<boost::asio::io_context>();

    auto client = std::make_shared<ManagementClient>(*ioCtx);
    auto ui     = std::make_shared<ClusterUI>(proxyHost, proxyPort, client);

    // Wire up callbacks — these are called from the io_context thread
    ManagementClient::StatusCallback statusCb = [ui](std::vector<NodeInfo> nodes)
    {
        ui->UpdateNodes(std::move(nodes));
    };
    ManagementClient::ConnectCallback connectCb = [ui](bool connected)
    {
        ui->SetConnected(connected);
    };

    client->Start(proxyHost, proxyPort, sharedSecret,
                  std::move(statusCb), std::move(connectCb));

    // ── Launch io_context in background thread ─────────────────────────────────
    std::thread ioThread([&ioCtx]()
    {
        try
        {
            ioCtx->run();
        }
        catch (std::exception const& e)
        {
            LOG_ERROR("clustermgr", "IO thread exception: {}", e.what());
        }
    });

    // ── Run TUI (blocks main thread until user presses F10 / q) ───────────────
    LOG_INFO("server.clustermgr", "clustermgr: Starting TUI. Press F10 to quit.");
    ui->Run();

    // ── Shutdown ───────────────────────────────────────────────────────────────
    ioCtx->stop();
    ioThread.join();

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
