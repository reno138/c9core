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

#ifndef NatsBus_h__
#define NatsBus_h__

#include "Define.h"
#include <nats.h>
#include <string>

/**
 * @brief Proxy-side NATS transport singleton — replaces the TCP ControlSocket channel.
 *
 * All worldserver/instance-server nodes connect to the NATS server (default port 4222)
 * instead of the proxy's old TCP control port (8090).
 *
 * Subject scheme:
 *   cluster.register      — worldnode → proxy  (NATS request-reply, one-time ID assignment)
 *   cluster.proxy         — worldnode → proxy  (all ongoing control messages)
 *   cluster.node.{N}      — proxy → specific worldnode N (targeted delivery)
 *   cluster.broadcast     — proxy → ALL worldnodes (fanout)
 *
 * Binary payload format is identical to the old TCP control-channel protocol.
 * First byte of every message is the message-type constant (see ClusterMessages.h).
 * All existing payload builders and handlers (ProxyMgr, ControlSocket handlers)
 * require zero modification — only the transport layer changes.
 */
class NatsBus
{
public:
    static NatsBus& Instance()
    {
        static NatsBus instance;
        return instance;
    }

    /// Connect to the NATS server and subscribe to cluster.register + cluster.proxy.
    /// Must be called once at proxy startup after config is loaded.
    void Initialize(std::string const& natsUrl);

    /// Drain subscriptions and close the connection gracefully.
    void Shutdown();

    /// Publish raw bytes to a specific worldnode (subject: cluster.node.{nodeId}).
    void PublishToNode(uint8 nodeId, uint8 const* data, int len);

    /// Publish raw bytes to ALL worldnodes (subject: cluster.broadcast).
    void PublishBroadcast(uint8 const* data, int len);

    /// Broadcast an RA command to all worldnodes (MSG_RA_COMMAND).
    void BroadcastRACommand(uint32 reqId, std::string const& cmd);

    bool IsConnected() const { return _nc != nullptr; }

private:
    NatsBus() = default;
    ~NatsBus() = default;

    natsConnection*   _nc{nullptr};
    natsSubscription* _subProxy{nullptr};     ///< cluster.proxy subscriber
    natsSubscription* _subRegister{nullptr};  ///< cluster.register subscriber (request-reply)
    natsSubscription* _subInstQuery{nullptr}; ///< cluster.instance.query subscriber (request-reply)
    natsSubscription* _subAnnounce{nullptr};  ///< cluster.announce subscriber (worldserver peer discovery)

    /// Dispatcher for all ongoing control messages (cluster.proxy).
    static void OnClusterProxyMsg(natsConnection* nc, natsSubscription* sub,
                                  natsMsg* msg, void* closure);

    /// Dispatcher for node registration requests (cluster.register).
    static void OnRegisterMsg(natsConnection* nc, natsSubscription* sub,
                              natsMsg* msg, void* closure);

    /// Dispatcher for "which instance server should I use?" queries (cluster.instance.query).
    /// Replies with [addrLen:1][addr:addrLen][port:2] for the best available instance node.
    static void OnInstanceQueryMsg(natsConnection* nc, natsSubscription* sub,
                                   natsMsg* msg, void* closure);

    /// Dispatcher for worldserver announce messages (cluster.announce).
    static void OnAnnounceMsg(natsConnection* nc, natsSubscription* sub,
                              natsMsg* msg, void* closure);
};

#define sNatsBus NatsBus::Instance()

#endif // NatsBus_h__
