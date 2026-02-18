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

/**
 * @file Main.cpp
 * @brief AzerothCore Proxy Server entry point.
 *
 * The proxy server sits between WoW clients and backend worldserver/instance servers.
 * Clients connect to the proxy on the public port (default 8085). The proxy maintains
 * the RC4 crypto state for the full session lifetime, routing packets transparently to
 * whichever backend server currently owns that player.
 */

#include "AppenderDB.h"
#include "Banner.h"
#include "Common.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "DatabaseLoader.h"
#include "GitRevision.h"
#include "IoContext.h"
#include "Log.h"
#include "MySQLThreading.h"
#include "OpenSSLCrypto.h"
#include "ProcessPriority.h"
#include "SharedDefines.h"
#include "ControlSocketMgr.h"
#include "ProxySocketMgr.h"
#include "SteadyTimer.h"
#include "Util.h"
#include <boost/asio/signal_set.hpp>
#include <boost/program_options.hpp>
#include <boost/version.hpp>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <openssl/crypto.h>
#include <openssl/opensslv.h>

#ifndef _ACORE_PROXY_CONFIG
#define _ACORE_PROXY_CONFIG "proxyserver.conf"
#endif

using namespace boost::program_options;
namespace fs = std::filesystem;

bool StartDB();
void StopDB();
void SignalHandler(std::weak_ptr<Acore::Asio::IoContext> ioContextRef,
                  boost::system::error_code const& error, int signalNumber);
void KeepDatabaseAliveHandler(std::weak_ptr<boost::asio::steady_timer> dbPingTimerRef,
                              int32 dbPingInterval, boost::system::error_code const& error);
variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile);

int main(int argc, char** argv)
{
    Acore::Impl::CurrentServerProcessHolder::_type = SERVER_PROCESS_AUTHSERVER; // closest existing type
    signal(SIGABRT, &Acore::AbortHandler);

    auto configFile = fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_PROXY_CONFIG));
    auto vm = GetConsoleArguments(argc, argv, configFile);

    if (vm.count("help") || vm.count("version"))
        return 0;

    sConfigMgr->Configure(configFile.generic_string(), std::vector<std::string>(argv, argv + argc));

    if (!sConfigMgr->LoadAppConfigs())
        return 1;

    sLog->RegisterAppender<AppenderDB>();
    sLog->Initialize(nullptr);

    Acore::Banner::Show("proxyserver",
        [](std::string_view text) { LOG_INFO("server.proxyserver", text); },
        []()
        {
            LOG_INFO("server.proxyserver", "> Using configuration file       {}", sConfigMgr->GetFilename());
            LOG_INFO("server.proxyserver", "> Using SSL version:             {} (library: {})", OPENSSL_VERSION_TEXT, OpenSSL_version(OPENSSL_VERSION));
            LOG_INFO("server.proxyserver", "> Using Boost version:           {}.{}.{}", BOOST_VERSION / 100000, BOOST_VERSION / 100 % 1000, BOOST_VERSION % 100);
        });

    OpenSSLCrypto::threadsSetup();
    std::shared_ptr<void> opensslHandle(nullptr, [](void*) { OpenSSLCrypto::threadsCleanup(); });

    std::string pidFile = sConfigMgr->GetOption<std::string>("PidFile", "");
    if (!pidFile.empty())
    {
        if (uint32 pid = CreatePIDFile(pidFile))
            LOG_INFO("server.proxyserver", "Daemon PID: {}\n", pid);
        else
        {
            LOG_ERROR("server.proxyserver", "Cannot create PID file {} (possible error: permission)\n", pidFile);
            return 1;
        }
    }

    if (!StartDB())
        return 1;

    std::shared_ptr<void> dbHandle(nullptr, [](void*) { StopDB(); });

    std::shared_ptr<Acore::Asio::IoContext> ioContext = std::make_shared<Acore::Asio::IoContext>();

    if (sConfigMgr->isDryRun())
    {
        LOG_INFO("server.proxyserver", "Dry run completed, terminating.");
        return 0;
    }

    int32 port = sConfigMgr->GetOption<int32>("WorldServerPort", 8085);
    if (port < 0 || port > 0xFFFF)
    {
        LOG_ERROR("server.proxyserver", "Specified port out of allowed range (1-65535)");
        return 1;
    }

    std::string bindIp = sConfigMgr->GetOption<std::string>("BindIP", "0.0.0.0");

    if (!sProxySocketMgr.StartNetwork(*ioContext, bindIp, static_cast<uint16>(port)))
    {
        LOG_ERROR("server.proxyserver", "Failed to initialize network");
        return 1;
    }

    std::shared_ptr<void> proxyNetHandle(nullptr, [](void*) { sProxySocketMgr.StopNetwork(); });

    int32 controlPort = sConfigMgr->GetOption<int32>("ControlPort", 8090);
    if (controlPort < 0 || controlPort > 0xFFFF)
    {
        LOG_ERROR("server.proxyserver", "Specified ControlPort out of allowed range (1-65535)");
        return 1;
    }

    if (!sControlSocketMgr.StartNetwork(*ioContext, bindIp, static_cast<uint16>(controlPort)))
    {
        LOG_ERROR("server.proxyserver", "Failed to initialize control channel");
        return 1;
    }

    std::shared_ptr<void> controlNetHandle(nullptr, [](void*) { sControlSocketMgr.StopNetwork(); });

    boost::asio::signal_set signals(*ioContext, SIGINT, SIGTERM);
