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
#include "BoostProcess.h"
#include <mutex>
#include <ncurses.h>
#include <stdexcept>
#include <thread>


// ── Constructor ───────────────────────────────────────────────────────────────

DeployWizard::DeployWizard(Config defaults)
    : _cfg(std::move(defaults))
{
    // Fields 0–10: Remote Host, SSH User, SSH Password, Node Type, Node ID,
    //              SSH Port, SSH Key, Remote Path, Worldserver, Nodemgr, Start After
    _fields[0]  = { "Remote Host  ", _cfg.sshHost,                                    false };
    _fields[1]  = { "SSH User     ", _cfg.sshUser,                                    false };
    _fields[2]  = { "SSH Password ", _cfg.sshPassword,                                true  };
    _fields[3]  = { "Node Type    ", _cfg.nodeType,                                   false };
    _fields[4]  = { "Node ID      ", std::to_string(_cfg.nodeId),                     false };
    _fields[5]  = { "SSH Port     ", std::to_string(_cfg.sshPort),                    false };
    _fields[6]  = { "SSH Key      ", _cfg.sshKey,                                     false };
    _fields[7]  = { "Remote Path  ", _cfg.remotePath,                                 false };
    _fields[8]  = { "Worldserver  ", _cfg.worldserverBin,                             false };
    _fields[9]  = { "Nodemgr      ", _cfg.nodemgrBin,                                 false };
    _fields[10] = { "Start After  ", _cfg.startAfterDeploy ? "yes" : "no",            false };
}

// ── Public interface ─────────────────────────────────────────────────────────

bool DeployWizard::Run(std::string& errMsg)
{
    // Compute window geometry
    _winH = std::min(LINES - 2, 26);
    _winW = std::min(COLS  - 4, 86);
    _winY = (LINES - _winH) / 2;
    _winX = (COLS  - _winW) / 2;

    if (!RunForm())
        return false;   // user cancelled

    // Parse fields back into Config
    _cfg.sshHost          = _fields[0].value;
    _cfg.sshUser          = _fields[1].value;
    _cfg.sshPassword      = _fields[2].value;
    _cfg.nodeType         = _fields[3].value;
    _cfg.remotePath       = _fields[7].value;
    _cfg.worldserverBin   = _fields[8].value;
    _cfg.nodemgrBin       = _fields[9].value;
    _cfg.startAfterDeploy = (_fields[10].value == "yes" || _fields[10].value == "y" ||
                             _fields[10].value == "1"   || _fields[10].value == "true");

    try { _cfg.nodeId  = std::stoi(_fields[4].value); } catch (...) { _cfg.nodeId  = 1; }
    try { _cfg.sshPort = std::stoi(_fields[5].value); } catch (...) { _cfg.sshPort = 22; }
    _cfg.sshKey = _fields[6].value;

    // Normalise nodeType
    if (_cfg.nodeType != "instance")
        _cfg.nodeType = "worldserver";

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

        // Value (mask password fields)
        std::string displayVal = _fields[i].isPassword
            ? std::string(_fields[i].value.size(), '*')
            : _fields[i].value;

        if (static_cast<int>(displayVal.size()) > fieldW)
            displayVal = displayVal.substr(displayVal.size() - fieldW);

        if (active)
        {
            wattron(win, A_REVERSE | A_BOLD);
            mvwprintw(win, row, 17, "%-*s", fieldW, displayVal.c_str());
            int cursorX = 17 + static_cast<int>(displayVal.size());
            if (cursorX < _winW - 2)
                mvwaddch(win, row, cursorX, '_');
            wattroff(win, A_REVERSE | A_BOLD);
        }
        else
        {
            mvwprintw(win, row, 17, "%-*s", fieldW, displayVal.c_str());
        }
    }

    // Bottom separator
    int sepRow = _winH - 3;
    if (sepRow > 2)
    {
        mvwhline(win, sepRow, 1, ACS_HLINE, _winW - 2);
        mvwaddch(win, sepRow, 0, ACS_LTEE);
        mvwaddch(win, sepRow, _winW - 1, ACS_RTEE);
        mvwprintw(win, _winH - 2, 2,
                  "Tab/Arrow: Navigate    F10: Deploy    Esc: Cancel");
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

        DrawForm();
    }
}

// ── sshpass helper ────────────────────────────────────────────────────────────

std::vector<std::string> DeployWizard::SshpassPrefix() const
{
    if (_cfg.sshPassword.empty())
        return {};
    return { "sshpass", "-p", _cfg.sshPassword };
}

// ── Deploy execution ──────────────────────────────────────────────────────────

