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

#include "ClusterMgr.h"
#include "Config.h"
#include "Log.h"
#include "DBCStores.h"
#include <algorithm>
#include <cstring>
#include <sstream>

void ClusterMgr::LoadLocalMaps()
{
    std::lock_guard<std::mutex> lock(_localMapsMutex);
    _localMaps.clear();
    _instanceServerMode = false;
    _allMapsMode        = false;

    // Mode 1: Instance-server — handle all instanceable maps (dungeons/raids/BGs/arenas).
    // Use ClusterServer.InstanceServer = 1. Do NOT use InstanceServer.Enable for this check:
    // InstanceServer.Enable = 1 breaks MapMgr startup (continent maps needed by pool system).
    if (sConfigMgr->GetOption<bool>("ClusterServer.InstanceServer", false))
    {
        _instanceServerMode = true;
        LOG_INFO("server.worldserver", "ClusterMgr: InstanceServer mode — handling all instanceable maps");
        return;
    }

    std::string raw = sConfigMgr->GetOption<std::string>("ClusterServer.Maps", "");

    // Mode 2: All-maps — no rerouting (standalone or mega-node)
    if (raw == "-1")
    {
        _allMapsMode = true;
        LOG_INFO("server.worldserver", "ClusterMgr: ClusterServer.Maps = -1 — all maps local (no rerouting)");
        return;
    }

    // Mode 3: Empty — all maps local (standalone/dev mode)
    if (raw.empty())
    {
        LOG_INFO("server.worldserver", "ClusterMgr: ClusterServer.Maps is empty — all maps local");
        return;
    }

    // Mode 4: Explicit comma-separated map ID list
    std::istringstream ss(raw);
    std::string token;
    while (std::getline(ss, token, ','))
    {
        token.erase(0, token.find_first_not_of(" 	"));
        token.erase(token.find_last_not_of(" 	") + 1);
        if (token.empty())
            continue;
        uint32 mapId = static_cast<uint32>(std::stoul(token));
        _localMaps.insert(mapId);
    }

    std::string mapList;
    for (uint32 m : _localMaps)
        mapList += std::to_string(m) + " ";
    LOG_INFO("server.worldserver", "ClusterMgr: Local maps: [{}]", mapList);
}

bool ClusterMgr::IsMapLocal(uint32 mapId) const
{
    std::lock_guard<std::mutex> lock(_localMapsMutex);

    if (_allMapsMode)
        return true;

    if (_instanceServerMode)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        return entry && entry->Instanceable();
    }

    if (_localMaps.empty())
        return true;

    return _localMaps.count(mapId) > 0;
}

void ClusterMgr::OnRemotePlayerOnline(uint64 guid, std::string name,
                                       uint32 zoneId, uint8 level, uint8 classId,
                                       uint8 raceId, uint8 teamId, uint8 nodeId)
{
    ClusterPlayerInfo info;
    info.guid    = guid;
    info.name    = name;   // already lowercase
    info.zoneId  = zoneId;
    info.level   = level;
    info.classId = classId;
    info.raceId  = raceId;
    info.teamId  = teamId;
    info.nodeId  = nodeId;

    std::unique_lock lock(_mutex);
    _byGuid[guid] = info;
    _byName[name] = guid;

    LOG_INFO("server.worldserver", "ClusterMgr: Remote player ONLINE  GUID {:016X} '{}' zone={} level={} node={}", guid, name, zoneId, level, nodeId);
}

