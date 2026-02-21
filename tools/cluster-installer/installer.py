#!/usr/bin/env python3
"""
C9Core Cluster Installer
========================
Curses-based interactive wizard for deploying a multi-node WoW 3.3.5a cluster.

Covers:
  - Distro detection and dependency installation (apt / pacman / dnf)
  - MariaDB setup (local install if not present)
  - Multi-node cluster configuration with SSH deployment
  - CMake build + install
  - Database creation and SQL population
  - WoW client map / vmap / mmap extraction
  - Per-node continent map assignment (with checkboxes)
  - Config generation for proxy, auth, worldserver, nodemgr on every node

Usage:
  python3 installer.py

Requirements: Python 3.8+, standard library only (no pip packages).
External tools used: cmake, make, mysql, ssh, scp, sshpass (optional).
"""

import curses
import os
import re
import json
import sys
import time
import socket
import shutil
import secrets
import subprocess
import threading
from pathlib import Path

# ─────────────────────────────────────────────────────────────────────────────
# Constants
# ─────────────────────────────────────────────────────────────────────────────

VERSION     = "1.0.0"
TITLE       = f"C9Core Cluster Installer  v{VERSION}"
SCRIPT_DIR  = Path(__file__).parent.resolve()
DEPS_FILE   = SCRIPT_DIR / "deps.json"

CONTINENTS = [
    ("Eastern Kingdoms", 0),
    ("Kalimdor",        1),
    ("Outland",       530),
    ("Northrend",     571),
]

# Default game port base; node N gets BASE + N - 1
GAME_PORT_BASE   = 8086
CLIENT_PORT      = 8085   # clients always connect here (proxy)
CONTROL_PORT     = 8090
NODEMGR_PORT     = 8091
MGMT_PORT        = 9090
AUTH_PORT        = 3724

# ─────────────────────────────────────────────────────────────────────────────
# Colour palette (initialised in UI.__init__)
# ─────────────────────────────────────────────────────────────────────────────
C_NORMAL = C_TITLE = C_HIGHLIGHT = C_ERROR = C_OK = C_DIM = C_INPUT = 0

# ─────────────────────────────────────────────────────────────────────────────
# Section 1: Low-level curses helpers
# ─────────────────────────────────────────────────────────────────────────────

