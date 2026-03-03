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

#ifndef HistoryStore_h__
#define HistoryStore_h__

#include "Define.h"
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

/// One time-series sample recorded from cluster.mgmt.status.
struct NodeSample
{
    uint32 timestampSec { 0 };   ///< Unix time
    uint16 playerCount  { 0 };
    uint32 memUsageMB   { 0 };
    uint8  cpuPercent   { 0 };
};

/// One crash event recorded when a node transitions RUNNING → CRASHED.
struct CrashEvent
{
    uint32 timestampSec { 0 };   ///< When the crash was detected
    uint32 uptimeSecs   { 0 };   ///< How long the node had been up
    uint16 playerCount  { 0 };   ///< Players online at time of crash
};

/**
 * @brief In-memory circular buffer of node health history.
 *
 * Retains up to maxSamples NodeSamples per node (ring-buffer style: oldest
 * evicted first).  Crash events are kept in a bounded deque (max 50 per node).
 *
 * Thread-safe via a single mutex.
 */
class HistoryStore
{
public:
    /// @param maxSamples  Maximum samples per node (default 720 = 1h @ 5s interval).
    explicit HistoryStore(std::size_t maxSamples = 720);

    /// Record a periodic sample for nodeId.
    void RecordSample(uint8 nodeId, NodeSample const& sample);

    /// Record a crash event for nodeId.
    void RecordCrash(uint8 nodeId, CrashEvent const& event);

    /// Return all retained samples for nodeId (oldest first).
    std::vector<NodeSample> GetSamples(uint8 nodeId) const;

    /// Return all retained crash events for nodeId (oldest first).
    std::vector<CrashEvent> GetCrashes(uint8 nodeId) const;

private:
    std::size_t _maxSamples;
    static constexpr std::size_t MAX_CRASH_EVENTS = 50;

    struct NodeHistory
    {
        std::deque<NodeSample> samples;
        std::deque<CrashEvent> crashes;
    };

    mutable std::mutex _mutex;
    std::unordered_map<uint8, NodeHistory> _history;
};

#endif // HistoryStore_h__
