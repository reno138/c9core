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

#include "ManagementClient.h"
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief ncurses-based Midnight Commander-style TUI for the cluster manager.
 *
 * Layout:
 *   ┌──────────────────────────────────────────────────────────────────┐
 *   │  C9Core Cluster Manager                    [CONNECTED]      │  <- title
 *   ├───┬──────────┬──────────┬────────┬─────────┬────────────────────┤
 *   │ID │ State    │ Players  │  PID   │ Uptime  │ Host               │  <- header
 *   ├───┼──────────┼──────────┼────────┼─────────┼────────────────────┤
 *   │ 1 │ RUNNING  │ 143/500  │  1234  │  2h14m  │ 10.0.0.1:8086      │  <- rows
 *   │ 2 │ STOPPED  │   0/500  │    -   │     -   │ 10.0.0.2:8086      │
 *   ├───┴──────────┴──────────┴────────┴─────────┴────────────────────┤
 *   │  Proxy: 127.0.0.1:9090  Nodes: 2  Last update: 3s ago           │  <- status
 *   ├──────────────────────────────────────────────────────────────────┤
 *   │ F2:Start  F3:Stop  F4:Deploy  F5:Refresh  F10:Quit              │  <- fkey bar
 *   └──────────────────────────────────────────────────────────────────┘
 *
 * Threading:
 *   - Run() blocks the calling thread (main thread) running the ncurses loop.
 *   - UpdateNodes() / SetConnected() are called from the io_context thread
 *     and are therefore thread-safe (mutex-protected + atomic dirty flag).
 */
class ClusterUI
{
public:
    ClusterUI(std::string proxyHost, uint16 proxyPort,
              std::shared_ptr<ManagementClient> client);
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
    void OpenDeployWizard();
    void Refresh();

    std::string FormatUptime(uint32 secs) const;
    std::string FormatState(uint8 state) const;
    int         StateColorPair(uint8 state) const;
    std::string FormatLatency(int32 ms) const;
    std::string FormatBandwidth(uint32 txBps, uint32 rxBps) const;

    // ── Ping / latency thread ─────────────────────────────────────────────────
    void PingLoop();
    void PingNode(uint8 nodeId, std::string const& addr);

    std::map<uint8, int32> _latencyMs;   ///< nodeId → ms (-1 = unknown)
    std::mutex             _latencyMutex;
    std::atomic<bool>      _pingRunning { false };
    std::thread            _pingThread;

    // ── Shared state (io_context thread writes, main thread reads) ────────────
    std::mutex           _nodesMutex;
    std::vector<NodeInfo> _nodes;
    std::atomic<bool>    _dirty   { false };
    std::atomic<bool>    _connected { false };

    // ── Selection state ────────────────────────────────────────────────────────
    int _selectedRow { 0 };

    // ── Connection info ────────────────────────────────────────────────────────
    std::string _proxyHost;
    uint16      _proxyPort { 0 };
    std::shared_ptr<ManagementClient> _client;

    // ── Timing ────────────────────────────────────────────────────────────────
    std::chrono::steady_clock::time_point _lastUpdate;

    // ── Color pair IDs ────────────────────────────────────────────────────────
    static constexpr int COLOR_TITLE       = 1;  ///< Blue bg, white fg
    static constexpr int COLOR_HEADER      = 2;  ///< Cyan fg
    static constexpr int COLOR_SELECTED    = 3;  ///< Reverse highlight
    static constexpr int COLOR_RUNNING     = 4;  ///< Green fg
    static constexpr int COLOR_STOPPED     = 5;  ///< White/normal
    static constexpr int COLOR_STARTING    = 6;  ///< Yellow fg
    static constexpr int COLOR_CRASHED     = 7;  ///< Red fg
    static constexpr int COLOR_STATUS      = 8;  ///< Status bar
    static constexpr int COLOR_FKEY        = 9;  ///< Function-key bar
    static constexpr int COLOR_CONNECTED   = 10; ///< Green for "CONNECTED"
    static constexpr int COLOR_DISCONNECTED= 11; ///< Red for "DISCONNECTED"
};

#endif // ClusterUI_h__
