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

#include "ClusterAuth.h"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace
{
    std::string           g_key;
    bool                  g_ready = false;

    constexpr uint8 FRAME_VERSION = 1;

    // Replay window: nonce -> expiry. A deque keeps insertion order so pruning
    // is O(expired) rather than O(all).
    std::mutex                        g_replayMutex;
    std::unordered_set<uint64>        g_seenNonces;
    std::deque<std::pair<uint64, uint64>> g_nonceExpiry; ///< (nonce, expiresAtMs)

    uint64 NowMs()
    {
        using namespace std::chrono;
        return static_cast<uint64>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    void PutLE64(uint8* out, uint64 v)
    {
        for (int i = 0; i < 8; ++i)
            out[i] = static_cast<uint8>((v >> (i * 8)) & 0xFF);
    }

    uint64 GetLE64(uint8 const* in)
    {
        uint64 v = 0;
        for (int i = 0; i < 8; ++i)
            v |= static_cast<uint64>(in[i]) << (i * 8);
        return v;
    }

    /// Compute the tag over everything before the tag field.
    void ComputeTag(uint8 const* framePrefix, std::size_t prefixLen, uint8 out[32])
    {
        unsigned int len = 32;
        HMAC(EVP_sha256(),
             g_key.data(), static_cast<int>(g_key.size()),
             framePrefix, prefixLen,
             out, &len);
    }

    /// @return false if this nonce was already used inside the accept window.
    bool CheckAndRecordNonce(uint64 nonce, uint64 nowMs)
    {
        std::lock_guard<std::mutex> lock(g_replayMutex);

        // Prune anything that has aged out of the window.
        while (!g_nonceExpiry.empty() && g_nonceExpiry.front().second <= nowMs)
        {
            g_seenNonces.erase(g_nonceExpiry.front().first);
            g_nonceExpiry.pop_front();
        }

        if (!g_seenNonces.insert(nonce).second)
            return false; // replay

        g_nonceExpiry.emplace_back(nonce, nowMs + ClusterAuth::ACCEPT_SKEW_MS * 2);
        return true;
    }
}

namespace ClusterAuth
{

bool Init(std::string const& sharedKey)
{
    if (sharedKey.size() < MIN_KEY_BYTES)
    {
        g_ready = false;
        return false;
    }

    g_key   = sharedKey;
    g_ready = true;
    return true;
}

bool IsInitialised()
{
    return g_ready;
}

std::vector<uint8> Seal(uint8 srcNodeId, uint8 msgType, uint8 const* payload, std::size_t payloadLen)
{
    std::vector<uint8> frame;
    if (!g_ready)
        return frame; // caller must treat empty as failure

    frame.resize(HEADER_BYTES + payloadLen + TAG_BYTES);

    uint64 nonce = 0;
    // RAND_bytes is the only acceptable source here: a predictable nonce would
    // let an observer pre-compute a replay that lands inside the window.
    if (RAND_bytes(reinterpret_cast<unsigned char*>(&nonce), sizeof(nonce)) != 1)
    {
        frame.clear();
        return frame;
    }

    std::size_t o = 0;
    frame[o++] = FRAME_VERSION;
    frame[o++] = srcNodeId;
    PutLE64(&frame[o], NowMs()); o += 8;
    PutLE64(&frame[o], nonce);   o += 8;
    frame[o++] = msgType;

    if (payloadLen)
        std::memcpy(&frame[o], payload, payloadLen);
    o += payloadLen;

    ComputeTag(frame.data(), o, &frame[o]);
    return frame;
}

bool Open(uint8 const* frame, std::size_t frameLen,
          uint8& outSrcNodeId, uint8& outMsgType, std::vector<uint8>& outPayload)
{
    if (!g_ready || frameLen < MIN_FRAME)
        return false;

    if (frame[0] != FRAME_VERSION)
        return false;

    std::size_t const prefixLen = frameLen - TAG_BYTES;

    uint8 expected[32];
    ComputeTag(frame, prefixLen, expected);

    // Constant-time: a byte-by-byte early-out comparison here would leak the
    // tag one byte at a time to an attacker able to time our responses.
    if (CRYPTO_memcmp(expected, frame + prefixLen, TAG_BYTES) != 0)
        return false;

    uint64 const ts    = GetLE64(frame + 2);
    uint64 const nonce = GetLE64(frame + 10);
    uint64 const now   = NowMs();

    uint64 const delta = (now > ts) ? (now - ts) : (ts - now);
    if (delta > ACCEPT_SKEW_MS)
        return false;

    if (!CheckAndRecordNonce(nonce, now))
        return false;

    outSrcNodeId = frame[1];
    outMsgType   = frame[HEADER_BYTES - 1];

    outPayload.assign(frame + HEADER_BYTES, frame + prefixLen);
    return true;
}

} // namespace ClusterAuth
