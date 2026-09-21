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

// ── Column widths (content width, not counting `|` separator) ─────────────────
static constexpr int W_ID      = 3;
static constexpr int W_STATE   = 9;
static constexpr int W_PLAYERS = 9;
static constexpr int W_MEM     = 7;   // "1200MB"
static constexpr int W_CPU     = 5;   // " 14%"
static constexpr int W_PID     = 7;
static constexpr int W_UPTIME  = 7;
// W_HOST: remaining

static int64 NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

ClusterUI::ClusterUI(std::string natsUrl, std::shared_ptr<NatsMonitor> monitor)
    : _natsUrl(std::move(natsUrl))
    , _monitor(std::move(monitor))
{
    _lastUpdateNs = NowNs();
}

std::vector<NodeInfo> ClusterUI::SortedNodes()
{
    std::vector<NodeInfo> nodes;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        nodes = _nodes;
    }
    std::sort(nodes.begin(), nodes.end(),
              [](NodeInfo const& a, NodeInfo const& b) { return a.nodeId < b.nodeId; });
    return nodes;
}

std::optional<uint8> ClusterUI::SelectedNodeId()
{
    auto nodes = SortedNodes();
    if (nodes.empty())
        return std::nullopt;
    if (_selectedNodeId)
        for (auto const& n : nodes)
            if (n.nodeId == *_selectedNodeId)
                return _selectedNodeId;
    // Selected node vanished (or nothing selected yet): fall back to the first row.
    _selectedNodeId = nodes.front().nodeId;
    return _selectedNodeId;
}