class UI:
    """Thin wrapper around curses providing widgets used by the installer."""

    def __init__(self, stdscr):
        self.scr = stdscr
        curses.curs_set(0)
        curses.start_color()
        curses.use_default_colors()
        # Colour pairs
        curses.init_pair(1, curses.COLOR_WHITE,  curses.COLOR_BLUE)    # title bar
        curses.init_pair(2, curses.COLOR_BLACK,  curses.COLOR_CYAN)    # highlight
        curses.init_pair(3, curses.COLOR_RED,    -1)                   # error
        curses.init_pair(4, curses.COLOR_GREEN,  -1)                   # ok
        curses.init_pair(5, curses.COLOR_WHITE,  -1)                   # normal
        curses.init_pair(6, curses.COLOR_BLACK,  curses.COLOR_WHITE)   # input
        curses.init_pair(7, curses.COLOR_YELLOW, -1)                   # dim/info
        global C_TITLE, C_HIGHLIGHT, C_ERROR, C_OK, C_NORMAL, C_INPUT, C_DIM
        C_TITLE     = curses.color_pair(1) | curses.A_BOLD
        C_HIGHLIGHT = curses.color_pair(2) | curses.A_BOLD
        C_ERROR     = curses.color_pair(3) | curses.A_BOLD
        C_OK        = curses.color_pair(4) | curses.A_BOLD
        C_NORMAL    = curses.color_pair(5)
        C_INPUT     = curses.color_pair(6)
        C_DIM       = curses.color_pair(7)
        self.scr.keypad(True)

    # ── Layout helpers ────────────────────────────────────────────────────────

    def rows(self): return self.scr.getmaxyx()[0]
    def cols(self): return self.scr.getmaxyx()[1]

    def title_bar(self, text=TITLE):
        try:
            self.scr.attron(C_TITLE)
            self.scr.addstr(0, 0, text.center(self.cols())[:self.cols()])
            self.scr.attroff(C_TITLE)
        except curses.error:
            pass

    def status_bar(self, text):
        r = self.rows() - 1
        try:
            self.scr.attron(C_TITLE)
            self.scr.addstr(r, 0, text[:self.cols()].ljust(self.cols())[:self.cols()])
            self.scr.attroff(C_TITLE)
        except curses.error:
            pass

    def clear_body(self):
        for r in range(1, self.rows() - 1):
            try:
                self.scr.move(r, 0)
                self.scr.clrtoeol()
            except curses.error:
                pass

    def draw_box(self, y, x, h, w, title=""):
        """Draw an ASCII box; returns inner (y, x, h, w)."""
        try:
            win = self.scr.subwin(h, w, y, x)
            win.box()
            if title:
                win.addstr(0, 2, f" {title} ", C_HIGHLIGHT)
        except curses.error:
            pass
        return y + 1, x + 1, h - 2, w - 2

    def put(self, r, c, text, attr=0):
        try:
            self.scr.addstr(r, c, str(text)[:self.cols() - c], attr or C_NORMAL)
        except curses.error:
            pass

    def progress_bar(self, r, c, w, done, total, label=""):
        filled = int(w * done / max(total, 1))
        bar = ("█" * filled).ljust(w)
        pct = f" {100*done//max(total,1):3d}%"
        self.put(r, c, f"{label[:20]:<20} [{bar}]{pct}", C_NORMAL)

    # ── Input widgets ─────────────────────────────────────────────────────────

    def prompt_text(self, r, c, label, default="", password=False, width=40):
        """Single-line text input; returns entered string."""
        curses.curs_set(1)
        prompt = f"{label}: "
        self.put(r, c, prompt, C_NORMAL)
        cx = c + len(prompt)
        buf = list(default)
        pos = len(buf)

        while True:
            display = ("*" * len(buf)) if password else "".join(buf)
            field = display[max(0, pos - width + 1):][:width]
            self.put(r, cx, field.ljust(width), C_INPUT)
            self.scr.move(r, cx + min(pos, width - 1))
            self.scr.refresh()
            ch = self.scr.getch()
            if ch in (curses.KEY_ENTER, 10, 13):
                break
            elif ch in (curses.KEY_BACKSPACE, 127, 8):
                if pos > 0:
                    buf.pop(pos - 1)
                    pos -= 1
            elif ch == curses.KEY_LEFT and pos > 0:
                pos -= 1
            elif ch == curses.KEY_RIGHT and pos < len(buf):
                pos += 1
            elif ch == curses.KEY_HOME:
                pos = 0
            elif ch == curses.KEY_END:
                pos = len(buf)
            elif 32 <= ch < 127:
                buf.insert(pos, chr(ch))
                pos += 1

        curses.curs_set(0)
        return "".join(buf)

    def prompt_select(self, title, options, status_hint="↑↓ Navigate  ENTER Select"):
        """Full-screen single-select menu; returns index."""
        idx = 0
        while True:
            self.scr.erase()
            self.title_bar()
            self.status_bar(status_hint)
            self.put(2, 2, title, C_HIGHLIGHT)
            for i, opt in enumerate(options):
                attr = C_HIGHLIGHT if i == idx else C_NORMAL
                marker = "▶" if i == idx else " "
                self.put(4 + i, 4, f"{marker} {opt}", attr)
            self.scr.refresh()
            ch = self.scr.getch()
            if ch == curses.KEY_UP and idx > 0:
                idx -= 1
            elif ch == curses.KEY_DOWN and idx < len(options) - 1:
                idx += 1
            elif ch in (curses.KEY_ENTER, 10, 13):
                return idx

    def prompt_checkboxes(self, title, items, hint="↑↓ Navigate  SPACE Toggle  ENTER Confirm"):
        """
        Checkbox list.  items = list of (label, bool_initial).
        Returns list of bools (same order).
        If first item is toggled, others are disabled (instance-only mode).
        """
        selected = [v for _, v in items]
        idx = 0
        while True:
            self.scr.erase()
            self.title_bar()
            self.status_bar(hint)
            self.put(2, 2, title, C_HIGHLIGHT)
            instance_only = selected[0]  # first item = "Instance-only"
            for i, (label, _) in enumerate(items):
                disabled = (instance_only and i > 0)
                check = "[X]" if selected[i] else "[ ]"
                attr = C_HIGHLIGHT if i == idx else (C_DIM if disabled else C_NORMAL)
                self.put(4 + i, 4, f"{check}  {label}", attr)
            if instance_only:
                self.put(5 + len(items), 4,
                         "NOTE: Instance-only overrides continent selection.", C_DIM)
            self.scr.refresh()
            ch = self.scr.getch()
            if ch == curses.KEY_UP and idx > 0:
                idx -= 1
            elif ch == curses.KEY_DOWN and idx < len(items) - 1:
                idx += 1
            elif ch == ord(" "):
                if idx == 0:
                    selected[0] = not selected[0]
                elif not instance_only:
                    selected[idx] = not selected[idx]
            elif ch in (curses.KEY_ENTER, 10, 13):
                return selected

    def confirm(self, message, default=True):
        """Yes/No confirmation; returns bool."""
        yn = "Y/n" if default else "y/N"
        self.scr.erase()
        self.title_bar()
        self.status_bar("ENTER=Accept  n=No")
        self.put(self.rows()//2, 2, f"{message}  [{yn}]: ", C_HIGHLIGHT)
        self.scr.refresh()
        ch = self.scr.getch()
        if ch in (curses.KEY_ENTER, 10, 13):
            return default
        return chr(ch).lower() != "n" if default else chr(ch).lower() == "y"

    def message(self, lines, wait=True, attr=None):
        """Display a list of strings; optionally wait for keypress."""
        self.scr.erase()
        self.title_bar()
        self.status_bar("Press any key to continue…" if wait else "")
        for i, line in enumerate(lines[:self.rows() - 4]):
            self.put(2 + i, 2, line, attr or C_NORMAL)
        self.scr.refresh()
        if wait:
            self.scr.getch()

    def run_with_log(self, title, cmd, cwd=None, env=None):
        """
        Run *cmd* (list) in a subprocess and stream output into a scrolling log
        window.  Returns (returncode, combined_output_str).
        """
        self.scr.erase()
        self.title_bar()
        self.status_bar("Running… (Ctrl-C to abort)")
        self.put(1, 2, title, C_HIGHLIGHT)
        log_rows = self.rows() - 4
        log_cols = self.cols() - 4
        lines = []
        output_buf = []

        def redraw():
            visible = lines[-log_rows:]
            for i, ln in enumerate(visible):
                try:
                    self.scr.addstr(2 + i, 2, ln[:log_cols].ljust(log_cols))
                except curses.error:
                    pass
            self.scr.refresh()

        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            cwd=cwd, env=env, text=True, bufsize=1
        )
        for raw in proc.stdout:
            ln = raw.rstrip()
            output_buf.append(ln)
            lines.append(ln)
            redraw()
        proc.wait()
        return proc.returncode, "\n".join(output_buf)


# ─────────────────────────────────────────────────────────────────────────────
# Section 2: System / distro detection
# ─────────────────────────────────────────────────────────────────────────────

