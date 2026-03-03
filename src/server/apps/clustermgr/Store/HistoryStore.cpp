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

#include "HistoryStore.h"

HistoryStore::HistoryStore(std::size_t maxSamples)
    : _maxSamples(maxSamples)
{
}

void HistoryStore::RecordSample(uint8 nodeId, NodeSample const& sample)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto& h = _history[nodeId];
    h.samples.push_back(sample);
    while (h.samples.size() > _maxSamples)
        h.samples.pop_front();
}

void HistoryStore::RecordCrash(uint8 nodeId, CrashEvent const& event)
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto& h = _history[nodeId];
    h.crashes.push_back(event);
    while (h.crashes.size() > MAX_CRASH_EVENTS)
        h.crashes.pop_front();
}

std::vector<NodeSample> HistoryStore::GetSamples(uint8 nodeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _history.find(nodeId);
    if (it == _history.end())
        return {};
    return std::vector<NodeSample>(it->second.samples.begin(), it->second.samples.end());
}

std::vector<CrashEvent> HistoryStore::GetCrashes(uint8 nodeId) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    auto it = _history.find(nodeId);
    if (it == _history.end())
        return {};
    return std::vector<CrashEvent>(it->second.crashes.begin(), it->second.crashes.end());
}
