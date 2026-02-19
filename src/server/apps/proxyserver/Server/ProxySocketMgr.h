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

#ifndef ProxySocketMgr_h__
#define ProxySocketMgr_h__

#include "Config.h"
#include "IoContext.h"
#include "NetworkThread.h"
#include "ProxySocket.h"
#include "SocketMgr.h"

/**
 * @brief Manages incoming WoW client connections to the proxy.
 *
 * Mirrors AuthSocketMgr in structure. Stores the IoContext so
 * ProxySocket::Start() can create outgoing BackendSession connections
 * on the correct io_context.
 */
class ProxySocketMgr : public SocketMgr<ProxySocket>
{
    typedef SocketMgr<ProxySocket> BaseSocketMgr;

public:
    static ProxySocketMgr& Instance()
    {
        static ProxySocketMgr instance;
        return instance;
    }

    bool StartNetwork(Acore::Asio::IoContext& ioContext, std::string const& bindIp,
                      uint16 port, int threadCount = 1) override
    {
        _ioContextPtr = &ioContext;

        if (!BaseSocketMgr::StartNetwork(ioContext, bindIp, port, threadCount))
            return false;

        _acceptor->AsyncAcceptWithCallback<&ProxySocketMgr::OnSocketAccept>();
        return true;
    }

    /// Returns the IoContext so ProxySocket can create BackendSession connections.
    Acore::Asio::IoContext& GetIoContext()
    {
        ASSERT(_ioContextPtr);
        return *_ioContextPtr;
    }

protected:
    NetworkThread<ProxySocket>* CreateThreads() const override
    {
        return new NetworkThread<ProxySocket>[1];
    }

    static void OnSocketAccept(IoContextTcpSocket&& sock, uint32 threadIndex)
    {
        Instance().OnSocketOpen(std::move(sock), threadIndex);
    }

private:
    Acore::Asio::IoContext* _ioContextPtr{ nullptr };
};

#define sProxySocketMgr ProxySocketMgr::Instance()

#endif // ProxySocketMgr_h__
