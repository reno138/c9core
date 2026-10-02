# Getting help

**Setting up c9core:** start with the [README](../README.md). It covers
building, shared databases, the NATS bus, per-node config, systemd and ports,
plus a list of config traps that cause most "node won't join" problems.

**Something broken in the distributed-server layer** (handoffs, redirects,
the bus, `nodemgr`, `clustermgr`): open an
[issue](https://github.com/reno138/c9core/issues). Please include:

- the commit you built (`git rev-parse --short HEAD`)
- how many nodes you run, and each node's `ClusterServer.*` settings, **with
  `AuthKey` removed**
- the worldserver log from both the source and the destination node around
  the time of the problem

**General AzerothCore questions** (installing the core, extracting client
data, database setup, game content) are best asked in
[AzerothCore's community](https://www.azerothcore.org/), which has far more
people who can answer them.

This is a one-person project, so replies can take a while.
