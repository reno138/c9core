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

#include "ClusterUI.h"
#include "Config.h"
#include "DeployWizard.h"
#include <algorithm>
#include "BoostProcess.h"
#include <ncurses.h>
#include <sstream>
#include <thread>


// ── Column widths (content only, not counting the `|` separator) ──────────────
static constexpr int W_ID      = 4;
static constexpr int W_STATE   = 10;
static constexpr int W_PLAYERS = 10;
static constexpr int W_PID     = 7;
static constexpr int W_UPTIME  = 9;
static constexpr int W_LATENCY = 8;
static constexpr int W_BW      = 13;  // "1.2M↑ 900K↓"
// W_HOST: remaining width after all columns + separators

ClusterUI::ClusterUI(std::string proxyHost, uint16 proxyPort, std::shared_ptr<ManagementClient> client)
    : _proxyHost(std::move(proxyHost))
    , _proxyPort(proxyPort)
    , _client(std::move(client))
    , _lastUpdate(std::chrono::steady_clock::now())
{
}

ClusterUI::~ClusterUI()
{
    // Stop ping thread if still running
    _pingRunning = false;
    if (_pingThread.joinable())
        _pingThread.join();

    endwin();
}

void ClusterUI::InitColors()
{
    // Background colors use COLOR_BLACK; title/status bars use COLOR_BLUE bg
    init_pair(COLOR_TITLE,        COLOR_WHITE,  COLOR_BLUE);
    init_pair(COLOR_HEADER,       COLOR_CYAN,   COLOR_BLACK);
    init_pair(COLOR_SELECTED,     COLOR_BLACK,  COLOR_CYAN);
    init_pair(COLOR_RUNNING,      COLOR_GREEN,  COLOR_BLACK);
    init_pair(COLOR_STOPPED,      COLOR_WHITE,  COLOR_BLACK);
    init_pair(COLOR_STARTING,     COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_CRASHED,      COLOR_RED,    COLOR_BLACK);
    init_pair(COLOR_STATUS,       COLOR_WHITE,  COLOR_BLUE);
    init_pair(COLOR_FKEY,         COLOR_BLACK,  COLOR_CYAN);
    init_pair(COLOR_CONNECTED,    COLOR_GREEN,  COLOR_BLUE);
    init_pair(COLOR_DISCONNECTED, COLOR_RED,    COLOR_BLUE);
}

void ClusterUI::Run()
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    halfdelay(5);   // 0.5 second timeout on getch()
    curs_set(0);    // hide cursor

    if (has_colors())
    {
        start_color();
        use_default_colors();
        InitColors();
    }

    _dirty = true;

    // Start background latency ping thread
    _pingRunning = true;
    _pingThread = std::thread(&ClusterUI::PingLoop, this);

    bool running = true;
    while (running)
    {
        if (_dirty.exchange(false))
            Draw();

        int ch = getch();
        if (ch == ERR)
        {
            // Half-delay timeout — redraw to update uptime counters in status bar
            Draw();
            continue;
        }

        if (ch == KEY_F(10) || ch == 'q' || ch == 'Q')
        {
            running = false;
        }
        else
        {
            HandleInput(ch);
            Draw();
        }
    }

    endwin();

    // Stop ping thread
    _pingRunning = false;
    if (_pingThread.joinable())
        _pingThread.join();
}

void ClusterUI::UpdateNodes(std::vector<NodeInfo> nodes)
{
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        _nodes = std::move(nodes);
    }
    _lastUpdate = std::chrono::steady_clock::now();
    _dirty = true;
}

void ClusterUI::SetConnected(bool connected)
{
    _connected = connected;
    _dirty = true;
}

void ClusterUI::Draw()
{
    if (LINES < 6 || COLS < 40)
    {
        clear();
        mvprintw(0, 0, "Terminal too small. Resize to at least 80x24.");
        refresh();
        return;
    }
    DrawTitle();
    DrawNodeTable();
    DrawStatusBar();
    DrawFkeyBar();
    refresh();
}

void ClusterUI::DrawTitle()
{
    attron(COLOR_PAIR(COLOR_TITLE) | A_BOLD);
    mvhline(0, 0, ' ', COLS);
    mvprintw(0, 1, "C9Core Cluster Manager");
    attroff(COLOR_PAIR(COLOR_TITLE) | A_BOLD);

    bool conn = _connected.load();
    std::string status = conn ? " [CONNECTED] " : " [DISCONNECTED] ";
    int statusX = COLS - static_cast<int>(status.size());
    if (statusX > 28)
    {
        int cp = conn ? COLOR_CONNECTED : COLOR_DISCONNECTED;
        attron(COLOR_PAIR(cp) | A_BOLD);
        mvprintw(0, statusX, "%s", status.c_str());
        attroff(COLOR_PAIR(cp) | A_BOLD);
    }
}

