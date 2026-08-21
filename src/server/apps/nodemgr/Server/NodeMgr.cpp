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

#include "NodeMgr.h"
#include "Log.h"
#include "BoostProcess.h"
#include <csignal>
#include <chrono>

NodeMgr::~NodeMgr()
{
    if (_child.running())
    {
        _child.terminate();
        _child.wait();
    }
}

void NodeMgr::Configure(std::string worldserverBin, std::string worldserverConf,
                        std::string logFile, bool useGdb, uint32 killTimeoutSecs)
{
    _worldserverBin  = std::move(worldserverBin);
    _worldserverConf = std::move(worldserverConf);
    _logFile         = std::move(logFile);
    _useGdb          = useGdb;
    _killTimeoutSecs = killTimeoutSecs ? killTimeoutSecs : 15;
}

bool NodeMgr::Start()
{
    if (_state == State::Running || _state == State::Starting)
    {
        LOG_WARN("nodemgr", "NodeMgr::Start — worldserver already running (state={})",
                 static_cast<int>(_state));
        return false;
    }

    try
    {
        if (_useGdb)
        {
            // Launch under GDB in batch mode: crash backtraces are written to the log.
            // Command: gdb -batch -ex run -ex 'bt full' --args <bin> -c <conf> >> <log> 2>&1
            std::string gdbCmd =
                "gdb -batch -ex run -ex 'bt full' --args \""
                + _worldserverBin + "\" -c \"" + _worldserverConf
                + "\" >> \"" + _logFile + "\" 2>&1";

            _child = bp::child(
                bp::search_path("bash"),
                std::vector<std::string>{"-c", gdbCmd},
                bp::std_in < bp::null
            );
            LOG_INFO("nodemgr", "NodeMgr: Launched worldserver under GDB PID={}", _child.id());
        }
        else
        {
            // Normal launch: stdout to log file, stderr discarded, stdin from /dev/null.
            _child = bp::child(
                _worldserverBin,
                "-c", _worldserverConf,
                bp::std_out > _logFile,
                bp::std_err > bp::null,
                bp::std_in  < bp::null
            );
            LOG_INFO("nodemgr", "NodeMgr: Launched worldserver PID={} ({})",
                     _child.id(), _worldserverBin);
        }

        _pid       = static_cast<uint32>(_child.id());
        _state     = State::Starting;
        _startTime = std::chrono::steady_clock::now();
        return true;
    }
    catch (std::exception const& e)
    {
        LOG_ERROR("nodemgr", "NodeMgr: Failed to launch worldserver: {}", e.what());
        _state = State::Crashed;
        return false;
    }
}

void NodeMgr::Stop()
{
    if (_state == State::Stopped || _state == State::Stopping)
        return;

    if (!_child.running())
    {
        _state = State::Stopped;
        _pid   = 0;
        return;
    }

    _state           = State::Stopping;
    _stopRequestedAt = std::chrono::steady_clock::now();
    _sigkillSent     = false;
    LOG_INFO("nodemgr", "NodeMgr: Sending SIGTERM to worldserver PID={} (SIGKILL in {}s if it does not exit)",
             _pid, _killTimeoutSecs);

#if defined(_WIN32) || defined(_WIN64)
    _child.terminate();
#else
    ::kill(static_cast<pid_t>(_child.id()), SIGTERM);
#endif
}

void NodeMgr::Kill()
{
    if (!_child.valid() || !_child.running())
    {
        LOG_INFO("nodemgr", "NodeMgr::Kill — no running worldserver to kill");
        _state = State::Stopped;
        _pid   = 0;
        return;
    }

    _state           = State::Stopping;
    _stopRequestedAt = std::chrono::steady_clock::now();
    _sigkillSent     = true;
    LOG_WARN("nodemgr", "NodeMgr: Sending SIGKILL to worldserver PID={} (operator kill)", _pid);

#if defined(_WIN32) || defined(_WIN64)
    _child.terminate();
#else
    ::kill(static_cast<pid_t>(_child.id()), SIGKILL);
#endif
}

void NodeMgr::Restart()
{
    LOG_INFO("nodemgr", "NodeMgr: Restart requested (state={})", static_cast<int>(_state));
    _restartPending = true;

    if (_child.valid() && _child.running())
        Stop();          // Poll() starts the new process once this one reaps
    // If it is already down, Poll() picks up _restartPending immediately.
}

void NodeMgr::Poll()
{
    // A pending restart must be serviced even from Stopped/Unknown, otherwise a
    // Restart() issued against an already-dead process would never fire.
    if (_state == State::Stopped || _state == State::Unknown)
    {
        if (_restartPending)
        {
            _restartPending = false;
            LOG_INFO("nodemgr", "NodeMgr: restart — starting worldserver");
            Start();
        }
        return;
    }

    if (!_child.valid())
    {
        _state = State::Stopped;
        _pid   = 0;
        if (_restartPending)
        {
            _restartPending = false;
            Start();
        }
        return;
    }

    // SIGTERM escalation. A worldserver wedged in a deadlock never reaches its
    // signal handler, so without this the supervisor sits in Stopping forever
    // and the operator has no in-band way to recover the node.
    if (_state == State::Stopping && !_sigkillSent && _child.running())
    {
        auto waited = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _stopRequestedAt).count();
        if (waited >= static_cast<long long>(_killTimeoutSecs))
        {
            _sigkillSent = true;
            LOG_WARN("nodemgr",
                     "NodeMgr: worldserver PID={} ignored SIGTERM for {}s — escalating to SIGKILL",
                     _pid, waited);
#if defined(_WIN32) || defined(_WIN64)
            _child.terminate();
#else
            ::kill(static_cast<pid_t>(_child.id()), SIGKILL);
#endif
        }
    }

    if (!_child.running())
    {
        int exitCode = -1;
        try { _child.wait(); exitCode = _child.exit_code(); } catch (...) {}

        if (_state == State::Stopping)
        {
            LOG_INFO("nodemgr", "NodeMgr: worldserver exited cleanly (exit={})", exitCode);
            _state = State::Stopped;
        }
        else
        {
            LOG_ERROR("nodemgr", "NodeMgr: worldserver CRASHED (exit={})", exitCode);
            _state = State::Crashed;
        }
        _pid         = 0;
        _sigkillSent = false;

        if (_restartPending)
        {
            _restartPending = false;
            LOG_INFO("nodemgr", "NodeMgr: restart — child reaped, starting worldserver");
            Start();
        }
    }
    else if (_state == State::Starting)
    {
        // Consider it "running" after it has been alive for 5s.
        auto up = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _startTime).count();
        if (up >= 5)
        {
            _state = State::Running;
            LOG_INFO("nodemgr", "NodeMgr: worldserver PID={} is now RUNNING (uptime={}s)", _pid, up);
        }
    }
}

uint32 NodeMgr::GetUptime() const
{
    if (_state == State::Stopped || _state == State::Unknown || _state == State::Crashed)
        return 0;
    return static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - _startTime).count());
}
