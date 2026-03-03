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

#ifndef NatsMonitor_h__
#define NatsMonitor_h__

#include "Define.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Forward-declare nats.c opaque types (same pattern as ProxyClient.h).
struct __natsConnection;
struct __natsSubscription;
struct __natsMsg;
typedef struct __natsConnection   natsConnection;
typedef struct __natsSubscription natsSubscription;
typedef struct __natsMsg          natsMsg;

/// Extended node snapshot populated from cluster.mgmt.status.
struct NodeInfo
{
    uint8       nodeId      { 0 };
    uint8       state       { 0 };   ///< 0=unknown 1=stopped 2=starting 3=running 4=stopping 5=crashed
    uint16      playerCount { 0 };
    uint16      maxPlayers  { 0 };
    uint32      pid         { 0 };
    uint32      uptimeSecs  { 0 };
    uint32      memUsageMB  { 0 };   ///< Resident memory in MB
    uint8       cpuPercent  { 0 };   ///< CPU usage 0–100
    uint16      crashCount  { 0 };   ///< Peer-crash events detected by this node
    uint32      txBps       { 0 };   ///< NATS bytes/sec TX
    uint32      rxBps       { 0 };   ///< NATS bytes/sec RX
    uint32      latencyMs   { 0 };   ///< ICMP RTT in ms (0 = not yet measured)
    std::vector<uint32> mapIds;      ///< Map IDs hosted by this node
    std::string address;             ///< Node LAN IP
    uint16      port        { 8086 };
    std::chrono::steady_clock::time_point lastSeen; ///< When last mgmt.status received
};

/// Per-player position snapshot from cluster.mgmt.players.
struct PlayerInfo
{
    uint64      guid      { 0 };
    uint8       nodeId    { 0 };
    uint16      mapId     { 0 };
    float       x         { 0.0f };
    float       y         { 0.0f };
    float       z         { 0.0f };
    uint16      zoneId    { 0 };
    uint8       level     { 0 };
    uint8       classId   { 0 };
    uint8       raceId    { 0 };
    uint8       teamId    { 0 };
    std::string name;
};

/**
 * @brief NATS subscriber that replaces ManagementClient for the proxy-less architecture.
 *
 * Subscribes to:
 *   cluster.mgmt.status  — node health snapshots (every 5 s)
 *   cluster.mgmt.players — player positions (every 3 s)
 *
 * Calls StatusCallback whenever status data changes.
 * Player data is available via GetPlayers().
 *
 * Dead-node detection: a watchdog thread marks nodes CRASHED (state=5)
 * when no cluster.mgmt.status has arrived for > DeadThresholdSecs (default 30 s).
 */
class NatsMonitor
{
public:
    /// Invoked from the NATS callback thread; receives a snapshot of all known nodes.
    using StatusCallback = std::function<void(std::vector<NodeInfo>)>;

    NatsMonitor();
    ~NatsMonitor();

    /// Connect to NATS and begin subscribing.  Call once before the UI loop starts.
    void Start(std::string const& natsUrl, StatusCallback statusCb,
               uint32 deadThresholdSecs = 30);

    bool IsConnected() const { return _connected.load(); }

    /// Thread-safe: returns a snapshot of all currently known nodes.
    std::vector<NodeInfo> GetNodes() const;

    /// Thread-safe: returns a snapshot of all currently online players.
    std::vector<PlayerInfo> GetPlayers() const;

    /// No-op stubs — kept for ClusterUI F2/F3 key wiring (will be a NATS control message later).
    void SendStartNode(uint8 nodeId);
    void SendStopNode(uint8 nodeId);

private:
    static void OnStatusMsg(natsConnection* nc, natsSubscription* sub,
                            natsMsg* msg, void* closure);
    static void OnPlayersMsg(natsConnection* nc, natsSubscription* sub,
                             natsMsg* msg, void* closure);

    void ParseStatus(natsMsg* msg);
    void ParsePlayers(natsMsg* msg);

    /// Background thread that marks nodes CRASHED after silence > _deadThresholdSecs.
    void WatchdogLoop();

    natsConnection*   _nc{ nullptr };
    natsSubscription* _subStatus{ nullptr };
    natsSubscription* _subPlayers{ nullptr };

    StatusCallback _statusCb;
    uint32         _deadThresholdSecs{ 30 };

    mutable std::mutex    _nodesMutex;
    std::map<uint8, NodeInfo> _nodes;   ///< keyed by nodeId

    mutable std::mutex    _playersMutex;
    std::vector<PlayerInfo> _players;

    std::atomic<bool> _connected{ false };

    std::thread      _watchdog;
    std::atomic<bool> _watchdogRunning{ false };
};

#endif // NatsMonitor_h__
