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

#ifndef C9CORE_PACKETTRACE_H
#define C9CORE_PACKETTRACE_H

#include "ClusterMgr.h"
#include "Log.h"
#include <cstdint>
#include <string>

/**
 * @file PacketTrace.h
 * @brief Verbose packet + state trace instrumentation for cluster login /
 *        redirect flows.
 *
 * Enabled at runtime via the `ClusterServer.PacketTrace.Enable` config option.
 * When disabled, every trace point collapses to a single atomic-bool load
 * and branch — zero-cost on the hot path.
 *
 * Output is routed to the `cluster.packettrace` logger. Configure an
 * appender for that logger name in worldserver.conf to redirect output
 * to a dedicated file (recommended: PacketTrace.log).
 *
 * Every emitted line is prefixed with `[PT]` so grep-based filtering and
 * cross-node log correlation are trivial.
 *
 * Thread safety: PT_ENABLED() reads an atomic<bool>. All emission macros
 * delegate to the existing Log infrastructure, which is already thread-safe.
 */

/// True iff packet tracing is currently enabled. Use this to gate any
/// computation whose only purpose is feeding a trace (hex dumps, expensive
/// fmt::format calls, etc.). The individual PT_* macros also gate on this
/// internally, so plain trace lines do NOT need an outer `if (PT_ENABLED())`.
#define PT_ENABLED() (sClusterMgr.IsPacketTraceEnabled())

/// Emit a free-form trace event. Accepts an fmt-style format string with `{}`
/// placeholders and a variadic arg list, matching the Log module's API.
///
/// Example:
///     PT_EVENT("PLAYER_LOGIN_OPCODE guid={} path={}", guid.ToString(), "cold");
#define PT_EVENT(...) \
    do { if (PT_ENABLED()) { LOG_INFO("cluster.packettrace", "[PT] " __VA_ARGS__); } } while (0)

/// Emit an opcode-trace line at the socket layer.
///
/// @param dir    "S>C" or "C>S"
/// @param op     OpcodeServer / OpcodeClient enum value (uint32 on wire)
/// @param size   Payload byte count (excluding header)
/// @param who    Short session identity tag (account name, or remote ip if unauthed)
///
/// Rendering: "[PT] PKT {dir} 0x{op:04X} {name} size={size} who={who}"
#define PT_OPCODE(dir, op, size, who) \
    do { if (PT_ENABLED()) { \
        LOG_INFO("cluster.packettrace", "[PT] PKT {} 0x{:04X} {} size={} who={}", \
                 (dir), uint32(op), Acore::PacketTrace::OpcodeName(uint32(op)), uint32(size), (who)); \
    } } while (0)

/// Emit an opcode-trace line plus hex dump of the packet payload. Reserved
/// for the "critical" packets in the redirect flow — everything else should
/// use PT_OPCODE which skips the hex to keep log volume sane.
///
/// Hex is dumped up to `maxBytes` bytes of the payload, space-separated.
#define PT_OPCODE_HEX(dir, op, payload, size, who, maxBytes) \
    do { if (PT_ENABLED()) { \
        std::string __hex = Acore::PacketTrace::HexDump(payload, size, maxBytes); \
        LOG_INFO("cluster.packettrace", "[PT] PKT {} 0x{:04X} {} size={} who={} hex={}", \
                 (dir), uint32(op), Acore::PacketTrace::OpcodeName(uint32(op)), uint32(size), (who), __hex); \
    } } while (0)

namespace Acore::PacketTrace
{
    /// Resolve an opcode number to its symbolic name for human-readable
    /// traces. Falls back to "UNKNOWN" when the opcode is not in the table.
    std::string OpcodeName(std::uint32_t opcode);

    /// Render the first N bytes of a byte buffer as space-separated hex.
    /// Empty string if `data` is null or `size` is 0.
    std::string HexDump(std::uint8_t const* data, std::size_t size, std::size_t maxBytes);

    /// Render the first 8 bytes of a session key as space-separated hex for
    /// cross-node key-sync verification. Safe to emit at v1 verbosity: 64
    /// bits leaks no key material of cryptographic concern.
    std::string SessionKeyFingerprint(std::uint8_t const* key);

    /// True iff this opcode is in the small "critical for redirect debugging"
    /// set that gets a full hex-dump in the trace instead of the normal
    /// opcode+size line. Keep this list small — hex-dumping every packet
    /// would drown the log in addon-info and movement churn.
    ///
    /// Current set:
    ///   - SMSG_REDIRECT_CLIENT         (server tells client "go to X:Y")
    ///   - SMSG_NEW_WORLD               (server tells client "start loading map")
    ///   - SMSG_LOGIN_VERIFY_WORLD      (server confirms map+pos after login)
    ///   - SMSG_AUTH_RESPONSE           (server tells client auth ok/fail)
    ///   - CMSG_REDIRECTION_AUTH_PROOF  (client's post-redirect auth proof)
    ///   - CMSG_REDIRECTION_FAILED      (client reports "redirect didn't work")
    ///   - CMSG_PLAYER_LOGIN            (client asks to log a specific char in)
    bool IsCriticalOpcodeForTrace(std::uint32_t opcode);
}

#endif // C9CORE_PACKETTRACE_H