bool DeployWizard::RunDeploy()
{
    {
        std::lock_guard<std::mutex> lock(_logMutex);
        _logLines.clear();
    }
    _deployDone   = false;
    _deployOk     = false;
    _hasNewLines  = false;

    // Run deployment in a background thread
    std::thread worker([this]()
    {
        auto log = [&](std::string const& line)
        {
            AppendLog(line);
            _hasNewLines = true;
        };

        std::string authMethod = _cfg.sshPassword.empty() ? "key" : "password";
        log("=== C9Core Node Deployment ===");
        log("  Remote:  " + _cfg.sshUser + "@" + _cfg.sshHost +
            ":" + std::to_string(_cfg.sshPort) + "  (auth: " + authMethod + ")");
        log("  RemPath: " + _cfg.remotePath);
        log("  NodeType:" + _cfg.nodeType + "  NodeID:" + std::to_string(_cfg.nodeId));
        if (!_cfg.proxyAddress.empty())
            log("  Proxy:   " + _cfg.proxyAddress);
        log("");

        // Step 1: worldserver binary
        log("[1/6] Copying worldserver binary...");
        if (!ScpFile(_cfg.worldserverBin, _cfg.remotePath, "worldserver"))
        {
            _deployErrMsg = "Failed to copy worldserver binary";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 2: nodemgr binary
        log("[2/6] Copying nodemgr binary...");
        if (!ScpFile(_cfg.nodemgrBin, _cfg.remotePath, "nodemgr"))
        {
            _deployErrMsg = "Failed to copy nodemgr binary";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 3: worldserver config
        log("[3/6] Copying worldserver.conf...");
        if (!ScpFile(_cfg.worldserverConf, _cfg.remotePath, "worldserver.conf"))
        {
            _deployErrMsg = "Failed to copy worldserver.conf";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 4: nodemgr config
        log("[4/6] Copying nodemgr.conf...");
        if (!ScpFile(_cfg.nodemgrConf, _cfg.remotePath, "nodemgr.conf"))
        {
            _deployErrMsg = "Failed to copy nodemgr.conf";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 5: Set permissions
        log("[5/6] Setting permissions...");
        std::string chmodCmd = "chmod +x " + _cfg.remotePath + "/worldserver "
                               + _cfg.remotePath + "/nodemgr";
        if (!SshCommand(chmodCmd))
        {
            _deployErrMsg = "Failed to chmod binaries";
            _deployOk = false; _deployDone = true; return;
        }
        log("      OK");

        // Step 6: Patch configuration via sed
        log("[6/6] Patching configuration...");

        // Always patch nodemgr.conf: NodeId and ProxyServer.Address
        std::string rp = _cfg.remotePath;
        std::string nc = rp + "/nodemgr.conf";

        if (!SshCommand("sed -i 's/NodeId = [0-9]*/NodeId = " +
                        std::to_string(_cfg.nodeId) + "/' " + nc))
            log("    WARNING: Could not patch NodeId in nodemgr.conf");

        if (!_cfg.proxyAddress.empty())
        {
            // Patch proxy address in nodemgr.conf
            if (!SshCommand("sed -i 's/ProxyServer\\.Address = \"[^\"]*\"/ProxyServer.Address = \"" +
                            _cfg.proxyAddress + "\"/' " + nc))
                log("    WARNING: Could not patch ProxyServer.Address in nodemgr.conf");

            // Patch worldserver.conf
            std::string wc = rp + "/worldserver.conf";
            if (!SshCommand("sed -i 's/ProxyServer\\.Enable = 0/ProxyServer.Enable = 1/' " + wc))
                log("    WARNING: Could not patch ProxyServer.Enable in worldserver.conf");

            if (!SshCommand("sed -i 's/ProxyServer\\.Address = \"[^\"]*\"/ProxyServer.Address = \"" +
                            _cfg.proxyAddress + "\"/' " + wc))
                log("    WARNING: Could not patch ProxyServer.Address in worldserver.conf");

            // If instance node, enable InstanceServer
            if (_cfg.nodeType == "instance")
            {
                if (!SshCommand("sed -i 's/InstanceServer\\.Enable = 0/InstanceServer.Enable = 1/' " + wc))
                    log("    WARNING: Could not patch InstanceServer.Enable in worldserver.conf");
            }
        }
        log("      OK");

        // Optional: Start nodemgr after deploy
        if (_cfg.startAfterDeploy)
        {
            log("");
            log("[+] Starting nodemgr on remote host...");
            std::string startCmd = "nohup " + rp + "/nodemgr"
                                   + " -c " + rp + "/nodemgr.conf"
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
        if (_hasNewLines.exchange(false))
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

    int logAreaH = _winH - 3;
    int logAreaY = 1;
    int logAreaX = 2;
    int logAreaW = _winW - 4;

    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(_logMutex);
        lines = _logLines;
    }

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

    // Base scp arguments
    std::vector<std::string> scpArgs = {
        "-P", std::to_string(_cfg.sshPort),
        "-o", "StrictHostKeyChecking=no",
    };

    std::string program;
    std::vector<std::string> args;

    if (!_cfg.sshPassword.empty())
    {
        // sshpass -p <password> scp [opts] src dst
        program = "sshpass";
        args = { "-p", _cfg.sshPassword, "scp" };
        args.insert(args.end(), scpArgs.begin(), scpArgs.end());
        // BatchMode conflicts with sshpass; don't add it
    }
    else
    {
        // scp -i key -o BatchMode=yes [opts] src dst
        program = "scp";
        args = scpArgs;
        args.insert(args.end(), { "-i", _cfg.sshKey, "-o", "BatchMode=yes" });
    }
    args.push_back(localPath);
    args.push_back(target);

    try
    {
        bp::ipstream out;
        bp::child proc(program, bp::args(args), bp::std_out > out, bp::std_err > out);

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
    std::vector<std::string> sshArgs = {
        "-p", std::to_string(_cfg.sshPort),
        "-o", "StrictHostKeyChecking=no",
    };

    std::string program;
    std::vector<std::string> args;

    if (!_cfg.sshPassword.empty())
    {
        program = "sshpass";
        args = { "-p", _cfg.sshPassword, "ssh" };
        args.insert(args.end(), sshArgs.begin(), sshArgs.end());
    }
    else
    {
        program = "ssh";
        args = sshArgs;
        args.insert(args.end(), { "-i", _cfg.sshKey, "-o", "BatchMode=yes" });
    }
    args.push_back(_cfg.sshUser + "@" + _cfg.sshHost);
    args.push_back(cmd);

    try
    {
        bp::ipstream out;
        bp::child proc(program, bp::args(args), bp::std_out > out, bp::std_err > out);

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
