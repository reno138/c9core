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
#include "ProxyClient.h"
#include "Config.h"
#include "Log.h"
#include "DBCStores.h"
#include "Timer.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

// ── Peer node routing table ───────────────────────────────────────────────────

bool ClusterMgr::RegisterRemoteNode(ClusterNodeInfo info)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);

    uint8 nodeId = info.nodeId;

    // Check if this node was previously known (may have been dead).
    bool wasRevived = false;
    auto existingIt = _nodes.find(nodeId);
    if (existingIt != _nodes.end())
    {
        wasRevived = existingIt->second.dead;
        // Treat a re-announcement as a freshly-seen heartbeat so dead detection
        // doesn't immediately fire again before the first MSG_NODE_STATUS arrives.
        info.lastSeenMs  = getMSTime();
        info.playerCount = existingIt->second.playerCount;
    }

    // Remove old map→node entries for this nodeId (handles re-announce after dead removal).
    for (auto it = _mapToNode.begin(); it != _mapToNode.end(); )
    {
        if (it->second == nodeId)
            it = _mapToNode.erase(it);
        else
            ++it;
    }

    // Register new map→node entries.
    for (uint32 mapId : info.maps)
        _mapToNode[mapId] = nodeId;

    // Clear stale zone entries for this node before re-adding
    for (auto zit = _zoneToNode.begin(); zit != _zoneToNode.end(); )
    {
        if (zit->second == nodeId)
            zit = _zoneToNode.erase(zit);
        else
            ++zit;
    }
    for (uint32 zoneId : info.zones)
        _zoneToNode[zoneId] = nodeId;

    if (wasRevived)
    {
        info.dead = false;
        LOG_WARN("server.worldserver",
                 "ClusterMgr: Node {} REVIVED (re-announced) addr={}:{} type={} maps={}",
                 nodeId, info.address, info.port, info.type, info.maps.size());
    }
    else
    {
        LOG_INFO("server.worldserver",
                 "ClusterMgr: RegisterRemoteNode nodeId={} addr={}:{} type={} maps={}",
                 nodeId, info.address, info.port, info.type, info.maps.size());
    }

    _nodes[nodeId] = std::move(info);
    return wasRevived;
}

bool ClusterMgr::UpdateNodeStatus(uint8 nodeId, uint32 playerCount, uint32 nowMs)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(nodeId);
    if (it == _nodes.end())
        return false;

    bool wasRevived = it->second.dead;
    it->second.lastSeenMs  = nowMs;
    it->second.playerCount = playerCount;

    if (wasRevived)
    {
        // Node came back — restore its map routing entries.
        it->second.dead = false;
        for (uint32 mapId : it->second.maps)
            _mapToNode[mapId] = nodeId;
        LOG_WARN("server.worldserver",
                 "ClusterMgr: Node {} revived via heartbeat — {} map(s) restored", nodeId, it->second.maps.size());
    }

    return wasRevived;
}

std::vector<uint8> ClusterMgr::GetStaleNodeIds(uint32 deadThresholdMs, uint32 nowMs) const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    std::vector<uint8> result;
    for (auto const& [id, info] : _nodes)
    {
        // Only consider nodes that have sent at least one heartbeat and aren't already dead.
        if (info.dead || info.lastSeenMs == 0)
            continue;
        if (nowMs - info.lastSeenMs >= deadThresholdMs)
            result.push_back(id);
    }
    return result;
}

std::unordered_set<uint32> ClusterMgr::MarkNodeDead(uint8 nodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    std::unordered_set<uint32> orphaned;

    auto it = _nodes.find(nodeId);
    if (it == _nodes.end() || it->second.dead)
        return orphaned;  // unknown or already dead

    it->second.dead = true;

    // Remove this node's maps from the active routing table.
    for (auto mit = _mapToNode.begin(); mit != _mapToNode.end(); )
    {
        if (mit->second == nodeId)
        {
            orphaned.insert(mit->first);
            mit = _mapToNode.erase(mit);
        }
        else
            ++mit;
    }

    // Also clean zone routing entries for the dead node
    for (auto zit = _zoneToNode.begin(); zit != _zoneToNode.end(); )
    {
        if (zit->second == nodeId)
            zit = _zoneToNode.erase(zit);
        else
            ++zit;
    }

    return orphaned;
}

