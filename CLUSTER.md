# C9Core Multi-Worldserver Clustering

This document covers the cluster architecture, configuration reference, and step-by-step deployment instructions for running multiple worldserver nodes behind a proxy.

---

## Architecture Overview

```
WoW Client A ──┐
WoW Client B ──┤──► proxyserver :8085 ──► worldserver node 1 :8086
WoW Client C ──┤          │
WoW Client D ──┘          ├──► worldserver node 2 :8087
                          │
                    control bus :8090
                          │
                 worldserver node 1 ◄──► worldserver node 2
                       (via proxy — nodes never talk directly)
```

**proxyserver** sits between WoW clients and the worldserver pool. It:
- Intercepts `CMSG_AUTH_SESSION` to authenticate clients and pick a node (least-loaded)
- Maintains a TCP control channel (port 8090) with every worldserver node
- Routes cross-node messages: player directory, packet delivery, group/LFG relay

**worldserver nodes** are standard `worldserver` processes with minor additions:
- `ProxyClient` — connects to proxy control port, registers, sends/receives cluster messages
- `ClusterMgr` — in-memory cache of players logged in on remote nodes

**Shared state via database**: all nodes share the same `acore_characters` and `acore_world` databases, so character data, mail, auctions, etc. are always consistent.

---

## What Works Across Nodes

| Feature | Status | Notes |
|---------|--------|-------|
| Player directory (online/offline) | ✅ | Broadcast to all nodes |
| Cross-node whispers | ✅ | Proxy delivers packet to client's TCP session |
| Cross-node guild chat | ✅ | |
| Cross-node party/raid chat | ✅ | |
| Cross-node channel chat | ✅ | |
| Cross-node /who | ✅ | Merges remote players into results |
| Cross-node friend status | ✅ | |
| Cross-node group formation (invite) | ✅ | Groups owned by inviter's node |
| Cross-node group updates/disband | ✅ | Broadcast to all member nodes |
| LFG dungeon finder across nodes | ✅ | Master-node model; see below |
| Player reroute to instance server | ✅ | Proxy rewrites TCP session |
| Seeing other players (world presence) | ❌ by design | Nodes are separate map layers |
| Cross-node raid browser (LFR) | ❌ | Not implemented |

---

## LFG Master-Node Model

One worldserver is designated the **LFG master** (`ClusterServer.LFGMasterNode`). It runs the authoritative LFG queue. Non-master nodes relay `JOIN`, `LEAVE`, and `PROPOSAL_RESULT` opcodes to the master via the proxy. When a group is formed, each player receives a reroute to the instance server address.

---

## Building

Build all three applications (default `APPS_BUILD=all` includes proxyserver):

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/azeroth-server \
         -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DSCRIPTS=static -DMODULES=static
make -j$(nproc)
make install
```

To build proxyserver without worldserver/authserver:

```bash
cmake .. -DAPPS_BUILD=none -DAPP_PROXYSERVER=enabled
```

---

## Configuration

### proxyserver.conf

```ini
# ── Network ──────────────────────────────────────────────────
# Port WoW clients connect to (replace your worldserver port in realmlist)
WorldServerPort = 8085

# Port worldserver nodes connect to for the control channel
ControlPort = 8090

BindIP = "0.0.0.0"

# Realm ID that this proxy serves (must match acore_auth.realmlist.id)
RealmID = 1

# ── Cluster nodes ─────────────────────────────────────────────
# Number of worldserver nodes in the pool
WorldServer.Node.Count = 2

# Address and game port for each node (1-based index)
# These must match the game port each worldserver listens on.
WorldServer.Node.1.Address = "127.0.0.1"
WorldServer.Node.1.Port    = 8086

WorldServer.Node.2.Address = "127.0.0.1"
WorldServer.Node.2.Port    = 8087

# Node ID of the LFG master (default 1)
ClusterServer.LFGMasterNode = 1

# ── Database ──────────────────────────────────────────────────
# Proxy only needs the login database (for session key verification)
LoginDatabaseInfo = "127.0.0.1;3306;acore;acore;acore_auth"

# Disable DB schema updates (authserver or node 1 handles those)
Updates.EnableDatabases = 0

# ── Logging ───────────────────────────────────────────────────
Logger.root = 2,Console Server
```

### worldserver.conf (per-node additions)

Add these keys to each node's `worldserver.conf`:

```ini
# ── Cluster ───────────────────────────────────────────────────
# Address and control port of the proxy server
ClusterServer.Address     = "127.0.0.1"
ClusterServer.ControlPort = 8090

# The game port THIS node listens on (must match proxy's Node.N.Port)
# Node 1:
WorldServerPort = 8086
# Node 2:
WorldServerPort = 8087

# LFG master node ID (match proxyserver.conf)
ClusterServer.LFGMasterNode = 1

# ── Disable per-node DB updates on non-primary nodes ──────────
# Only node 1 should apply schema updates
Updates.EnableDatabases = 0   # set to 6 on node 1 only

