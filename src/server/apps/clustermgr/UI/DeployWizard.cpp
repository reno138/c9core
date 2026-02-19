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

#include "DeployWizard.h"
#include "Log.h"
#include <boost/process.hpp>
#include <mutex>
#include <ncurses.h>
#include <stdexcept>
#include <thread>

namespace bp = boost::process;

// ── Constructor ───────────────────────────────────────────────────────────────

DeployWizard::DeployWizard(Config defaults)
    : _cfg(std::move(defaults))
{
    // Map Config to editable fields
    _fields[0] = { "Remote Host  ", _cfg.sshHost,         false };
    _fields[1] = { "SSH User     ", _cfg.sshUser,         false };
    _fields[2] = { "SSH Port     ", std::to_string(_cfg.sshPort), false };
    _fields[3] = { "SSH Key      ", _cfg.sshKey,          false };
    _fields[4] = { "Remote Path  ", _cfg.remotePath,      false };
    _fields[5] = { "Worldserver  ", _cfg.worldserverBin,  false };
    _fields[6] = { "Nodemgr      ", _cfg.nodemgrBin,      false };
    _fields[7] = { "Start After  ", _cfg.startAfterDeploy ? "yes" : "no", false };
}

// ── Public interface ─────────────────────────────────────────────────────────

bool DeployWizard::Run(std::string& errMsg)
{
    // Compute window geometry
    _winH = std::min(LINES - 2, 24);
    _winW = std::min(COLS  - 4, 86);
    _winY = (LINES - _winH) / 2;
    _winX = (COLS  - _winW) / 2;

    if (!RunForm())
        return false;   // user cancelled

    // Parse fields back into Config
    _cfg.sshHost           = _fields[0].value;
    _cfg.sshUser           = _fields[1].value;
    _cfg.remotePath        = _fields[4].value;
    _cfg.worldserverBin    = _fields[5].value;
    _cfg.nodemgrBin        = _fields[6].value;
    _cfg.worldserverConf   = _cfg.worldserverBin.empty() ? "./worldserver.conf" : _cfg.worldserverConf;
    _cfg.nodemgrConf       = _cfg.nodemgrBin.empty()     ? "./nodemgr.conf"     : _cfg.nodemgrConf;
    _cfg.startAfterDeploy  = (_fields[7].value == "yes" || _fields[7].value == "y" ||
                              _fields[7].value == "1"   || _fields[7].value == "true");

    try
    {
        _cfg.sshPort = std::stoi(_fields[2].value);
    }
    catch (...)
    {
        _cfg.sshPort = 22;
    }
    _cfg.sshKey = _fields[3].value;

    if (_cfg.sshHost.empty())
    {
        errMsg = "Remote host cannot be empty";
        return false;
    }

    bool ok = RunDeploy();
    if (!ok)
        errMsg = _deployErrMsg;
    return ok;
}

// ── Form rendering ────────────────────────────────────────────────────────────

void DeployWizard::DrawForm()
{
    WINDOW* win = newwin(_winH, _winW, _winY, _winX);
    if (!win) return;

    keypad(win, TRUE);
    box(win, 0, 0);

    // Title
    std::string title = " Deploy New Node ";
    int titleX = (_winW - static_cast<int>(title.size())) / 2;
    wattron(win, A_BOLD);
    mvwprintw(win, 0, titleX, "%s", title.c_str());
    wattroff(win, A_BOLD);

    // Field separator
    mvwhline(win, 2, 1, ACS_HLINE, _winW - 2);
    mvwaddch(win, 2, 0, ACS_LTEE);
    mvwaddch(win, 2, _winW - 1, ACS_RTEE);

    int fieldW = _winW - 20;  // value field width
    if (fieldW < 20) fieldW = 20;

    for (int i = 0; i < FIELD_COUNT; ++i)
    {
        int row = 3 + i;
        if (row >= _winH - 3) break;

        bool active = (i == _activeField);

        // Label
        mvwprintw(win, row, 2, "%s: ", _fields[i].label.c_str());

        // Value field
        std::string val = _fields[i].value;
        if (static_cast<int>(val.size()) > fieldW)
            val = val.substr(val.size() - fieldW);  // show tail end

        if (active)
        {
            wattron(win, A_REVERSE | A_BOLD);
            mvwprintw(win, row, 17, "%-*s", fieldW, val.c_str());
            // Draw cursor at end of value
            int cursorX = 17 + static_cast<int>(val.size());
            if (cursorX < _winW - 2)
                mvwaddch(win, row, cursorX, '_');
            wattroff(win, A_REVERSE | A_BOLD);
        }
        else
        {
            mvwprintw(win, row, 17, "%-*s", fieldW, val.c_str());
        }
    }

    // Bottom separator
    int sepRow = _winH - 3;
    if (sepRow > 2)
    {
        mvwhline(win, sepRow, 1, ACS_HLINE, _winW - 2);
        mvwaddch(win, sepRow, 0, ACS_LTEE);
        mvwaddch(win, sepRow, _winW - 1, ACS_RTEE);
        mvwprintw(win, _winH - 2, 2, "Tab/Arrow: Navigate    F10: Deploy    Esc: Cancel");
    }

    wrefresh(win);
    delwin(win);
}

