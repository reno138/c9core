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

#ifndef RAServer_h__
#define RAServer_h__

#include "Define.h"
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

/**
 * @brief Proxy-side Remote Access server.
 *
 * Listens on Proxy.Ra.Port (default 3444) for telnet RA connections.
 * Each command is broadcast to all registered cluster nodes via NATS (MSG_RA_COMMAND).
 * Replies from nodes (MSG_RA_REPLY) are collected with a 5-second timeout and
 * returned to the RA client.  When more than one node responds, each node's output
 * is prefixed with "[NodeN] " so the operator can identify the source.
 *
 * Authentication: same credentials as worldserver RA (LoginDatabase account_access,
 * Ra.MinLevel applies from the Proxy config).
 */
struct PendingRACommand
{
    std::mutex              mtx;
    std::condition_variable cv;
    std::string             output;       ///< Accumulated output from all nodes
    int                     expected{0};  ///< Number of nodes we broadcast to
    int                     received{0};  ///< Replies received so far
    bool                    done{false};
};

class RAServer
{
public:
    static RAServer& Instance()
    {
        static RAServer instance;
        return instance;
    }

    /// Start the RA listener.  Must be called after LoginDatabase is ready.
    void Start(std::string const& bindIp, uint16 port);

    /// Stop accepting connections and drain pending commands.
    void Stop();

    /// Called from the NATS dispatch thread when MSG_RA_REPLY arrives from a node.
    void AppendReply(uint8 nodeId, uint32 reqId, std::string const& text, int totalNodes);

    /// Allocate a unique request ID for a new RA command.
    uint32 AllocReqId() { return _nextReqId.fetch_add(1, std::memory_order_relaxed); }

    /// Register a pending command (called from the RA session thread before broadcasting).
    std::shared_ptr<PendingRACommand> CreatePending(uint32 reqId, int expected);

    /// Remove a pending command after it completes or times out.
    void RemovePending(uint32 reqId);

private:
    RAServer()  = default;
    ~RAServer() = default;

    void AcceptLoop(std::string bindIp, uint16 port);

    std::atomic<uint32> _nextReqId{1};

    std::mutex _pendingMtx;
    std::unordered_map<uint32, std::shared_ptr<PendingRACommand>> _pending;

    std::thread      _thread;
    std::atomic<bool> _running{false};
};

#define sRAServer RAServer::Instance()

#endif // RAServer_h__
