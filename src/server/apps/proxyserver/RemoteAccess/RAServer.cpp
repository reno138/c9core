/*
 * This file is part of the C9Core Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY, to the extent permitted by law; without even the implied
 * warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include "RAServer.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include "NatsBus.h"
#include "ProxyMgr.h"
#include "SRP6.h"
#include "Util.h"
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <thread>

using tcp = boost::asio::ip::tcp;
using namespace std::chrono_literals;

// ── Pending command registry ──────────────────────────────────────────────────

std::shared_ptr<PendingRACommand> RAServer::CreatePending(uint32 reqId, int expected)
{
    auto cmd = std::make_shared<PendingRACommand>();
    cmd->expected = expected;
    std::lock_guard<std::mutex> lk(_pendingMtx);
    _pending[reqId] = cmd;
    return cmd;
}

void RAServer::RemovePending(uint32 reqId)
{
    std::lock_guard<std::mutex> lk(_pendingMtx);
    _pending.erase(reqId);
}

void RAServer::AppendReply(uint8 nodeId, uint32 reqId, std::string const& text, int totalNodes)
{
    std::shared_ptr<PendingRACommand> cmd;
    {
        std::lock_guard<std::mutex> lk(_pendingMtx);
        auto it = _pending.find(reqId);
        if (it == _pending.end())
            return; // timed out and removed already
        cmd = it->second;
    }

    std::lock_guard<std::mutex> lk(cmd->mtx);
    if (cmd->done)
        return;

    // Prefix with [NodeN] if we expect replies from more than one node.
    if (totalNodes > 1 && !text.empty())
    {
        cmd->output += "[Node" + std::to_string(nodeId) + "] ";
        // Indent continuation lines.
        std::string prefixed = text;
        std::string pfx = "\n[Node" + std::to_string(nodeId) + "] ";
        size_t pos = 0;
        while ((pos = prefixed.find('\n', pos)) != std::string::npos)
        {
            if (pos + 1 < prefixed.size()) // don't add prefix after trailing newline
                prefixed.replace(pos, 1, pfx);
            pos += pfx.size();
        }
        cmd->output += prefixed;
        if (cmd->output.empty() || cmd->output.back() != '\n')
            cmd->output += '\n';
    }
    else if (!text.empty())
    {
        cmd->output += text;
    }

    ++cmd->received;
    if (cmd->received >= cmd->expected)
    {
        cmd->done = true;
        cmd->cv.notify_all();
    }
}

// ── Per-session helpers ───────────────────────────────────────────────────────

static void SessionSend(tcp::socket& sock, std::string_view data)
{
    boost::system::error_code ec;
    boost::asio::write(sock, boost::asio::buffer(data), ec);
}

static std::string SessionReadLine(tcp::socket& sock, boost::asio::streambuf& buf)
{
    boost::system::error_code ec;
    boost::asio::read_until(sock, buf, "\r\n", ec);
    if (ec)
        return {};

    std::istream is(&buf);
    std::string line;
    std::getline(is, line);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

static bool CheckAccessLevel(std::string const& user, int minLevel)
{
    std::string safe = user;
    Utf8ToUpperOnlyLatin(safe);

    auto* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_ACCOUNT_ACCESS);
    stmt->SetData(0, safe);
    PreparedQueryResult result = LoginDatabase.Query(stmt);
    if (!result)
        return false;
    Field* f = result->Fetch();
    return f[1].Get<uint8>() >= static_cast<uint8>(minLevel) && f[2].Get<int32>() == -1;
}

static bool CheckPassword(std::string const& user, std::string const& pass)
{
    std::string su = user, sp = pass;
    Utf8ToUpperOnlyLatin(su);
    Utf8ToUpperOnlyLatin(sp);
    std::transform(su.begin(), su.end(), su.begin(), ::toupper);
    std::transform(sp.begin(), sp.end(), sp.begin(), ::toupper);

    auto* stmt = LoginDatabase.GetPreparedStatement(LOGIN_SEL_CHECK_PASSWORD_BY_NAME);
    stmt->SetData(0, su);
    PreparedQueryResult result = LoginDatabase.Query(stmt);
    if (!result)
        return false;

    auto salt     = (*result)[0].Get<Binary, Acore::Crypto::SRP6::SALT_LENGTH>();
    auto verifier = (*result)[1].Get<Binary, Acore::Crypto::SRP6::VERIFIER_LENGTH>();
    return Acore::Crypto::SRP6::CheckLogin(su, sp, salt, verifier);
}

// ── Session thread ────────────────────────────────────────────────────────────

static void RunSession(tcp::socket sock, int minLevel)
{
    // Telnet negotiation: wait briefly for negotiation bytes, consume them, send IAC DONT.
    for (int i = 0; i < 10 && sock.available() == 0; ++i)
        std::this_thread::sleep_for(100ms);
    if (sock.available() > 0)
    {
        char buf[256];
        boost::system::error_code ec;
        sock.read_some(boost::asio::buffer(buf), ec);
        uint8 const reply[2] = {0xFF, 0xF0};
        boost::asio::write(sock, boost::asio::buffer(reply), ec);
    }

    boost::asio::streambuf readBuf;

    SessionSend(sock, "Authentication Required\r\n");
    SessionSend(sock, "Username: ");
    std::string username = SessionReadLine(sock, readBuf);
    if (username.empty()) return;

    SessionSend(sock, "Password: ");
    std::string password = SessionReadLine(sock, readBuf);
    if (password.empty()) return;

    if (!CheckAccessLevel(username, minLevel) || !CheckPassword(username, password))
    {
        SessionSend(sock, "Authentication failed\r\n");
        return;
    }

    LOG_INFO("commands.ra", "Proxy RA: user {} authenticated from {}",
             username, sock.remote_endpoint().address().to_string());

    for (;;)
    {
        SessionSend(sock, "AC>");
        std::string command = SessionReadLine(sock, readBuf);

        if (command.empty() || command == "quit" || command == "exit" || command == "logout")
        {
            SessionSend(sock, "Bye\r\n");
            break;
        }

        LOG_INFO("commands.ra", "Proxy RA: user {} command: {}", username, command);

        int nodeCount = sProxyMgr.GetRegisteredNodeCount();
        if (nodeCount == 0)
        {
            SessionSend(sock, "No cluster nodes connected.\r\n");
            continue;
        }

        uint32 reqId = sRAServer.AllocReqId();
        auto pending = sRAServer.CreatePending(reqId, nodeCount);

        sNatsBus.BroadcastRACommand(reqId, command);

        // Wait up to 5 seconds for all nodes to reply.
        {
            std::unique_lock<std::mutex> lk(pending->mtx);
            pending->cv.wait_for(lk, 5s, [&] { return pending->done; });
            pending->done = true; // mark done even if timed out
        }

        sRAServer.RemovePending(reqId);

        if (pending->output.empty())
            SessionSend(sock, "(no output)\r\n");
        else
            SessionSend(sock, pending->output);
    }
}

// ── Accept loop ───────────────────────────────────────────────────────────────

void RAServer::AcceptLoop(std::string bindIp, uint16 port)
{
    int minLevel = sConfigMgr->GetOption<int32>("Ra.MinLevel", 3);

    try
    {
        boost::asio::io_context ioc;
        tcp::acceptor acceptor(ioc,
            tcp::endpoint(boost::asio::ip::make_address(bindIp), port));

        acceptor.set_option(tcp::acceptor::reuse_address(true));
        LOG_INFO("server.proxyserver", "Proxy RA listening on {}:{}", bindIp, port);

        while (_running)
        {
            tcp::socket sock(ioc);
            boost::system::error_code ec;
            acceptor.accept(sock, ec);
            if (ec)
                break;

            std::thread([s = std::move(sock), minLevel]() mutable {
                try { RunSession(std::move(s), minLevel); }
                catch (std::exception const& e)
                {
                    LOG_DEBUG("commands.ra", "Proxy RA session exception: {}", e.what());
                }
            }).detach();
        }
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("server.proxyserver", "Proxy RA accept loop error: {}", e.what());
    }
}

void RAServer::Start(std::string const& bindIp, uint16 port)
{
    if (!sConfigMgr->GetOption<bool>("Proxy.Ra.Enable", true))
        return;

    _running = true;
    _thread = std::thread(&RAServer::AcceptLoop, this, bindIp, port);
}

void RAServer::Stop()
{
    _running = false;
    // Unblock pending commands so session threads can exit cleanly.
    std::lock_guard<std::mutex> lk(_pendingMtx);
    for (auto& [id, cmd] : _pending)
    {
        std::lock_guard<std::mutex> clk(cmd->mtx);
        cmd->done = true;
        cmd->cv.notify_all();
    }
}