uint8 ClusterMgr::GetLowestAliveNonInstanceNodeId() const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    uint8 lowest = 0;
    for (auto const& [id, info] : _nodes)
    {
        if (!info.dead && info.type != 1)   // alive + not instance server
        {
            if (lowest == 0 || id < lowest)
                lowest = id;
        }
    }
    return lowest;
}

std::optional<ClusterNodeInfo> ClusterMgr::GetNodeInfo(uint8 nodeId) const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(nodeId);
    if (it == _nodes.end())
        return std::nullopt;
    return it->second;
}

std::optional<ClusterNodeInfo> ClusterMgr::GetNodeForMap(uint32 mapId) const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto mit = _mapToNode.find(mapId);
    if (mit == _mapToNode.end())
        return std::nullopt;
    auto nit = _nodes.find(mit->second);
    if (nit == _nodes.end())
        return std::nullopt;
    return nit->second;
}

uint8 ClusterMgr::GetInstanceNodeId() const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    for (auto const& [id, info] : _nodes)
        if (info.type == 1)
            return id;
    return 0;
}

// ── Local map set ─────────────────────────────────────────────────────────────

bool ClusterMgr::IsEnabled() const
{
    return !_allMapsMode && (_instanceServerMode || !_localMaps.empty() || !_localZones.empty());
}

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

    // Load zone-level routing (optional - subdivides maps across nodes)
    std::string zoneStr = sConfigMgr->GetOption<std::string>("ClusterServer.Zones", "");
    if (!zoneStr.empty())
    {
        std::istringstream zss(zoneStr);
        std::string ztok;
        while (std::getline(zss, ztok, ','))
        {
            ztok.erase(0, ztok.find_first_not_of(" \t"));
            ztok.erase(ztok.find_last_not_of(" \t") + 1);
            if (!ztok.empty())
                _localZones.insert(static_cast<uint32>(std::stoul(ztok)));
        }
        std::string zList;
        for (uint32 z : _localZones)
            zList += std::to_string(z) + " ";
        LOG_INFO("server.worldserver", "ClusterMgr: Local zones: [{}]", zList);
    }

    // Load NAT/redirect config
    _gameAddress = sConfigMgr->GetOption<std::string>("ClusterServer.GameAddress", "127.0.0.1");
    _gamePort = sConfigMgr->GetOption<uint16>("WorldServerPort", 8085);
    _externalAddress = sConfigMgr->GetOption<std::string>("ClusterServer.ExternalAddress", "");
    _externalPort = sConfigMgr->GetOption<uint16>("ClusterServer.ExternalPort", 0);
    _nodeId = sConfigMgr->GetOption<uint8>("ClusterServer.NodeId", 1);

    if (!_externalAddress.empty())
        LOG_INFO("server.worldserver", "ClusterMgr: NAT redirect: external {}:{}, internal {}:{}",
                 _externalAddress, _externalPort, _gameAddress, _gamePort);
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

bool ClusterMgr::IsZoneLocal(uint32 zoneId) const
{
    // If this node explicitly owns the zone, it is local
    {
        std::lock_guard<std::mutex> lock(_localMapsMutex);
        if (!_localZones.empty())
            return _localZones.count(zoneId) > 0;
    }

    // No explicit zones configured on this node.
    // Check if ANY peer claims this zone — if so, it is not local to us.
    {
        std::lock_guard<std::mutex> lock(_nodeMutex);
        if (_zoneToNode.count(zoneId) > 0)
            return false;  // another node owns this zone
    }

    return true;  // no one claims it, so it is local
}

