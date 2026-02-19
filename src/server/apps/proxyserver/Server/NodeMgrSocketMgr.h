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

#ifndef NodeMgrSocketMgr_h__
#define NodeMgrSocketMgr_h__

#include "NodeMgrSocket.h"
#include "SocketMgr.h"

/**
 * @brief Accepts incoming connections from nodemgr daemons on the management port (default 8091).
 *
 * Each accepted connection becomes a NodeMgrSocket which handles PSK authentication
 * and the nodemgr control protocol (start/stop worldserver, status updates).
 */
class NodeMgrSocketMgr final : public SocketMgr<NodeMgrSocket>
{
    typedef SocketMgr<NodeMgrSocket> BaseSocketMgr;

public:
    static NodeMgrSocketMgr& Instance()
    {
        static NodeMgrSocketMgr instance;
        return instance;
    }

    bool StartNetwork(Acore::Asio::IoContext& ioContext,
                      std::string const& bindIp,
                      uint16 port,
                      int threadCount = 1) override
    {
        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&NodeMgrSocketMgr::OnSocketAccept>();
        return true;
    }

    NetworkThread<NodeMgrSocket>* CreateThreads() const override
    {
        return new NetworkThread<NodeMgrSocket>[1];
    }

    static void OnSocketAccept(IoContextTcpSocket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::move(sock), threadIndex);
    }
};

#define sNodeMgrSocketMgr NodeMgrSocketMgr::Instance()

#endif // NodeMgrSocketMgr_h__