void ClusterMgr::OnRemotePlayerOffline(uint64 guid)
{
    {
        std::unique_lock lock(_mutex);
        auto it = _byGuid.find(guid);
        if (it != _byGuid.end())
        {
            LOG_INFO("server.worldserver", "ClusterMgr: Remote player OFFLINE GUID {:016X} '{}'", guid, it->second.name);
            _byName.erase(it->second.name);
            _byGuid.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> ulock(_unitStateMutex);
        _unitStates.erase(guid);
    }
}

ClusterPlayerInfo const* ClusterMgr::FindRemotePlayer(std::string const& lowerName) const
{
    std::shared_lock lock(_mutex);
    auto it = _byName.find(lowerName);
    if (it == _byName.end())
        return nullptr;

    auto git = _byGuid.find(it->second);
    return (git != _byGuid.end()) ? &git->second : nullptr;
}

ClusterPlayerInfo const* ClusterMgr::FindRemotePlayerByGuid(uint64 guid) const
{
    std::shared_lock lock(_mutex);
    auto it = _byGuid.find(guid);
    return (it != _byGuid.end()) ? &it->second : nullptr;
}

std::vector<ClusterPlayerInfo> ClusterMgr::GetAllRemotePlayers() const
{
    std::shared_lock lock(_mutex);
    std::vector<ClusterPlayerInfo> result;
    result.reserve(_byGuid.size());
    for (auto const& [guid, info] : _byGuid)
        result.push_back(info);
    return result;
}

bool ClusterMgr::HasRemotePlayers() const
{
    std::shared_lock lock(_mutex);
    return !_byGuid.empty();
}

// ── Cross-node group invite state ─────────────────────────────────────────────

void ClusterMgr::SetPendingCrossNodeInvite(uint64 inviteeGuid, uint64 inviterGuid, std::string const& inviterName)
{
    std::lock_guard lock(_inviteMutex);
    CrossNodeInvite& inv = _pendingInvites[inviteeGuid];
    inv.inviterGuid = inviterGuid;
    inv.inviterName = inviterName;
    LOG_DEBUG("server.worldserver", "ClusterMgr: Stored pending cross-node invite for {:016X} from {:016X} '{}'",
              inviteeGuid, inviterGuid, inviterName);
}

bool ClusterMgr::GetAndClearPendingCrossNodeInvite(uint64 inviteeGuid, CrossNodeInvite& out)
{
    std::lock_guard lock(_inviteMutex);
    auto it = _pendingInvites.find(inviteeGuid);
    if (it == _pendingInvites.end())
        return false;
    out = it->second;
    _pendingInvites.erase(it);
    return true;
}

void ClusterMgr::QueueCrossNodeInviteResult(uint64 inviteeGuid, uint8 result)
{
    std::lock_guard lock(_inviteMutex);
    _inviteResults.push_back({ inviteeGuid, result });
}

std::vector<ClusterMgr::CrossNodeInviteResult> ClusterMgr::DrainInviteResults()
{
    std::lock_guard lock(_inviteMutex);
    std::vector<CrossNodeInviteResult> out;
    out.swap(_inviteResults);
    return out;
}

// ── Cross-node group state ────────────────────────────────────────────────────

void ClusterMgr::OnGroupUpdate(uint64 groupGuid, uint8 memberCount, std::vector<uint8> const& memberData)
{
    std::vector<CrossNodeGroupMember> members;
    members.reserve(memberCount);

    uint8 const* p = memberData.data();
    for (uint8 i = 0; i < memberCount && (p + 11) <= (memberData.data() + memberData.size()); ++i, p += 11)
    {
        CrossNodeGroupMember m;
        std::memcpy(&m.guid, p, 8);
        m.subgroup  = p[8];
        m.roleFlags = p[9];
        m.nodeId    = p[10];
        members.push_back(m);
    }

    std::lock_guard lock(_groupMutex);
    _groupMembers[groupGuid] = std::move(members);
    LOG_DEBUG("server.worldserver", "ClusterMgr: GroupUpdate {:016X} {} members", groupGuid, memberCount);
}

void ClusterMgr::OnGroupDisband(uint64 groupGuid)
{
    std::lock_guard lock(_groupMutex);
    _groupMembers.erase(groupGuid);
    LOG_DEBUG("server.worldserver", "ClusterMgr: GroupDisband {:016X}", groupGuid);
}

std::vector<ClusterMgr::CrossNodeGroupMember> ClusterMgr::GetGroupRemoteMembers(uint64 groupGuid) const
{
    std::lock_guard lock(_groupMutex);
    auto it = _groupMembers.find(groupGuid);
    if (it == _groupMembers.end())
        return {};
    return it->second;
}

// ── Cross-node unit state ─────────────────────────────────────────────────────

void ClusterMgr::UpdateUnitState(ClusterUnitState state)
{
    std::lock_guard<std::mutex> lock(_unitStateMutex);
    _unitStates[state.guid] = std::move(state);
}

bool ClusterMgr::GetUnitState(uint64 guid, ClusterUnitState& out) const
{
    std::lock_guard<std::mutex> lock(_unitStateMutex);
    auto it = _unitStates.find(guid);
    if (it == _unitStates.end())
        return false;
    out = it->second;
    return true;
}

std::unordered_set<uint32> ClusterMgr::GetLocalMaps() const
{
    std::lock_guard<std::mutex> lock(_localMapsMutex);
    return _localMaps;  // empty means "all maps local" — caller handles this
}
