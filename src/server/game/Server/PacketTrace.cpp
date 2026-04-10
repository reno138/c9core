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

#include "PacketTrace.h"
#include "Opcodes.h"
#include <algorithm>
#include <cstdio>

namespace Acore::PacketTrace
{
    std::string OpcodeName(std::uint32_t opcode)
    {
        // Delegate to the existing opcode table's name resolution. The
        // GetOpcodeNameForLogging helper handles both OpcodeClient and
        // OpcodeServer ranges and returns "UNKNOWN OPCODE ..." for
        // out-of-range values, which is fine for trace output.
        return GetOpcodeNameForLogging(static_cast<OpcodeClient>(opcode));
    }

    std::string HexDump(std::uint8_t const* data, std::size_t size, std::size_t maxBytes)
    {
        if (!data || size == 0)
            return {};

        std::size_t const n = std::min(size, maxBytes);
        std::string out;
        out.reserve(n * 3);

        char buf[4];
        for (std::size_t i = 0; i < n; ++i)
        {
            std::snprintf(buf, sizeof(buf), "%02X ", data[i]);
            out.append(buf);
        }

        // Strip trailing space.
        if (!out.empty())
            out.pop_back();

        // Indicate truncation.
        if (size > maxBytes)
            out.append("...");

        return out;
    }

    std::string SessionKeyFingerprint(std::uint8_t const* key)
    {
        if (!key)
            return "<null>";

        // First 8 bytes only. 64 bits is not enough to derive the full 40-byte
        // BN_SHA1 session key, so this is safe to emit on a test server while
        // still being visually matchable across node logs.
        return HexDump(key, 8, 8);
    }

    bool IsCriticalOpcodeForTrace(std::uint32_t opcode)
    {
        switch (opcode)
        {
            case SMSG_REDIRECT_CLIENT:
            case SMSG_NEW_WORLD:
            case SMSG_LOGIN_VERIFY_WORLD:
            case SMSG_AUTH_RESPONSE:
            case CMSG_REDIRECTION_AUTH_PROOF:
            case CMSG_REDIRECTION_FAILED:
            case CMSG_PLAYER_LOGIN:
                return true;
            default:
                return false;
        }
    }
}
