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

#include "AuthCrypt.h"
#include "Errors.h"
#include "HMAC.h"
#include "Log.h"
#include <cstring>
#include <fmt/format.h>

void AuthCrypt::Init(SessionKey const& K)
{
    uint8 ServerEncryptionKey[] = { 0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA, 0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57 };
    _serverEncrypt.Init(Acore::Crypto::HMAC_SHA1::GetDigestOf(ServerEncryptionKey, K));

    uint8 ServerDecryptionKey[] = { 0xC2, 0xB3, 0x72, 0x3C, 0xC6, 0xAE, 0xD9, 0xB5, 0x34, 0x3C, 0x53, 0xEE, 0x2F, 0x43, 0x67, 0xCE };
    _clientDecrypt.Init(Acore::Crypto::HMAC_SHA1::GetDigestOf(ServerDecryptionKey, K));

    // Drop first 1024 bytes, as WoW uses ARC4-drop1024.
    std::array<uint8, 1024> syncBuf{};
    _serverEncrypt.UpdateData(syncBuf);
    _clientDecrypt.UpdateData(syncBuf);

    _initialized = true;
}

void AuthCrypt::InitRedirect(SessionKey const& K, std::array<uint8, 32> const& encryptionSeeds)
{
    // Redirect connections use the random encryption seeds from
    // SMSG_AUTH_CHALLENGE instead of the hardcoded ServerEncryptionKey /
    // ServerDecryptionKey.  The client calls InitCrypt with direction=1,
    // which swaps the seed halves relative to direction=0 (normal auth).
    //
    // Client direction=1 key derivation (from InitCrypt at 0x466bf0):
    //   key1 = HMAC(seeds[16..31], K) → ARC4 ctx at [+0x148] → CLIENT SEND
    //   key2 = HMAC(seeds[0..15],  K) → ARC4 ctx at [+0x24a] → CLIENT RECV
    //
    // RE of WowConnection (confirmed 2026-04-10):
    //   Send encrypt (0x4665b0) uses [+0x148] = key1 = HMAC(seeds[16..31], K)
    //   Recv decrypt (0x467ebf) uses [+0x24a] = key2 = HMAC(seeds[0..15], K)
    //
    // Server must MATCH:
    //   _serverEncrypt (S→C) = client RECV key = HMAC(seeds[0..15], K)
    //   _clientDecrypt (C→S) = client SEND key = HMAC(seeds[16..31], K)
    uint8 encryptSeed[16];
    std::memcpy(encryptSeed, encryptionSeeds.data(), 16);  // seeds[0..15]
    auto encDigest = Acore::Crypto::HMAC_SHA1::GetDigestOf(encryptSeed, K);
    _serverEncrypt.Init(encDigest);

    uint8 decryptSeed[16];
    std::memcpy(decryptSeed, encryptionSeeds.data() + 16, 16);  // seeds[16..31]
    auto decDigest = Acore::Crypto::HMAC_SHA1::GetDigestOf(decryptSeed, K);
    _clientDecrypt.Init(decDigest);

    // Drop first 1024 bytes, as WoW uses ARC4-drop1024.
    std::array<uint8, 1024> syncBuf{};
    _serverEncrypt.UpdateData(syncBuf);
    _clientDecrypt.UpdateData(syncBuf);

    _initialized = true;

    // The RE-era key dump that used to live here (session key, seeds, both
    // derived ARC4 keys, at LOG_INFO on every redirect) is gone. The derivation
    // above is the verified one; anyone re-deriving it should log fingerprints
    // behind ClusterServer.RedirectDebug in ClientRedirect.cpp, never raw keys.
}

void AuthCrypt::DecryptRecv(uint8* data, std::size_t len)
{
    ASSERT(_initialized);
    _clientDecrypt.UpdateData(data, len);
}

void AuthCrypt::EncryptSend(uint8* data, std::size_t len)
{
    ASSERT(_initialized);
    _serverEncrypt.UpdateData(data, len);
}