class SysInfo:
    def __init__(self):
        self.distro = self._detect()
        self.pkg_cmd, self.pkg_list = self._pkg_manager()

    @staticmethod
    def _detect():
        for f in ("/etc/os-release", "/usr/lib/os-release"):
            if os.path.exists(f):
                with open(f) as fh:
                    for line in fh:
                        if line.startswith("ID="):
                            return line.split("=")[1].strip().strip('"').lower()
        return "unknown"

    def _pkg_manager(self):
        if shutil.which("apt-get"):
            return ("apt-get", "apt")
        if shutil.which("pacman"):
            return ("pacman", "pacman")
        if shutil.which("dnf"):
            return ("dnf", "dnf")
        return (None, "apt")

    def install_packages(self, ui: UI):
        try:
            with open(DEPS_FILE) as f:
                deps = json.load(f)
        except Exception as e:
            ui.message([f"Cannot read deps.json: {e}"], attr=C_ERROR)
            return False
        pkgs = deps.get(self.pkg_list, deps.get("apt", []))
        if not self.pkg_cmd:
            ui.message(["No supported package manager found.",
                        "Install deps manually from deps.json and rerun."], attr=C_ERROR)
            return False

        if self.pkg_cmd == "apt-get":
            rc, _ = ui.run_with_log("Updating package index…",
                                    ["sudo", "apt-get", "update", "-y"])
            cmd = ["sudo", "apt-get", "install", "-y"] + pkgs
        elif self.pkg_cmd == "pacman":
            cmd = ["sudo", "pacman", "-Sy", "--noconfirm"] + pkgs
        else:
            cmd = ["sudo", "dnf", "install", "-y"] + pkgs

        rc, _ = ui.run_with_log("Installing dependencies…", cmd)
        return rc == 0

    @staticmethod
    def mariadb_running():
        r = subprocess.run(["mysqladmin", "ping", "--silent"],
                           capture_output=True)
        return r.returncode == 0

    def install_mariadb(self, ui: UI):
        if self.pkg_cmd == "apt-get":
            cmd = ["sudo", "apt-get", "install", "-y", "mariadb-server"]
        elif self.pkg_cmd == "pacman":
            cmd = ["sudo", "pacman", "-Sy", "--noconfirm", "mariadb"]
        else:
            cmd = ["sudo", "dnf", "install", "-y", "mariadb-server"]
        rc, _ = ui.run_with_log("Installing MariaDB…", cmd)
        if rc == 0:
            subprocess.run(["sudo", "systemctl", "enable", "--now", "mariadb"],
                           capture_output=True)
        return rc == 0


# ─────────────────────────────────────────────────────────────────────────────
# Section 3: CMake build & install
# ─────────────────────────────────────────────────────────────────────────────

class Builder:
    def __init__(self, src_dir: Path, build_dir: Path, install_dir: Path, build_type="RelWithDebInfo"):
        self.src      = src_dir
        self.build    = build_dir
        self.install  = install_dir
        self.btype    = build_type

    def configure(self, ui: UI):
        self.build.mkdir(parents=True, exist_ok=True)
        cmd = [
            "cmake", str(self.src),
            f"-DCMAKE_INSTALL_PREFIX={self.install}",
            f"-DCMAKE_BUILD_TYPE={self.btype}",
            "-DSCRIPTS=static",
            "-DMODULES=static",
            "-DTOOLS_BUILD=maps-only",
            "-DAPPS_BUILD=all",
        ]
        rc, _ = ui.run_with_log("Configuring with CMake…", cmd, cwd=self.build)
        return rc == 0

    def build_all(self, ui: UI):
        jobs = str(os.cpu_count() or 4)
        rc, _ = ui.run_with_log("Building (this will take a while)…",
                                 ["make", f"-j{jobs}"], cwd=self.build)
        return rc == 0

    def install_all(self, ui: UI):
        rc, _ = ui.run_with_log("Installing binaries…",
                                 ["make", "install"], cwd=self.build)
        return rc == 0


# ─────────────────────────────────────────────────────────────────────────────
# Section 4: Database setup
# ─────────────────────────────────────────────────────────────────────────────