#if AC_PLATFORM == AC_PLATFORM_WINDOWS
    signals.add(SIGBREAK);
#endif
    signals.async_wait(std::bind(&SignalHandler,
        std::weak_ptr<Acore::Asio::IoContext>(ioContext),
        std::placeholders::_1, std::placeholders::_2));

    SetProcessPriority("server.proxyserver",
        sConfigMgr->GetOption<int32>(CONFIG_PROCESSOR_AFFINITY, 0),
        sConfigMgr->GetOption<bool>(CONFIG_HIGH_PRIORITY, false));

    int32 dbPingInterval = sConfigMgr->GetOption<int32>("MaxPingTime", 30);
    std::shared_ptr<boost::asio::steady_timer> dbPingTimer =
        std::make_shared<boost::asio::steady_timer>(*ioContext);

    dbPingTimer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(dbPingInterval * MINUTE));
    dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler,
        std::weak_ptr<boost::asio::steady_timer>(dbPingTimer),
        dbPingInterval, std::placeholders::_1));

    LOG_INFO("server.proxyserver", "Proxy server listening on {}:{} (client) / {}:{} (control)",
             bindIp, port, bindIp, controlPort);
    LOG_INFO("server.proxyserver", "Backend world server: {}:{}",
        sConfigMgr->GetOption<std::string>("WorldServer.Address", "127.0.0.1"),
        sConfigMgr->GetOption<int32>("WorldServer.Port", 8086));

    ioContext->run();

    dbPingTimer->cancel();
    LOG_INFO("server.proxyserver", "Halting process...");
    signals.cancel();

    return 0;
}

bool StartDB()
{
    MySQL::Library_Init();

    DatabaseLoader loader("server.proxyserver");
    loader.AddDatabase(LoginDatabase, "Login");

    if (!loader.Load())
        return false;

    LOG_INFO("server.proxyserver", "Started auth database connection pool.");
    sLog->SetRealmId(0);
    return true;
}

void StopDB()
{
    LoginDatabase.Close();
    MySQL::Library_End();
}

void SignalHandler(std::weak_ptr<Acore::Asio::IoContext> ioContextRef,
                  boost::system::error_code const& error, int /*signalNumber*/)
{
    if (!error)
        if (std::shared_ptr<Acore::Asio::IoContext> ioContext = ioContextRef.lock())
            ioContext->stop();
}

void KeepDatabaseAliveHandler(std::weak_ptr<boost::asio::steady_timer> dbPingTimerRef,
                              int32 dbPingInterval, boost::system::error_code const& error)
{
    if (!error)
    {
        if (std::shared_ptr<boost::asio::steady_timer> dbPingTimer = dbPingTimerRef.lock())
        {
            LOG_DEBUG("sql.driver", "Ping MySQL to keep connection alive");
            LoginDatabase.KeepAlive();

            dbPingTimer->expires_at(Acore::Asio::SteadyTimer::GetExpirationTime(dbPingInterval));
            dbPingTimer->async_wait(std::bind(&KeepDatabaseAliveHandler,
                dbPingTimerRef, dbPingInterval, std::placeholders::_1));
        }
    }
}

variables_map GetConsoleArguments(int argc, char** argv, fs::path& configFile)
{
    options_description all("Allowed options");
    all.add_options()
        ("help,h", "print usage message")
        ("version,v", "print version build info")
        ("dry-run,d", "Dry run")
        ("config,c", value<fs::path>(&configFile)->default_value(
            fs::path(sConfigMgr->GetConfigPath() + std::string(_ACORE_PROXY_CONFIG))),
            "use <arg> as configuration file")
        ("config-policy", value<std::string>()->value_name("policy"),
            "override config severity policy");

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
    else if (variablesMap.count("dry-run"))
        sConfigMgr->setDryRun(true);

    return variablesMap;
}
