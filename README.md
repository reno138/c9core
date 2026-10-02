# c9core: a distributed World of Warcraft 3.3.5a server

c9core is a fork of [AzerothCore](https://www.azerothcore.org/) that runs
**one realm across several machines**. Each `worldserver` process owns a set
of maps (and, optionally, individual zones). When a player crosses into
territory owned by another process, the server hands the player off and tells
the client to reconnect there. The client sees this as an ordinary loading
screen.

There is no proxy in the data path. Clients connect directly to the
worldserver that owns their location. The handoff uses the 3.3.5a client's own
`SMSG_REDIRECT_CLIENT` mechanism, so stock 3.3.5a (build 12340) clients work
unmodified. All processes share one set of databases, so characters, mail,
auctions and guilds are consistent everywhere. The processes coordinate over a
[NATS](https://nats.io/) message bus.

> **Status: experimental.** Map-based and zone-based handoffs, cross-node
> chat, groups, LFG and battleground queues all run on a four-node lab
> deployment. This has not run a public realm. See [Known
> limitations](#known-limitations) before relying on it.

---

## How it works

```
                        authserver :3724
                              │
   WoW client ──────────────► node 1  :8085   maps 0, 530   (Eastern Kingdoms, Outland)
        │   SMSG_REDIRECT_CLIENT
        └─(reconnects)──────► node 2  :8085   maps 1, 571   (Kalimdor, Northrend)
                              node 3  :8085   zones 1519, 1637, 4395 (Stormwind, Orgrimmar, Dalaran)
                              node 4  :8085   instances (all dungeons, raids, BGs, arenas)

   all nodes ◄──── NATS bus (HMAC-authenticated) ────► all nodes
   all nodes ◄──── one shared MySQL/MariaDB (auth, characters, world)
```

**A handoff, step by step:**

1. A player teleports, takes a portal, or walks into a zone owned by another
   node.
2. The source node saves the character, then sends the destination node a
   one-time redirect token over the bus.
3. The source node sends the client `SMSG_REDIRECT_CLIENT` with the destination
   address, and the client reconnects there.
4. The destination node checks the token, creates the session and loads the
   character. The player arrives with no extra login screen.

Every node announces which maps and zones it owns. All nodes keep the same
routing table, so any node can tell where a player belongs. Nodes heartbeat
each other. A node that goes quiet is declared dead and dropped from routing
until it announces itself again.

**What also works across nodes:** whispers, guild, party and raid chat, `/who`,
friend status, group health/mana/auras, the LFG queue (one master node with
relays on the others), the battleground queue, and transport (zeppelin and
ship) positions.

## Components

| Program | Role |
|---|---|
| `worldserver` | The game server, with the distributed-server code. One per node. |
| `authserver` | Standard AzerothCore login server. One per realm. |
| `nodemgr` | Supervisor. Starts the local worldserver, restarts it with backoff if it dies, and accepts start/stop/restart commands over the bus. That lets you restart even a hung worldserver remotely. |
| `clustermgr` | Operator console. A terminal UI plus a web UI (default `127.0.0.1:9191`) showing every node's state, players and a live map. It can also deploy binaries to nodes over SSH. |
| `nats-server` | Message broker, built from vendored source (v2.10.24) and installed alongside the other binaries. |

---

## Setting it up

### 1. Requirements

- **Linux x86_64.** Developed on Ubuntu 26.04. Everything AzerothCore needs
  ([upstream install guide](https://www.azerothcore.org/wiki/installation)),
  plus:
  - **Go**: `nats-server` is compiled from `deps/nats-server`, with no
    network access needed.
  - **ncurses dev headers** for `clustermgr`.
  - **The MariaDB client library** (`libmariadb-dev` and
    `libmariadb-dev-compat`). A build linked against `libmysqlclient` exits as
    soon as it connects to a MariaDB server.
- **A MySQL or MariaDB server** that every node can reach.
- **A WoW 3.3.5a (12340) client.** You extract the map data from it yourself;
  none is included here.

### 2. Build once and copy to the other nodes

```bash
git clone https://github.com/reno138/c9core.git && cd c9core
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DCMAKE_INSTALL_PREFIX=$HOME/335 \
         -DTOOLS_BUILD=all          # map/vmap/mmap extractors
make -j"$(nproc)" && make install
```

The binaries link only system libraries, so copy `~/335/bin` to the other
nodes as-is. They need the same OS release. When replacing a running binary,
copy it to a new name and `mv` it into place, which avoids "Text file busy".

Extract `dbc`, `maps`, `vmaps` and `mmaps` from the client with the tools in
`~/335/bin`, exactly as for AzerothCore. Every node needs the full set (about
5 GB).

### 3. Databases

Create the `acore_auth`, `acore_world` and `acore_characters` databases once
(see the AzerothCore guide). Every node points at the **same** three.

Let only **one** node apply database updates. On every other node set:

```ini
Updates.EnableDatabases = 0
```

The SQL source path is compiled into the binary. A node that didn't build the
source would otherwise abort at startup looking for a directory it doesn't
have.

### 4. The message bus

Start `nats-server` on one machine. A starter config is installed as
`~/335/etc/nats-server.conf.dist`. It listens on `0.0.0.0:4222` with
monitoring on `127.0.0.1:8222`.

```bash
cp ~/335/etc/nats-server.conf.dist ~/335/etc/nats-server.conf
~/335/bin/nats-server -c ~/335/etc/nats-server.conf
```

Generate **one** shared key for the whole realm:

```bash
openssl rand -hex 32
```

Every message on the bus is authenticated with this key using HMAC-SHA256,
with a 30-second replay window. The bus **refuses to start** without a key of
at least 32 bytes. The HMAC proves who sent a message but doesn't encrypt it,
so keep port 4222 on a private network, or turn on TLS in NATS if it has to
cross one you don't trust.

### 5. Configure each worldserver

In each node's `worldserver.conf`, the distributed-server settings are under
**CLUSTER SERVER**:

```ini
ClusterServer.NodeId      = 2                       # unique per node
ClusterServer.AuthKey     = "<the shared key>"
ClusterServer.NatsURL     = "nats://192.0.2.20:4222"
ClusterServer.GameAddress = "192.0.2.21"            # this node's LAN IP
ClusterServer.GamePort    = 8085                    # = WorldServerPort
ClusterServer.Maps        = "1,571"                 # maps this node owns
```

Plus the usual AzerothCore settings: the three `*DatabaseInfo` strings,
`DataDir` (use an absolute path), and `WorldServerPort`.

**Splitting the world:**

| Setting | Use |
|---|---|
| `ClusterServer.Maps = "0,530"` | This node owns those continents. |
| `ClusterServer.Maps = ""` | Owns everything. Use this for a single node or for development. |
| `ClusterServer.InstanceServer = 1` | Owns every instanceable map (dungeons, raids, BGs, arenas). Leave `Maps` empty. |
| `ClusterServer.Zones = "1519,1637"` | Takes individual zones off a continent. **Also list those zones' maps in `Maps`** (see [Known limitations](#known-limitations)). |
| `ClusterServer.LFGMasterNode`, `BgCoordinatorNode` | Which node runs the LFG queue and coordinates battlegrounds. Default is node 1. |

**Players on the internet.** By default the client is redirected to
`GameAddress`, which is a LAN IP. Players outside your network need a
reachable address:

```ini
ClusterServer.RedirectAddress = "203.0.113.10"   # public IP, or NAT address
ClusterServer.RedirectPort    = 8085             # external port, if it differs
```

Give each node its own external port and forward each one to that node.

### 6. Supervisor and console

`nodemgr.conf` (one per node, next to `worldserver`):

```ini
WorldserverBin    = "./worldserver"
WorldserverConfig = "/home/wow/335/etc/worldserver.conf"
NodeMgr.NodeId    = 2                         # same as ClusterServer.NodeId
NodeMgr.NatsUrl   = "nats://192.0.2.20:4222"
NodeMgr.AuthKey   = "<the shared key>"
```

`clustermgr.conf` (one, on any machine that can reach the bus):

```ini
ClusterServer.NatsUrl = "nats://192.0.2.20:4222"
ClusterMgr.AuthKey    = "<the shared key>"
Web.BindAddr          = "127.0.0.1"
Web.AuthToken         = "<a long random string>"
```

### 7. Run it under systemd

`src/server/apps/clustermgr/c9-clustermgr.service` ships as an example. Units
for the rest look like this:

```ini
# /etc/systemd/system/c9-nodemgr.service, one per node
[Unit]
Description=c9core node supervisor
After=network-online.target
Wants=network-online.target

[Service]
User=wow
WorkingDirectory=/home/wow/335/bin
ExecStart=/home/wow/335/bin/nodemgr -c /home/wow/335/etc/nodemgr.conf
Restart=always
KillMode=process
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
```

`KillMode=process` matters. `nodemgr` stops its worldserver itself: it sends
SIGTERM, then SIGKILL after `NodeMgr.KillTimeout` seconds. So
`systemctl restart c9-nodemgr` gives a clean restart of the whole node.
`c9-nats.service` and `c9-authserver.service` follow the same pattern.

**Start order:** NATS, then authserver, then the nodemgr on each node. Point
the realm address in `acore_auth.realmlist` at node 1.

### Ports

| Port | Who needs it |
|---|---|
| 3724/tcp | authserver, for clients |
| 8085/tcp (per node) | worldservers, for clients |
| 4222/tcp | NATS. **Nodes only, never the internet.** |
| 9191/tcp | clustermgr web UI, bound to localhost by default |

---

## Things that will bite you

- **Config keys are case-sensitive, and unknown keys are ignored silently.**
  A misspelled key gives no warning; the built-in default wins and the server
  looks healthy. The bus URL is spelled differently in each program:
  `ClusterServer.NatsURL` (worldserver), `ClusterServer.NatsUrl`
  (clustermgr), `NodeMgr.NatsUrl` (nodemgr). If a node never joins, this is
  the first thing to check. The symptom is the worldserver dialling the
  default `nats://127.0.0.1:4222`.
- **Edit keys where they are.** Don't append a second copy at the end of
  the file, then make sure exactly one active copy exists.
- **Leave `Cluster.Enabled = 0`.** Upstream AzerothCore has its own,
  unrelated clustering hooks for [ToCloud9](https://github.com/walkline/ToCloud9),
  configured under `Cluster.*`, not to be confused with c9core's
  `ClusterServer.*`. Turning it on makes the worldserver trust an external
  gateway and skip session-key checks, encryption and ban enforcement. c9core
  doesn't use it, so never enable both.
- **`ClusterServer.RedirectDebug = 1` writes session keys to the log.** It
  is for debugging the handshake on a test realm only.
- **`ClusterServer.AllowRemoteConsole = 1` lets the bus run console
  commands** with full server authority. Enable it only where you mean to,
  and only with a strong key.

## Known limitations

- **A node with zones but no maps can't host players.** Routing checks the
  map before the zone. A node with `Zones` set and `Maps` empty therefore
  owns no map and turns every arrival away. Workaround: also list the maps
  those zones are on, for example zones `1519,1637,4395` with Maps
  `0,1,571`. That node then loads the whole map, so spawns are duplicated
  with the continent node. The real fix is zone-scoped spawning.
- **Intermittent worldserver crashes** have been seen on some nodes and
  haven't been tracked down yet. `nodemgr` restarts the process.
  `NodeMgr.UseGdb = 1` captures backtraces.
- **Upstream lag.** Last merged with AzerothCore as of 2026-10-02.

## Licence and credits

c9core is GNU GPL v2, like AzerothCore; see [LICENSE](LICENSE). Nearly all
of the game itself is AzerothCore's work and that of the projects before it:
see [AUTHORS](AUTHORS) and [THANKS](THANKS). Files that came from AzerothCore
keep its headers and copyright notices. Files written for c9core say so in
their headers.

Vendored dependencies keep their own licences: [nats.c](deps/cnats/LICENSE)
and [nats-server](deps/nats-server/LICENSE) are both Apache-2.0.

World of Warcraft is a trademark of Blizzard Entertainment. This project is
not affiliated with or endorsed by Blizzard and includes no Blizzard game
data.