class Database:
    DB_NAMES = {
        "auth":       "acore_auth",
        "characters": "acore_characters",
        "world":      "acore_world",
    }

    def __init__(self, host, port, root_user, root_pass, db_user, db_pass):
        self.host      = host
        self.port      = str(port)
        self.root_user = root_user
        self.root_pass = root_pass
        self.db_user   = db_user
        self.db_pass   = db_pass

    def _mysql(self, sql, db=None):
        cmd = ["mysql",
               f"-h{self.host}", f"-P{self.port}",
               f"-u{self.root_user}", f"-p{self.root_pass}",
               "--batch", "--silent"]
        if db:
            cmd += [db]
        proc = subprocess.run(cmd, input=sql, capture_output=True, text=True)
        return proc.returncode, proc.stderr

    def _mysql_file(self, filepath, db):
        cmd = ["mysql",
               f"-h{self.host}", f"-P{self.port}",
               f"-u{self.root_user}", f"-p{self.root_pass}",
               db]
        with open(filepath, "rb") as f:
            proc = subprocess.run(cmd, stdin=f, capture_output=True)
        return proc.returncode

    def create_databases(self, ui: UI):
        """Create databases and grant privileges to db_user."""
        sql_lines = []
        for db in self.DB_NAMES.values():
            sql_lines.append(f"CREATE DATABASE IF NOT EXISTS `{db}` CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;")
        sql_lines.append(
            f"CREATE USER IF NOT EXISTS '{self.db_user}'@'%' IDENTIFIED BY '{self.db_pass}';"
        )
        sql_lines.append(
            f"CREATE USER IF NOT EXISTS '{self.db_user}'@'localhost' IDENTIFIED BY '{self.db_pass}';"
        )
        for db in self.DB_NAMES.values():
            sql_lines.append(f"GRANT ALL PRIVILEGES ON `{db}`.* TO '{self.db_user}'@'%';")
            sql_lines.append(f"GRANT ALL PRIVILEGES ON `{db}`.* TO '{self.db_user}'@'localhost';")
        sql_lines.append("FLUSH PRIVILEGES;")
        rc, err = self._mysql("\n".join(sql_lines))
        if rc != 0:
            ui.message([f"DB create failed: {err[:200]}"], attr=C_ERROR)
        return rc == 0

    def import_sql_dir(self, ui: UI, sql_dir: Path, db_key: str):
        """Import all .sql files in sql_dir (sorted) into the named DB."""
        db = self.DB_NAMES[db_key]
        files = sorted(sql_dir.glob("*.sql"))
        total = len(files)
        if total == 0:
            return True
        for i, f in enumerate(files):
            ui.progress_bar(
                ui.rows() // 2, 2, ui.cols() - 30, i + 1, total,
                f"{db_key}:{f.name[:20]}"
            )
            ui.scr.refresh()
            rc = self._mysql_file(f, db)
            if rc != 0:
                ui.message([f"Failed importing {f.name}", f"into {db}"], attr=C_ERROR)
                return False
        return True

    def populate(self, ui: UI, src_dir: Path):
        """Import base SQL then updates then pending updates."""
        base = src_dir / "data" / "sql" / "base"
        updates = src_dir / "data" / "sql" / "updates"
        for key in ("auth", "characters", "world"):
            ui.scr.erase(); ui.title_bar()
            ui.put(2, 2, f"Importing database: {self.DB_NAMES[key]}", C_HIGHLIGHT)
            ui.scr.refresh()
            for sub in (
                base / f"db_{key}",
                updates / f"db_{key}",
                updates / f"pending_db_{key}",
            ):
                if sub.is_dir():
                    if not self.import_sql_dir(ui, sub, key):
                        return False
        return True

    def set_realmlist(self, proxy_ip):
        sql = (
            f"DELETE FROM `realmlist`;"
            f"INSERT INTO `realmlist` (id,name,address,localAddress,localSubnetMask,port,icon,flag,timezone,allowedSecurityLevel,population,gamebuild) "
            f"VALUES (1,'C9Core','{proxy_ip}','{proxy_ip}','255.255.255.0',{CLIENT_PORT},0,0,1,0,0,12340);"
        )
        self._mysql(sql, self.DB_NAMES["auth"])

    def info_string(self, db_key):
        """Return worldserver-style database connection string."""
        return f"{self.host};{self.port};{self.db_user};{self.db_pass};{self.DB_NAMES[db_key]}"


# ─────────────────────────────────────────────────────────────────────────────
# Section 5: Map extraction
# ─────────────────────────────────────────────────────────────────────────────

class MapExtractor:
    def __init__(self, bin_dir: Path, client_dir: Path, data_dir: Path):
        self.bin    = bin_dir
        self.client = client_dir
        self.data   = data_dir   # destination: install_dir/data

    def _tool(self, name):
        return str(self.bin / name)

    def extract_maps(self, ui: UI):
        self.data.mkdir(parents=True, exist_ok=True)
        rc, _ = ui.run_with_log(
            "Extracting maps from client…",
            [self._tool("map_extractor"), "-i", str(self.client), "-o", str(self.data)],
        )
        return rc == 0

    def extract_vmaps(self, ui: UI):
        vmap_dir = self.data / "vmaps"
        vmap_dir.mkdir(exist_ok=True)
        rc, _ = ui.run_with_log(
            "Extracting VMaps (step 1/2)…",
            [self._tool("vmap4_extractor"), "-i", str(self.client), "-o", str(self.data / "Buildings")],
        )
        if rc != 0:
            return False
        rc, _ = ui.run_with_log(
            "Assembling VMaps (step 2/2)…",
            [self._tool("vmap4_assembler"),
             str(self.data / "Buildings"), str(vmap_dir)],
        )
        return rc == 0

    def extract_mmaps(self, ui: UI):
        mmap_dir = self.data / "mmaps"
        mmap_dir.mkdir(exist_ok=True)
        config = self.bin / "mmaps-config.yaml"
        cmd = [self._tool("mmaps_generator")]
        if config.exists():
            cmd += ["-c", str(config)]
        cmd += ["-i", str(self.data / "maps"),
                "-v", str(self.data / "vmaps"),
                "-o", str(mmap_dir)]
        rc, _ = ui.run_with_log(
            "Building MMaps (this can take hours)…", cmd
        )
        return rc == 0


# ─────────────────────────────────────────────────────────────────────────────
# Section 6: Config generation
# ─────────────────────────────────────────────────────────────────────────────

def _set_key(content: str, key: str, value) -> str:
    """Replace or append a key=value line in an AzerothCore .conf file."""
    if isinstance(value, str):
        new_line = f'{key} = "{value}"'
    else:
        new_line = f"{key} = {value}"
    pattern = rf'^[ \t]*{re.escape(key)}[ \t]*=.*$'
    if re.search(pattern, content, re.MULTILINE):
        return re.sub(pattern, new_line, content, flags=re.MULTILINE)
    return content + f"\n{new_line}\n"


def _read_dist(install_dir: Path, conf_name: str) -> str:
    """Read .conf.dist from the installed etc/ directory."""
    dist = install_dir / "etc" / f"{conf_name}.dist"
    real = install_dir / "etc" / conf_name
    src = real if real.exists() else dist
    if not src.exists():
        return f"# {conf_name} — generated by cluster installer\n"
    return src.read_text()


