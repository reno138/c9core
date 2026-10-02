# Contributing to c9core

Thanks for looking. c9core is AzerothCore plus a distributed-server layer, so
it helps to know which half your change belongs to.

## Where a change belongs

- **The distributed-server layer** (NATS bus, cross-node handoffs and redirects,
  routing by map and zone, `nodemgr`, `clustermgr`) lives here. Open an issue
  or pull request in this repository.
- **Game logic, scripts, spells, quests and database content** that are wrong
  in stock AzerothCore too belong
  [upstream](https://github.com/azerothcore/azerothcore-wotlk). Fixing them there
  helps everyone, and c9core picks them up in its next merge.

If you're not sure, open an issue here and ask.

## Pull requests

- Base your branch on `main` and keep each pull request to one change.
- Follow AzerothCore's
  [C++](https://www.azerothcore.org/wiki/cpp-code-standards) and
  [SQL](https://www.azerothcore.org/wiki/sql-standards) code standards.
- Write the commit message as `type(Scope): summary`, for example
  `fix(Core/Cluster): ...`, and use the body to explain *why*.
- Say how you tested it. Changes to handoffs or the bus should be tried on at
  least two nodes, and the pull request should say which handoff paths you
  exercised: login, teleport, portal, zone crossing.
- New config settings go in the matching `.conf.dist`, with a description of
  what the code actually does with them. Remember that config keys are
  case-sensitive and silently ignored when misspelled.
- New files written for c9core use the c9core file header. Files that came
  from AzerothCore keep AzerothCore's header and copyright notice.

## Licence

By contributing you agree that your work is licensed under the GNU GPL v2,
the same as the rest of the project.

Security problems: see [SECURITY.md](SECURITY.md) and please don't post them
publicly.