void ClusterUI::DrawNodeTable()
{
    // Compute host column width from remaining space
    // Columns: space(1) + ID + | + STATE + | + PLAYERS + | + PID + | + UPTIME + | + LATENCY + | + BW + | + HOST
    int hostW = COLS - 1 - W_ID - 1 - W_STATE - 1 - W_PLAYERS - 1 - W_PID - 1 - W_UPTIME - 1 - W_LATENCY - 1 - W_BW - 1;
    if (hostW < 8) hostW = 8;

    // ── Column header row ─────────────────────────────────────────────────────
    attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
    mvhline(1, 0, ' ', COLS);
    mvprintw(1, 0, " %-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s",
             W_ID,      "ID",
             W_STATE,   " State",
             W_PLAYERS, " Players",
             W_PID,     " PID",
             W_UPTIME,  " Uptime",
             W_LATENCY, " Ping",
             W_BW,      " Bandwidth",
             hostW,     " Host");
    attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);

    // Separator below header
    mvhline(2, 0, ACS_HLINE, COLS);

    // ── Node data rows ────────────────────────────────────────────────────────
    std::vector<NodeInfo> nodes;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        nodes = _nodes;
    }

    // Clamp selection
    if (!nodes.empty())
    {
        if (_selectedRow >= static_cast<int>(nodes.size()))
            _selectedRow = static_cast<int>(nodes.size()) - 1;
        if (_selectedRow < 0) _selectedRow = 0;
    }

    int maxDataRows = LINES - 6;  // title(1)+header(1)+sep(1)+sep(1)+status(1)+fkey(1)
    int startRow    = 3;

    for (int i = 0; i < maxDataRows; ++i)
    {
        int row = startRow + i;
        mvhline(row, 0, ' ', COLS);  // clear line

        if (nodes.empty())
        {
            if (i == 0)
            {
                attron(COLOR_PAIR(COLOR_STOPPED));
                mvprintw(row, 2, "No nodes registered with proxy.");
                attroff(COLOR_PAIR(COLOR_STOPPED));
            }
            continue;
        }

        if (i >= static_cast<int>(nodes.size()))
            continue;

        NodeInfo const& n = nodes[i];
        bool selected = (i == _selectedRow);

        std::string stateStr  = FormatState(n.state);
        std::string players   = std::to_string(n.playerCount) + "/" + std::to_string(n.maxPlayers);
        std::string pidStr    = n.pid ? std::to_string(n.pid) : "-";
        std::string uptime    = FormatUptime(n.uptimeSecs);
        std::string host      = n.address.empty() ? "-" : (n.address + ":" + std::to_string(n.port));
        if (static_cast<int>(host.size()) > hostW)
            host = host.substr(0, hostW - 1) + ">";

        // Latency: prefer control-channel RTT from proxy; fall back to ICMP ping.
        int32 latMs = -1;
        if (n.latencyMs > 0)
        {
            // Control-channel RTT measured by proxy via MSG_PING/MSG_PONG.
            latMs = static_cast<int32>(n.latencyMs);
        }
        else
        {
            std::lock_guard<std::mutex> lock(_latencyMutex);
            auto it = _latencyMs.find(n.nodeId);
            if (it != _latencyMs.end())
                latMs = it->second;
        }
        std::string latStr = FormatLatency(latMs);

        // Bandwidth
        std::string bwStr = FormatBandwidth(n.txBps, n.rxBps);

        // Print whole row in selected/normal color
        if (selected)
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);

        mvprintw(row, 0, " %-*d|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s",
                 W_ID,      n.nodeId,
                 W_STATE,   stateStr.c_str(),
                 W_PLAYERS, players.c_str(),
                 W_PID,     pidStr.c_str(),
                 W_UPTIME,  uptime.c_str(),
                 W_LATENCY, latStr.c_str(),
                 W_BW,      bwStr.c_str(),
                 hostW,     host.c_str());

        if (selected)
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);

        // For non-selected rows, recolor the state column
        if (!selected)
        {
            int stateCol = 1 + W_ID + 1;  // space + ID + '|'
            attron(COLOR_PAIR(StateColorPair(n.state)) | A_BOLD);
            mvprintw(row, stateCol, "%-*s", W_STATE, stateStr.c_str());
            attroff(COLOR_PAIR(StateColorPair(n.state)) | A_BOLD);
        }
    }

    // Separator above status bar
    mvhline(LINES - 3, 0, ACS_HLINE, COLS);
}

