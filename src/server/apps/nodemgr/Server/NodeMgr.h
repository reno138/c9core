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

#ifndef NodeMgr_h__
#define NodeMgr_h__

#include "Define.h"
#include <boost/process/v1.hpp>
#include <chrono>
#include <string>

namespace bp = boost::process::v1;

/**
 * @brief Manages the lifecycle of a single worldserver child process.
 *
 * NodeMgr is responsible for spawning, monitoring, and stopping the
 * worldserver binary. It is owned by Main.cpp and called from ProxyLink
 * when the proxy sends MSG_NODE_START or MSG_NODE_STOP.
 */
class NodeMgr
{
public:
    NodeMgr() = default;
    ~NodeMgr();

    // Non-copyable
    NodeMgr(NodeMgr const&) = delete;
    NodeMgr& operator=(NodeMgr const&) = delete;

    /// Configure from command-line / nodemgr.conf.
    /// If useGdb is true, the worldserver is spawned under GDB in batch mode so
    /// crash backtraces are written to the log file automatically.
    void Configure(std::string worldserverBin, std::string worldserverConf,
                   std::string logFile, bool useGdb = false);

    /// NodeState values matching the wire protocol.
    enum class State : uint8
    {
        Unknown  = 0,
        Stopped  = 1,
        Starting = 2,
        Running  = 3,
        Stopping = 4,
        Crashed  = 5,
    };

    /// Start the worldserver process (noop if already running).
    bool Start();

    /// Gracefully stop the worldserver process (SIGTERM, then wait up to 30s).
    void Stop();

    /// Poll worldserver health; update state. Call periodically (e.g. every 5s).
    void Poll();

    State   GetState()    const { return _state; }
    uint32  GetPid()      const { return _pid; }
    uint32  GetUptime()   const;

private:
    std::string _worldserverBin;
    std::string _worldserverConf;
    std::string _logFile;
    bool        _useGdb{ false };

    bp::child   _child;
    State       _state{ State::Stopped };
    uint32      _pid{ 0 };
    std::chrono::steady_clock::time_point _startTime;
};

#endif // NodeMgr_h__
