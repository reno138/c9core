#!/usr/bin/env python3
"""Generate per-node worldserver configs from the dist template."""
import re, shutil, os

DIST = os.path.expanduser("~/c9core-install/etc/worldserver.conf.dist")
OUT  = os.path.expanduser("~/c9core-install/etc")

# Per-node overrides  (key -> value as it should appear in the file)
NODES = {
    1: {
        "RealmID":                        "1",
        "WorldServerPort":                "8086",
        "DataDir":                        '"."',
        "LogsDir":                        '"../logs"',
        "Console.Enable":                 "0",
        "LogLevel":                       "1",
        "Appender.Console":               '"1,1,0"',
        "Appender.Server":                '"2,5,0,Server.log"',
        "Appender.Errors":                '"2,2,0,Errors.log,w"',
        "Appender.PacketTrace":           '"2,5,0,PacketTrace.log,w"',
        "Logger.root":                    '"5,Console Server"',
        "Logger.cluster.packettrace":     '"5,PacketTrace"',
        "ClusterServer.Enable":           "1",
        "ClusterServer.NodeId":           "1",
        "ClusterServer.GameAddress":      '"192.0.2.70"',
        "ClusterServer.GamePort":         "8086",
        "ClusterServer.Maps":             '"0"',
        "ClusterServer.NatsURL":          '"nats://127.0.0.1:4222"',
        "ClusterServer.BgCoordinator":    "1",
        "ClusterServer.BgCoordinatorNode":"1",
        "ClusterServer.LfgCoordinator":   "1",
        "ClusterServer.LFGMasterNode":    "1",
        "ClusterServer.InstanceNode":     "0",
        "ClusterServer.InstanceServer":   "0",
    },
    2: {
        "RealmID":                        "1",
        "WorldServerPort":                "8086",
        "LoginDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_auth"',
        "WorldDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_world"',
        "CharacterDatabaseInfo":          '"192.0.2.70;3306;acore;acore;acore_characters"',
        "Updates.EnableDatabases":        "0",
        "DataDir":                        '"."',
        "LogsDir":                        '"../logs"',
        "Console.Enable":                 "0",
        "LogLevel":                       "1",
        "Appender.Console":               '"1,1,0"',
        "Appender.Server":                '"2,4,0,Server.log"',
        "Appender.Errors":                '"2,2,0,Errors.log,w"',
        "Appender.PacketTrace":           '"2,5,0,PacketTrace.log,w"',
        "Logger.root":                    '"4,Console Server"',
        "Logger.cluster.packettrace":     '"5,PacketTrace"',
        "ClusterServer.Enable":           "1",
        "ClusterServer.NodeId":           "2",
        "ClusterServer.GameAddress":      '"192.0.2.71"',
        "ClusterServer.GamePort":         "8086",
        "ClusterServer.Maps":             '"1"',
        "ClusterServer.NatsURL":          '"nats://192.0.2.70:4222"',
        "ClusterServer.BgCoordinator":    "1",
        "ClusterServer.BgCoordinatorNode":"1",
        "ClusterServer.LfgCoordinator":   "1",
        "ClusterServer.LFGMasterNode":    "1",
        "ClusterServer.InstanceNode":     "0",
        "ClusterServer.InstanceServer":   "0",
    },
    3: {
        "RealmID":                        "1",
        "WorldServerPort":                "8086",
        "LoginDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_auth"',
        "WorldDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_world"',
        "CharacterDatabaseInfo":          '"192.0.2.70;3306;acore;acore;acore_characters"',
        "Updates.EnableDatabases":        "0",
        "DataDir":                        '"."',
        "LogsDir":                        '"../logs"',
        "Console.Enable":                 "0",
        "LogLevel":                       "1",
        "Appender.Console":               '"1,1,0"',
        "Appender.Server":                '"2,4,0,Server.log"',
        "Appender.Errors":                '"2,2,0,Errors.log,w"',
        "Appender.PacketTrace":           '"2,3,0,PacketTrace.log,w"',
        "Logger.root":                    '"4,Console Server"',
        "Logger.cluster.packettrace":     '"3,PacketTrace"',
        "ClusterServer.Enable":           "1",
        "ClusterServer.NodeId":           "3",
        "ClusterServer.GameAddress":      '"192.0.2.72"',
        "ClusterServer.GamePort":         "8086",
        "ClusterServer.Maps":             '"530,571"',
        "ClusterServer.NatsURL":          '"nats://192.0.2.70:4222"',
        "ClusterServer.BgCoordinator":    "1",
        "ClusterServer.BgCoordinatorNode":"1",
        "ClusterServer.LfgCoordinator":   "1",
        "ClusterServer.LFGMasterNode":    "1",
        "ClusterServer.InstanceNode":     "0",
        "ClusterServer.InstanceServer":   "0",
    },
    4: {
        "RealmID":                        "1",
        "WorldServerPort":                "8086",
        "LoginDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_auth"',
        "WorldDatabaseInfo":              '"192.0.2.70;3306;acore;acore;acore_world"',
        "CharacterDatabaseInfo":          '"192.0.2.70;3306;acore;acore;acore_characters"',
        "Updates.EnableDatabases":        "0",
        "DataDir":                        '"."',
        "LogsDir":                        '"../logs"',
        "Console.Enable":                 "0",
        "LogLevel":                       "1",
        "Appender.Console":               '"1,1,0"',
        "Appender.Server":                '"2,4,0,Server.log"',
        "Appender.Errors":                '"2,2,0,Errors.log,w"',
        "Appender.PacketTrace":           '"2,3,0,PacketTrace.log,w"',
        "Logger.root":                    '"4,Console Server"',
        "Logger.cluster.packettrace":     '"3,PacketTrace"',
        "ClusterServer.Enable":           "1",
        "ClusterServer.NodeId":           "4",
        "ClusterServer.GameAddress":      '"192.0.2.73"',
        "ClusterServer.GamePort":         "8086",
        "ClusterServer.Maps":             '""',
        "ClusterServer.NatsURL":          '"nats://192.0.2.70:4222"',
        "ClusterServer.BgCoordinator":    "1",
        "ClusterServer.BgCoordinatorNode":"1",
        "ClusterServer.LfgCoordinator":   "1",
        "ClusterServer.LFGMasterNode":    "1",
        "ClusterServer.InstanceNode":     "1",
        "ClusterServer.InstanceServer":   "1",
    },
}

with open(DIST) as f:
    dist_lines = f.readlines()

for node, overrides in NODES.items():
    applied = set()
    out_lines = []
    for line in dist_lines:
        m = re.match(r'^([A-Za-z][A-Za-z0-9_.]*)\s*=', line)
        if m:
            key = m.group(1)
            if key in overrides:
                line = f"{key} = {overrides[key]}\n"
                applied.add(key)
        out_lines.append(line)

    # Append any overrides that weren't found in the dist (e.g. Appender.Errors)
    missing = set(overrides) - applied
    if missing:
        out_lines.append("\n# Node-specific additions\n")
        for key in sorted(missing):
            out_lines.append(f"{key} = {overrides[key]}\n")

    out_path = os.path.join(OUT, f"worldserver-node{node}.conf")
    with open(out_path, "w") as f:
        f.writelines(out_lines)
    print(f"Node {node}: wrote {out_path} ({len(out_lines)} lines, {len(missing)} appended keys: {sorted(missing)})")
