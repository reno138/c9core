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

#ifndef DeployWizard_h__
#define DeployWizard_h__

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

/**
 * @brief ncurses popup wizard for deploying a new cluster node via SCP/SSH.
 *
 * Presents a form with deployment parameters, then runs the following sequence:
 *   1. scp worldserver        → remote host
 *   2. scp nodemgr            → remote host
 *   3. scp worldserver.conf   → remote host
 *   4. scp nodemgr.conf       → remote host
 *   5. ssh chmod +x worldserver nodemgr
 *   6. (optional) ssh nohup ./nodemgr ... &
 *
 * The wizard blocks the calling thread while the ncurses modal is visible.
 * Deployment steps run in a background thread; output is streamed to a
 * scrollable log area within the wizard window.
 */
class DeployWizard
{
public:
    struct Config
    {
        std::string worldserverBin  { "./worldserver" };
        std::string nodemgrBin      { "./nodemgr" };
        std::string worldserverConf { "./worldserver.conf" };
        std::string nodemgrConf     { "./nodemgr.conf" };
        std::string sshUser         { "wow" };
        std::string sshHost;
        int         sshPort         { 22 };
        std::string sshKey          { "~/.ssh/id_rsa" };
        std::string remotePath      { "/opt/c9core" };
        bool        startAfterDeploy{ true };
    };

    explicit DeployWizard(Config defaults);

    /// Show the form wizard and block until user completes or cancels.
    /// Returns true if deployment was attempted; sets errMsg if it failed.
    bool Run(std::string& errMsg);

private:
    // ── Rendering helpers ─────────────────────────────────────────────────────
    void DrawForm();
    void DrawDeployLog();
    bool RunForm();         ///< Returns true if user confirmed, false if cancelled
    bool RunDeploy();       ///< Returns true on success

    // ── Deploy steps ──────────────────────────────────────────────────────────
    bool ScpFile(std::string const& localPath, std::string const& remoteDir,
                 std::string const& remoteName);
    bool SshCommand(std::string const& cmd);

    void AppendLog(std::string const& line);

    // ── Field editing helpers ─────────────────────────────────────────────────
    static constexpr int FIELD_COUNT = 8;

    struct Field
    {
        std::string label;
        std::string value;
        bool        isPassword { false };
    };

    Field _fields[FIELD_COUNT];
    int   _activeField { 0 };

    Config _cfg;

    // ── Deploy log (mutex guards _logLines against worker/UI thread races) ───
    std::mutex               _logMutex;
    std::vector<std::string> _logLines;
    std::atomic<bool>        _deployDone   { false };
    std::atomic<bool>        _deployOk     { false };
    std::string              _deployErrMsg;

    // ── Window geometry (set in DrawForm) ─────────────────────────────────────
    int _winH { 0 };
    int _winW { 0 };
    int _winY { 0 };
    int _winX { 0 };
};

#endif // DeployWizard_h__