class ConfigGen:
    def __init__(self, install_dir: Path, nodes: list, db: Database,
                 shared_secret: str, proxy_node_idx: int):
        self.install      = install_dir
        self.etc          = install_dir / "etc"
        self.bin          = install_dir / "bin"
        self.nodes        = nodes          # list of node dicts
        self.db           = db
        self.secret       = shared_secret
        self.proxy_idx    = proxy_node_idx  # 0-based index into nodes
        self.proxy_ip     = nodes[proxy_node_idx]["ip"]

    def _write(self, filename: str, content: str):
        self.etc.mkdir(parents=True, exist_ok=True)
        (self.etc / filename).write_text(content)

    # ── authserver.conf ───────────────────────────────────────────────────────

    def gen_authserver(self):
        c = _read_dist(self.install, "authserver.conf")
        c = _set_key(c, "LoginDatabaseInfo", self.db.info_string("auth"))
        c = _set_key(c, "BindIP", "0.0.0.0")
        c = _set_key(c, "RealmServerPort", AUTH_PORT)
        c = _set_key(c, "LogsDir", str(self.install / "logs"))
        self._write("authserver.conf", c)

    # ── proxyserver.conf ──────────────────────────────────────────────────────

    def gen_proxyserver(self):
        c = _read_dist(self.install, "proxyserver.conf")
        c = _set_key(c, "BindIP", "0.0.0.0")
        c = _set_key(c, "WorldServerPort", CLIENT_PORT)
        c = _set_key(c, "ControlPort", CONTROL_PORT)
        c = _set_key(c, "NodeMgrPort", NODEMGR_PORT)
        c = _set_key(c, "ManagementPort", MGMT_PORT)
        c = _set_key(c, "Management.SharedSecret", self.secret)
        c = _set_key(c, "LoginDatabaseInfo", self.db.info_string("auth"))
        c = _set_key(c, "LogsDir", str(self.install / "logs"))
        c = _set_key(c, "Cluster.LFGMasterNode", 1)

        # Node count and addresses (worldserver nodes only)
        ws_nodes = [n for n in self.nodes if n["role"] != "proxy"]
        c = _set_key(c, "WorldServer.Node.Count", len(ws_nodes))
        for i, n in enumerate(ws_nodes, start=1):
            c = _set_key(c, f"WorldServer.Node.{i}.Address", n["ip"])
            c = _set_key(c, f"WorldServer.Node.{i}.Port",
                         GAME_PORT_BASE + n["node_num"] - 1)

        # Default routing: instance node (last node) or first
        instance_nodes = [n for n in ws_nodes if n.get("instance_only")]
        default_node = instance_nodes[-1]["node_num"] if instance_nodes else ws_nodes[-1]["node_num"]
        c = _set_key(c, "Cluster.MapRouting.Default", default_node)

        self._write("proxyserver.conf", c)

    # ── worldserver conf (one per worldserver / instance node) ────────────────

    def gen_worldserver(self, node: dict):
        n = node["node_num"]
        if node.get("instance_only"):
            tpl = "worldserver-instance.conf"
        else:
            tpl = f"worldserver-node{n}.conf"

        c = _read_dist(self.install, "worldserver.conf")

        c = _set_key(c, "LoginDatabaseInfo",     self.db.info_string("auth"))
        c = _set_key(c, "WorldDatabaseInfo",      self.db.info_string("world"))
        c = _set_key(c, "CharacterDatabaseInfo",  self.db.info_string("characters"))
        c = _set_key(c, "WorldServerPort",        GAME_PORT_BASE + n - 1)
        c = _set_key(c, "ClusterServer.Address",  self.proxy_ip)
        c = _set_key(c, "ClusterServer.ControlPort", CONTROL_PORT)
        c = _set_key(c, "ClusterServer.LFGMasterNode", 1)
        c = _set_key(c, "LogsDir", str(self.install / "logs"))
        c = _set_key(c, "DataDir", str(self.install / "data"))

        if node.get("instance_only"):
            c = _set_key(c, "ClusterServer.InstanceServer", 1)
            c = _set_key(c, "InstanceServer.Enable", 0)
        else:
            c = _set_key(c, "ClusterServer.InstanceServer", 0)
            maps = node.get("maps", [])
            c = _set_key(c, "ClusterServer.Maps",
                         ",".join(str(m) for m in maps) if maps else "-1")

        self._write(tpl, c)
        return tpl

    # ── nodemgr conf (one per worldserver / instance node) ───────────────────

    def gen_nodemgr(self, node: dict):
        n = node["node_num"]
        tpl = f"nodemgr-node{n}.conf"
        ws_conf = ("worldserver-instance.conf"
                   if node.get("instance_only")
                   else f"worldserver-node{n}.conf")

        c = _read_dist(self.install, "nodemgr.conf")
        c = _set_key(c, "ProxyAddress", self.proxy_ip)
        c = _set_key(c, "ProxyNodeMgrPort", NODEMGR_PORT)
        c = _set_key(c, "Management.SharedSecret", self.secret)
        c = _set_key(c, "NodeId", n)
        c = _set_key(c, "WorldServerPort", GAME_PORT_BASE + n - 1)
        c = _set_key(c, "WorldserverBin", str(self.bin / "worldserver"))
        c = _set_key(c, "WorldserverConfig", str(self.install / "etc" / ws_conf))
        c = _set_key(c, "WorldserverLog", str(self.install / "logs" / f"worldserver-node{n}.log"))
        c = _set_key(c, "LogsDir", str(self.install / "logs"))
        self._write(tpl, c)
        return tpl

    def gen_all(self, ui: UI):
        ui.put(2, 2, "Generating configuration files…", C_HIGHLIGHT)
        ui.scr.refresh()
        self.gen_authserver()
        self.gen_proxyserver()
        for node in self.nodes:
            if node["role"] in ("worldserver", "instance"):
                self.gen_worldserver(node)
                self.gen_nodemgr(node)
        ui.put(3, 2, "Done.", C_OK)
        ui.scr.refresh()
        time.sleep(1)


# ─────────────────────────────────────────────────────────────────────────────
# Section 7: SSH deployment
# ─────────────────────────────────────────────────────────────────────────────