# ── Realm entry ───────────────────────────────────────────────
# All nodes share the same RealmID; proxy is the public-facing address
RealmID = 1
```

---

## Deploying a New Cluster Node

### Step 1: Install the binary

Copy the `worldserver` binary (and `libscripts.a`/modules if dynamic) to the new host. All nodes run the same binary.

### Step 2: Create a node config

Copy `worldserver.conf` from node 1. Change:

```ini
WorldServerPort = 808X          # unique port for this node
Updates.EnableDatabases = 0     # node 1 already handles updates
```

Keep `RealmID`, `LoginDatabaseInfo`, `WorldDatabaseInfo`, `CharacterDatabaseInfo` identical across all nodes.

### Step 3: Register the node in proxyserver.conf

```ini
WorldServer.Node.Count = 3       # increment

WorldServer.Node.3.Address = "192.168.1.5"   # new host IP
WorldServer.Node.3.Port    = 8088
```

Restart the proxyserver for config changes to take effect.

### Step 4: Start the new node

```bash
./worldserver -c worldserver-node3.conf >> /tmp/node3.log 2>&1 < <(sleep infinity) &
```

The node will connect to the proxy control port, receive a node ID assignment, and begin accepting players routed to it.

---

## Startup Procedure

Start services in this order:

```bash
BIN=/path/to/bin
ETC=/path/to/etc

# 1. Auth server
./authserver -c $ETC/authserver.conf >> /tmp/auth.log 2>&1 &

# 2. Proxy server  (stdbuf -oL keeps stdout line-buffered when redirecting)
stdbuf -oL ./proxyserver -c $ETC/proxyserver.conf >> /tmp/proxy.log 2>&1 &

# 3. Worldserver nodes  (< <(sleep infinity) prevents CLI thread from spinning)
./worldserver -c $ETC/worldserver-node1.conf >> /tmp/node1.log 2>&1 < <(sleep infinity) &
./worldserver -c $ETC/worldserver-node2.conf >> /tmp/node2.log 2>&1 < <(sleep infinity) &
```

Wait ~10 seconds for nodes to register, then verify in `/tmp/proxy.log`:

```
ControlSocket: Node 1 registered (addr=127.0.0.1:8086, id=1)
ControlSocket: Node 2 registered (addr=127.0.0.1:8087, id=2)
```

And in each node log:

```
ProxyClient: Connected to proxy 127.0.0.1:8090
ProxyClient: Registered as node 1
```

---

## Realm Database

The `acore_auth.realmlist` entry must point to the **proxy**, not to an individual worldserver:

```sql
UPDATE realmlist SET address='<proxy-host>', port=8085 WHERE id=1;
```

Clients always connect to the proxy port. The proxy transparently forwards them to a backend node.

---

## Wire Protocol Reference

All control-channel messages are binary over TCP. No framing beyond the message type byte and per-message length fields.

| Msg | Direction | Format |
|-----|-----------|--------|
| `0x01` MSG_REGISTER | node → proxy | `type(1) + server_type(1) + game_port(2)` |
| `0x10` MSG_REGISTER_ACK | proxy → node | `type(1) + node_id(1)` |
| `0x02` MSG_REROUTE_PLAYER | node → proxy | `type(1) + guid(8) + addr_len(1) + addr(n) + port(2)` |
| `0x03` MSG_CLUSTER_PLAYER_ONLINE | bidirectional | `type(1) + guid(8) + name_len(1) + name(n) + zone(4) + level(1) + class(1) + race(1) + team(1) + node_id(1)` |
| `0x04` MSG_CLUSTER_PLAYER_OFFLINE | bidirectional | `type(1) + guid(8)` |
| `0x05` MSG_CLUSTER_DELIVER_PACKET | node → proxy | `type(1) + guid(8) + pkt_len(2) + pkt_bytes` |
| `0x06` MSG_CLUSTER_RELAY_TO_NODE | node → proxy → node | `type(1) + target_node(1) + inner_type(1) + len(2) + payload` |
| `0x07` MSG_CLUSTER_GROUP_UPDATE | node → proxy → all | `type(1) + group_guid(8) + count(1) + [guid(8)+sub(1)+role(1)+node(1)]×n` |
| `0x08` MSG_CLUSTER_GROUP_DISBAND | node → proxy → all | `type(1) + group_guid(8)` |
| `0x09` MSG_CLUSTER_LFG_RELAY | non-master → proxy → master | `type(1) + len(2) + inner_type(1) + payload` |
| `0x0A` MSG_CLUSTER_LFG_RELAY_RESP | master → proxy → node | `type(1) + target_node(1) + len(2) + inner_type(1) + payload` |

`0x06` inner types: `0x01` GROUP_INVITE, `0x02` GROUP_INVITE_RESULT
`0x09`/`0x0A` inner types: `0x01` LFG_JOIN, `0x02` LFG_LEAVE, `0x03` LFG_PROPOSAL_RESULT, `0x11` LFG_MATCH_NOTIFY

---

## Known Limitations

- **World presence is node-local**: players on different nodes do not see each other in the game world (intended; layering model).
- **Group ownership**: the group object lives on the inviter's node. Remote members are tracked via `ClusterMgr::GetGroupRemoteMembers()` but are not full `Group` member objects on those nodes until they join an instance.
- **Instance server**: when LFG matches a group, each player is rerouted to the instance server address. The instance server then owns the group for the duration of the dungeon.
- **No cross-node LFR**: raid browser stays node-local.
- **Config reload**: adding/removing nodes requires a proxy restart (config is read at startup).