std::optional<ClusterNodeInfo> ClusterMgr::GetNodeForZone(uint32 zoneId) const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    // Check explicit zone->node mapping first
    auto zit = _zoneToNode.find(zoneId);
    if (zit != _zoneToNode.end())
    {
        auto nit = _nodes.find(zit->second);
        if (nit != _nodes.end() && !nit->second.dead)
            return nit->second;
    }

    // No explicit zone owner — find the map owner for this zone.
    // Look up which map this zone belongs to, then find the node that
    // owns that map but does NOT have explicit zones (i.e. the continent node).
    // This handles the case where a zone-server only owns specific zones
    // and the player moves to an unclaimed zone on the same map.
    AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
    if (area)
    {
        uint32 mapId = area->mapid;
        auto mit = _mapToNode.find(mapId);
        if (mit != _mapToNode.end())
        {
            auto nit = _nodes.find(mit->second);
            if (nit != _nodes.end() && !nit->second.dead)
                return nit->second;
        }
    }

    return std::nullopt;
}


void ClusterMgr::AddLocalMap(uint32 mapId)
{
    {
        std::lock_guard<std::mutex> lock(_localMapsMutex);
        _localMaps.insert(mapId);
    }
    LOG_INFO("server.worldserver", "ClusterMgr: Dynamically added map {} to local node", mapId);
    // Re-announce so proxy updates routing
    sProxyClient.PublishAnnounce();
}

void ClusterMgr::RemoveLocalMap(uint32 mapId)
{
    {
        std::lock_guard<std::mutex> lock(_localMapsMutex);
        _localMaps.erase(mapId);
    }
    LOG_INFO("server.worldserver", "ClusterMgr: Dynamically removed map {} from local node", mapId);
    sProxyClient.PublishAnnounce();
}

void ClusterMgr::ClaimOrphanedMaps(uint8 deadNodeId)
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(deadNodeId);
    if (it == _nodes.end())
        return;

    auto const& deadNode = it->second;
    LOG_WARN("server.worldserver", "ClusterMgr: Claiming {} maps from dead node {}",
             deadNode.maps.size(), deadNodeId);

    {
        std::lock_guard<std::mutex> mlock(_localMapsMutex);
        for (uint32 m : deadNode.maps)
        {
            _localMaps.insert(m);
            LOG_INFO("server.worldserver", "ClusterMgr: Claimed map {} from dead node {}", m, deadNodeId);
        }
    }

    // Re-announce with expanded map set
    sProxyClient.PublishAnnounce();
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

    // Sanity check: a corrupted vector (e.g. from NATS UAF writing garbage into
    // adjacent BSS memory) would have an invalid start pointer.  Calling swap()
    // or the destructor on such a vector would call free() on the bad pointer
    // and crash.  Detect and recover by zeroing the internals before swap.
    auto const startAddr = reinterpret_cast<uintptr_t>(_inviteResults.data());
    constexpr std::size_t kMaxSaneSize = 10000;
    if (!_inviteResults.empty() &&
        (startAddr < 4096 || _inviteResults.size() > kMaxSaneSize))
    {
        LOG_ERROR("server.worldserver",
                  "ClusterMgr::DrainInviteResults: _inviteResults corrupted "
                  "(data={:#x} size={}) — resetting",
                  startAddr, _inviteResults.size());
        // Zero the three internal pointers so the "destructor" on the now-cleared
        // vector becomes a no-op (free(nullptr) is safe).
        std::memset(&_inviteResults, 0, sizeof(_inviteResults));
        return {};
    }

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

    // Instance-server mode: enumerate all Instanceable map IDs from DBC so that
    // PublishAnnounce sends a full list and other nodes can route players here.
    if (_instanceServerMode)
    {
        std::unordered_set<uint32> instanceMaps;
        for (auto const* mapEntry : sMapStore)
            if (mapEntry && mapEntry->Instanceable())
                instanceMaps.insert(mapEntry->MapID);
        return instanceMaps;
    }

    return _localMaps;  // empty means "all maps local" — caller handles this
}

std::unordered_set<uint32> ClusterMgr::GetLocalZones() const
{
    std::lock_guard<std::mutex> lock(_localMapsMutex);
    return _localZones;
}


// ── Player ownership ─────────────────────────────────────────────────────────

