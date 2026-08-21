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
#include "BoostProcess.h"
#include <chrono>
#include <string>

/**
 * @brief Manages the lifecycle of a single worldserver child process.
 *
 * NodeMgr is responsible for spawning, monitoring, and stopping the
 * worldserver binary. It is owned by Main.cpp and started automatically
 * after NodeMgr.StartupDelay seconds.  Crash detection and restart backoff
 * are handled by the poll loop in Main.cpp.
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
                   std::string logFile, bool useGdb = false,
                   uint32 killTimeoutSecs = 15);

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

    /// Gracefully stop the worldserver process: SIGTERM, escalating to SIGKILL
    /// after NodeMgr.KillTimeout seconds. The escalation is what makes this
    /// usable against a HUNG worldserver — a process wedged in a game-loop
    /// deadlock never services SIGTERM, and the previous implementation would
    /// sit in Stopping forever.
    void Stop();

    /// Immediate SIGKILL, no grace period. For the operator "kill" action when
    /// the node is known-hung and waiting out the timeout serves no purpose.
    void Kill();

    /// Stop (with escalation) then Start once the child has actually reaped.
    /// Only latches intent; Poll() performs the restart when the child exits.
    void Restart();

    /// Poll worldserver health; update state. Call periodically (e.g. every 5s).
    void Poll();

    /// True while a Restart() is waiting for the child to exit.
    [[nodiscard]] bool IsRestartPending() const { return _restartPending; }

    State   GetState()    const { return _state; }
    uint32  GetPid()      const { return _pid; }
    uint32  GetUptime()   const;

private:
    /// When Stop() sent SIGTERM; Poll() escalates to SIGKILL after _killTimeoutSecs.
    std::chrono::steady_clock::time_point _stopRequestedAt{};
    bool   _sigkillSent{ false };
    bool   _restartPending{ false };
    uint32 _killTimeoutSecs{ 15 };

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
