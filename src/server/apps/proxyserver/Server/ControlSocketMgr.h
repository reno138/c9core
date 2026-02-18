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

#ifndef ControlSocketMgr_h__
#define ControlSocketMgr_h__

#include "ControlSocket.h"
#include "NetworkThread.h"
#include "SocketMgr.h"

/**
 * @brief Manages incoming backend control connections to the proxy.
 *
 * Listens on the control port (default 8090). Both worldserver and instance
 * servers connect here to register themselves and send reroute commands.
 */
class ControlSocketMgr : public SocketMgr<ControlSocket>
{
    typedef SocketMgr<ControlSocket> BaseSocketMgr;

public:
    static ControlSocketMgr& Instance()
    {
        static ControlSocketMgr instance;
        return instance;
    }

    bool StartNetwork(Acore::Asio::IoContext& ioContext, std::string const& bindIp,
                      uint16 port, int threadCount = 1) override
    {
        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&ControlSocketMgr::OnSocketAccept>();
        return true;
    }

protected:
    NetworkThread<ControlSocket>* CreateThreads() const override
    {
        return new NetworkThread<ControlSocket>[1];
    }

    static void OnSocketAccept(IoContextTcpSocket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::move(sock), threadIndex);
    }
};

#define sControlSocketMgr ControlSocketMgr::Instance()

#endif // ControlSocketMgr_h__
