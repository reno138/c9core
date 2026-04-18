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

#ifndef PskCrypt_h__
#define PskCrypt_h__

/**
 * @file PskCrypt.h
 * @brief AES-256-CTR stream cipher keyed from a pre-shared secret + nonce.
 *
 * Usage (server side — proxy):
 *   PskCrypt crypt;
 *   uint8 nonce[16];
 *   PskCrypt::GenerateNonce(nonce);
 *   // Send nonce to client (unencrypted)
 *   crypt.Init(sharedSecret, nonce, true);
 *   // Now call Encrypt() before every send, Decrypt() after every recv.
 *
 * Usage (client side — nodemgr / clustermgr):
 *   PskCrypt crypt;
 *   uint8 nonce[16];
 *   // Read nonce from server (unencrypted)
 *   crypt.Init(sharedSecret, nonce, false);
 *
 * Key derivation (both sides compute the same values):
 *   session_key  = HMAC-SHA256(sharedSecret, nonce)              — 32 bytes
 *   enc_iv       = SHA256("s2c" || nonce)[0:16]                  — server→client CTR IV
 *   dec_iv       = SHA256("c2s" || nonce)[0:16]                  — client→server CTR IV
 *
 * Thread safety: NOT thread-safe. Use one instance per connection.
 */

#include "Define.h"
#include <openssl/evp.h>
#include <string>

class PskCrypt
{
public:
    PskCrypt();
    ~PskCrypt();

    // Non-copyable
    PskCrypt(PskCrypt const&) = delete;
    PskCrypt& operator=(PskCrypt const&) = delete;

    /**
     * @brief Fill out16 with 16 random bytes suitable for use as a nonce.
     * Uses OpenSSL RAND_bytes — fails hard if entropy unavailable.
     */
    static void GenerateNonce(uint8* out16);

    /**
     * @brief Initialise cipher contexts from the shared secret and nonce.
     * @param sharedSecret  Pre-shared password string (any length).
     * @param nonce16       16-byte nonce received/sent during handshake.
     * @param isServer      true if this side SENT the nonce (proxy);
     *                      false if this side RECEIVED the nonce (client).
     * @return true on success.
     */
    bool Init(std::string const& sharedSecret, uint8 const* nonce16, bool isServer);

    bool IsInitialized() const { return _initialized; }

    /**
     * @brief Encrypt data in-place (XOR keystream over data before sending).
     * May only be called after a successful Init().
     */
    void Encrypt(uint8* data, std::size_t len);

    /**
     * @brief Decrypt data in-place (XOR keystream over received data).
     * May only be called after a successful Init().
     */
    void Decrypt(uint8* data, std::size_t len);

private:
    EVP_CIPHER_CTX* _encCtx{ nullptr }; ///< Outgoing stream (this side → peer)
    EVP_CIPHER_CTX* _decCtx{ nullptr }; ///< Incoming stream (peer → this side)
    bool _initialized{ false };
};

#endif // PskCrypt_h__