bool ClusterUI::Confirm(std::string const& question)
{
    std::string prompt = " " + question + " [y/N] ";
    attron(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    mvhline(LINES - 2, 0, ' ', COLS);
    mvprintw(LINES - 2, 1, "%s", prompt.c_str());
    attroff(COLOR_PAIR(COLOR_STATUS) | A_BOLD);
    refresh();

    nocbreak();
    cbreak();          // blocking getch for the answer
    int ch = getch();
    halfdelay(5);
    _dirty = true;
    return ch == 'y' || ch == 'Y';
}

ClusterUI::~ClusterUI()
{
    _pingRunning = false;
    if (_pingThread.joinable())
        _pingThread.join();

    endwin();
}

void ClusterUI::InitColors()
{
    init_pair(COLOR_TITLE,        COLOR_WHITE,   COLOR_BLUE);
    init_pair(COLOR_HEADER,       COLOR_CYAN,    COLOR_BLACK);
    init_pair(COLOR_SELECTED,     COLOR_BLACK,   COLOR_CYAN);
    init_pair(COLOR_RUNNING,      COLOR_GREEN,   COLOR_BLACK);
    init_pair(COLOR_STOPPED,      COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_STARTING,     COLOR_YELLOW,  COLOR_BLACK);
    init_pair(COLOR_CRASHED,      COLOR_RED,     COLOR_BLACK);
    init_pair(COLOR_STATUS,       COLOR_WHITE,   COLOR_BLUE);
    init_pair(COLOR_FKEY,         COLOR_BLACK,   COLOR_CYAN);
    init_pair(COLOR_CONNECTED,    COLOR_GREEN,   COLOR_BLUE);
    init_pair(COLOR_DISCONNECTED, COLOR_RED,     COLOR_BLUE);
    init_pair(COLOR_CRASH_SUB,    COLOR_RED,     COLOR_BLACK);
    init_pair(COLOR_MAP_BORDER,   COLOR_CYAN,    COLOR_BLACK);
    init_pair(COLOR_MAP_PLAYER,   COLOR_GREEN,   COLOR_BLACK);
    init_pair(COLOR_HUNG,         COLOR_MAGENTA, COLOR_BLACK);
}

void ClusterUI::Run()
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    halfdelay(5);
    curs_set(0);

    if (has_colors())
    {
        start_color();
        use_default_colors();
        InitColors();
    }

    _dirty = true;

    _pingRunning = true;
    _pingThread  = std::thread(&ClusterUI::PingLoop, this);

    bool running = true;
    while (running)
    {
        if (_dirty.exchange(false))
            Draw();

        int ch = getch();
        if (ch == ERR)
        {
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
    _lastUpdateNs = NowNs();
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
    if (_showMap)
        DrawMapPanel();
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
    // Compute host column width from remaining space.
    // Fixed columns: space(1)+ID(W_ID)+|+STATE(W_STATE)+|+PLAYERS(W_PLAYERS)+|+MEM(W_MEM)+|+CPU(W_CPU)+|+PID(W_PID)+|+UPTIME(W_UPTIME)+|+HOST
    int fixedW = 1 + W_ID + 1 + W_STATE + 1 + W_PLAYERS + 1 + W_MEM + 1 + W_CPU + 1 + W_PID + 1 + W_UPTIME + 1;
    int hostW  = COLS - fixedW;
    if (hostW < 8) hostW = 8;

    // Header
    attron(COLOR_PAIR(COLOR_HEADER) | A_BOLD);
    mvhline(1, 0, ' ', COLS);
    mvprintw(1, 0, " %-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s",
             W_ID,      "ID",
             W_STATE,   " State",
             W_PLAYERS, " Players",
             W_MEM,     " Mem",
             W_CPU,     " CPU",
             W_PID,     " PID",
             W_UPTIME,  " Uptime",
             hostW,     " Host");
    attroff(COLOR_PAIR(COLOR_HEADER) | A_BOLD);

    mvhline(2, 0, ACS_HLINE, COLS);

    // Node rows (sorted by nodeId)
    std::vector<NodeInfo> nodes = SortedNodes();
    std::optional<uint8> selectedId = SelectedNodeId();

    int screenRow   = 3;
    int nodeIdx     = 0;

    for (; nodeIdx < static_cast<int>(nodes.size()) && screenRow < LINES - 3; ++nodeIdx)
    {
        NodeInfo const& n  = nodes[nodeIdx];
        bool selected      = selectedId && n.nodeId == *selectedId;
        bool isCrashed     = (n.state == 5);

        std::string stateStr = FormatState(n.state);
        std::string players  = std::to_string(n.playerCount) + "/" + std::to_string(n.maxPlayers);
        std::string pidStr   = n.pid ? std::to_string(n.pid) : "-";
        std::string uptime   = FormatUptime(n.uptimeSecs);
        std::string memStr   = isCrashed ? "-" : FormatMemory(n.memUsageMB);
        std::string cpuStr   = isCrashed ? "-" : (std::to_string(n.cpuPercent) + "%");
        std::string host     = n.address.empty() ? "-" : (n.address + ":" + std::to_string(n.port));
        if (static_cast<int>(host.size()) > hostW)
            host = host.substr(0, hostW - 1) + ">";

        // Main node row
        mvhline(screenRow, 0, ' ', COLS);
        if (selected)
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);

        mvprintw(screenRow, 0, " %-*d|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s|%-*s",
                 W_ID,      n.nodeId,
                 W_STATE,   stateStr.c_str(),
                 W_PLAYERS, players.c_str(),
                 W_MEM,     memStr.c_str(),
                 W_CPU,     cpuStr.c_str(),
                 W_PID,     pidStr.c_str(),
                 W_UPTIME,  uptime.c_str(),
                 hostW,     host.c_str());

        if (selected)
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);

        if (!selected)
        {
            int stateCol = 1 + W_ID + 1;
            attron(COLOR_PAIR(StateColorPair(n.state)) | A_BOLD);
            mvprintw(screenRow, stateCol, "%-*s", W_STATE, stateStr.c_str());
            attroff(COLOR_PAIR(StateColorPair(n.state)) | A_BOLD);
        }

        ++screenRow;

        // Crash sub-row
        if (isCrashed && screenRow < LINES - 3)
        {
            std::string sub;
            if (n.uptimeSecs > 0 || n.playerCount > 0)
            {
                sub = "   \u2514 Was up " + FormatUptime(n.uptimeSecs) +
                      " \xB7 " + std::to_string(n.playerCount) + " players" +
                      " \xB7 Crashes detected: " + std::to_string(n.crashCount);
            }
            else
            {
                sub = "   \u2514 Crashes detected: " + std::to_string(n.crashCount);
            }
            if (static_cast<int>(sub.size()) > COLS - 2)
                sub = sub.substr(0, COLS - 5) + "...";

            mvhline(screenRow, 0, ' ', COLS);
            attron(COLOR_PAIR(COLOR_CRASH_SUB) | A_DIM);
            mvprintw(screenRow, 0, "%s", sub.c_str());
            attroff(COLOR_PAIR(COLOR_CRASH_SUB) | A_DIM);
            ++screenRow;
        }
    }

    // Clear remaining rows
    for (; screenRow < LINES - 3; ++screenRow)
        mvhline(screenRow, 0, ' ', COLS);

    if (nodes.empty())
    {
        attron(COLOR_PAIR(COLOR_STOPPED));
        mvprintw(3, 2, "No nodes seen yet — waiting for cluster.mgmt.status...");
        attroff(COLOR_PAIR(COLOR_STOPPED));
    }

    mvhline(LINES - 3, 0, ACS_HLINE, COLS);
}