void ClusterMgr::SetPlayerOwner(uint64 guid, uint8 nodeId)
{
    std::lock_guard<std::mutex> lock(_ownershipMutex);
    _playerOwnership[guid] = nodeId;
}

uint8 ClusterMgr::GetPlayerOwner(uint64 guid) const
{
    std::lock_guard<std::mutex> lock(_ownershipMutex);
    auto it = _playerOwnership.find(guid);
    return (it != _playerOwnership.end()) ? it->second : 0;
}

bool ClusterMgr::IsPlayerOwnedLocally(uint64 guid) const
{
    std::lock_guard<std::mutex> lock(_ownershipMutex);
    auto it = _playerOwnership.find(guid);
    return (it != _playerOwnership.end()) ? (it->second == sProxyClient.GetNodeId()) : false;
}

// ── Pending player transfers ─────────────────────────────────────────────────

void ClusterMgr::StorePendingTransfer(uint64 guid, PlayerTransferData&& data)
{
    std::lock_guard<std::mutex> lock(_transferMutex);
    LOG_INFO("server.worldserver", "ClusterMgr: Stored pending transfer for GUID {:016X} (map {})",
             guid, data.mapId);
    _pendingTransfers[guid] = std::move(data);
}

std::optional<PlayerTransferData> ClusterMgr::TakePendingTransfer(uint64 guid)
{
    std::lock_guard<std::mutex> lock(_transferMutex);
    auto it = _pendingTransfers.find(guid);
    if (it == _pendingTransfers.end())
        return std::nullopt;
    PlayerTransferData data = std::move(it->second);
    _pendingTransfers.erase(it);
    return data;
}

// ── Client redirect (proxy-less transfers) ──────────────────────────────────

void ClusterMgr::StorePendingRedirect(uint32 accountId, PendingRedirect&& redirect)
{
    std::lock_guard<std::mutex> lock(_redirectMutex);
    _pendingRedirects[accountId] = std::move(redirect);
    LOG_INFO("server.worldserver", "ClusterMgr: Stored redirect token for account {} (GUID {:016X} from node {})",
             accountId, redirect.playerGuid, redirect.sourceNodeId);
}

std::optional<ClusterMgr::PendingRedirect> ClusterMgr::TakePendingRedirect(uint32 accountId)
{
    std::lock_guard<std::mutex> lock(_redirectMutex);
    auto it = _pendingRedirects.find(accountId);
    if (it == _pendingRedirects.end())
        return std::nullopt;
    PendingRedirect data = std::move(it->second);
    _pendingRedirects.erase(it);
    return data;
}

std::pair<std::string, uint16> ClusterMgr::GetRedirectAddressForNode(uint8 destNodeId, std::string const& clientIp) const
{
    std::lock_guard<std::mutex> lock(_nodeMutex);
    auto it = _nodes.find(destNodeId);
    if (it == _nodes.end())
        return {"", 0};

    auto const& node = it->second;

    // If client is on a local network and node has NAT config, use internal address
    if (!node.externalAddress.empty() && node.externalPort != 0)
    {
        // Check if client is local (RFC1918)
        uint32 addr = ntohl(inet_addr(clientIp.c_str()));
        bool isLocal = ((addr & 0xFF000000) == 0x0A000000) ||   // 10.0.0.0/8
                       ((addr & 0xFFF00000) == 0xAC100000) ||   // 172.16.0.0/12
                       ((addr & 0xFFFF0000) == 0xC0A80000) ||   // 192.168.0.0/16
                       ((addr & 0xFF000000) == 0x7F000000);     // 127.0.0.0/8

        if (isLocal)
            return {node.address, node.port};
        else
            return {node.externalAddress, node.externalPort};
    }

    return {node.address, node.port};
}

std::string ClusterMgr::GetGameAddress() const { return _gameAddress; }
uint16 ClusterMgr::GetGamePort() const { return _gamePort; }
std::string ClusterMgr::GetExternalAddress() const { return _externalAddress; }
uint16 ClusterMgr::GetExternalPort() const { return _externalPort; }
uint8 ClusterMgr::GetNodeId() const { return _nodeId; }
