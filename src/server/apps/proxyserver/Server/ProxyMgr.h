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

#ifndef ProxyMgr_h__
#define ProxyMgr_h__

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

class ProxySocket;

/**
 * @brief Central proxy manager — maps player GUIDs to their active ProxySockets.
 *
 * ProxySocket calls RegisterSession() when CMSG_PLAYER_LOGIN is intercepted
 * (giving us the GUID), and UnregisterSession() on disconnect.
 *
 * ControlSocket calls ReroutePlayer() when a backend sends MSG_REROUTE_PLAYER.
 * Phase 2: validates the request and logs it.
 * Phase 3: initiates the actual backend switch.
 */
class ProxyMgr
{
public:
    static ProxyMgr& Instance()
    {
        static ProxyMgr instance;
        return instance;
    }

    /// Register a player's ProxySocket by GUID (called after CMSG_PLAYER_LOGIN).
    void RegisterSession(uint64 guid, std::shared_ptr<ProxySocket> socket);

    /// Remove a player's registration (called when the ProxySocket is destroyed).
    void UnregisterSession(uint64 guid);

    /// Look up the ProxySocket for a given player GUID. Returns nullptr if not found.
    std::shared_ptr<ProxySocket> GetSession(uint64 guid);

    /**
     * @brief Reroute a player to a different backend server.
     *
     * Phase 2: logs the request and validates the GUID exists.
     * Phase 3: calls ProxySocket::RerouteToBackend(address, port).
     *
     * @param guid    Player GUID (from MSG_REROUTE_PLAYER).
     * @param address Target backend IP address.
     * @param port    Target backend port.
     */
    void ReroutePlayer(uint64 guid, std::string const& address, uint16 port);

private:
    ProxyMgr() = default;

    std::mutex _sessionMutex;
    std::unordered_map<uint64, std::weak_ptr<ProxySocket>> _sessions;
};

#define sProxyMgr ProxyMgr::Instance()

#endif // ProxyMgr_h__