class Deployer:
    def __init__(self, local_install: Path):
        self.local = local_install

    def _ssh(self, node: dict, cmd: str, ui: UI):
        return ui.run_with_log(
            f"SSH {node['ip']}: {cmd[:60]}",
            self._ssh_cmd(node) + [cmd],
        )

    @staticmethod
    def _ssh_cmd(node: dict):
        base = []
        if node.get("ssh_pass"):
            base = ["sshpass", f"-p{node['ssh_pass']}"]
        base += ["ssh", "-o", "StrictHostKeyChecking=no",
                 "-p", str(node.get("ssh_port", 22))]
        if node.get("ssh_key"):
            base += ["-i", node["ssh_key"]]
        base.append(f"{node['ssh_user']}@{node['ip']}")
        return base

    @staticmethod
    def _scp_cmd(node: dict, src: str, dst: str):
        base = []
        if node.get("ssh_pass"):
            base = ["sshpass", f"-p{node['ssh_pass']}"]
        base += ["scp", "-o", "StrictHostKeyChecking=no", "-r",
                 "-P", str(node.get("ssh_port", 22))]
        if node.get("ssh_key"):
            base += ["-i", node["ssh_key"]]
        base += [src, f"{node['ssh_user']}@{node['ip']}:{dst}"]
        return base

    def install_deps_remote(self, node: dict, deps_file: Path, ui: UI):
        """Copy deps.json and run the appropriate package manager on remote host."""
        # Detect remote distro
        rc, out = self._ssh(node, "cat /etc/os-release 2>/dev/null | grep '^ID=' | head -1", ui)
        pkg_mgr = "apt"
        if "arch" in out.lower() or "manjaro" in out.lower():
            pkg_mgr = "pacman"
        elif "fedora" in out.lower() or "rhel" in out.lower() or "centos" in out.lower():
            pkg_mgr = "dnf"

        try:
            with open(deps_file) as f:
                deps = json.load(f)
            pkgs = deps.get(pkg_mgr, deps["apt"])
        except Exception:
            pkgs = []

        if pkg_mgr == "apt":
            self._ssh(node, "sudo apt-get update -y", ui)
            pkg_cmd = "sudo apt-get install -y " + " ".join(pkgs)
        elif pkg_mgr == "pacman":
            pkg_cmd = "sudo pacman -Sy --noconfirm " + " ".join(pkgs)
        else:
            pkg_cmd = "sudo dnf install -y " + " ".join(pkgs)

        self._ssh(node, pkg_cmd, ui)

    def deploy_node(self, node: dict, ui: UI):
        """
        Push all install files to a remote node and ensure directories exist.
        The remote install path mirrors the local install path.
        """
        remote_base = str(self.local)
        self._ssh(node, f"mkdir -p {remote_base}/{{bin,etc,logs,data}}", ui)

        # Transfer binaries
        ui.put(2, 2, f"Copying binaries to {node['ip']}…", C_HIGHLIGHT)
        ui.scr.refresh()
        subprocess.run(self._scp_cmd(node, str(self.local / "bin"), remote_base),
                       capture_output=True)

        # Transfer configs
        ui.put(3, 2, f"Copying configs to {node['ip']}…", C_HIGHLIGHT)
        ui.scr.refresh()
        subprocess.run(self._scp_cmd(node, str(self.local / "etc"), remote_base),
                       capture_output=True)

        # Transfer data (maps, vmaps, mmaps)
        if (self.local / "data").exists():
            ui.put(4, 2, f"Copying data to {node['ip']} (may be slow)…", C_HIGHLIGHT)
            ui.scr.refresh()
            subprocess.run(self._scp_cmd(node, str(self.local / "data"), remote_base),
                           capture_output=True)

    def deploy_all(self, nodes: list, ui: UI):
        local_node_ips = {socket.gethostbyname(socket.gethostname()), "127.0.0.1"}
        for node in nodes:
            if node["ip"] in local_node_ips:
                continue  # skip local node
            ui.scr.erase(); ui.title_bar()
            ui.put(2, 2, f"Deploying to node {node['node_num']} ({node['ip']})…", C_HIGHLIGHT)
            ui.scr.refresh()
            self.deploy_node(node, ui)


# ─────────────────────────────────────────────────────────────────────────────
# Section 8: Main installer flow
# ─────────────────────────────────────────────────────────────────────────────