void ClusterUI::DrawStatusBar()
{
    int64 const ageNs = NowNs() - _lastUpdateNs.load();
    auto ageSecs = static_cast<uint32>(ageNs > 0 ? ageNs / 1000000000LL : 0);
    std::string ageStr = (ageSecs == 0) ? "just now" : (std::to_string(ageSecs) + "s ago");

    std::size_t nodeCount;
    {
        std::lock_guard<std::mutex> lock(_nodesMutex);
        nodeCount = _nodes.size();
    }

    attron(COLOR_PAIR(COLOR_STATUS));
    mvhline(LINES - 2, 0, ' ', COLS);
    mvprintw(LINES - 2, 1, "NATS: %s  |  Nodes: %zu  |  Last update: %s",
             _natsUrl.c_str(), nodeCount, ageStr.c_str());
    attroff(COLOR_PAIR(COLOR_STATUS));
}

void ClusterUI::DrawMapPanel()
{
    // Panel occupies the lower half of the screen, above the status/fkey bars.
    int const top    = LINES / 2;
    int const bottom = LINES - 3;          // leave the status + fkey rows
    int const height = bottom - top - 1;   // minus the border/title row
    int const width  = COLS - 2;

    if (height < 4 || width < 20)
        return;                            // not enough room to be useful

    // Which node is selected, and which maps does it serve?
    std::optional<uint8> selectedId = SelectedNodeId();
    if (!selectedId)
        return;
    uint8 nodeId = *selectedId;
    std::vector<uint32> mapIds;
    for (auto const& n : SortedNodes())
        if (n.nodeId == nodeId)
            mapIds = n.mapIds;

    // Only players on the selected node — the panel answers "who is on THIS
    // node and where", which is the question an operator asks before draining it.
    std::vector<PlayerInfo> all = _monitor ? _monitor->GetPlayers() : std::vector<PlayerInfo>{};
    std::vector<PlayerInfo> mine;
    mine.reserve(all.size());
    for (auto const& pl : all)
        if (pl.nodeId == nodeId)
            mine.push_back(pl);

    // Title row
    attron(COLOR_PAIR(COLOR_MAP_BORDER));
    mvhline(top, 0, ACS_HLINE, COLS);
    std::string mapList;
    for (size_t i = 0; i < mapIds.size(); ++i)
    {
        if (i) mapList += ",";
        mapList += std::to_string(mapIds[i]);
    }
    if (mapList.empty())
        mapList = "none";

    std::string title = " node " + std::to_string(nodeId) +
                        "  maps [" + mapList + "]  players " + std::to_string(mine.size()) + " ";
    mvprintw(top, 2, "%s", title.c_str());
    attroff(COLOR_PAIR(COLOR_MAP_BORDER));

    if (mine.empty())
    {
        mvprintw(top + 1 + height / 2, (COLS - 22) / 2, "(no players on this node)");
        return;
    }

    // Normalise to the actual extent of the players rather than the theoretical
    // +/-17066 map bounds — otherwise everyone in one city collapses to a dot.
    float minX = mine[0].x, maxX = mine[0].x;
    float minY = mine[0].y, maxY = mine[0].y;
    for (auto const& pl : mine)
    {
        minX = std::min(minX, pl.x); maxX = std::max(maxX, pl.x);
        minY = std::min(minY, pl.y); maxY = std::max(maxY, pl.y);
    }
    float spanX = std::max(1.0f, maxX - minX);
    float spanY = std::max(1.0f, maxY - minY);

    attron(COLOR_PAIR(COLOR_MAP_PLAYER));
    for (auto const& pl : mine)
    {
        // WoW's +X is north and +Y is west, so map X->row and Y->column, with
        // Y inverted to put west on the left as a player expects.
        int row = top + 1 + static_cast<int>((1.0f - (pl.x - minX) / spanX) * (height - 1));
        int col = 1     + static_cast<int>((1.0f - (pl.y - minY) / spanY) * (width  - 1));

        row = std::max(top + 1, std::min(row, bottom - 1));
        col = std::max(1,       std::min(col, COLS - 2));

        chtype existing = mvinch(row, col) & A_CHARTEXT;
        // Overlapping players escalate . -> o -> O so density is visible.
        char glyph = '.';
        if (existing == '.') glyph = 'o';
        else if (existing == 'o' || existing == 'O') glyph = 'O';
        mvaddch(row, col, glyph);
    }
    attroff(COLOR_PAIR(COLOR_MAP_PLAYER));
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
        { "F6",  " Kill  " },
        { "F7",  " Restrt" },
        { "F8",  " Map   " },
        { "F10", " Quit  " },
    };

    int x = 0;
    for (auto const& fk : keys)
    {
        int keyLen   = static_cast<int>(std::strlen(fk.key));
        int labelLen = static_cast<int>(std::strlen(fk.label));
        if (x + keyLen + labelLen >= COLS) break;

        attron(A_REVERSE);
        mvprintw(LINES - 1, x, "%s", fk.key);
        attroff(A_REVERSE);
        x += keyLen;

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
        case KEY_DOWN:
        {
            auto nodes = SortedNodes();
            auto cur   = SelectedNodeId();
            if (!cur) break;
            for (std::size_t i = 0; i < nodes.size(); ++i)
            {
                if (nodes[i].nodeId != *cur) continue;
                if (key == KEY_UP && i > 0)
                    _selectedNodeId = nodes[i - 1].nodeId;
                else if (key == KEY_DOWN && i + 1 < nodes.size())
                    _selectedNodeId = nodes[i + 1].nodeId;
                break;
            }
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
        case KEY_F(6):
            KillSelected();
            break;
        case KEY_F(7):
            RestartSelected();
            break;
        case KEY_F(8):
            _showMap = !_showMap;
            _dirty.store(true);
            break;
        default:
            break;
    }
}

