# C9Core — WoW 3.3.5a Cluster Server

**C9Core** is a fork of [AzerothCore](https://www.azerothcore.org/) adding a **multi-worldserver cluster** for World of Warcraft 3.3.5a (Wrath of the Lich King).

Players connect through a transparent **proxy server** that routes them to one of several backend worldserver nodes. All nodes share a single database cluster, so characters, mail, auctions, guilds, and all persistent data are always consistent across nodes.

---

## Features

### Cluster Features
- **Transparent proxy**: clients connect to one address; the proxy routes them to the right node
- **Zone-based routing**: configure which maps live on which node (Eastern Kingdoms on node 1, Outland on node 2, etc.)
- **Cross-node party / raid**: health bars, mana, auras, and stats update live across nodes
- **Cross-node social**: whispers, guild chat, party chat, /who, friend status
- **Cross-node group formation**: invite players on any node, form groups naturally
- **Cross-node LFG / dungeon finder**: master-node model with relay to all nodes
- **Cross-node rerouting**: teleporting to a map on another node transparently reconnects the client

### Gameplay (unchanged from AzerothCore)
All core gameplay mechanics — combat, spells, pathfinding, AI, quests, crafting, battlegrounds, arenas — are preserved exactly as upstream AzerothCore. Cluster code is additive-only.

---

## Architecture

```
WoW Client A ──┐
WoW Client B ──┤──► proxyserver :8085 ──► worldserver node 1 :8086
WoW Client C ──┤          │
WoW Client D ──┘          └──► worldserver node 2 :8087
                                └──► worldserver node 3 :8088
```

See [CLUSTER.md](CLUSTER.md) for full architecture, wire protocol, configuration reference, and deployment guide.

---

## Building

```bash
# Build all (worldserver + authserver + proxyserver)
mkdir build && cd build
cmake .. -DCMAKE_INSTALL_PREFIX=$HOME/wowcluster \
         -DCMAKE_BUILD_TYPE=RelWithDebInfo \
         -DSCRIPTS=static -DMODULES=static
make -j4    # use -j4 max; -j6 can exhaust swap on 8GB systems
make install
```

> **Note**: Remote cluster nodes (Boost 1.83 + MySQL 8) must be built natively on each machine.
> Use `-DAPPS_BUILD=world-only` on nodes that only run `worldserver`.

---

## Cluster Nodes — Current Production Layout

| Node | IP | Role | Maps |
|------|----|------|------|
| 1 | 192.0.2.70 | proxy + auth + worldserver-node1 | Eastern Kingdoms, Kalimdor |
| 2 | 192.0.2.71 | worldserver-node2 | Outland, Northrend |
| 3 | 192.0.2.72 | worldserver-node3 | All instances |

---

## Implementation Status

| Phase | Feature | Commit | Status |
|-------|---------|--------|--------|
| — | PathGenerator sliced-path fix | `007baa789` | ✅ |
| 6 | Zone-based map routing + player rerouting | `b6d1bf431` | ✅ |
| 7 | Cross-node party visibility (HP/mana/auras) | `cf1b0c5df` | ✅ |
| 8 | Cross-node battleground queue | — | Planned |
| 8 | Cross-node LFD improvements | — | Planned |

---

## Based On

- [AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) — the upstream WoW 3.3.5a server emulator
- [TrinityCore](https://github.com/TrinityCore/TrinityCore) — referenced for specific patches

