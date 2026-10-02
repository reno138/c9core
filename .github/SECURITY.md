# Security policy

## Reporting a vulnerability

**Please don't open a public issue for a security problem.**

Report it privately through GitHub instead: open the repository's
**Security** tab and choose **Report a vulnerability**, or go to
<https://github.com/reno138/c9core/security/advisories/new>. Only the
maintainer can see the report.

Please include:

- what is affected (a file, function or config setting) and the commit you
  tested
- how to reproduce it, and what an attacker gains
- whether it needs access to the NATS bus, a game account, or nothing at all

This is a one-person project, so a fix may take a while. You'll get a reply
before anything is disclosed.

## Scope

**In scope:** code this repository adds to AzerothCore, which means the
distributed-server layer. That includes the NATS bus and its authentication
(`ClusterAuth`, `NatsBus`), cross-node client redirects (`ClientRedirect`,
`WorldSocket` redirect handling), `nodemgr` and `clustermgr`, including the
clustermgr web UI.

**Report upstream instead:** problems that also exist in
[AzerothCore](https://github.com/azerothcore/azerothcore-wotlk) itself should
go to AzerothCore under its own security policy. Feel free to tell us too.

## What the bus does and doesn't protect

Every message on the NATS bus is authenticated with HMAC-SHA256 using the
shared `ClusterServer.AuthKey`, and carries a 30-second replay window. It is
**not encrypted**. Keep port 4222 on a private network, or turn on TLS in NATS
if it has to cross one you don't trust. Anyone who holds the AuthKey controls
the whole realm, including remote console commands on any node that sets
`ClusterServer.AllowRemoteConsole = 1`.

## Supported versions

Only the latest commit on `main` gets fixes. There are no releases yet.