class InstallerApp:
    def __init__(self, stdscr):
        self.ui = UI(stdscr)
        self.cfg = {}  # accumulated user answers

    # ── Helper: full-screen form ───────────────────────────────────────────────

    def form(self, title: str, fields: list) -> dict:
        """
        Display a series of labelled text inputs.
        fields = list of (key, label, default, password_bool)
        Returns dict of {key: value}.
        """
        result = {}
        self.ui.scr.erase()
        self.ui.title_bar()
        self.ui.status_bar("ENTER to advance  Ctrl-C to quit")
        self.ui.put(2, 2, title, C_HIGHLIGHT)
        for i, (key, label, default, is_pw) in enumerate(fields):
            val = self.ui.prompt_text(4 + i * 2, 4, label, default,
                                      password=is_pw, width=50)
            result[key] = val
        return result

    # ── Screen: welcome ───────────────────────────────────────────────────────

    def screen_welcome(self):
        self.ui.message([
            "",
            "  Welcome to the C9Core Cluster Installer",
            "",
            "  This wizard will guide you through:",
            "   • Installing build dependencies",
            "   • Setting up MariaDB",
            "   • Building and installing C9Core",
            "   • Importing the three game databases",
            "   • Extracting maps, VMaps, and MMaps from your WoW client",
            "   • Assigning maps to cluster nodes",
            "   • Generating and deploying configuration files",
            "",
            "  You will need:",
            "   • Root / sudo access on all nodes",
            "   • A WoW 3.3.5a client directory (for map extraction)",
            "   • SSH access to remote cluster nodes",
            "",
        ], wait=True)

    # ── Screen: distro + deps ─────────────────────────────────────────────────

    def screen_deps(self):
        si = SysInfo()
        self.ui.scr.erase(); self.ui.title_bar()
        self.ui.put(2, 2, f"Detected distro: {si.distro}  (pkg manager: {si.pkg_cmd})", C_OK)
        self.ui.put(3, 2, "Install build dependencies now?", C_NORMAL)
        self.ui.scr.refresh()
        if self.ui.confirm("Install dependencies?", default=True):
            ok = si.install_packages(self.ui)
            if not ok:
                self.ui.message(["Dependency install failed — continuing anyway."], attr=C_ERROR)
        self.cfg["sysinfo"] = si

    # ── Screen: MariaDB ───────────────────────────────────────────────────────

    def screen_mariadb(self):
        si = self.cfg["sysinfo"]
        if not si.mariadb_running():
            if self.ui.confirm("MariaDB not detected. Install locally?", default=True):
                si.install_mariadb(self.ui)

        d = self.form("Database Configuration", [
            ("db_host",      "MariaDB host",       "127.0.0.1", False),
            ("db_port",      "MariaDB port",        "3306",      False),
            ("db_root_user", "Root username",       "root",      False),
            ("db_root_pass", "Root password",       "",          True),
            ("db_user",      "Game DB username",    "acore",     False),
            ("db_pass",      "Game DB password",    "acore",     True),
        ])
        self.cfg["db"] = Database(
            d["db_host"], int(d["db_port"]),
            d["db_root_user"], d["db_root_pass"],
            d["db_user"], d["db_pass"],
        )

    # ── Screen: source + install paths ────────────────────────────────────────

    def screen_paths(self):
        default_src = str(Path(__file__).parent.parent.parent)
        d = self.form("Build Paths", [
            ("src_dir",     "Source directory",  default_src,                   False),
            ("build_dir",   "CMake build dir",   default_src + "/build",        False),
            ("install_dir", "Install directory", str(Path.home() / "wowcluster"), False),
            ("build_type",  "Build type",        "RelWithDebInfo",              False),
        ])
        self.cfg.update(d)
        self.cfg["src"]     = Path(d["src_dir"])
        self.cfg["build"]   = Path(d["build_dir"])
        self.cfg["install"] = Path(d["install_dir"])

    # ── Screen: node count + node details ─────────────────────────────────────

    def screen_nodes(self):
        self.ui.scr.erase(); self.ui.title_bar()
        self.ui.put(2, 2, "Cluster Node Configuration", C_HIGHLIGHT)
        self.ui.put(4, 2, "One node will host the proxy server + auth server.", C_DIM)
        self.ui.put(5, 2, "Remaining nodes run worldserver / instance processes.", C_DIM)
        n_str = self.ui.prompt_text(7, 2, "Number of nodes (1–9)", "3")
        n = max(1, min(9, int(n_str) if n_str.isdigit() else 3))

        nodes = []
        for i in range(n):
            self.ui.scr.erase(); self.ui.title_bar()
            self.ui.put(2, 2, f"Configure Node {i+1} of {n}", C_HIGHLIGHT)
            ip   = self.ui.prompt_text(4, 2, f"Node {i+1} IP address", "127.0.0.1" if i==0 else "")
            user = self.ui.prompt_text(5, 2, "SSH username", "reno")
            key  = self.ui.prompt_text(6, 2, "SSH key path (blank = password)", str(Path.home()/".ssh"/"id_rsa"))
            pw   = self.ui.prompt_text(7, 2, "SSH password (blank = use key)", "", password=True) if not key else ""

            roles = ["proxy + auth (primary node)", "worldserver", "instance server"]
            default_role = 0 if i == 0 else (2 if i == n-1 and n > 1 else 1)
            role_idx = self.ui.prompt_select(
                f"Role for Node {i+1} ({ip})", roles,
                status_hint="↑↓ Navigate  ENTER Select"
            )
            role_map = {0: "proxy", 1: "worldserver", 2: "instance"}
            nodes.append({
                "node_num":    i + 1,
                "ip":          ip,
                "ssh_user":    user,
                "ssh_key":     key,
                "ssh_pass":    pw,
                "ssh_port":    22,
                "role":        role_map[role_idx],
                "instance_only": role_idx == 2,
                "maps":        [],
            })

        self.cfg["nodes"] = nodes
        # Proxy node index
        proxy_idxs = [i for i, n in enumerate(nodes) if n["role"] == "proxy"]
        self.cfg["proxy_idx"] = proxy_idxs[0] if proxy_idxs else 0

    # ── Screen: build ─────────────────────────────────────────────────────────

    def screen_build(self):
        if not self.ui.confirm("Configure and build C9Core now?", default=True):
            return
        b = Builder(self.cfg["src"], self.cfg["build"],
                    self.cfg["install"], self.cfg["build_type"])
        if not b.configure(self.ui):
            self.ui.message(["CMake configure failed!"], attr=C_ERROR)
            return
        if not b.build_all(self.ui):
            self.ui.message(["Build failed!"], attr=C_ERROR)
            return
        if not b.install_all(self.ui):
            self.ui.message(["Install failed!"], attr=C_ERROR)
            return
        self.ui.message(["Build and install complete."], attr=C_OK, wait=True)

    # ── Screen: database import ───────────────────────────────────────────────

    def screen_database(self):
        db = self.cfg["db"]
        self.ui.scr.erase(); self.ui.title_bar()
        self.ui.put(2, 2, "Setting up databases…", C_HIGHLIGHT)
        self.ui.scr.refresh()
        if not db.create_databases(self.ui):
            return
        if not db.populate(self.ui, self.cfg["src"]):
            self.ui.message(["Database import failed!"], attr=C_ERROR)
            return
        proxy_ip = self.cfg["nodes"][self.cfg["proxy_idx"]]["ip"]
        db.set_realmlist(proxy_ip)
        self.ui.message(["Database import complete."], attr=C_OK, wait=True)

    # ── Screen: client data + map extraction ──────────────────────────────────

    def screen_maps(self):
        self.ui.scr.erase(); self.ui.title_bar()
        self.ui.put(2, 2, "Map Extraction", C_HIGHLIGHT)
        self.ui.put(4, 2, "Point to your WoW 3.3.5a client folder.", C_DIM)
        self.ui.put(5, 2, "All extracted data will go to <install>/data/", C_DIM)
        client = self.ui.prompt_text(7, 2, "WoW client path",
                                     str(Path.home() / "wow_client"), width=60)
        if not client or not Path(client).is_dir():
            self.ui.message([f"Path not found: {client}", "Skipping map extraction."],
                            attr=C_ERROR, wait=True)
            return

        install = self.cfg["install"]
        me = MapExtractor(install / "bin", Path(client), install / "data")

        steps = [
            ("Extract maps",  me.extract_maps),
            ("Extract VMaps", me.extract_vmaps),
            ("Build MMaps (very slow)", me.extract_mmaps),
        ]
        for label, fn in steps:
            if self.ui.confirm(f"{label}?", default=True):
                ok = fn(self.ui)
                if not ok:
                    self.ui.message([f"{label} failed — continuing."], attr=C_ERROR, wait=True)

    # ── Screen: map assignment per node ───────────────────────────────────────

    def screen_map_assignment(self):
        nodes = self.cfg["nodes"]
        ws_nodes = [n for n in nodes if n["role"] in ("worldserver", "instance")]
        if not ws_nodes:
            return

        continent_opts = [("Instance-only (all dungeons/raids/BGs/arenas)", False)] + \
                         [(f"{name}  (Map {mid})", False) for name, mid in CONTINENTS]

        for node in ws_nodes:
            if node.get("instance_only"):
                # Instance node — force instance-only, skip UI
                node["maps"] = []
                continue

            title = (f"Node {node['node_num']}  ({node['ip']})  — Map Assignment\n"
                     f"  Select which continents this node hosts:")
            result = self.ui.prompt_checkboxes(title, continent_opts)

            if result[0]:  # Instance-only selected
                node["instance_only"] = True
                node["maps"] = []
            else:
                node["maps"] = [mid for i, (_, mid) in enumerate(CONTINENTS)
                                 if result[i + 1]]

    # ── Screen: config generation ─────────────────────────────────────────────

    def screen_configs(self):
        secret = secrets.token_hex(32)
        self.cfg["shared_secret"] = secret
        cg = ConfigGen(
            self.cfg["install"],
            self.cfg["nodes"],
            self.cfg["db"],
            secret,
            self.cfg["proxy_idx"],
        )
        self.ui.scr.erase(); self.ui.title_bar()
        cg.gen_all(self.ui)
        self.ui.message(
            ["Configuration files written to:", str(self.cfg["install"] / "etc")],
            attr=C_OK, wait=True,
        )

    # ── Screen: remote deps + deployment ─────────────────────────────────────

    def screen_deploy(self):
        nodes = self.cfg["nodes"]
        remote_nodes = [n for n in nodes if n["ip"] not in ("127.0.0.1", "localhost")]
        if not remote_nodes:
            return

        dep = Deployer(self.cfg["install"])

        if self.ui.confirm("Install dependencies on remote nodes via SSH?", default=True):
            for node in remote_nodes:
                self.ui.scr.erase(); self.ui.title_bar()
                self.ui.put(2, 2, f"Installing deps on node {node['node_num']} ({node['ip']})…", C_HIGHLIGHT)
                self.ui.scr.refresh()
                dep.install_deps_remote(node, DEPS_FILE, self.ui)

        if self.ui.confirm("Deploy binaries and configs to remote nodes?", default=True):
            dep.deploy_all(nodes, self.ui)

    # ── Screen: final summary ─────────────────────────────────────────────────

    def screen_done(self):
        nodes     = self.cfg["nodes"]
        install   = self.cfg["install"]
        proxy     = nodes[self.cfg["proxy_idx"]]
        lines = [
            "",
            "  ✓  C9Core Cluster Setup Complete",
            "",
            f"  Install directory : {install}",
            f"  Proxy / Auth node : {proxy['ip']}",
            "",
            "  Node summary:",
        ]
        for n in nodes:
            role = n["role"].upper()
            maps = "instance-only" if n.get("instance_only") else \
                   (", ".join(name for name, mid in CONTINENTS if mid in n.get("maps",[])) or "all")
            lines.append(f"    Node {n['node_num']}  {n['ip']:16s}  {role:12s}  {maps}")

        lines += [
            "",
            "  To start the cluster:",
            f"    {install}/bin/authserver   -c {install}/etc/authserver.conf",
            f"    {install}/bin/proxyserver  -c {install}/etc/proxyserver.conf",
            f"    {install}/bin/nodemgr      -c {install}/etc/nodemgr-nodeN.conf  (each node)",
            "",
            "  Set your WoW client realmlist to:",
            f"    set realmlist {proxy['ip']}",
            "",
        ]
        self.ui.message(lines, wait=True, attr=C_OK)

    # ── Main flow ─────────────────────────────────────────────────────────────

    def run(self):
        self.screen_welcome()
        self.screen_deps()
        self.screen_mariadb()
        self.screen_paths()
        self.screen_nodes()
        self.screen_build()
        self.screen_database()
        self.screen_maps()
        self.screen_map_assignment()
        self.screen_configs()
        self.screen_deploy()
        self.screen_done()


# ─────────────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────────────

def main(stdscr):
    try:
        app = InstallerApp(stdscr)
        app.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    # Verify terminal is large enough
    rows, cols = os.popen("stty size", "r").read().split()
    if int(rows) < 24 or int(cols) < 80:
        print("Terminal must be at least 80x24. Please resize and rerun.")
        sys.exit(1)
    curses.wrapper(main)
