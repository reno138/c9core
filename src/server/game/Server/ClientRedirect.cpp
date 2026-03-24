/*
 * C9Core — Native client redirect for proxy-less cluster transfers.
 */

#include "ClientRedirect.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include "Opcodes.h"
#include "Player.h"
#include "Log.h"

#include <openssl/hmac.h>
#include <openssl/evp.h>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

#include <cstring>
#include <random>

namespace ClientRedirect
{

uint32 SuspendClient(WorldSession* session)
{
    if (!session || !session->GetPlayer())
        return 0;

    // Generate random suspension token
    static std::mt19937 rng(std::random_device{}());
    uint32 token = rng();

    // SMSG_SUSPEND_COMMS (0x50F) — payload: uint32 token
    WorldPacket data(SMSG_SUSPEND_COMMS, 4);
    data << uint32(token);
    session->SendPacket(&data);

    LOG_INFO("server.worldserver", "ClientRedirect: Sent SMSG_SUSPEND_COMMS to {} (token=0x{:08X})",
             session->GetPlayer()->GetName(), token);

    return token;
}

void RedirectClient(WorldSession* session, std::string const& destIp, uint16 destPort, uint32 token)
{
    if (!session)
        return;

    // Session key (40 bytes) cached on WorldSession from auth handshake
    SessionKey const& sessionKey = session->GetSessionKey();

    // Convert IP string to network byte order uint32
    uint32 ipNetOrder = inet_addr(destIp.c_str());
    if (ipNetOrder == INADDR_NONE)
    {
        LOG_ERROR("server.worldserver", "ClientRedirect: Invalid destination IP '{}'", destIp);
        return;
    }

    // Port in network byte order (big-endian)
    uint16 portNetOrder = htons(destPort);

    // Build HMAC-SHA1 input: ip(4) || port(2) || token(4) = 10 bytes
    uint8 hmacInput[10];
    std::memcpy(hmacInput, &ipNetOrder, 4);
    std::memcpy(hmacInput + 4, &portNetOrder, 2);
    // Token in little-endian (matching how the client reads it from the packet)
    std::memcpy(hmacInput + 6, &token, 4);

    // Compute HMAC-SHA1 with session key
    uint8 hmacResult[20];
    unsigned int hmacLen = 20;
    HMAC(EVP_sha1(),
         sessionKey.data(), static_cast<int>(sessionKey.size()),
         hmacInput, sizeof(hmacInput),
         hmacResult, &hmacLen);

    // Build SMSG_REDIRECT_CLIENT (0x50D)
    // Payload: ip(4) + port(2) + token(4) + hmac(20) = 30 bytes
    //
    // The client reads these with its standard packet reader then uses them
    // directly for connect() and HMAC validation. IP and port must be in
    // network byte order since the client passes them to connect().
    WorldPacket data(SMSG_REDIRECT_CLIENT, 30);
    data.append(&ipNetOrder, 4);          // IP — raw network byte order
    data.append(&portNetOrder, 2);        // Port — raw network byte order
    data << uint32(token);                // Token — little-endian (client reads as uint32)
    data.append(hmacResult, 20);          // HMAC-SHA1 proof

    session->SendPacket(&data);

    LOG_INFO("server.worldserver", "ClientRedirect: Sent SMSG_REDIRECT_CLIENT to {} -> {}:{} (token=0x{:08X})",
             session->GetPlayer() ? session->GetPlayer()->GetName() : "<unknown>",
             destIp, destPort, token);
}

std::pair<std::string, uint16> ResolveRedirectAddress(
    std::string const& clientIp,
    std::string const& internalIp, uint16 internalPort,
    std::string const& externalIp, uint16 externalPort)
{
    // No external address configured — always use internal
    if (externalIp.empty() || externalPort == 0)
        return {internalIp, internalPort};

    // If client is on a local network, use internal address (direct LAN path)
    if (IsLocalAddress(clientIp))
        return {internalIp, internalPort};

    // Client is external — use NAT'd external address
    return {externalIp, externalPort};
}

bool IsLocalAddress(std::string const& ip)
{
    uint32 addr = ntohl(inet_addr(ip.c_str()));
    if (addr == INADDR_NONE)
        return false;

    // RFC1918 private ranges
    // 10.0.0.0/8
    if ((addr & 0xFF000000) == 0x0A000000)
        return true;
    // 172.16.0.0/12
    if ((addr & 0xFFF00000) == 0xAC100000)
        return true;
    // 192.168.0.0/16
    if ((addr & 0xFFFF0000) == 0xC0A80000)
        return true;
    // 127.0.0.0/8 (loopback)
    if ((addr & 0xFF000000) == 0x7F000000)
        return true;

    return false;
}

} // namespace ClientRedirect
