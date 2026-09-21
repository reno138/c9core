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
#include <memory>
#include <mutex>
#include <ncurses.h>
#include <stdexcept>
#include <thread>

// ── Shell helpers ─────────────────────────────────────────────────────────────

/// Single-quote @p s for a POSIX shell: every ' becomes '\''.
static std::string ShellQuote(std::string const& s)
{
    std::string out = "'";
    for (char c : s)
    {
        if (c == '\'') out += "'\\''";
        else           out += c;
    }
    out += "'";
    return out;
}

/// Escape @p s for use as a literal in a sed BRE pattern (delimiter is '|').
static std::string SedPatternEscape(std::string const& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '\\' || c == '|' || c == '.' || c == '*' || c == '[' || c == ']' || c == '^' || c == '$')
            out += '\\';
        out += c;
    }
    return out;
}

/// Escape @p s for use in a sed replacement (delimiter is '|').
static std::string SedReplacementEscape(std::string const& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '\\' || c == '|' || c == '&' || c == '\n')
            out += '\\';
        out += c;
    }
    return out;
}


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

// ── Process helpers ───────────────────────────────────────────────────────────

bool DeployWizard::RunLogged(std::string const& program, std::vector<std::string> args)
{
    try
    {
        bp::ipstream out;
        std::unique_ptr<bp::child> proc;
        if (!_cfg.sshPassword.empty())
        {
            // sshpass -e reads the password from $SSHPASS. `-p` put it on the
            // command line, visible in /proc/<pid>/cmdline to every local user
            // for the life of each transfer.
            bp::environment env = boost::this_process::environment();
            env["SSHPASS"] = _cfg.sshPassword;
            std::vector<std::string> full = { "-e", program };
            full.insert(full.end(), args.begin(), args.end());
            proc = std::make_unique<bp::child>(bp::search_path("sshpass"), bp::args(full), bp::env = env,
                                               bp::std_out > out, bp::std_err > out, bp::std_in < bp::null);
        }
        else
        {
            proc = std::make_unique<bp::child>(bp::search_path(program), bp::args(args),
                                               bp::std_out > out, bp::std_err > out, bp::std_in < bp::null);
        }

        std::string line;
        while (std::getline(out, line))
            AppendLog("    " + line);

        proc->wait();
        return proc->exit_code() == 0;
    }
    catch (std::exception const& e)
    {
        AppendLog(std::string("    ERROR: ") + e.what());
        return false;
    }
}

