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

    LOG_INFO("server.worldserver", "ClientRedirect: Sent SMSG_SUSPEND_COMMS to {} (token=0x{:08X}, opcode=0x{:04X})",
             session->GetPlayer()->GetName(), token, uint32(SMSG_SUSPEND_COMMS));

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

    // Build HMAC-SHA1 input: ip(4) || port(2) = 6 bytes ONLY.
    // Binary analysis of Wow.exe (0x632E30) confirmed:
    //   - Client reads IP as LE uint32, port as LE uint16 from the packet
    //   - Client feeds the raw memory bytes of those values into HMAC_Update
    //   - So the HMAC input must use the SAME byte representation as the packet
    //
    // The packet writes: data << uint32(ipNetOrder) << uint16(destPort)
    //   IP:   ipNetOrder stored as LE bytes in packet — same as memcpy(&ipNetOrder)
    //   Port: destPort stored as LE bytes in packet — must use destPort, NOT htons(destPort)
    uint8 hmacInput[6];
    std::memcpy(hmacInput, &ipNetOrder, 4);
    uint16 portRaw = destPort;
    std::memcpy(hmacInput + 4, &portRaw, 2);

    // Compute HMAC-SHA1 with session key
    uint8 hmacResult[20];
    unsigned int hmacLen = 20;
    HMAC(EVP_sha1(),
         sessionKey.data(), static_cast<int>(sessionKey.size()),
         hmacInput, sizeof(hmacInput),
         hmacResult, &hmacLen);

    // === DETAILED REDIRECT DEBUG LOGGING ===
    {
        // Log FULL session key (40 bytes)
        std::string skHex;
        for (size_t i = 0; i < sessionKey.size(); ++i)
            skHex += fmt::format("{:02X}", sessionKey[i]);

        // Log HMAC input bytes (6 bytes: ip + port only, no token)
        std::string inputHex;
        for (int i = 0; i < 6; ++i)
            inputHex += fmt::format("{:02X} ", hmacInput[i]);

        // Log HMAC result
        std::string resultHex;
        for (int i = 0; i < 20; ++i)
            resultHex += fmt::format("{:02X} ", hmacResult[i]);

        LOG_INFO("server.worldserver", "ClientRedirect DEBUG:");
        LOG_INFO("server.worldserver", "  destIp={} destPort={} token=0x{:08X}", destIp, destPort, token);
        LOG_INFO("server.worldserver", "  ipNetOrder=0x{:08X} portRaw=0x{:04X}", ipNetOrder, portRaw);
        LOG_INFO("server.worldserver", "  sessionKey (first 8): {}", skHex);
        LOG_INFO("server.worldserver", "  HMAC input (6 bytes): {}", inputHex);
        LOG_INFO("server.worldserver", "  HMAC result (20 bytes): {}", resultHex);

        // Log raw packet bytes we're about to send
        std::string pktHex;
        // ip(4)
        for (int i = 0; i < 4; ++i) pktHex += fmt::format("{:02X} ", ((uint8*)&ipNetOrder)[i]);
        // port(2)
        for (int i = 0; i < 2; ++i) pktHex += fmt::format("{:02X} ", ((uint8*)&portRaw)[i]);
        // token(4)
        for (int i = 0; i < 4; ++i) pktHex += fmt::format("{:02X} ", ((uint8*)&token)[i]);
        // hmac(20)
        for (int i = 0; i < 20; ++i) pktHex += fmt::format("{:02X} ", hmacResult[i]);
        LOG_INFO("server.worldserver", "  Raw packet payload (30 bytes): {}", pktHex);
    }

    // Build SMSG_REDIRECT_CLIENT (0x50D)
    // Payload: ip(4) + port(2) + token(4) + hmac(20) = 30 bytes
    //
    // The client reads IP via ReadUInt32 (LE) and passes the value directly to
    // its Connect() function. We need to figure out if Connect() expects NBO or HBO.
    // Try BOTH formats and log which one we're sending.
    //
    // Current: send IP in NBO (network byte order) via append
    // The client reads it as LE uint32 and gets the NBO value.
    // If Connect() expects the value to already be NBO, this works.
    // If Connect() calls htonl() on it, it would double-swap and fail.
    //
    // Let's try sending IP as a regular LE uint32 (host byte order).
    // The client reads it as LE uint32 and gets the HBO value.
    // If Connect() calls inet_addr or htonl internally, this would work.
    // Byte order analysis from pcap:
    // Client ReadUInt32 reads LE bytes into a uint32 VALUE.
    // Client copies that VALUE directly to sin_addr.s_addr (which is NBO).
    // So the uint32 VALUE must already be in NBO = inet_addr result.
    // We use operator<< which writes the VALUE as LE bytes.
    // Client reads LE bytes → gets the original VALUE → stores as sin_addr. ✓
    //
    // inet_addr("192.0.2.73") returns 0x490200C0 (NBO as uint32 on LE machine)
    // We write with <<: LE bytes = C0 00 02 49
    // Client reads LE: value = 0x490200C0 → sin_addr = NBO bytes C0 00 02 49 = 192.0.2.73 ✓
    //
    // Same for port: htons(8086) = 0x961F. Write with <<: LE bytes 1F 96.
    // Client reads: value 0x961F → sin_port = NBO bytes 1F 96 = port 8086 ✓
    // Pcap proved: client reads uint32 via LE ReadUInt32, stores VALUE in sin_addr.
    // sin_addr is interpreted as big-endian by the OS.
    // For IP 192.0.2.73: we need sin_addr bytes to be C0 00 02 49.
    // That means the uint32 VALUE stored must be 0xC0000249 (on LE: interpreted as 192.0.2.73).
    // ntohl(inet_addr("192.0.2.73")) = ntohl(0x490200C0) = 0xC0000249 ✓
    WorldPacket data(SMSG_REDIRECT_CLIENT, 30);
    data << uint32(ipNetOrder);               // inet_addr result — client passes to sin_addr directly
    data << uint16(destPort);                 // raw port number — client passes to sin_port via htons internally
    data << uint32(token);                    // Token
    data.append(hmacResult, 20);              // HMAC-SHA1 proof

    // DIAGNOSTIC: Also log what the HMAC would be with REVERSED session key
    {
        SessionKey reversedKey;
        for (size_t i = 0; i < sessionKey.size(); ++i)
            reversedKey[i] = sessionKey[sessionKey.size() - 1 - i];
        uint8 revHmac[20];
        unsigned int revLen = 20;
        HMAC(EVP_sha1(), reversedKey.data(), static_cast<int>(reversedKey.size()),
             hmacInput, sizeof(hmacInput), revHmac, &revLen);
        std::string revHex;
        for (int i = 0; i < 20; ++i) revHex += fmt::format("{:02X} ", revHmac[i]);
        LOG_INFO("server.worldserver", "  HMAC with REVERSED key: {}", revHex);

        // Also try HMAC with token included (ip+port+token = 10 bytes)
        uint8 hmacInputWithToken[10];
        std::memcpy(hmacInputWithToken, &ipNetOrder, 4);
        std::memcpy(hmacInputWithToken + 4, &portRaw, 2);
        std::memcpy(hmacInputWithToken + 6, &token, 4);
        uint8 tokenHmac[20];
        unsigned int tokenLen = 20;
        HMAC(EVP_sha1(), sessionKey.data(), static_cast<int>(sessionKey.size()),
             hmacInputWithToken, 10, tokenHmac, &tokenLen);
        std::string tokHex;
        for (int i = 0; i < 20; ++i) tokHex += fmt::format("{:02X} ", tokenHmac[i]);
        LOG_INFO("server.worldserver", "  HMAC with TOKEN included: {}", tokHex);

        // And try with ip+port in SWAPPED byte order
        uint32 ipLE = ntohl(ipNetOrder);  // swap to LE/host order
        uint16 portLE = ntohs(portRaw);
        uint8 hmacSwapped[6];
        std::memcpy(hmacSwapped, &ipLE, 4);
        std::memcpy(hmacSwapped + 4, &portLE, 2);
        uint8 swapHmac[20];
        unsigned int swapLen = 20;
        HMAC(EVP_sha1(), sessionKey.data(), static_cast<int>(sessionKey.size()),
             hmacSwapped, 6, swapHmac, &swapLen);
        std::string swapHex;
        for (int i = 0; i < 20; ++i) swapHex += fmt::format("{:02X} ", swapHmac[i]);
        LOG_INFO("server.worldserver", "  HMAC with SWAPPED ip/port: {}", swapHex);

        // Try HMAC with auth seed as key (4 bytes + 36 zeros = 40 bytes)
        auto const& authSeed = session->GetAuthSeed();
        uint8 seedKey[40] = {};
        std::memcpy(seedKey, authSeed.data(), 4);
        uint8 seedHmac[20];
        unsigned int seedHmacLen = 20;
        HMAC(EVP_sha1(), seedKey, 40,
             hmacInput, sizeof(hmacInput), seedHmac, &seedHmacLen);
        std::string seedHmacHex;
        for (int i = 0; i < 20; ++i) seedHmacHex += fmt::format("{:02X} ", seedHmac[i]);
        std::string authSeedHex;
        for (auto b : authSeed) authSeedHex += fmt::format("{:02X}", b);
        LOG_INFO("server.worldserver", "  AuthSeed: {}", authSeedHex);
        LOG_INFO("server.worldserver", "  HMAC with SEED-ONLY key: {}", seedHmacHex);
    }

    session->SendPacket(&data);
    session->SetRedirectPending();

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