// ── Form input loop ───────────────────────────────────────────────────────────

bool DeployWizard::RunForm()
{
    DrawForm();

    while (true)
    {
        int ch = getch();

        if (ch == 27)                   // ESC
            return false;
        if (ch == KEY_F(10))            // F10 = confirm
            return true;
        if (ch == '\n' || ch == '\r')   // Enter = next field / confirm on last
        {
            if (_activeField < FIELD_COUNT - 1)
                ++_activeField;
            else
                return true;
        }
        else if (ch == '\t' || ch == KEY_DOWN)
        {
            _activeField = (_activeField + 1) % FIELD_COUNT;
        }
        else if (ch == KEY_UP)
        {
            _activeField = (_activeField - 1 + FIELD_COUNT) % FIELD_COUNT;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
        {
            if (!_fields[_activeField].value.empty())
                _fields[_activeField].value.pop_back();
        }
        else if (ch >= 32 && ch < 127)
        {
            _fields[_activeField].value += static_cast<char>(ch);
        }
        // Ignore other keys

        DrawForm();
    }
}

// ── Deploy execution ──────────────────────────────────────────────────────────

bool DeployWizard::RunDeploy()
{
    {
        std::lock_guard<std::mutex> lock(_logMutex);
        _logLines.clear();
    }
    _deployDone = false;
    _deployOk   = false;
    std::atomic<bool> hasNewLines{ false };

    // Run deployment in a background thread
    std::thread worker([this, &hasNewLines]()
    {
        auto log = [&](std::string const& line)
        {
            AppendLog(line);
            hasNewLines = true;
        };

        log("=== C9Core Node Deployment ===");
        log("  Remote: " + _cfg.sshUser + "@" + _cfg.sshHost + ":" + std::to_string(_cfg.sshPort));
        log("  Remote path: " + _cfg.remotePath);
        log("");

        // Step 1: worldserver binary
        log("[1/5] Copying worldserver binary...");
        if (!ScpFile(_cfg.worldserverBin, _cfg.remotePath, "worldserver"))
        {
            _deployErrMsg = "Failed to copy worldserver binary";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 2: nodemgr binary
        log("[2/5] Copying nodemgr binary...");
        if (!ScpFile(_cfg.nodemgrBin, _cfg.remotePath, "nodemgr"))
        {
            _deployErrMsg = "Failed to copy nodemgr binary";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 3: worldserver config
        log("[3/5] Copying worldserver.conf...");
        if (!ScpFile(_cfg.worldserverConf, _cfg.remotePath, "worldserver.conf"))
        {
            _deployErrMsg = "Failed to copy worldserver.conf";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 4: nodemgr config
        log("[4/5] Copying nodemgr.conf...");
        if (!ScpFile(_cfg.nodemgrConf, _cfg.remotePath, "nodemgr.conf"))
        {
            _deployErrMsg = "Failed to copy nodemgr.conf";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 5: Set permissions
        log("[5/5] Setting permissions...");
        std::string chmodCmd = "chmod +x " + _cfg.remotePath + "/worldserver "
                               + _cfg.remotePath + "/nodemgr";
        if (!SshCommand(chmodCmd))
        {
            _deployErrMsg = "Failed to chmod binaries";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Optional: Start nodemgr after deploy
        if (_cfg.startAfterDeploy)
        {
            log("");
            log("[+] Starting nodemgr on remote host...");
            std::string startCmd = "nohup " + _cfg.remotePath + "/nodemgr"
                                   + " -c " + _cfg.remotePath + "/nodemgr.conf"
                                   + " </dev/null >/dev/null 2>&1 &";
            if (!SshCommand(startCmd))
                log("    WARNING: Failed to start nodemgr (may already be running)");
            else
                log("    nodemgr started.");
        }

        log("");
        log("=== DEPLOYMENT SUCCESSFUL ===");
        _deployOk = true; _deployDone = true;
    });

    // ── UI loop while deploying ───────────────────────────────────────────────
    while (!_deployDone)
    {
        if (hasNewLines.exchange(false))
            DrawDeployLog();
        getch();  // will return ERR (halfdelay) or a key press
    }
    worker.join();

    DrawDeployLog();

    // Wait for keypress to dismiss
    nocbreak();
    cbreak();
    mvprintw(_winY + _winH - 2, _winX + 2, "Press any key to return...");
    refresh();
    getch();
    halfdelay(5);

    // Restore main screen
    touchwin(stdscr);
    refresh();
    return _deployOk;
}

void DeployWizard::DrawDeployLog()
{
    WINDOW* win = newwin(_winH, _winW, _winY, _winX);
    if (!win) return;

    box(win, 0, 0);

    std::string title = _deployDone
        ? (_deployOk ? " Deployment Complete " : " Deployment FAILED ")
        : " Deploying... ";
    int titleX = (_winW - static_cast<int>(title.size())) / 2;
    wattron(win, A_BOLD);
    mvwprintw(win, 0, titleX, "%s", title.c_str());
    wattroff(win, A_BOLD);

    int logAreaH = _winH - 3;   // top border + title + bottom border + hint
    int logAreaY = 1;
    int logAreaX = 2;
    int logAreaW = _winW - 4;

    // Snapshot lines under lock to avoid race with worker thread
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(_logMutex);
        lines = _logLines;
    }

    // Show last logAreaH lines
    int startLine = static_cast<int>(lines.size()) - logAreaH;
    if (startLine < 0) startLine = 0;

    for (int i = 0; i < logAreaH; ++i)
    {
        int lineIdx = startLine + i;
        if (lineIdx < static_cast<int>(lines.size()))
        {
            std::string const& line = lines[lineIdx];
            std::string display = line.size() > static_cast<size_t>(logAreaW)
                ? line.substr(0, logAreaW) : line;
            mvwprintw(win, logAreaY + i, logAreaX, "%s", display.c_str());
        }
    }

    wrefresh(win);
    delwin(win);
}

// ── Deploy helpers ────────────────────────────────────────────────────────────

bool DeployWizard::ScpFile(std::string const& localPath, std::string const& remoteDir,
                           std::string const& remoteName)
{
    std::string target = _cfg.sshUser + "@" + _cfg.sshHost + ":" + remoteDir + "/" + remoteName;

    std::vector<std::string> args = {
        "-P", std::to_string(_cfg.sshPort),
        "-i", _cfg.sshKey,
        "-o", "StrictHostKeyChecking=no",
        "-o", "BatchMode=yes",
        localPath,
        target
    };

    try
    {
        bp::ipstream out;
        bp::child proc("scp", bp::args(args), bp::std_out > out, bp::std_err > out);

        std::string line;
        while (std::getline(out, line))
            AppendLog("    " + line);

        proc.wait();
        return proc.exit_code() == 0;
    }
    catch (std::exception const& e)
    {
        AppendLog(std::string("    ERROR: ") + e.what());
        return false;
    }
}

bool DeployWizard::SshCommand(std::string const& cmd)
{
    std::vector<std::string> args = {
        "-p", std::to_string(_cfg.sshPort),
        "-i", _cfg.sshKey,
        "-o", "StrictHostKeyChecking=no",
        "-o", "BatchMode=yes",
        _cfg.sshUser + "@" + _cfg.sshHost,
        cmd
    };

    try
    {
        bp::ipstream out;
        bp::child proc("ssh", bp::args(args), bp::std_out > out, bp::std_err > out);

        std::string line;
        while (std::getline(out, line))
            AppendLog("    " + line);

        proc.wait();
        return proc.exit_code() == 0;
    }
    catch (std::exception const& e)
    {
        AppendLog(std::string("    ERROR: ") + e.what());
        return false;
    }
}

void DeployWizard::AppendLog(std::string const& line)
{
    std::lock_guard<std::mutex> lock(_logMutex);
    _logLines.push_back(line);
}
