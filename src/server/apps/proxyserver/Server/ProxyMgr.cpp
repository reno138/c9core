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

#include "ProxyMgr.h"
#include "Log.h"
#include "ProxySocket.h"

void ProxyMgr::RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions[guid] = socket;
    LOG_DEBUG("proxy", "ProxyMgr: Registered GUID {:016X} ({} total sessions)",
              guid, _sessions.size());
}

void ProxyMgr::UnregisterSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    _sessions.erase(guid);
    LOG_DEBUG("proxy", "ProxyMgr: Unregistered GUID {:016X} ({} total sessions)",
              guid, _sessions.size());
}

std::shared_ptr<ProxySocket> ProxyMgr::GetSession(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_sessionMutex);
    auto it = _sessions.find(guid);
    if (it == _sessions.end())
        return nullptr;

    auto socket = it->second.lock();
    if (!socket)
        _sessions.erase(it); // stale entry

    return socket;
}

void ProxyMgr::ReroutePlayer(uint64 guid, std::string const& address, uint16 port)
{
    auto socket = GetSession(guid);
    if (!socket)
    {
        LOG_WARN("proxy", "ProxyMgr: ReroutePlayer — GUID {:016X} not found (player not connected?)",
                 guid);
        return;
    }

    LOG_INFO("proxy", "ProxyMgr: Rerouting GUID {:016X} to {}:{}", guid, address, port);

    socket->RerouteToBackend(address, port);
}
