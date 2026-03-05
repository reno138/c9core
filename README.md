# C9Core — WoW 3.3.5a Multi-Node Cluster

**C9Core** is a fork of [AzerothCore](https://www.azerothcore.org/) adding a **multi-worldserver cluster** for World of Warcraft 3.3.5a (Wrath of the Lich King).

Clients connect **directly** to worldserver nodes. Cross-zone travel transparently redirects the client to the destination node using the WoW client's native `SMSG_REDIRECT_CLIENT` mechanism — no proxy, no shared cipher state.

All nodes share a single MySQL database cluster, so characters, mail, auctions, guilds, and all persistent data are always consistent.

---

## Architecture

```
WoW Client ──────────────────────► Node 1 (Kalimdor)   :8085
                SMSG_REDIRECT_CLIENT ├───► Node 2 (EK/Outland/Northrend) :8086
                (client reconnects)  └───► Node 3 (Instances)             :8087

                         NATS pub/sub (nats://node1:4222)
                 Node 1 ◄──────────────────────────────► Node 2
                         cluster.announce / cluster.node.{N}
                         cluster.broadcast
```

### Cross-Node Redirect Flow
1. Player enters portal/teleport to map on another node
2. Source node: `SaveToDB` with destination coords → sends `MSG_CLUSTER_REDIRECT_PREP` via NATS to dest node (pre-registers session key)
3. Source node: sends `SMSG_SUSPEND_COMMS` → `SMSG_REDIRECT_CLIENT` (SHA1 token) → `SMSG_FORCE_SEND_QUEUED_PACKETS` → closes socket
4. Client TCP-reconnects to dest node, sends `CMSG_REDIRECTION_AUTH_PROOF`
5. Dest node verifies SHA1, creates `WorldSession`, receives `CMSG_PLAYER_LOGIN`, loads character

---

## Cluster Features

- **Direct client connections** — no proxy bottleneck or ARC4 sync issues
- **NATS-based peer coordination** — `cluster.announce` peer discovery with ACK, heartbeat, dead-node detection
- **Automatic NATS reconnect** — worldserver retries every 10s if NATS is down at startup
- **Cross-node routing table** — `ClusterMgr` maps mapId → nodeId, updated live via NATS
- **Cross-node party** — HP/mana/auras/stats sync via `MSG_CLUSTER_UNIT_UPDATE`
- **Cross-node social** — whisper, guild chat, party chat, /who, friend status
- **Cross-node LFG** — master-node model with relay
- **Cross-node battleground queue**
- **NodeMgr watchdog** — Boost.Asio process supervisor with exponential backoff restart

---

## Building

All cluster nodes run identical Ubuntu 24.04 with the same library versions. **Build once on node 1, rsync the binary to other nodes.**

```bash
# On the primary build node (node 1):
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DAPPS_BUILD=world-only \
         -DCMAKE_INSTALL_PREFIX=$HOME/wowcluster
make worldserver -j8
make install

# Push binary to other nodes (they run the same Ubuntu 24.04 + libs):
rsync ~/wowcluster/bin/worldserver wow@node2:~/wowcluster/bin/worldserver
rsync ~/wowcluster/bin/worldserver wow@node3:~/wowcluster/bin/worldserver
```

> Use `mv newbinary binary` when replacing a running binary to avoid "Text file busy". NodeMgr will restart the worldserver automatically.

---

## Cluster Configuration

Each node needs in its `worldserver-nodeN.conf`:
```ini
ClusterServer.Enable    = 1
ClusterServer.NodeId    = 1          # unique per node
ClusterServer.NatsUrl   = nats://192.0.2.70:4222
ClusterServer.GameAddress = 192.0.2.70
ClusterServer.GamePort  = 8085
ClusterServer.Maps      = 1          # comma-separated map IDs handled by this node
```

NodeMgr config (`nodemgr-nodeN.conf`):
```ini
NodeMgr.UseGdb          = 0          # GDB 17.1 broken for this binary on Ubuntu 24.04
NodeMgr.MaxRestarts     = 10
NodeMgr.StartupDelay    = 3
```

---

## Production Node Layout

| Node | IP | vCPU | RAM | Role | Maps |
|------|----|------|-----|------|------|
| 1 | 192.0.2.70 | 12 | 20 GB | auth + worldserver + NATS | Kalimdor (1) |
| 2 | 192.0.2.71 | 12 | 20 GB | worldserver | EK (0), Outland (530), Northrend (571) |
| 3 | 192.0.2.72 | 12 | 20 GB | worldserver (instances) | All instance maps |

---

## Implementation Status

| Feature | Status |
|---------|--------|
| NATS cluster bus | ✅ Live |
| Peer discovery (cluster.announce + ACK) | ✅ Live |
| NATS reconnect retry | ✅ Live |
| Cross-node routing table (ClusterMgr) | ✅ Live |
| SMSG_REDIRECT_CLIENT cross-node redirect | ✅ Implemented, testing |
| MSG_CLUSTER_REDIRECT_PREP session pre-auth | ✅ Live |
| NodeMgr watchdog | ✅ Live |
| Cross-node party/raid unit updates | ✅ Live |
| Cross-node social (chat/guild/whisper) | ✅ Live |
| Cross-node LFG | ✅ Live |
| Cross-node battleground queue | ✅ Live |
| Login to wrong node → auto-redirect | ✅ Live |
| Proxy server removal | 🔄 In progress |
| Transport clock sync across nodes | 🔄 Planned |

---

## Based On

- [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) — upstream WoW 3.3.5a emulator
