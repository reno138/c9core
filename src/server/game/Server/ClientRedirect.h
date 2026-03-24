/*
 * C9Core — Native client redirect for proxy-less cluster transfers.
 *
 * Uses SMSG_REDIRECT_CLIENT (0x50D) and SMSG_SUSPEND_COMMS (0x50F) —
 * both confirmed implemented in WoW.exe 3.3.5a (12340) via binary analysis.
 *
 * The client natively handles disconnect/reconnect to a new server,
 * validating the redirect via HMAC-SHA1 proof.
 */

#pragma once

#include "Define.h"
#include <string>

class WorldSession;

namespace ClientRedirect
{
    /// Send SMSG_SUSPEND_COMMS (0x50F) to freeze client packet transmission.
    /// Client responds with CMSG_SUSPEND_COMMS_ACK (0x510) and stops sending.
    /// @return the generated suspension token
    uint32 SuspendClient(WorldSession* session);

    /// Send SMSG_REDIRECT_CLIENT (0x50D) to redirect client to another server.
    /// Computes HMAC-SHA1(ip || port || token, sessionKey) for authentication.
    /// The client validates the HMAC, disconnects, and reconnects to destIp:destPort.
    ///
    /// @param session   current WorldSession (provides session key and socket)
    /// @param destIp    destination IP as string ("192.0.2.73" or "203.0.113.232")
    /// @param destPort  destination port (what the client connects to)
    /// @param token     redirect token (use the value returned by SuspendClient)
    void RedirectClient(WorldSession* session, std::string const& destIp, uint16 destPort, uint32 token);

    /// Determine the correct redirect address for a client based on whether
    /// they're connecting from a local subnet or externally.
    /// @param clientIp     the client's IP address
    /// @param internalIp   the destination node's internal LAN IP
    /// @param internalPort the destination node's internal game port
    /// @param externalIp   the destination node's external/NAT IP (empty = no NAT)
    /// @param externalPort the destination node's external port (0 = no NAT)
    /// @return {ip, port} to use in the redirect packet
    std::pair<std::string, uint16> ResolveRedirectAddress(
        std::string const& clientIp,
        std::string const& internalIp, uint16 internalPort,
        std::string const& externalIp, uint16 externalPort);

    /// Check if an IP address is in a private/local subnet (RFC1918).
    bool IsLocalAddress(std::string const& ip);
}