bool DeployWizard::PatchRemoteConf(std::string const& remoteFile, std::string const& key,
                                   std::string const& value)
{
    // Match an uncommented `Key = anything` line. '|' is the delimiter so a
    // nats:// URL in the replacement does not terminate the expression.
    std::string const expr = "s|^\\(" + SedPatternEscape(key) + "\\)[[:space:]]*=.*|\\1 = "
                             + SedReplacementEscape(value) + "|";
    // grep first so a missing key is a hard failure rather than a silent no-op.
    std::string const cmd = "grep -q " + ShellQuote("^" + SedPatternEscape(key) + "[[:space:]]*=") + " "
                            + ShellQuote(remoteFile) + " && sed -i " + ShellQuote(expr) + " " + ShellQuote(remoteFile);
    bool ok = SshCommand(cmd);
    if (!ok)
        AppendLog("    ERROR: could not patch " + key + " in " + remoteFile);
    return ok;
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

        auto fail = [&](std::string const& why)
        {
            _deployErrMsg = why;
            log("");
            log("=== DEPLOYMENT FAILED: " + why + " ===");
            _deployOk = false; _deployDone = true;
        };

        std::string authMethod = _cfg.sshPassword.empty() ? "key" : "password";
        log("=== C9Core Node Deployment ===");
        log("  Remote:  " + _cfg.sshUser + "@" + _cfg.sshHost +
            ":" + std::to_string(_cfg.sshPort) + "  (auth: " + authMethod + ")");
        log("  RemPath: " + _cfg.remotePath);
        log("  NodeType:" + _cfg.nodeType + "  NodeID:" + std::to_string(_cfg.nodeId));
        if (!_cfg.natsUrl.empty())
            log("  NATS:    " + _cfg.natsUrl);
        log("  AuthKey: " + std::string(_cfg.authKey.empty() ? "(not set — remote confs left as shipped)" : "(set)"));
        log("");

        std::string const rp = _cfg.remotePath;
        std::string const wc = rp + "/worldserver.conf";
        std::string const nc = rp + "/nodemgr.conf";

        // Step 0: the remote directory must exist; scp does not create it.
        if (!SshCommand("mkdir -p " + ShellQuote(rp)))
            return fail("Could not create " + rp + " on remote host");

        // Step 1: worldserver binary
        log("[1/6] Copying worldserver binary...");
        if (!ScpFile(_cfg.worldserverBin, rp, "worldserver"))
            return fail("Failed to copy worldserver binary");
        log("      OK");

        // Step 2: nodemgr binary
        log("[2/6] Copying nodemgr binary...");
        if (!ScpFile(_cfg.nodemgrBin, rp, "nodemgr"))
            return fail("Failed to copy nodemgr binary");
        log("      OK");

        // Step 3: worldserver config
        log("[3/6] Copying worldserver.conf...");
        if (!ScpFile(_cfg.worldserverConf, rp, "worldserver.conf"))
            return fail("Failed to copy worldserver.conf");
        log("      OK");

        // Step 4: nodemgr config
        log("[4/6] Copying nodemgr.conf...");
        if (!ScpFile(_cfg.nodemgrConf, rp, "nodemgr.conf"))
            return fail("Failed to copy nodemgr.conf");
        log("      OK");

        // Step 5: Set permissions
        log("[5/6] Setting permissions...");
        if (!SshCommand("chmod +x " + ShellQuote(rp + "/worldserver") + " " + ShellQuote(rp + "/nodemgr")))
            return fail("Failed to chmod binaries");
        log("      OK");

        // Step 6: Patch configuration. These are the keys a node actually
        // needs in the proxy-less architecture; the old ProxyServer.* /
        // InstanceServer.Enable patches targeted options that no longer exist,
        // and the '/' in a nats:// URL broke the sed expression every time —
        // silently, because failures were logged as warnings and the deploy
        // still reported SUCCESS.
        log("[6/6] Patching configuration...");
        std::string const nodeIdStr = std::to_string(_cfg.nodeId);
        bool const isInstance = (_cfg.nodeType == "instance");

        if (!PatchRemoteConf(wc, "ClusterServer.NodeId", nodeIdStr))
            return fail("Could not set ClusterServer.NodeId in worldserver.conf");
        if (!PatchRemoteConf(nc, "NodeMgr.NodeId", nodeIdStr))
            return fail("Could not set NodeMgr.NodeId in nodemgr.conf");

        if (!_cfg.natsUrl.empty())
        {
            if (!PatchRemoteConf(wc, "ClusterServer.NatsURL", "\"" + _cfg.natsUrl + "\""))
                return fail("Could not set ClusterServer.NatsURL in worldserver.conf");
            if (!PatchRemoteConf(nc, "NodeMgr.NatsUrl", "\"" + _cfg.natsUrl + "\""))
                return fail("Could not set NodeMgr.NatsUrl in nodemgr.conf");
        }

        if (!_cfg.authKey.empty())
        {
            if (!PatchRemoteConf(wc, "ClusterServer.AuthKey", "\"" + _cfg.authKey + "\""))
                return fail("Could not set ClusterServer.AuthKey in worldserver.conf");
            if (!PatchRemoteConf(nc, "NodeMgr.AuthKey", "\"" + _cfg.authKey + "\""))
                return fail("Could not set NodeMgr.AuthKey in nodemgr.conf");
        }

        if (!PatchRemoteConf(wc, "ClusterServer.InstanceServer", isInstance ? "1" : "0"))
            return fail("Could not set ClusterServer.InstanceServer in worldserver.conf");

        // nodemgr must be able to find what it launches.
        if (!PatchRemoteConf(nc, "WorldserverBin",    "\"" + rp + "/worldserver\"") ||
            !PatchRemoteConf(nc, "WorldserverConfig", "\"" + rp + "/worldserver.conf\"") ||
            !PatchRemoteConf(nc, "WorldserverLog",    "\"" + rp + "/worldserver-node" + nodeIdStr + ".log\""))
            return fail("Could not set worldserver paths in nodemgr.conf");
        log("      OK");

        // Optional: Start nodemgr after deploy
        if (_cfg.startAfterDeploy)
        {
            log("");
            log("[+] Starting nodemgr on remote host...");
            std::string startCmd = "cd " + ShellQuote(rp) + " && nohup ./nodemgr -c ./nodemgr.conf"
                                   " </dev/null >/dev/null 2>&1 &";
            if (!SshCommand(startCmd))
                return fail("Failed to start nodemgr");
            log("    nodemgr started.");
        }

        log("");
        log("=== DEPLOYMENT SUCCESSFUL ===");
        _deployOk = true; _deployDone = true;
    });

    // ── UI loop while deploying ───────────────────────────────────────────────
    // getch() must not block here or the log never streams and completion is
    // only noticed on a keypress. OpenDeployWizard() switched to plain cbreak
    // (blocking), so set an explicit read timeout for the duration.
    timeout(500);
    while (!_deployDone)
    {
        if (_hasNewLines.exchange(false))
            DrawDeployLog();
        getch();  // ERR after 500 ms, or a key press
    }
    timeout(-1);
    worker.join();

    DrawDeployLog();

    // Wait for keypress to dismiss
    mvprintw(_winY + _winH - 2, _winX + 2, "Press any key to return...");
    refresh();
    getch();

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
    // The remote path is interpreted by the remote shell (scp semantics), so
    // it is shell-quoted; the local path is passed as one argv entry.
    std::string target = _cfg.sshUser + "@" + _cfg.sshHost + ":" + ShellQuote(remoteDir + "/" + remoteName);

    // accept-new: pin the host key on first contact and refuse a changed one
    // afterwards. `=no` accepted any key presented, on the connection that
    // carries the binaries, the confs and the cluster auth key.
    std::vector<std::string> args = {
        "-P", std::to_string(_cfg.sshPort),
        "-o", "StrictHostKeyChecking=accept-new",
    };
    if (_cfg.sshPassword.empty())
        args.insert(args.end(), { "-i", _cfg.sshKey, "-o", "BatchMode=yes" });
    // (BatchMode conflicts with sshpass; not added for password auth)
    args.push_back(localPath);
    args.push_back(target);
    return RunLogged("scp", std::move(args));
}

bool DeployWizard::SshCommand(std::string const& cmd)
{
    std::vector<std::string> args = {
        "-p", std::to_string(_cfg.sshPort),
        "-o", "StrictHostKeyChecking=accept-new",
    };
    if (_cfg.sshPassword.empty())
        args.insert(args.end(), { "-i", _cfg.sshKey, "-o", "BatchMode=yes" });
    args.push_back(_cfg.sshUser + "@" + _cfg.sshHost);
    args.push_back(cmd);
    return RunLogged("ssh", std::move(args));
}

void DeployWizard::AppendLog(std::string const& line)
{
    std::lock_guard<std::mutex> lock(_logMutex);
    _logLines.push_back(line);
}