void ClusterUI::KillSelected()
{
    auto nodeId = SelectedNodeId();
    if (!nodeId)
        return;
    if (!Confirm("SIGKILL worldserver on node " + std::to_string(*nodeId) + " (no save)?"))
        return;
    _monitor->SendKillNode(*nodeId);
}

void ClusterUI::RestartSelected()
{
    auto nodeId = SelectedNodeId();
    if (!nodeId)
        return;
    if (!Confirm("Restart worldserver on node " + std::to_string(*nodeId) + "?"))
        return;
    _monitor->SendRestartNode(*nodeId);
}

void ClusterUI::StartSelected()
{
    auto nodeId = SelectedNodeId();
    if (!nodeId)
        return;
    _monitor->SendStartNode(*nodeId);
}

void ClusterUI::StopSelected()
{
    auto nodeId = SelectedNodeId();
    if (!nodeId)
        return;
    if (!Confirm("Stop worldserver on node " + std::to_string(*nodeId) + " (SIGTERM, players saved)?"))
        return;
    _monitor->SendStopNode(*nodeId);
}

void ClusterUI::OpenDeployWizard()
{
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
    cfg.natsUrl         = _natsUrl;
    cfg.authKey         = sConfigMgr->GetOption<std::string>("ClusterMgr.AuthKey", "");

    DeployWizard wizard(std::move(cfg));
    std::string errMsg;
    wizard.Run(errMsg);

    halfdelay(5);
    curs_set(0);
    _dirty = true;
}

void ClusterUI::Refresh()
{
    _dirty = true;
}

// ── Latency ping thread ───────────────────────────────────────────────────────

void ClusterUI::PingLoop()
{
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
            if (!_pingRunning) break;
            if (!n.address.empty())
                PingNode(n.nodeId, n.address);
        }

        for (int i = 0; i < 20 && _pingRunning; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

void ClusterUI::PingNode(uint8 nodeId, std::string const& addr)
{
    std::vector<std::string> args = { "-c", "1", "-W", "1", addr };
    try
    {
        bp::ipstream out;
        bp::child proc("ping", bp::args(args), bp::std_out > out, bp::std_err > bp::null);

        int32 latMs = -1;
        std::string line;
        while (std::getline(out, line))
        {
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
    if (h > 0) return std::to_string(h) + "h" + std::to_string(m) + "m";
    if (m > 0) return std::to_string(m) + "m" + std::to_string(s) + "s";
    return std::to_string(s) + "s";
}

std::string ClusterUI::FormatLatency(int32 ms) const
{
    if (ms < 0) return "  ?";
    return std::to_string(ms) + "ms";
}

std::string ClusterUI::FormatBandwidth(uint32 txBps, uint32 rxBps) const
{
    auto fmtBps = [](uint32 bps) -> std::string
    {
        if (bps >= 1048576) return std::to_string(bps / 1048576) + "M";
        if (bps >= 1024)    return std::to_string(bps / 1024) + "K";
        return std::to_string(bps) + "B";
    };
    if (txBps == 0 && rxBps == 0) return "-";
    return fmtBps(txBps) + "u " + fmtBps(rxBps) + "d";
}

std::string ClusterUI::FormatMemory(uint32 mb) const
{
    if (mb == 0) return "-";
    if (mb >= 1024)
        return std::to_string(mb / 1024) + "." + std::to_string((mb % 1024) * 10 / 1024) + "G";
    return std::to_string(mb) + "M";
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
        case 2:  return COLOR_STARTING;
        case 3:  return COLOR_RUNNING;
        case 4:  return COLOR_STARTING;
        case 5:  return COLOR_CRASHED;
        default: return COLOR_STOPPED;
    }
}
