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

#ifndef ManagementSocketMgr_h__
#define ManagementSocketMgr_h__

#include "ManagementSocket.h"
#include "SocketMgr.h"

/**
 * @brief Accepts incoming connections from clustermgr TUI clients on the management port (default 9090).
 *
 * Each accepted connection becomes a ManagementSocket which handles PSK authentication
 * and the cluster management protocol (status subscribe, node start/stop commands).
 */
class ManagementSocketMgr final : public SocketMgr<ManagementSocket>
{
    typedef SocketMgr<ManagementSocket> BaseSocketMgr;

public:
    static ManagementSocketMgr& Instance()
    {
        static ManagementSocketMgr instance;
        return instance;
    }

    bool StartNetwork(Acore::Asio::IoContext& ioContext,
                      std::string const& bindIp,
                      uint16 port,
                      int threadCount = 1) override
    {
        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&ManagementSocketMgr::OnSocketAccept>();
        return true;
    }

    NetworkThread<ManagementSocket>* CreateThreads() const override
    {
        return new NetworkThread<ManagementSocket>[1];
    }

    static void OnSocketAccept(IoContextTcpSocket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::move(sock), threadIndex);
    }
};

#define sManagementSocketMgr ManagementSocketMgr::Instance()

#endif // ManagementSocketMgr_h__
