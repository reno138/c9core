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

NodeMgr::~NodeMgr()
{
    if (_child.running())
    {
        _child.terminate();
        _child.wait();
    }
}

void NodeMgr::Configure(std::string worldserverBin, std::string worldserverConf,
                        std::string logFile, bool useGdb)
{
    _worldserverBin  = std::move(worldserverBin);
    _worldserverConf = std::move(worldserverConf);
    _logFile         = std::move(logFile);
    _useGdb          = useGdb;
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

    _state = State::Stopping;
    LOG_INFO("nodemgr", "NodeMgr: Sending SIGTERM to worldserver PID={}", _pid);

#if defined(_WIN32) || defined(_WIN64)
    _child.terminate();
#else
    ::kill(static_cast<pid_t>(_child.id()), SIGTERM);
#endif
}

void NodeMgr::Poll()
{
    if (_state == State::Stopped || _state == State::Unknown)
        return;

    if (!_child.valid())
    {
        _state = State::Stopped;
        _pid   = 0;
        return;
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
        _pid = 0;
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
