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

#ifndef PlayerStateSync_h__
#define PlayerStateSync_h__

#include "SharedPlayerState.h"
#include <vector>

/// Serialize a delta state update for a single player.
/// Wire format: [guid:8][fieldMask:1][...fields based on mask...]
std::vector<uint8> SerializeStateDelta(uint64 guid, uint8 fieldMask, SharedPlayerState const& state);

/// Deserialize a delta state update from a binary buffer.
/// Returns false if the buffer is malformed or truncated.
bool DeserializeStateDelta(uint8 const* buf, int len, uint64& outGuid, uint8& outMask, SharedPlayerState& outState);

/// Serialize a full player state snapshot.
/// Prepends identity fields (accountId, ownerNodeId, level, raceId, classId, gender, displayId)
/// then a delta with fieldMask = 0xFF.
std::vector<uint8> SerializeFullState(SharedPlayerState const& state);

/// Deserialize a full player state snapshot from a binary buffer.
/// Returns false if the buffer is malformed or truncated.
bool DeserializeFullState(uint8 const* buf, int len, SharedPlayerState& outState);

#endif // PlayerStateSync_h__
