/*
 * This file is part of the c9core Project. See AUTHORS file for Copyright information
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

#include "PskCrypt.h"
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <cstring>
#include <stdexcept>

PskCrypt::PskCrypt()
{
    _encCtx = EVP_CIPHER_CTX_new();
    _decCtx = EVP_CIPHER_CTX_new();
    if (!_encCtx || !_decCtx)
        throw std::runtime_error("PskCrypt: EVP_CIPHER_CTX_new() failed");
}

PskCrypt::~PskCrypt()
{
    if (_encCtx)
        EVP_CIPHER_CTX_free(_encCtx);
    if (_decCtx)
        EVP_CIPHER_CTX_free(_decCtx);
}

/*static*/
void PskCrypt::GenerateNonce(uint8* out16)
{
    if (RAND_bytes(out16, 16) != 1)
        throw std::runtime_error("PskCrypt: RAND_bytes() failed");
}

bool PskCrypt::Init(std::string const& sharedSecret, uint8 const* nonce16, bool isServer)
{
    // ── 1. Derive 32-byte session key via HMAC-SHA256(secret, nonce) ──────────
    uint8 sessionKey[32]{};
    unsigned int keyLen = 32;
    if (!HMAC(EVP_sha256(),
              sharedSecret.data(), static_cast<int>(sharedSecret.size()),
              nonce16, 16,
              sessionKey, &keyLen))
        return false;

    // ── 2. Derive two independent IVs for the two traffic directions ──────────
    //    enc_iv  = SHA256("s2c" || nonce)[0:16]   — used by server for outgoing
    //    dec_iv  = SHA256("c2s" || nonce)[0:16]   — used by server for incoming
    uint8 s2cInput[19]; // "s2c" (3 bytes) + nonce (16 bytes)
    s2cInput[0] = 's'; s2cInput[1] = '2'; s2cInput[2] = 'c';
    std::memcpy(s2cInput + 3, nonce16, 16);

    uint8 c2sInput[19]; // "c2s" (3 bytes) + nonce (16 bytes)
    c2sInput[0] = 'c'; c2sInput[1] = '2'; c2sInput[2] = 's';
    std::memcpy(c2sInput + 3, nonce16, 16);

    uint8 s2cHash[32]{};
    uint8 c2sHash[32]{};
    SHA256(s2cInput, sizeof(s2cInput), s2cHash);
    SHA256(c2sInput, sizeof(c2sInput), c2sHash);

    // First 16 bytes of each hash become the AES-CTR IV.
    uint8 const* encIv = isServer ? s2cHash : c2sHash; // this side's outgoing IV
    uint8 const* decIv = isServer ? c2sHash : s2cHash; // this side's incoming IV

    // ── 3. Initialise AES-256-CTR contexts ───────────────────────────────────
    // In CTR mode EVP_EncryptInit/Update works equally well for both directions.
    if (!EVP_EncryptInit_ex(_encCtx, EVP_aes_256_ctr(), nullptr, sessionKey, encIv))
        return false;
    if (!EVP_EncryptInit_ex(_decCtx, EVP_aes_256_ctr(), nullptr, sessionKey, decIv))
        return false;

    // Disable automatic padding (CTR mode produces no padding).
    EVP_CIPHER_CTX_set_padding(_encCtx, 0);
    EVP_CIPHER_CTX_set_padding(_decCtx, 0);

    _initialized = true;
    return true;
}

void PskCrypt::Encrypt(uint8* data, std::size_t len)
{
    if (!_initialized || len == 0)
        return;

    int outLen = 0;
    // In CTR mode, in-place encryption (out == in) is safe; output length == input length.
    EVP_EncryptUpdate(_encCtx, data, &outLen, data, static_cast<int>(len));
}

void PskCrypt::Decrypt(uint8* data, std::size_t len)
{
    if (!_initialized || len == 0)
        return;

    int outLen = 0;
    EVP_EncryptUpdate(_decCtx, data, &outLen, data, static_cast<int>(len));
}
