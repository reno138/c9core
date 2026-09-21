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

#ifndef ClusterUI_h__
#define ClusterUI_h__

#include "NatsMonitor.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief ncurses-based TUI for the cluster manager.
 *
 * Layout:
 *   ┌──────────────────────────────────────────────────────────────────────────────┐
 *   │  C9Core Cluster Manager                                  [CONNECTED]         │
 *   ├───┬──────────┬──────────┬───────┬───────┬───────┬────────┬──────────────────┤
 *   │ID │ State    │ Players  │  Mem  │ CPU%  │  PID  │ Uptime │ Host             │
 *   ├───┼──────────┼──────────┼───────┼───────┼───────┼────────┼──────────────────┤
 *   │ 1 │ RUNNING  │  12/500  │1200MB │  14%  │ 51182 │  3h22m │ 192.0.2.70:8086  │
 *   │ 2 │ CRASHED  │   0/500  │   -   │   -   │     - │      - │ 192.0.2.71:8086  │
 *   │   │  ↳ Was up 2h14m · 23 players · Crashes: 3                               │
 *   └──────────────────────────────────────────────────────────────────────────────┘
 *
 * Threading:
 *   Run() blocks the main thread running the ncurses loop.
 *   UpdateNodes() / SetConnected() are called from NATS callback threads
 *   and are therefore thread-safe (mutex-protected + atomic dirty flag).
 */
class ClusterUI
{
public:
    ClusterUI(std::string natsUrl, std::shared_ptr<NatsMonitor> monitor);
    ~ClusterUI();

    /// Block and run the UI loop. Returns when the user quits (F10).
    void Run();

    /// Called from any thread when a new status snapshot arrives.
    void UpdateNodes(std::vector<NodeInfo> nodes);

    /// Called from any thread when connection state changes.
    void SetConnected(bool connected);

private:
    void InitColors();
    void Draw();
    void DrawTitle();
    void DrawNodeTable();
    void DrawStatusBar();
    void DrawFkeyBar();
    void HandleInput(int key);

    void StartSelected();
    void StopSelected();
    void KillSelected();      ///< immediate SIGKILL via the supervisor (asks first)
    void RestartSelected();
    void OpenDeployWizard();
    void Refresh();

    /// Sorted snapshot of the node table as drawn.
    std::vector<NodeInfo> SortedNodes();

    /// nodeId of the highlighted row, or nullopt if the table is empty.
    /// Selection is tracked by nodeId, not row index: the NATS thread can
    /// replace _nodes between the last Draw() and a keypress, and a node
    /// appearing or disappearing in that gap would otherwise send F6 KILL to
    /// whichever node slid into the highlighted row.
    std::optional<uint8> SelectedNodeId();

    /// Modal yes/no prompt on the status line. Blocks for the answer.
    bool Confirm(std::string const& question);

    /// Scatter-plot of live player positions for the selected node's maps.
    /// Worldserver coordinates are roughly +/-17066 on both axes; positions are
    /// normalised to the panel rather than assuming a fixed extent, so a map
    /// whose players are clustered still fills the view.
    void DrawMapPanel();

    std::string FormatUptime(uint32 secs) const;
    std::string FormatState(uint8 state) const;
    int         StateColorPair(uint8 state) const;
    std::string FormatLatency(int32 ms) const;
    std::string FormatBandwidth(uint32 txBps, uint32 rxBps) const;
    std::string FormatMemory(uint32 mb) const;

    // ── Ping / latency thread ─────────────────────────────────────────────────
    void PingLoop();
    void PingNode(uint8 nodeId, std::string const& addr);

    std::map<uint8, int32> _latencyMs;   ///< nodeId → ms (-1 = unknown)
    std::mutex             _latencyMutex;
    std::atomic<bool>      _pingRunning { false };
    std::thread            _pingThread;

    // ── Shared state (NATS callback threads write, main thread reads) ─────────
    std::mutex            _nodesMutex;
    std::vector<NodeInfo> _nodes;
    std::atomic<bool>     _dirty    { false };
    std::atomic<bool>     _connected{ false };

    // ── Selection state ────────────────────────────────────────────────────────
    std::optional<uint8> _selectedNodeId;   ///< keyed by nodeId, see SelectedNodeId()
    bool _showMap     { true };   ///< F8 toggles the map/player panel

    // ── Connection info ────────────────────────────────────────────────────────
    std::string _natsUrl;
    std::shared_ptr<NatsMonitor> _monitor;

    // ── Timing ────────────────────────────────────────────────────────────────
    /// steady_clock ns of the last UpdateNodes(); written on the NATS thread,
    /// read by DrawStatusBar() on the main thread.
    std::atomic<int64> _lastUpdateNs{ 0 };

    // ── Color pair IDs ────────────────────────────────────────────────────────
    static constexpr int COLOR_TITLE        = 1;
    static constexpr int COLOR_HEADER       = 2;
    static constexpr int COLOR_SELECTED     = 3;
    static constexpr int COLOR_RUNNING      = 4;
    static constexpr int COLOR_STOPPED      = 5;
    static constexpr int COLOR_STARTING     = 6;
    static constexpr int COLOR_CRASHED      = 7;
    static constexpr int COLOR_STATUS       = 8;
    static constexpr int COLOR_FKEY         = 9;
    static constexpr int COLOR_CONNECTED    = 10;
    static constexpr int COLOR_DISCONNECTED = 11;
    static constexpr int COLOR_CRASH_SUB    = 12;  ///< Dim red for crashed sub-row
    static constexpr int COLOR_MAP_BORDER   = 13;
    static constexpr int COLOR_MAP_PLAYER   = 14;
    static constexpr int COLOR_HUNG         = 15;  ///< supervisor up, worldserver silent
};

#endif // ClusterUI_h__
