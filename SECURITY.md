# Security Policy

## Supported versions

This project is pre-1.0 and maintained by one person. Only the most
recently released version of each piece (the addon, `timeshift_buffer`,
`recording_edl` -- they version independently, see `CHANGELOG.md`) gets
security fixes. There's no backport policy for older releases.

## Reporting a vulnerability

**Please don't open a public GitHub issue for a security report.**

Use GitHub's private vulnerability reporting for this repo instead:
[Report a vulnerability](https://github.com/BruiserBrody17/pvr.dispatcharr-unofficial/security/advisories/new)
(also reachable from the repo's "Security" tab). If that's not
available for some reason, open a regular issue asking for a private
contact channel rather than describing the issue itself.

This is a single-maintainer hobby project, not a company -- there's no
SLA, but reports are taken seriously and I'll respond as soon as I
reasonably can. Happy to credit reporters in the fix's changelog entry
if you'd like that; just say so in the report.

## Scope

- **The C++ Kodi addon** talks to a Dispatcharr server you configure
  yourself. Data it receives from that server (API responses, EPG data,
  WebSocket messages) is treated as semi-trusted network input in this
  project's own threat model -- a bug in how the addon *handles* that
  data is in scope. The Dispatcharr server itself, and your own
  credentials/network setup, are not.
- **The two `dispatcharr-plugin/` Python plugins run server-side on
  Dispatcharr**, exposed over Dispatcharr's own plugin `run/` HTTP API.
  This is real attack surface -- anything a caller with API access could
  do that the plugin didn't intend (path traversal, injection, hitting
  something outside the plugin's own intended scope) is in scope. Two
  real vulnerabilities of exactly this shape have already been found and
  fixed here via review; see `CHANGELOG.md` and `docs/TIMESHIFT.md` for
  the write-ups.
- **Out of scope:** vulnerabilities in Dispatcharr itself (report to
  [that project](https://github.com/Dispatcharr/Dispatcharr) instead),
  Kodi-core, or ffmpeg -- and anything that requires an attacker to
  already have the level of access a vulnerability report is usually
  about *preventing* (e.g. physical access to the machine, or an
  already-compromised Dispatcharr admin account acting entirely within
  its own real permissions).