void ClusterUI::DrawStatusBar()
{
    auto now = std::chrono::steady_clock::now();
    auto ageSecs = static_cast<uint32>(
        std::chrono::duration_cast<std::chrono::seconds>(now - _lastUpdate).count());
    std::string ageStr = (ageSecs == 0) ? "just now" : (std::to_string(ageSecs) + "s ago");

    std::size_t nodeCount;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        nodeCount = _nodes.size();
    }

    attron(COLOR_PAIR(COLOR_STATUS));
    mvhline(LINES - 2, 0, ' ', COLS);
    mvprintw(LINES - 2, 1, "Proxy: %s:%d  |  Nodes: %zu  |  Last update: %s",
             _proxyHost.c_str(), _proxyPort, nodeCount, ageStr.c_str());
    attroff(COLOR_PAIR(COLOR_STATUS));
}

void ClusterUI::DrawFkeyBar()
{
    attron(COLOR_PAIR(COLOR_FKEY));
    mvhline(LINES - 1, 0, ' ', COLS);

    struct FKeyEntry { const char* key; const char* label; };
    static constexpr FKeyEntry keys[] = {
        { "F2",  " Start " },
        { "F3",  " Stop  " },
        { "F4",  " Deploy" },
        { "F5",  " Refr  " },
        { "F10", " Quit  " },
    };

    int x = 0;
    for (auto const& fk : keys)
    {
        int keyLen   = static_cast<int>(std::strlen(fk.key));
        int labelLen = static_cast<int>(std::strlen(fk.label));
        if (x + keyLen + labelLen >= COLS)
            break;

        // Key number: reverse video on cyan
        attron(A_REVERSE);
        mvprintw(LINES - 1, x, "%s", fk.key);
        attroff(A_REVERSE);
        x += keyLen;

        // Label: normal cyan background
        mvprintw(LINES - 1, x, "%s", fk.label);
        x += labelLen;
    }
    attroff(COLOR_PAIR(COLOR_FKEY));
}

void ClusterUI::HandleInput(int key)
{
    switch (key)
    {
        case KEY_UP:
            if (_selectedRow > 0) --_selectedRow;
            break;
        case KEY_DOWN:
        {
            std::lock_guard<std::mutex> lock(_nodesMutex);
            if (_selectedRow < static_cast<int>(_nodes.size()) - 1)
                ++_selectedRow;
            break;
        }
        case KEY_F(2):
            StartSelected();
            break;
        case KEY_F(3):
            StopSelected();
            break;
        case KEY_F(4):
            OpenDeployWizard();
            break;
        case KEY_F(5):
            Refresh();
            break;
        default:
            break;
    }
}

void ClusterUI::StartSelected()
{
    uint8 nodeId;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        if (_nodes.empty() || _selectedRow >= static_cast<int>(_nodes.size()))
            return;
        nodeId = _nodes[_selectedRow].nodeId;
    }
    _client->SendStartNode(nodeId);
}

void ClusterUI::StopSelected()
{
    uint8 nodeId;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        if (_nodes.empty() || _selectedRow >= static_cast<int>(_nodes.size()))
            return;
        nodeId = _nodes[_selectedRow].nodeId;
    }
    _client->SendStopNode(nodeId);
}

void ClusterUI::OpenDeployWizard()
{
    // Transition to blocking input mode for the wizard form
    nocbreak();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(1);

    DeployWizard::Config cfg;
    cfg.worldserverBin  = sConfigMgr->GetOption<std::string>("Deploy.WorldserverBin",  "./worldserver");
    cfg.nodemgrBin      = sConfigMgr->GetOption<std::string>("Deploy.NodemgrBin",      "./nodemgr");
    cfg.worldserverConf = sConfigMgr->GetOption<std::string>("Deploy.WorldserverConf", "./worldserver.conf");
    cfg.nodemgrConf     = sConfigMgr->GetOption<std::string>("Deploy.NodemgrConf",     "./nodemgr.conf");
    cfg.sshUser         = sConfigMgr->GetOption<std::string>("Deploy.DefaultSSHUser",  "wow");
    cfg.sshPort         = sConfigMgr->GetOption<int32>      ("Deploy.DefaultSSHPort",  22);
    cfg.sshKey          = sConfigMgr->GetOption<std::string>("Deploy.DefaultSSHKey",   "~/.ssh/id_rsa");
    cfg.remotePath      = sConfigMgr->GetOption<std::string>("Deploy.DefaultRemotePath","/opt/c9core");
    cfg.proxyAddress    = _proxyHost;  // pass proxy address for sed patching

    DeployWizard wizard(std::move(cfg));
    std::string errMsg;
    wizard.Run(errMsg);

    // Restore half-delay for the main event loop
    halfdelay(5);
    curs_set(0);
    _dirty = true;
}

void ClusterUI::Refresh()
{
    // The proxy sends status pushes automatically; we just ask for a redraw.
    _dirty = true;
}

// ── Latency ping thread ───────────────────────────────────────────────────────

void ClusterUI::PingLoop()
{
    // Stagger the first ping by 2 seconds so the UI has time to get nodes
    for (int i = 0; i < 4 && _pingRunning; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    while (_pingRunning)
    {
        std::vector<NodeInfo> snap;
        {
            std::lock_guard<std::mutex> lock(_nodesMutex);
            snap = _nodes;
        }

        for (auto const& n : snap)
        {
            if (!_pingRunning)
                break;
            if (!n.address.empty())
                PingNode(n.nodeId, n.address);
        }

        // Sleep 10 seconds between rounds, checking stop flag
        for (int i = 0; i < 20 && _pingRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void ClusterUI::PingNode(uint8 nodeId, std::string const& addr)
{
    // ping -c 1 -W 1 <addr> → parse "time=X.X ms" from stdout
    std::vector<std::string> args = { "-c", "1", "-W", "1", addr };

    try
    {
        bp::ipstream out;
        bp::child proc("ping", bp::args(args), bp::std_out > out, bp::std_err > bp::null);

        int32 latMs = -1;
        std::string line;
        while (std::getline(out, line))
        {
            // Look for "time=X.X ms" or "time=X ms"
            auto pos = line.find("time=");
            if (pos != std::string::npos)
            {
                std::istringstream ss(line.substr(pos + 5));
                float ms = 0.0f;
                if (ss >> ms)
                    latMs = static_cast<int32>(ms + 0.5f);
            }
        }

        proc.wait();

        {
            std::lock_guard<std::mutex> lock(_latencyMutex);
            _latencyMs[nodeId] = latMs;
        }
        _dirty = true;
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(_latencyMutex);
        _latencyMs[nodeId] = -1;
    }
}

// ── Formatters ────────────────────────────────────────────────────────────────

std::string ClusterUI::FormatUptime(uint32 secs) const
{
    if (secs == 0) return "-";
    uint32 h = secs / 3600;
    uint32 m = (secs % 3600) / 60;
    uint32 s = secs % 60;
    if (h > 0) return std::to_string(h) + "h " + std::to_string(m) + "m";
    if (m > 0) return std::to_string(m) + "m " + std::to_string(s) + "s";
    return std::to_string(s) + "s";
}

std::string ClusterUI::FormatLatency(int32 ms) const
{
    if (ms < 0)  return "  ?";
    return std::to_string(ms) + "ms";
}

std::string ClusterUI::FormatBandwidth(uint32 txBps, uint32 rxBps) const
{
    // Format a single byte/sec value as "X.Xu" or "Xu"
    auto fmtBps = [](uint32 bps) -> std::string
    {
        if (bps >= 1048576)
            return std::to_string(bps / 1048576) + "M";
        if (bps >= 1024)
            return std::to_string(bps / 1024) + "K";
        return std::to_string(bps) + "B";
    };

    if (txBps == 0 && rxBps == 0)
        return "-";
    return fmtBps(txBps) + "u " + fmtBps(rxBps) + "d";
}

std::string ClusterUI::FormatState(uint8 state) const
{
    switch (state)
    {
        case 0: return "UNKNOWN";
        case 1: return "STOPPED";
        case 2: return "STARTING";
        case 3: return "RUNNING";
        case 4: return "STOPPING";
        case 5: return "CRASHED";
        default: return "?";
    }
}

int ClusterUI::StateColorPair(uint8 state) const
{
    switch (state)
    {
        case 2:  return COLOR_STARTING;  // Starting
        case 3:  return COLOR_RUNNING;   // Running
        case 4:  return COLOR_STARTING;  // Stopping
        case 5:  return COLOR_CRASHED;   // Crashed
        default: return COLOR_STOPPED;   // Unknown / Stopped
    }
}
